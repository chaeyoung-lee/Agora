/**
 * @file txrx.cc
 * @brief Implementation of PacketTXRX initialization functions, and datapath
 * functions for communicating with simulators.
 */

#include "txrx_sim.h"

#include "logger.h"

PacketTXRX::PacketTXRX(Config* cfg, size_t core_offset)
    : cfg_(cfg),
      core_offset_(core_offset),
      num_interfaces_(cfg->NumRadios()),
      num_worker_threads_(cfg->SocketThreadNum()) {}

PacketTXRX::PacketTXRX(Config* cfg, size_t core_offset,
                       moodycamel::ConcurrentQueue<EventData>* event_notify_q,
                       moodycamel::ConcurrentQueue<EventData>* tx_pending_q,
                       moodycamel::ProducerToken** notify_producer_tokens,
                       moodycamel::ProducerToken** tx_producer_tokens)
    : PacketTXRX(cfg, core_offset) {
  event_notify_q_ = event_notify_q;
  tx_pending_q_ = tx_pending_q;
  notify_producer_tokens_ = notify_producer_tokens;
  tx_producer_tokens_ = tx_producer_tokens;
}

PacketTXRX::~PacketTXRX() {
  for (auto& worker_threads : worker_threads_) {
    worker_threads.Stop();
  }
}

bool PacketTXRX::StartTxRx(Table<char>& rx_buffer, size_t packet_num_in_buffer,
                           Table<size_t>& frame_start, char* tx_buffer,
                           Table<complex_float>& calib_dl_buffer,
                           Table<complex_float>& calib_ul_buffer) {
  MLPD_INFO("PacketTXRX: txrx threads %zu, packet buffers %zu\n",
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

    MLPD_SYMBOL("PacketTXRX: Starting thread %zu\n", i);
    const size_t radio_lo = (i * num_interfaces_) / num_worker_threads_;
    const size_t radio_hi = ((i + 1) * num_interfaces_) / num_worker_threads_;
    worker_threads_.emplace_back(core_offset_, i, radio_hi, radio_lo, cfg_,
                                 frame_start[i], event_notify_q_, tx_pending_q_,
                                 *tx_producer_tokens_[i],
                                 *notify_producer_tokens_[i], rx_packets_.at(i),
                                 reinterpret_cast<std::byte* const>(tx_buffer));
  }

  for (auto& worker : worker_threads_) {
    worker.Start();
  }
  return true;
}