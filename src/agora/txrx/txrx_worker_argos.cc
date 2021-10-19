/**
 * @file txrx_worker.cc
 * @brief Implementation of PacketTXRX datapath functions for communicating
 * with real Argos hardware
 */

#include "txrx_worker_argos.h"

#include <cassert>

#include "logger.h"

static constexpr bool kDebugDownlink = false;

TxRxWorkerArgos::TxRxWorkerArgos(
    size_t core_offset, size_t tid, size_t radio_hi, size_t radio_lo,
    Config* const config, size_t* rx_frame_start,
    moodycamel::ConcurrentQueue<EventData>* event_notify_q,
    moodycamel::ConcurrentQueue<EventData>* tx_pending_q,
    moodycamel::ProducerToken& tx_producer,
    moodycamel::ProducerToken& notify_producer,
    std::vector<RxPacket>& rx_memory, std::byte* const tx_memory)
    : TxRxWorker(core_offset, tid, radio_hi, radio_lo, config, rx_frame_start,
                 event_notify_q, tx_pending_q, tx_producer, notify_producer,
                 rx_memory, tx_memory) {
  radioconfig_ = std::make_unique<RadioConfig>(Configuration());
}

void TxRxWorkerArgos::Start() {
  if (radioconfig_->RadioStart() == false) {
    std::fprintf(stderr, "Failed to start radio\n");
    return;
  }

  //RadioStart creates the following
  //radioconfig_->GetCalibDl();
  //radioconfig_->GetCalibUl();

  if (Configuration()->Frame().NumDLSyms() > 0) {
    //std::memcpy(
    //    calib_dl_buffer[kFrameWnd - 1], radioconfig_->GetCalibDl(),
    //    Configuration()->OfdmDataNum() * Configuration()->BfAntNum() * sizeof(arma::cx_float));
    //std::memcpy(
    //    calib_ul_buffer[kFrameWnd - 1], radioconfig_->GetCalibUl(),
    //   Configuration()->OfdmDataNum() * Configuration()->BfAntNum() * sizeof(arma::cx_float));
  }
  MLPD_INFO("TxRxWorkerArgos[%zu] starting\n", tid_);
  thread_ = std::thread(&TxRxWorkerArgos::DoTxRx, this);

  radioconfig_->Go();
}

void TxRxWorkerArgos::Stop() {
  MLPD_INFO("TxRxWorkerArgos[%zu] stopping\n", tid_);
  radioconfig_->RadioStop();
  Configuration()->Running(false);
  if (thread_.joinable()) {
    thread_.join();
  }
}

//Main Thread Execution loop
void TxRxWorkerArgos::DoTxRx() {
  PinToCoreWithOffset(ThreadType::kWorkerTXRX, core_offset_, tid_);

  MLPD_INFO("TxRxWorkerArgos[%zu] has %zu:%zu total radios %zu\n", tid_,
            interface_offset_, (interface_offset_ + num_interfaces_) - 1,
            num_interfaces_);

  size_t rx_slot = 0;
  ssize_t prev_frame_id = -1;
  size_t local_interface = 0;
  while (Configuration()->Running() == true) {
    if (-1 != DequeueSend()) {
      continue;
    }
    // receive data
    auto pkt = RecvEnqueue(local_interface, rx_slot);
    if (pkt.size() == 0) {
      continue;
    }
    rx_slot = (rx_slot + pkt.size()) % rx_memory_.size();

    if (kIsWorkerTimingEnabled) {
      int frame_id = pkt.front()->frame_id_;
      if (frame_id > prev_frame_id) {
        rx_frame_start_[frame_id % kNumStatsFrames] = GetTime::Rdtsc();
        prev_frame_id = frame_id;
      }
    }
    if (++local_interface == num_interfaces_) {
      local_interface = 0;
    }
  }
}

//RX data
std::vector<Packet*> TxRxWorkerArgos::RecvEnqueue(size_t interface_id,
                                                  size_t rx_slot) {
  const size_t channels_per_interface = Configuration()->NumChannels();
  const size_t global_interface_id = interface_id + interface_offset_;

  const size_t packet_length = Configuration()->DlPacketLength();

  size_t ant_id = global_interface_id * channels_per_interface;
  std::vector<size_t> ant_ids(channels_per_interface);
  std::vector<void*> samp(channels_per_interface);

  //Check for availble memory
  for (size_t ch = 0; ch < channels_per_interface; ++ch) {
    RxPacket& rx = rx_memory_.at(rx_slot + ch);

    // if rx_buffer is full, exit
    if (rx.Empty() == false) {
      MLPD_ERROR("TxRxWorkerArgos[%zu] rx_buffer full, rx slot: %zu\n", tid_,
                 rx_slot);
      Configuration()->Running(false);
      break;
    }
    ant_ids.at(ch) = ant_id + ch;
    samp.at(ch) = rx.RawPacket()->data_;
  }

  long long frame_time;
  if ((Configuration()->Running() == false) ||
      radioconfig_->RadioRx(global_interface_id, samp.data(), frame_time) <=
          0) {
    std::vector<Packet*> empty_pkt;
    return empty_pkt;
  }

  size_t frame_id = (size_t)(frame_time >> 32);
  size_t symbol_id = (size_t)((frame_time >> 16) & 0xFFFF);
  std::vector<size_t> symbol_ids(channels_per_interface, symbol_id);

  /// \TODO: What if ref_ant is set to the second channel?
  if ((Configuration()->Frame().IsRecCalEnabled() == true) &&
      (Configuration()->IsCalDlPilot(frame_id, symbol_id) == true) &&
      (global_interface_id == Configuration()->RefRadio()) &&
      (Configuration()->AntPerGroup() > 1)) {
    if (Configuration()->AntPerGroup() > channels_per_interface) {
      symbol_ids.resize(Configuration()->AntPerGroup());
      ant_ids.resize(Configuration()->AntPerGroup());
    }
    for (size_t s = 1; s < Configuration()->AntPerGroup(); s++) {
      RxPacket& rx = rx_memory_.at(rx_slot + s);

      std::vector<void*> tmp_samp(channels_per_interface);
      std::vector<char> dummy_buff(packet_length);
      tmp_samp.at(0) = rx.RawPacket()->data_;
      tmp_samp.at(1) = dummy_buff.data();
      if ((Configuration()->Running() == false) ||
          radioconfig_->RadioRx(global_interface_id, tmp_samp.data(),
                                frame_time) <= 0) {
        std::vector<Packet*> empty_pkt;
        return empty_pkt;
      }
      symbol_ids.at(s) = Configuration()->Frame().GetDLCalSymbol(s);
      ant_ids.at(s) = ant_id;
    }
  }

  std::vector<Packet*> pkt;
  for (size_t ch = 0; ch < symbol_ids.size(); ++ch) {
    RxPacket& rx = rx_memory_.at(rx_slot + ch);
    pkt.push_back(rx.RawPacket());
    new (rx.RawPacket())
        Packet(frame_id, symbol_ids.at(ch), 0 /* cell_id */, ant_ids.at(ch));

    rx.Use();
    // Push kPacketRX event into the queue.
    EventData rx_message(EventType::kPacketRX, rx_tag_t(rx).tag_);

    if (event_notify_q_->enqueue(notify_producer_token_, rx_message) == false) {
      std::printf("TxRxWorkerArgos[%zu]: socket message enqueue failed\n",
                  tid_);
      throw std::runtime_error(
          "TxRxWorkerArgos: socket message enqueue failed");
    }
  }
  return pkt;
}

//Tx data
int TxRxWorkerArgos::DequeueSend() {
  const size_t channels_per_interface = Configuration()->NumChannels();
  std::array<EventData, kMaxChannels> event;
  if (tx_pending_q_->try_dequeue_bulk_from_producer(
          tx_producer_token_, event.data(), channels_per_interface) == 0) {
    return -1;
  }

  // std::printf("tx queue length: %d\n", task_queue_->size_approx());
  assert(event.at(0).event_type_ == EventType::kPacketTX);

  size_t frame_id = gen_tag_t(event.at(0).tags_[0u]).frame_id_;
  const size_t symbol_id = gen_tag_t(event.at(0).tags_[0u]).symbol_id_;
  const size_t ant_id = gen_tag_t(event.at(0).tags_[0u]).ant_id_;
  const size_t radio_id = ant_id / channels_per_interface;

  assert((radio_id >= interface_offset_) &&
         (radio_id <= (interface_offset_ + num_interfaces_)));

  const size_t dl_symbol_idx =
      Configuration()->Frame().GetDLSymbolIdx(symbol_id);
  const size_t offset =
      (Configuration()->GetTotalDataSymbolIdxDl(frame_id, dl_symbol_idx) *
       Configuration()->BsAntNum()) +
      ant_id;

  // Transmit downlink calibration (array to ref) pilot
  std::array<void*, kMaxChannels> caltxbuf;
  if ((Configuration()->Frame().IsRecCalEnabled() == true) &&
      (symbol_id == Configuration()->Frame().GetDLSymbol(0)) &&
      (radio_id != Configuration()->RefRadio())) {
    std::vector<std::complex<int16_t>> zeros(Configuration()->SampsPerSymbol(),
                                             std::complex<int16_t>(0, 0));
    for (size_t s = 0; s < Configuration()->RadioPerGroup(); s++) {
      bool calib_turn = (frame_id % Configuration()->RadioGroupNum() ==
                             radio_id / Configuration()->RadioPerGroup() &&
                         s == radio_id % Configuration()->RadioPerGroup());
      for (size_t ch = 0; ch < channels_per_interface; ch++) {
        caltxbuf.at(ch) =
            calib_turn ? Configuration()->PilotCi16().data() : zeros.data();
        if (channels_per_interface > 1) {
          caltxbuf.at(1 - ch) = zeros.data();
        }
        long long frame_time = ((long long)(frame_id + TX_FRAME_DELTA) << 32) |
                               (Configuration()->Frame().GetDLCalSymbol(
                                    s * channels_per_interface + ch)
                                << 16);
        radioconfig_->RadioTx(radio_id, caltxbuf.data(), 1, frame_time);
      }
    }
  }

  std::array<void*, kMaxChannels> txbuf;
  if (kDebugDownlink == true) {
    std::vector<std::complex<int16_t>> zeros(Configuration()->SampsPerSymbol());
    for (size_t ch = 0; ch < channels_per_interface; ch++) {
      if (ant_id != 0) {
        txbuf.at(ch) = zeros.data();
      } else if (dl_symbol_idx <
                 Configuration()->Frame().ClientDlPilotSymbols()) {
        txbuf.at(ch) =
            reinterpret_cast<void*>(Configuration()->UeSpecificPilotT()[0]);
      } else {
        txbuf.at(ch) =
            reinterpret_cast<void*>(Configuration()->DlIqT()[dl_symbol_idx]);
      }
      if (channels_per_interface > 1) {
        txbuf.at(1 - ch) = zeros.data();
      }
    }
  } else {
    for (size_t ch = 0; ch < channels_per_interface; ch++) {
      auto* pkt = reinterpret_cast<Packet*>(
          &tx_memory_[((offset + ch) * Configuration()->DlPacketLength())]);
      txbuf.at(ch) = reinterpret_cast<void*>(pkt->data_);
    }
  }

  const size_t last = Configuration()->Frame().GetDLSymbolLast();
  const int flags = (symbol_id != last) ? 1   // HAS_TIME
                                        : 2;  // HAS_TIME & END_BURST, fixme
  frame_id += TX_FRAME_DELTA;
  long long frame_time = ((long long)frame_id << 32) | (symbol_id << 16);
  radioconfig_->RadioTx(radio_id, txbuf.data(), flags, frame_time);

  if (kDebugPrintInTask == true) {
    std::printf(
        "TxRxWorkerArgos[%zu]: Transmitted frame %zu, symbol %zu, "
        "ant %zu, offset: %zu, msg_queue_length: %zu\n",
        tid_, frame_id, symbol_id, ant_id, offset,
        event_notify_q_->size_approx());
  }

  for (size_t i = 0; i < Configuration()->NumChannels(); i++) {
    RtAssert(event_notify_q_->enqueue(
                 notify_producer_token_,
                 EventData(EventType::kPacketTX, event.at(i).tags_[0])),
             "Socket message enqueue failed\n");
  }
  return event.at(0).tags_[0];
}