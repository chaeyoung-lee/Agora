/**
 * @file txrx_worker.cc
 * @brief Implementation of PacketTXRX initialization functions, and datapath
 * functions for communicating with simulators.
 */

#include "txrx_worker.h"

#include <cassert>

#include "logger.h"

static constexpr bool kEnableSlowStart = true;
static constexpr bool kEnableSlowSending = false;
static constexpr bool kDebugPrintBeacon = false;

static constexpr size_t kSlowStartMulStage1 = 32;
static constexpr size_t kSlowStartMulStage2 = 8;

TxRxWorker::TxRxWorker(size_t core_offset, size_t tid, size_t radio_hi,
                       size_t radio_lo, Config* const config,
                       size_t* rx_frame_start,
                       moodycamel::ConcurrentQueue<EventData>* event_notify_q,
                       moodycamel::ConcurrentQueue<EventData>* tx_pending_q,
                       moodycamel::ProducerToken& tx_producer,
                       moodycamel::ProducerToken& notify_producer,
                       std::vector<RxPacket>& rx_memory,
                       std::byte* const tx_memory)
    : cfg_(config),
      tid_(tid),
      core_offset_(core_offset),
      num_interfaces_(radio_hi - radio_lo),
      interface_offset_(radio_lo),
      ant_per_cell_(config->BsAntNum() / config->NumCells()),
      rx_frame_start_(rx_frame_start),
      rx_memory_(rx_memory),
      tx_memory_(tx_memory),
      event_notify_q_(event_notify_q),
      tx_pending_q_(tx_pending_q),
      tx_producer_token_(tx_producer),
      notify_producer_token_(notify_producer) {
  static constexpr size_t kSocketRxBufferSize = (1024 * 1024 * 64 * 8) - 1;
  for (size_t interface = 0; interface < num_interfaces_; ++interface) {
    const uint16_t local_port_id =
        config->BsServerPort() + interface + interface_offset_;

    udp_servers_.emplace_back(
        std::make_unique<UDPServer>(local_port_id, kSocketRxBufferSize));
    udp_clients_.emplace_back(std::make_unique<UDPClient>());
    MLPD_FRAME(
        "TXRX thread [%zu]: set up UDP socket server listening to local port "
        "%d\n",
        tid_, local_port_id);
  }
  beacon_buffer_.resize(config->PacketLength());
}

void TxRxWorker::Start() {
  MLPD_FRAME("TxRxWorker[%zu] starting\n", tid_);
  thread_ = std::thread(&TxRxWorker::DoTxRx, this);
}

void TxRxWorker::Stop() {
  MLPD_FRAME("TxRxWorker[%zu] stoping\n", tid_);
  cfg_->Running(false);
  if (thread_.joinable()) {
    thread_.join();
  }
}

//Main Thread Execution loop
void TxRxWorker::DoTxRx() {
  PinToCoreWithOffset(ThreadType::kWorkerTXRX, core_offset_, tid_);

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
      SendBeacon(tx_frame_id++);

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

    int send_result = DequeueSend();
    if (-1 == send_result) {
      // receive data
      // Need to get NumChannels data here
      Packet* pkt = RecvEnqueue(rx_memory_.at(rx_slot), current_interface);
      if (pkt != nullptr) {
        rx_slot = (rx_slot + 1) % rx_memory_.size();

        if (kIsWorkerTimingEnabled) {
          uint32_t frame_id = pkt->frame_id_;
          if (frame_id != prev_frame_id) {
            rx_frame_start_[frame_id % kNumStatsFrames] = GetTime::Rdtsc();
            prev_frame_id = frame_id;
          }
        }

        if (++current_interface == num_interfaces_) {
          current_interface = 0;
        }
      }
    }  // end if -1 == send_result
  }    // end while
}

void TxRxWorker::SendBeacon(size_t frame_id) {
  const double time_now = GetTime::GetTimeUs() / 1000.0f;

  // Send a beacon packet in the downlink to trigger user pilot
  auto* pkt = reinterpret_cast<Packet*>(beacon_buffer_.data());

  if (kDebugPrintBeacon) {
    std::printf("TXRX [%zu]: Sending beacon for frame %zu tx delta %f ms\n",
                tid_, frame_id, time_now - beacon_send_time_);
  }
  beacon_send_time_ = time_now;

  for (size_t beacon_sym = 0u; beacon_sym < cfg_->Frame().NumBeaconSyms();
       beacon_sym++) {
    for (size_t interface = 0u; interface < num_interfaces_; interface++) {
      const size_t global_interface_id = interface + interface_offset_;
      for (size_t channel = 0u; channel < cfg_->NumChannels(); channel++) {
        const size_t ant_id =
            ((global_interface_id * cfg_->NumChannels()) + channel);
        new (pkt) Packet(frame_id, cfg_->Frame().GetBeaconSymbol(beacon_sym),
                         0 /* cell_id */, ant_id);

        udp_clients_.at(interface)->Send(
            cfg_->BsRruAddr(), cfg_->BsRruPort() + global_interface_id,
            beacon_buffer_.data(), beacon_buffer_.size());
      }
    }
  }
}

Packet* TxRxWorker::RecvEnqueue(RxPacket& rx_placement, size_t interface_id) {
  const size_t packet_length = cfg_->PacketLength();

  // if rx_buffer is full, exit
  if (rx_placement.Empty() == false) {
    MLPD_ERROR("PacketTXRX [%zu]: rx_buffer full\n", tid_);
    cfg_->Running(false);
    return (nullptr);
  }
  Packet* pkt = rx_placement.RawPacket();

  ssize_t rx_bytes = udp_servers_.at(interface_id)
                         ->Recv(reinterpret_cast<uint8_t*>(pkt), packet_length);
  if (0 > rx_bytes) {
    MLPD_ERROR("RecvEnqueue: Udp Recv failed with error\n");
    throw std::runtime_error("PacketTXRX: recv failed");
  } else if (rx_bytes == 0) {
    pkt = nullptr;
  } else if (static_cast<size_t>(rx_bytes) == packet_length) {
    if (kDebugPrintInTask) {
      std::printf("In TXRX thread %zu: Received frame %d, symbol %d, ant %d\n",
                  tid_, pkt->frame_id_, pkt->symbol_id_, pkt->ant_id_);
    }
    if (kDebugMulticell) {
      std::printf(
          "Before packet combining: receiving data stream from the antenna "
          "%d in cell %d,\n",
          pkt->ant_id_, pkt->cell_id_);
    }
    pkt->ant_id_ += pkt->cell_id_ * ant_per_cell_;
    if (kDebugMulticell) {
      std::printf(
          "After packet combining: the combined antenna ID is %d, it comes "
          "from the cell %d\n",
          pkt->ant_id_, pkt->cell_id_);
    }

    // Push kPacketRX event into the queue.
    rx_placement.Use();
    EventData rx_message(EventType::kPacketRX, rx_tag_t(rx_placement).tag_);
    if (event_notify_q_->enqueue(notify_producer_token_, rx_message) == false) {
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
int TxRxWorker::DequeueSend() {
  auto& c = cfg_;
  EventData event;
  if (tx_pending_q_->try_dequeue_from_producer(tx_producer_token_, event) ==
      false) {
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
        "PacketTXRX:DequeueSend [%zu]: Transmitted frame %zu, symbol %zu, "
        "ant %zu, tag %zu, offset: %zu, msg_queue_length: %zu\n",
        tid_, frame_id, symbol_id, ant_id, gen_tag_t(event.tags_[0]).tag_,
        offset, event_notify_q_->size_approx());
  }

  auto* pkt =
      reinterpret_cast<Packet*>(&tx_memory_[offset * c->DlPacketLength()]);
  new (pkt) Packet(frame_id, symbol_id, 0 /* cell_id */, ant_id);

  assert((ant_id >= interface_offset_) &&
             (ant_id <= (num_interfaces_ + interface_offset_)),
         "Antenna ID is not within range\n");

  // Send data (one OFDM symbol)
  udp_clients_.at(ant_id - interface_offset_)
      ->Send(cfg_->BsRruAddr(), cfg_->BsRruPort() + ant_id,
             reinterpret_cast<uint8_t*>(pkt), c->DlPacketLength());

  //Send tx completion event
  RtAssert(
      event_notify_q_->enqueue(notify_producer_token_,
                               EventData(EventType::kPacketTX, event.tags_[0])),
      "Socket message enqueue failed\n");
  return event.tags_[0];
}