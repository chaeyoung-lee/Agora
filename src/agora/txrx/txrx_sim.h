/**
 * @file txrx_sim.h
 * @brief Common definations for PacketTXRX. Including datapath
 * functions for communicating with simulators.
 */

#ifndef PACKETTXRX_H_
#define PACKETTXRX_H_

#include <vector>

#include "buffer.h"
#include "concurrentqueue.h"
#include "config.h"
#include "txrx_worker.h"

/**
 * @brief Implementations of this class provide packet I/O for Agora.
 *
 * In the vanilla mode, this class provides socket or DPDK-based packet I/O to
 * Agora (running on the base station server or client) for communicating
 * with simulated peers.
 *
 * In the "Argos" mode, this class provides SoapySDR-based communication for
 * Agora (running on the base station server or client) for communicating
 * with real wireless hardware peers (antenna hubs for the server, UE devices
 * for the client).
 */

class PacketTXRX {
 public:
  PacketTXRX(Config* cfg, size_t core_offset,
             moodycamel::ConcurrentQueue<EventData>* event_notify_q,
             moodycamel::ConcurrentQueue<EventData>* tx_pending_q,
             moodycamel::ProducerToken** notify_producer_tokens,
             moodycamel::ProducerToken** tx_producer_tokens);
  ~PacketTXRX();

  /**
   * @brief Start the network I/O threads
   *
   * @param buffer Ring buffer to save packets
   * @param packet_num_in_buffer Total number of buffers in an RX ring
   *
   * @return True on successfully starting the network I/O threads, false
   * otherwise
   */
  bool StartTxRx(Table<char>& rx_buffer, size_t packet_num_in_buffer,
                 Table<size_t>& frame_start, char* tx_buffer,
                 Table<complex_float>& calib_dl_buffer_,
                 Table<complex_float>& calib_ul_buffer_);

 private:
  explicit PacketTXRX(Config* cfg, size_t in_core_offset = 1);
  std::vector<TxRxWorker> worker_threads_;

  Config* const cfg_;

  // The network I/O threads run on cores
  // {core_offset, ..., core_offset + socket_thread_num - 1}
  const size_t core_offset_;
  const size_t num_interfaces_;
  const size_t num_worker_threads_;

  moodycamel::ConcurrentQueue<EventData>* event_notify_q_;
  moodycamel::ConcurrentQueue<EventData>* tx_pending_q_;

  //Producer tokens for posting messages to event_notify_q_
  moodycamel::ProducerToken** notify_producer_tokens_;
  //Producers assigned to tx_pending_q (used for retrieving messages)
  moodycamel::ProducerToken** tx_producer_tokens_;

  // Dimension 1: socket_thread
  // Dimension 2: rx_packet
  std::vector<std::vector<RxPacket>> rx_packets_;
};

#endif  // PACKETTXRX_H_
