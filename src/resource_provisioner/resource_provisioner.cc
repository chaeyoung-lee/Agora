/**
 * @file rp_thread.cc
 * @brief Implementation file for the ResourceProvisionerThread class.
 */
#include "resource_provisioner.h"

ResourceProvisionerThread::ResourceProvisionerThread(
    Config* cfg,
    moodycamel::ConcurrentQueue<EventData>* rx_queue,
    moodycamel::ConcurrentQueue<EventData>* tx_queue,
    const std::string& log_filename)
    : cfg_(cfg),
      freq_ghz_(GetTime::MeasureRdtscFreq()),
      tsc_delta_((cfg_->GetFrameDurationSec() * 1e9) / freq_ghz_),
      rx_queue_(rx_queue),
      tx_queue_(tx_queue) {

  // Set up log file
  if (log_filename.empty() == false) {
    log_filename_ = log_filename;  // Use a non-default log filename
  } else {
    log_filename_ = kDefaultLogFilename;
  }
  log_file_ = std::fopen(log_filename_.c_str(), "w");
  RtAssert(log_file_ != nullptr, "Failed to open DYNAMIC CORE log file");

  AGORA_LOG_INFO(
      "ResourceProvisionerThread: Frame duration %.2f ms, tsc_delta %zu\n",
      cfg_->GetFrameDurationSec() * 1000, tsc_delta_);

  const size_t udp_pkt_len = cfg_->RpDataBytesNumPerframe(Direction::kDownlink);
  udp_pkt_buf_.resize(udp_pkt_len + kUdpRxBufferPadding);

  // TODO: See if it makes more sense to split up the UE's by port here for
  // client mode.
  size_t udp_server_port = cfg_->RpRxPort();
  AGORA_LOG_INFO(
      "ResourceProvisionerThread: setting up udp server for rp data at port %zu\n",
      udp_server_port);
  udp_server_ = std::make_unique<UDPServer>(
      udp_server_port, udp_pkt_len * kMaxUEs * kMaxPktsPerUE);

  udp_client_ = std::make_unique<UDPClient>();
}

ResourceProvisionerThread::~ResourceProvisionerThread() {
  std::fclose(log_file_);
  AGORA_LOG_INFO("ResourceProvisionerThread: RP thread destroyed\n");
}

/*
 * RP -> Agora
 */
void ResourceProvisionerThread::SendEventtoAgora(event) {
  tx_queue_.enqueue(event);
}

void ResourceProvisionerThread::ReceiveUdpPacketsfromRp() {
  udp_server_->Recv();
  event = ~;
  SendEventtoAgora(event)
}

/*
 * RP <- Agora
 */
void ResourceProvisionerThread::ReceiveEventfromAgora() {
  rx_queue_.dequeue(event)
  SendUdpPacketstoRp(event);
}

void ResourceProvisionerThread::SendUdpPacketstoRp(event) {
  // Create traffic data packet
  RPTrafficMsg msg;
  msg.latency_ = event->latency_; // TODO: pseudo for now
  msg.queue_load_ = event->latency; // TODO: pseudo for now
  udp_client_->Send(kRpRemoteHostname, kRpRemotePort, (uint8_t*)&msg, sizeof(RPTrafficMsg));

  // update RAN config within Agora
  SendRanConfigUpdate(EventData(EventType::kRANUpdate));
}

void ResourceProvisionerThread::RunEventLoop() {
  AGORA_LOG_INFO(
    "ResourceProvisionerThread: Running RP thread event loop, logging to file "
    "%s\n",
    log_filename_.c_str()
  );

  PinToCoreWithOffset(ThreadType::kMaster, core_offset_,
                      0 /* thread ID */);

  while (cfg_->Running() == true) {
    // loop
  }
}