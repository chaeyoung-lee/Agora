/**
 * @file packet_txrx.cc
 * @brief Implementation of PacketTxRx initialization functions, and datapath
 * functions for communicating with simulators.
 */

#include "packet_txrx.h"

#include "logger.h"
#include "txrx_worker_argos.h"
#include "txrx_worker_sim.h"
#include "txrx_worker_usrp.h"

PacketTxRx::PacketTxRx(Config* cfg, size_t core_offset)
    : cfg_(cfg),
      core_offset_(core_offset),
      num_interfaces_(cfg->NumRadios()),
      num_worker_threads_(cfg->SocketThreadNum()) {}

PacketTxRx::PacketTxRx(Config* cfg, size_t core_offset,
                       moodycamel::ConcurrentQueue<EventData>* event_notify_q,
                       moodycamel::ConcurrentQueue<EventData>* tx_pending_q,
                       moodycamel::ProducerToken** notify_producer_tokens,
                       moodycamel::ProducerToken** tx_producer_tokens)
    : PacketTxRx(cfg, core_offset) {
  event_notify_q_ = event_notify_q;
  tx_pending_q_ = tx_pending_q;
  notify_producer_tokens_ = notify_producer_tokens;
  tx_producer_tokens_ = tx_producer_tokens;
}

PacketTxRx::~PacketTxRx() {
  for (auto& worker_threads : worker_threads_) {
    worker_threads->Stop();
  }
}

bool PacketTxRx::StartTxRx(Table<char>& rx_buffer, size_t packet_num_in_buffer,
                           Table<size_t>& frame_start, char* tx_buffer,
                           Table<complex_float>& calib_dl_buffer,
                           Table<complex_float>& calib_ul_buffer) {
  MLPD_INFO("PacketTxRx: txrx threads %zu, packet buffers %zu\n",
            num_worker_threads_, packet_num_in_buffer);

  const size_t buffers_per_thread = packet_num_in_buffer / num_worker_threads_;
  /// Make sure we can fit each channel in the tread buffer without rollover
  assert(buffers_per_thread % cfg_->NumChannels() == 0);

  rx_packets_.resize(num_worker_threads_);
  for (size_t i = 0; i < num_worker_threads_; i++) {
    rx_packets_.at(i).reserve(buffers_per_thread);
    for (size_t number_packets = 0; number_packets < buffers_per_thread;
         number_packets++) {
      auto* pkt_loc = reinterpret_cast<Packet*>(
          rx_buffer[i] + (number_packets * cfg_->PacketLength()));
      rx_packets_.at(i).emplace_back(pkt_loc);
    }

    MLPD_SYMBOL("PacketTxRx: Starting thread %zu\n", i);
    const size_t radio_lo = (i * num_interfaces_) / num_worker_threads_;
    const size_t radio_hi = ((i + 1) * num_interfaces_) / num_worker_threads_;

    //This is the spot to choose what type of TxRxWorker you want....
    if (kUseArgos) {
      worker_threads_.emplace_back(std::make_unique<TxRxWorkerArgos>(
          core_offset_, i, radio_hi, radio_lo, cfg_, frame_start[i],
          event_notify_q_, tx_pending_q_, *tx_producer_tokens_[i],
          *notify_producer_tokens_[i], rx_packets_.at(i),
          reinterpret_cast<std::byte* const>(tx_buffer)));
    } else if (kUseUHD) {
      worker_threads_.emplace_back(std::make_unique<TxRxWorkerUsrp>(
          core_offset_, i, radio_hi, radio_lo, cfg_, frame_start[i],
          event_notify_q_, tx_pending_q_, *tx_producer_tokens_[i],
          *notify_producer_tokens_[i], rx_packets_.at(i),
          reinterpret_cast<std::byte* const>(tx_buffer)));
    } else {
      worker_threads_.emplace_back(std::make_unique<TxRxWorkerSim>(
          core_offset_, i, radio_hi, radio_lo, cfg_, frame_start[i],
          event_notify_q_, tx_pending_q_, *tx_producer_tokens_[i],
          *notify_producer_tokens_[i], rx_packets_.at(i),
          reinterpret_cast<std::byte* const>(tx_buffer)));
    }
  }

  for (auto& worker : worker_threads_) {
    worker->Start();
  }
  return true;
}