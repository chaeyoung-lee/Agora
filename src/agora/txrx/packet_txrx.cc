/**
 * @file packet_txrx.cc
 * @brief Implementation of PacketTxRx initialization functions, and datapath
 * functions for communicating with simulators.
 */

#include "packet_txrx.h"

#include "logger.h"
#include "txrx_worker_sim.h"

PacketTxRx::PacketTxRx(Config* const cfg, size_t core_offset,
                       moodycamel::ConcurrentQueue<EventData>* event_notify_q,
                       moodycamel::ConcurrentQueue<EventData>* tx_pending_q,
                       moodycamel::ProducerToken** notify_producer_tokens,
                       moodycamel::ProducerToken** tx_producer_tokens,
                       Table<char>& rx_buffer, size_t packet_num_in_buffer,
                       Table<size_t>& frame_start, char* tx_buffer)
    : cfg_(cfg),
      core_offset_(core_offset),
      num_interfaces_(cfg->NumRadios()),
      num_worker_threads_(cfg->SocketThreadNum()),
      num_ant_per_worker_((cfg->NumAntennas() / num_worker_threads_) +
                          ((cfg->NumAntennas() % num_worker_threads_) != 0)) {
  event_notify_q_ = event_notify_q;
  tx_pending_q_ = tx_pending_q;
  notify_producer_tokens_ = notify_producer_tokens;
  tx_producer_tokens_ = tx_producer_tokens;

  const size_t interfaces_per_worker =
      num_ant_per_worker_ / cfg_->NumChannels();
  RtAssert((num_ant_per_worker_ % cfg_->NumChannels()) == 0,
           "Socket threads are misaligned with the number of channels\n");

  size_t min_threads = cfg->NumAntennas() / num_ant_per_worker_;
  std::printf("Number of worker threads %zu, min threads %zu\n",
              num_worker_threads_, min_threads);

  if (min_threads < num_worker_threads_) {
    MLPD_WARN(
        "Using less than requested number of socket worker threads %zu : %zu\n",
        min_threads, num_worker_threads_);
  } else {
    min_threads = num_worker_threads_;
  }

  const size_t buffers_per_thread = packet_num_in_buffer / min_threads;
  /// Make sure we can fit each channel in the tread buffer without rollover
  assert(buffers_per_thread % cfg_->NumChannels() == 0);

  rx_packets_.resize(min_threads);
  for (size_t i = 0; i < min_threads; i++) {
    const size_t interface_offset = (i * interfaces_per_worker);
    const size_t interface_count =
        std::min(interfaces_per_worker, num_interfaces_ - interface_offset);

    rx_packets_.at(i).reserve(buffers_per_thread);
    for (size_t number_packets = 0; number_packets < buffers_per_thread;
         number_packets++) {
      auto* pkt_loc = reinterpret_cast<Packet*>(
          rx_buffer[i] + (number_packets * cfg_->PacketLength()));
      rx_packets_.at(i).emplace_back(pkt_loc);
    }

    CreateWorker(i, interface_count, interface_offset, frame_start[i],
                 rx_packets_.at(i),
                 reinterpret_cast<std::byte* const>(tx_buffer));
  }
}

PacketTxRx::~PacketTxRx() {
  cfg_->Running(false);
  for (auto& worker_threads : worker_threads_) {
    worker_threads->Stop();
  }
}

bool PacketTxRx::StartTxRx(Table<complex_float>& calib_dl_buffer,
                           Table<complex_float>& calib_ul_buffer) {
  unused(calib_dl_buffer);
  unused(calib_ul_buffer);

  MLPD_INFO("PacketTxRx: StartTxRx threads %zu\n", worker_threads_.size());
  for (auto& worker : worker_threads_) {
    worker->Start();
  }
  return true;
}

size_t PacketTxRx::AntNumToWorkerId(size_t ant_num) const {
  return (ant_num / num_ant_per_worker_);
}

bool PacketTxRx::CreateWorker(size_t tid, size_t interface_count,
                              size_t interface_offset, size_t* rx_frame_start,
                              std::vector<RxPacket>& rx_memory,
                              std::byte* const tx_memory) {
  MLPD_INFO(
      "PacketTxRx[%zu]: Creating worker handling %zu interfaces starting at "
      "%zu - antennas %zu:%zu\n",
      tid, interface_count, interface_offset,
      interface_offset * cfg_->NumChannels(),
      ((interface_offset * cfg_->NumChannels()) +
       (interface_count * cfg_->NumChannels()) - 1));

  //This is the spot to choose what type of TxRxWorker you want....
  RtAssert((kUseArgos == false) && (kUseUHD == false),
           "This class does not support hardware implementations");
  worker_threads_.emplace_back(std::make_unique<TxRxWorkerSim>(
      core_offset_, tid, interface_count, interface_offset, cfg_,
      rx_frame_start, event_notify_q_, tx_pending_q_, *tx_producer_tokens_[tid],
      *notify_producer_tokens_[tid], rx_memory, tx_memory));
  return true;
}