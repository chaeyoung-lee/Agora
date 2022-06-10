/**
 * @file resource_manager.cc
 * @brief Implementation file for the ResourceManager class.
 */
#include "resource_manager.h"

#include "logger.h"
#include "utils_ldpc.h"

static constexpr size_t kUdpRxBufferPadding = 2048u;

ResourceManager::ResourceManager(
    Config* cfg, size_t core_offset,
    moodycamel::ConcurrentQueue<EventData>* rx_queue,
    moodycamel::ConcurrentQueue<EventData>* tx_queue,
    const std::string& log_filename)
    : cfg_(cfg),
      core_offset_(core_offset),
      rx_queue_(rx_queue),
      tx_queue_(tx_queue) {
  // Set up MAC log file
  if (log_filename.empty() == false) {
    log_filename_ = log_filename;  // Use a non-default log filename
  } else {
    log_filename_ = kDefaultLogFilename;
  }
  log_file_ = std::fopen(log_filename_.c_str(), "w");
  RtAssert(log_file_ != nullptr, "Failed to open MAC log file");

  AGORA_LOG_INFO(
      "MacThreadBaseStation: Frame duration %.2f ms, tsc_delta %zu\n",
      cfg_->GetFrameDurationSec() * 1000, tsc_delta_);

  // Set up buffers
  client_.dl_bits_buffer_id_.fill(0);
  client_.dl_bits_buffer_ = dl_bits_buffer;
  client_.dl_bits_buffer_status_ = dl_bits_buffer_status;

  server_.n_filled_in_frame_.fill(0);
  for (size_t ue_ant = 0; ue_ant < cfg_->UeAntTotal(); ue_ant++) {
    server_.data_size_.emplace_back(
        std::vector<size_t>(cfg->Frame().NumUlDataSyms()));
  }

  // The frame data will hold the data comming from the Phy (Received)
  for (auto& v : server_.frame_data_) {
    v.resize(cfg_->MacDataBytesNumPerframe(Direction::kUplink));
  }

  const size_t udp_pkt_len =
      cfg_->MacDataBytesNumPerframe(Direction::kDownlink);
  udp_pkt_buf_.resize(udp_pkt_len + kUdpRxBufferPadding);

  // TODO: See if it makes more sense to split up the UE's by port here for
  // client mode.
  size_t udp_server_port = cfg_->BsMacRxPort();
  AGORA_LOG_INFO(
      "MacThreadBaseStation: setting up udp server for mac data at port %zu\n",
      udp_server_port);
  udp_server_ = std::make_unique<UDPServer>(
      udp_server_port, udp_pkt_len * kMaxUEs * kMaxPktsPerUE);

  udp_client_ = std::make_unique<UDPClient>();
  crc_obj_ = std::make_unique<DoCRC>();
}

ResourceManager::~ResourceManager() {
  std::fclose(log_file_);
  AGORA_LOG_INFO("ResourceManager: MAC thread destroyed\n");
}