/**
 * @file txrx_sim.h
 * @brief Common definations for PacketTXRX. Including datapath
 * functions for communicating with simulators.
 */

#ifndef PACKETTXRX_H_
#define PACKETTXRX_H_

#include <memory>
#include <vector>

#include "buffer.h"
#include "concurrentqueue.h"
#include "config.h"
#include "udp_client.h"
#include "udp_server.h"
//For TxRxThreadStorage (remove when class is moved)
#include "logger.h"

class TxRxThreadStorage {
 public:
  TxRxThreadStorage() = delete;
  TxRxThreadStorage(size_t tid, size_t radio_hi, size_t radio_lo,
                    const Config* const config, size_t* rx_frame_start,
                    moodycamel::ProducerToken& tx_producer,
                    moodycamel::ProducerToken& notify_producer)
      : tid_(tid),
        num_interfaces_(radio_hi - radio_lo),
        interface_offset_(radio_lo),
        rx_frame_start_(rx_frame_start),
        tx_producer_token_(tx_producer),
        notify_producer_token_(notify_producer) {
    static constexpr size_t kSocketRxBufferSize = (1024 * 1024 * 64 * 8) - 1;
    for (size_t radio_id = radio_lo; radio_id < radio_hi; ++radio_id) {
      const uint16_t local_port_id = config->BsServerPort() + radio_id;

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

  const size_t tid_;
  const size_t num_interfaces_;
  const size_t interface_offset_;
  size_t* const rx_frame_start_;

  //foreign producer of tx messages (used for TX only)
  moodycamel::ProducerToken& tx_producer_token_;
  //local producer of notification messages (used for TX and RX)
  moodycamel::ProducerToken& notify_producer_token_;

  //1 for each responsible interface (ie radio)
  //socket for incomming messages (received data)
  std::vector<std::unique_ptr<UDPServer>> udp_servers_;

  //socket for outgoing messages (data to transmit)
  std::vector<std::unique_ptr<UDPClient>> udp_clients_;
  std::vector<uint8_t> beacon_buffer_;
  double beacon_send_time_;
};

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
  bool StartTxRx(Table<char>& buffer, size_t packet_num_in_buffer,
                 Table<size_t>& frame_start, char* tx_buffer,
                 Table<complex_float>& calib_dl_buffer_,
                 Table<complex_float>& calib_ul_buffer_);

 private:
  explicit PacketTXRX(Config* cfg, size_t in_core_offset = 1);

  void LoopTxRx(size_t tid);  // The thread function for thread [tid]
  int DequeueSend(TxRxThreadStorage& thread_info);
  void SendBeacon(TxRxThreadStorage& thread_info, size_t frame_id);
  Packet* RecvEnqueue(TxRxThreadStorage& thread_info, RxPacket& rx_placement,
                      size_t radio_id);

  //long long rx_time_bs_;
  //long long tx_time_bs_;

  Config* const cfg_;

  // The network I/O threads run on cores
  // {core_offset, ..., core_offset + socket_thread_num - 1}
  const size_t core_offset_;
  const size_t ant_per_cell_;
  const size_t num_socket_thread_;

  // Handle for socket threads
  std::vector<std::thread> socket_std_threads_;
  size_t buffers_per_socket_;

  char* tx_buffer_;
  Table<size_t>* frame_start_;
  moodycamel::ConcurrentQueue<EventData>* event_notify_q_;
  moodycamel::ConcurrentQueue<EventData>* tx_pending_q_;

  //Producer tokens for posting messages to event_notify_q_
  moodycamel::ProducerToken** notify_producer_tokens_;
  //Producers assigned to tx_pending_q (used for retrieving messages)
  moodycamel::ProducerToken** tx_producer_tokens_;

  // Dimension 1: socket_thread
  // Dimension 2: rx_packet
  std::vector<std::vector<RxPacket>> rx_packets_;
  const size_t num_interfaces_;
};

#endif  // PACKETTXRX_H_
