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
  crc_obj_ = std::make_unique<DoCRC>();
}

ResourceProvisionerThread::~ResourceProvisionerThread() {
  std::fclose(log_file_);
  AGORA_LOG_INFO("ResourceProvisionerThread: RP thread destroyed\n");
}


/*
 * UDP Processing
 */

void ResourceProvisionerThread::ProcessUdpPacketsFromApps() {
  const size_t max_data_bytes_per_frame =
      cfg_->RpDataBytesNumPerframe(Direction::kDownlink);
  const size_t num_rp_packets_per_frame =
      cfg_->RpPacketsPerframe(Direction::kDownlink);

  if (0 == max_data_bytes_per_frame) {
    return;
  }

  // Processes the packets of an entire frame (remove variable later)
  const size_t packets_required = num_rp_packets_per_frame;

  size_t packets_received = 0;
  size_t current_packet_bytes = 0;
  size_t current_packet_start_index = 0;

  size_t total_bytes_received = 0;

  const size_t max_recv_attempts = (packets_required * 10u);
  size_t rx_attempts;
  for (rx_attempts = 0u; rx_attempts < max_recv_attempts; rx_attempts++) {
    ssize_t ret = udp_server_->Recv(&udp_pkt_buf_.at(total_bytes_received),
                                    udp_pkt_buf_.size() - total_bytes_received);
    if (ret == 0) {
      AGORA_LOG_TRACE(
          "ResourceProvisionerThread: No data received with %zu pending\n",
          total_bytes_received);
      if (total_bytes_received == 0) {
        return;  // No data received
      } else {
        AGORA_LOG_INFO(
            "ResourceProvisionerThread: No data received but there was data in "
            "buffer pending %zu : try %zu out of %zu\n",
            total_bytes_received, rx_attempts, max_recv_attempts);
      }
    } else if (ret < 0) {
      // There was an error in receiving
      AGORA_LOG_ERROR("ResourceProvisionerThread: Error in reception %zu\n", ret);
      cfg_->Running(false);
      return;
    } else { /* Got some data */
      total_bytes_received += ret;
      current_packet_bytes += ret;

      // std::printf(
      //    "Received %zu bytes packet number %zu packet size %zu total %zu\n",
      //    ret, packets_received, total_bytes_received, current_packet_bytes);

      // While we have packets remaining and a header to process
      const size_t header_size = sizeof(RpPacketHeaderPacked);
      while ((packets_received < packets_required) &&
             (current_packet_bytes >= header_size)) {
        // See if we have enough data and process the RpPacket header
        const auto* rx_rp_packet_header =
            reinterpret_cast<const RpPacketPacked*>(
                &udp_pkt_buf_.at(current_packet_start_index));

        const size_t current_packet_size =
            header_size + rx_rp_packet_header->PayloadLength();

        // std::printf("Packet number %zu @ %zu packet size %d:%zu total %zu\n",
        //            packets_received, current_packet_start_index,
        //            rx_rp_packet_header->datalen_, current_packet_size,
        //            current_packet_bytes);

        if (current_packet_bytes >= current_packet_size) {
          current_packet_bytes = current_packet_bytes - current_packet_size;
          current_packet_start_index =
              current_packet_start_index + current_packet_size;
          packets_received++;
        } else {
          // Don't have the entire packet, keep trying
          break;
        }
      }
      AGORA_LOG_FRAME(
          "ResourceProvisionerThread: Received %zu : %zu bytes in packet %zu : "
          "%zu\n",
          ret, total_bytes_received, packets_received, packets_required);
    }

    // Check for completion
    if (packets_received == packets_required) {
      break;
    }
  }  // end rx attempts

  if (packets_received != packets_required) {
    AGORA_LOG_ERROR(
        "ResourceProvisionerThread: Received %zu : %zu packets with %zu total bytes "
        "in %zu attempts\n",
        packets_received, packets_required, total_bytes_received, rx_attempts);
  } else {
    AGORA_LOG_FRAME("RpThreadClient: Received Rp Frame Data\n");
  }
  RtAssert(packets_received == packets_required,
           "ResourceProvisionerThread: ProcessUdpPacketsFromApps incorrect data "
           "received!");

  // Currently this is a packet list of rp packets
  ProcessUdpPacketsFromAppsBs((char*)&udp_pkt_buf_[0]);
}

void ResourceProvisionerThread::ProcessUdpPacketsFromAppsBs(const char* payload) {
  const size_t rp_packet_length = cfg_->RpPacketLength(Direction::kDownlink);
  const size_t num_rp_packets_per_frame =
      cfg_->RpPacketsPerframe(Direction::kDownlink);
  const size_t num_pilot_symbols = cfg_->Frame().ClientDlPilotSymbols();

  /*
   * Get data and wrap it into packets
   */
  // Data integrity check
  size_t pkt_offset = 0;
  size_t ue_id = 0;
  size_t symbol_id = 0;
  size_t frame_id = 0;
  for (size_t packet = 0u; packet < num_rp_packets_per_frame; packet++) {
    const auto* pkt =
        reinterpret_cast<const RpPacketPacked*>(&payload[pkt_offset]);

    // std::printf("Frame %d, Packet %zu, symbol %d, user %d\n", pkt->Frame(),
    //            packet, pkt->Symbol(), pkt->Ue());
    if (packet == 0) {
      ue_id = pkt->Ue();
      frame_id = pkt->Frame();
    } else {
      if (ue_id != pkt->Ue()) {
        AGORA_LOG_ERROR(
            "Received pkt %zu data with unexpected UE id %zu, expected %d\n",
            packet, ue_id, pkt->Ue());
      }
      if ((symbol_id + 1) != pkt->Symbol()) {
        AGORA_LOG_ERROR("Received out of order symbol id %d, expected %zu\n",
                        pkt->Symbol(), symbol_id + 1);
      }

      if (frame_id != pkt->Frame()) {
        AGORA_LOG_ERROR(
            "Received pkt %zu data with unexpected frame id %zu, expected %d\n",
            packet, frame_id, pkt->Frame());
      }
    }
    symbol_id = pkt->Symbol();
    pkt_offset += RpPacketPacked::kHeaderSize + pkt->PayloadLength();
  }

  if (next_radio_id_ != ue_id) {
    AGORA_LOG_ERROR("Error - radio id %zu, expected %zu\n", ue_id,
                    next_radio_id_);
  }
  // End data integrity check

  next_radio_id_ = ue_id;

  // We've received bits for the uplink.
  size_t& radio_buf_id = client_.dl_bits_buffer_id_[next_radio_id_];

  if ((*client_.dl_bits_buffer_status_)[next_radio_id_][radio_buf_id] == 1) {
    std::fprintf(
        stderr,
        "ResourceProvisionerThread: UDP RX buffer full, buffer ID: %zu. Dropping "
        "rx frame data\n",
        radio_buf_id);
    return;
  }

#if ENABLE_RB_IND
  RBIndicator ri;
  ri.ue_id_ = next_radio_id_;
  ri.mod_order_bits_ = CommsLib::kQaM16;
#endif

  if (kLogRpPackets) {
    std::stringstream ss;
    std::fprintf(
        log_file_,
        "ResourceProvisionerThread: Received data from app for frame %zu, ue "
        "%zu size %zu\n",
        next_tx_frame_id_, next_radio_id_, pkt_offset);

    for (size_t i = 0; i < pkt_offset; i++) {
      ss << std::to_string((uint8_t)(payload[i])) << " ";
    }
    std::fprintf(log_file_, "%s\n", ss.str().c_str());
  }

  size_t src_pkt_offset = 0;
  // Copy from the packet rx buffer into ul_bits memory (unpacked)
  for (size_t pkt_id = 0; pkt_id < num_rp_packets_per_frame; pkt_id++) {
    const auto* src_packet =
        reinterpret_cast<const RpPacketPacked*>(&payload[src_pkt_offset]);
    const size_t symbol_idx =
        cfg_->Frame().GetDLSymbolIdx(src_packet->Symbol());
    // next_radio_id_ = src_packet->ue_id;

    // could use pkt_id vs src_packet->symbol_id_ but might reorder packets
    const size_t dest_pkt_offset = ((radio_buf_id * num_rp_packets_per_frame) +
                                    (symbol_idx - num_pilot_symbols)) *
                                   rp_packet_length;

    auto* pkt = reinterpret_cast<RpPacketPacked*>(
        &(*client_.dl_bits_buffer_)[next_radio_id_][dest_pkt_offset]);

    pkt->Set(next_tx_frame_id_, src_packet->Symbol(), src_packet->Ue(),
             src_packet->PayloadLength());

#if ENABLE_RB_IND
    pkt->rb_indicator_ = ri;
#endif

    pkt->LoadData(src_packet->Data());
    // Insert CRC
    pkt->Crc((uint16_t)(
        crc_obj_->CalculateCrc24(pkt->Data(), pkt->PayloadLength()) & 0xFFFF));

    if (kLogRpPackets) {
      std::stringstream ss;

      ss << "ResourceProvisionerThread: created packet frame " << next_tx_frame_id_
         << ", pkt " << pkt_id << ", size "
         << cfg_->RpPayloadMaxLength(Direction::kDownlink) << " radio buff id "
         << radio_buf_id << ", loc " << (size_t)pkt << " dest offset "
         << dest_pkt_offset << std::endl;

      ss << "Header Info:" << std::endl
         << "FRAME_ID: " << pkt->Frame() << std::endl
         << "SYMBOL_ID: " << pkt->Symbol() << std::endl
         << "UE_ID: " << pkt->Ue() << std::endl
         << "DATLEN: " << pkt->PayloadLength() << std::endl
         << "PAYLOAD:" << std::endl;
      for (size_t i = 0; i < pkt->PayloadLength(); i++) {
        ss << std::to_string(pkt->Data()[i]) << " ";
      }
      ss << std::endl;
      std::fprintf(stdout, "%s", ss.str().c_str());
      std::fprintf(log_file_, "%s", ss.str().c_str());
      ss.str("");
    }
    src_pkt_offset += pkt->PayloadLength() + RpPacketPacked::kHeaderSize;
  }  // end all packets


  /*
   * Enqueue to rx_queue
   */
  (*client_.dl_bits_buffer_status_)[next_radio_id_][radio_buf_id] = 1;
  EventData msg(EventType::kPacketFromRp,
                rx_rp_tag_t(next_radio_id_, radio_buf_id).tag_);
  AGORA_LOG_FRAME("ResourceProvisionerThread: Tx rp information to %zu %zu\n",
                  next_radio_id_, radio_buf_id);
  RtAssert(rx_queue_->enqueue(msg),
           "ResourceProvisionerThread: Failed to enqueue downlink packet");

  radio_buf_id = (radio_buf_id + 1) % kFrameWnd;
  // Might be unnecessary now.
  next_radio_id_ = (next_radio_id_ + 1) % cfg_->UeAntNum();
  if (next_radio_id_ == 0) {
    next_tx_frame_id_++;
  }
}

void ResourceProvisionerThread::RunEventLoop() {
  AGORA_LOG_INFO(
      "ResourceProvisionerThread: Running RP thread event loop, logging to file "
      "%s\n",
      log_filename_.c_str());
  // PinToCoreWithOffset(ThreadType::kWorkerRpTXRX, core_offset_,
  //                     0 /* thread ID */);

  size_t last_frame_tx_tsc = 0;

  while (cfg_->Running() == true) {
    ProcessRxFromPhy();

    if ((GetTime::Rdtsc() - last_frame_tx_tsc) > tsc_delta_) {
      SendControlInformation();
      last_frame_tx_tsc = GetTime::Rdtsc();
    }

    // No need to process incomming packets if we are finished
    if (next_tx_frame_id_ != cfg_->FramesToTest()) {
      ProcessUdpPacketsFromApps();
    }
  }
}