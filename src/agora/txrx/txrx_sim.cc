/**
 * @file txrx.cc
 * @brief Implementation of PacketTXRX initialization functions, and datapath
 * functions for communicating with simulators.
 */

#include "txrx_sim.h"

#include "logger.h"

static constexpr bool kEnableSlowStart = true;
static constexpr bool kEnableSlowSending = false;
static constexpr bool kDebugPrintBeacon = false;

static constexpr size_t kSlowStartMulStage1 = 32;
static constexpr size_t kSlowStartMulStage2 = 8;

PacketTXRX::PacketTXRX(Config* cfg, size_t core_offset)
    : cfg_(cfg),
      core_offset_(core_offset),
      ant_per_cell_(cfg->BsAntNum() / cfg->NumCells()),
      num_socket_thread_(cfg->SocketThreadNum()),
      beacon_send_time_(0.0f),
      num_interfaces_(cfg->NumRadios()) {}

PacketTXRX::PacketTXRX(Config* cfg, size_t core_offset,
                       moodycamel::ConcurrentQueue<EventData>* queue_message,
                       moodycamel::ConcurrentQueue<EventData>* tx_pending_q,
                       moodycamel::ProducerToken** notify_producer_tokens,
                       moodycamel::ProducerToken** tx_producer_tokens)
    : PacketTXRX(cfg, core_offset) {
  event_notify_q_ = queue_message;
  tx_pending_q_ = tx_pending_q;
  notify_producer_tokens_ = notify_producer_tokens;
  tx_producer_tokens_ = tx_producer_tokens;
}

PacketTXRX::~PacketTXRX() {
  for (auto& socket_std_thread : socket_std_threads_) {
    socket_std_thread.join();
  }
}

bool PacketTXRX::StartTxRx(Table<char>& buffer, size_t packet_num_in_buffer,
                           Table<size_t>& frame_start, char* tx_buffer,
                           Table<complex_float>& calib_dl_buffer,
                           Table<complex_float>& calib_ul_buffer) {
  frame_start_ = &frame_start;
  tx_buffer_ = tx_buffer;

  MLPD_INFO("PacketTXRX: rx threads %zu, packet buffers %zu\n",
            num_socket_thread_, packet_num_in_buffer);

  buffers_per_socket_ = packet_num_in_buffer / num_socket_thread_;
  /// Make sure we can fit each channel in the tread buffer without rollover
  assert(buffers_per_socket_ % cfg_->NumChannels() == 0);

  rx_packets_.resize(num_socket_thread_);
  for (size_t i = 0; i < num_socket_thread_; i++) {
    rx_packets_.at(i).reserve(buffers_per_socket_);
    for (size_t number_packets = 0; number_packets < buffers_per_socket_;
         number_packets++) {
      auto* pkt_loc = reinterpret_cast<Packet*>(
          buffer[i] + (number_packets * cfg_->PacketLength()));
      rx_packets_.at(i).emplace_back(pkt_loc);
    }

    MLPD_SYMBOL("LoopTXRX: Starting thread %zu\n", i);
    socket_std_threads_.emplace_back(
        std::thread(&PacketTXRX::LoopTxRx, this, i));
  }
  return true;
}

void PacketTXRX::SendBeacon(TxRxThreadStorage& local_storage, size_t frame_id) {
  const double time_now = GetTime::GetTimeUs() / 1000.0f;

  // Send a beacon packet in the downlink to trigger user pilot
  auto* pkt = reinterpret_cast<Packet*>(local_storage.beacon_buffer_.data());

  if (kDebugPrintBeacon) {
    std::printf("TXRX [%zu]: Sending beacon for frame %zu tx delta %f ms\n",
                local_storage.tid_, frame_id, time_now - beacon_send_time_);
  }
  beacon_send_time_ = time_now;

  for (size_t beacon_sym = 0u; beacon_sym < cfg_->Frame().NumBeaconSyms();
       beacon_sym++) {
    for (size_t interface = 0u; interface < local_storage.num_interfaces_;
         interface++) {
      const size_t global_interface_id =
          interface + local_storage.interface_offset_;
      for (size_t channel = 0u; channel < cfg_->NumChannels(); channel++) {
        const size_t ant_id =
            ((global_interface_id * cfg_->NumChannels()) + channel);
        new (pkt) Packet(frame_id, cfg_->Frame().GetBeaconSymbol(beacon_sym),
                         0 /* cell_id */, ant_id);

        local_storage.udp_clients_.at(interface)->Send(
            cfg_->BsRruAddr(), cfg_->BsRruPort() + global_interface_id,
            local_storage.beacon_buffer_.data(),
            local_storage.beacon_buffer_.size());
      }
    }
  }
}

void PacketTXRX::LoopTxRx(size_t tid) {
  PinToCoreWithOffset(ThreadType::kWorkerTXRX, core_offset_, tid);

  const size_t radio_lo = (tid * num_interfaces_) / num_socket_thread_;
  const size_t radio_hi = ((tid + 1) * num_interfaces_) / num_socket_thread_;

  //Thread is assigned a single producer
  TxRxThreadStorage thread_store(
      tid, radio_hi, radio_lo, cfg_, (*frame_start_)[tid],
      *tx_producer_tokens_[tid], *notify_producer_tokens_[tid]);

  const double rdtsc_freq = GetTime::MeasureRdtscFreq();
  const size_t frame_tsc_delta =
      cfg_->GetFrameDurationSec() * 1e9f * rdtsc_freq;
  //const size_t two_hundred_ms_ticks = (0.2f /* 200 ms */ * 1e9f * rdtsc_freq);

  // Slow start variables (Start with no less than 200 ms)
  //const size_t slow_start_tsc1 =
  //    std::max(kSlowStartMulStage1 * frame_tsc_delta, two_hundred_ms_ticks);

  const size_t slow_start_tsc1 = kSlowStartMulStage1 * frame_tsc_delta;
  const size_t slow_start_thresh1 = kFrameWnd;
  const size_t slow_start_tsc2 = kSlowStartMulStage2 * frame_tsc_delta;
  const size_t slow_start_thresh2 = kFrameWnd * 4;
  size_t delay_tsc = frame_tsc_delta;

  if (kEnableSlowStart) {
    delay_tsc = slow_start_tsc1;
  }

  size_t prev_frame_id = SIZE_MAX;
  size_t rx_slot = 0;
  size_t tx_frame_start = GetTime::Rdtsc();
  size_t tx_frame_id = 0;
  size_t send_time = delay_tsc + tx_frame_start;
  size_t current_interface = 0;
  // Send Beacons for the first time to kick off sim
  // SendBeacon(tid, tx_frame_id++);
  while (cfg_->Running() == true) {
    size_t rdtsc_now = GetTime::Rdtsc();

    if (rdtsc_now > send_time) {
      SendBeacon(thread_store, tx_frame_id++);

      if (kEnableSlowStart) {
        if (tx_frame_id == slow_start_thresh1) {
          delay_tsc = slow_start_tsc2;
        } else if (tx_frame_id == slow_start_thresh2) {
          delay_tsc = frame_tsc_delta;
          if (kEnableSlowSending) {
            // Temp for historic reasons
            delay_tsc = frame_tsc_delta * 4;
          }
        }
      }
      tx_frame_start = send_time;
      send_time += delay_tsc;
    }

    int send_result = DequeueSend(thread_store);
    if (-1 == send_result) {
      // receive data
      // Need to get NumChannels data here
      Packet* pkt = RecvEnqueue(thread_store,
                                rx_packets_.at(thread_store.tid_).at(rx_slot),
                                current_interface);
      if (pkt != nullptr) {
        rx_slot = (rx_slot + 1) % buffers_per_socket_;

        if (kIsWorkerTimingEnabled) {
          uint32_t frame_id = pkt->frame_id_;
          if (frame_id != prev_frame_id) {
            thread_store.rx_frame_start_[frame_id % kNumStatsFrames] =
                GetTime::Rdtsc();
            prev_frame_id = frame_id;
          }
        }

        if (++current_interface == thread_store.num_interfaces_) {
          current_interface = 0;
        }
      }
    }  // end if -1 == send_result
  }    // end while
}

Packet* PacketTXRX::RecvEnqueue(TxRxThreadStorage& thread_info,
                                RxPacket& rx_placement, size_t radio_id) {
  const size_t packet_length = cfg_->PacketLength();
  //Obtain memory to place incomming messages (move this to the thread store)
  //RxPacket& rx = rx_packets_.at(thread_info.tid_).at(rx_slot);

  // if rx_buffer is full, exit
  if (rx_placement.Empty() == false) {
    MLPD_ERROR("PacketTXRX [%zu]: rx_buffer full\n", thread_info.tid_);
    cfg_->Running(false);
    return (nullptr);
  }
  Packet* pkt = rx_placement.RawPacket();

  ssize_t rx_bytes = thread_info.udp_servers_.at(radio_id)->Recv(
      reinterpret_cast<uint8_t*>(pkt), packet_length);
  if (0 > rx_bytes) {
    MLPD_ERROR("RecvEnqueue: Udp Recv failed with error\n");
    throw std::runtime_error("PacketTXRX: recv failed");
  } else if (rx_bytes == 0) {
    pkt = nullptr;
  } else if (static_cast<size_t>(rx_bytes) == packet_length) {
    if (kDebugPrintInTask) {
      std::printf("In TXRX thread %zu: Received frame %d, symbol %d, ant %d\n",
                  thread_info.tid_, pkt->frame_id_, pkt->symbol_id_,
                  pkt->ant_id_);
    }
    if (kDebugMulticell) {
      std::printf(
          "Before packet combining: receiving data stream from the "
          "antenna %d in cell %d,\n",
          pkt->ant_id_, pkt->cell_id_);
    }
    pkt->ant_id_ += pkt->cell_id_ * ant_per_cell_;
    if (kDebugMulticell) {
      std::printf(
          "After packet combining: the combined antenna ID is %d, it "
          "comes from the cell %d\n",
          pkt->ant_id_, pkt->cell_id_);
    }

    // Push kPacketRX event into the queue.
    rx_placement.Use();
    EventData rx_message(EventType::kPacketRX, rx_tag_t(rx_placement).tag_);
    if (event_notify_q_->enqueue(thread_info.notify_producer_token_,
                                 rx_message) == false) {
      MLPD_ERROR("socket message enqueue failed\n");
      throw std::runtime_error("PacketTXRX: socket message enqueue failed");
    }
  } else {
    MLPD_ERROR("RecvEnqueue: Udp Recv failed to receive all expected bytes");
    throw std::runtime_error(
        "PacketTXRX::RecvEnqueue: Udp Recv failed to receive all expected "
        "bytes");
  }
  return pkt;
}

//Function of the TxRx thread
int PacketTXRX::DequeueSend(TxRxThreadStorage& thread_info) {
  auto& c = cfg_;
  EventData event;
  if (tx_pending_q_->try_dequeue_from_producer(thread_info.tx_producer_token_,
                                               event) == false) {
    return -1;
  }

  // std::printf("tx queue length: %d\n", tx_pending_q_->size_approx());
  assert(event.event_type_ == EventType::kPacketTX);

  const size_t ant_id = gen_tag_t(event.tags_[0u]).ant_id_;
  const size_t frame_id = gen_tag_t(event.tags_[0u]).frame_id_;
  const size_t symbol_id = gen_tag_t(event.tags_[0u]).symbol_id_;

  const size_t data_symbol_idx_dl = cfg_->Frame().GetDLSymbolIdx(symbol_id);
  const size_t offset =
      (c->GetTotalDataSymbolIdxDl(frame_id, data_symbol_idx_dl) *
       c->BsAntNum()) +
      ant_id;

  if (kDebugPrintInTask) {
    std::printf(
        "PacketTXRX:DequeueSend [%zu]: Transmitted frame %zu, symbol %zu, ant "
        "%zu, tag %zu, offset: %zu, msg_queue_length: %zu\n",
        thread_info.tid_, frame_id, symbol_id, ant_id,
        gen_tag_t(event.tags_[0]).tag_, offset, event_notify_q_->size_approx());
  }

  char* cur_buffer_ptr = tx_buffer_ + offset * c->DlPacketLength();
  auto* pkt = reinterpret_cast<Packet*>(cur_buffer_ptr);
  new (pkt) Packet(frame_id, symbol_id, 0 /* cell_id */, ant_id);

  // Send data (one OFDM symbol)
  thread_info.udp_clients_.at(ant_id)->Send(
      cfg_->BsRruAddr(), cfg_->BsRruPort() + ant_id,
      reinterpret_cast<uint8_t*>(cur_buffer_ptr), c->DlPacketLength());

  //Send tx completion event
  RtAssert(
      event_notify_q_->enqueue(thread_info.notify_producer_token_,
                               EventData(EventType::kPacketTX, event.tags_[0])),
      "Socket message enqueue failed\n");
  return event.tags_[0];
}