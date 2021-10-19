/**
 * @file txrx_worker.h
 * @brief txrx worker thread defination.  This is the parent / interface
 */

#ifndef TXRX_WORKER_H_
#define TXRX_WORKER_H_

#include <thread>
#include <vector>

#include "concurrentqueue.h"
#include "config.h"
#include "udp_client.h"
#include "udp_server.h"

class TxRxWorker {
 public:
  TxRxWorker() = delete;
  TxRxWorker(size_t core_offset, size_t tid, size_t radio_hi, size_t radio_lo,
             Config* const config, size_t* rx_frame_start,
             moodycamel::ConcurrentQueue<EventData>* event_notify_q,
             moodycamel::ConcurrentQueue<EventData>* tx_pending_q,
             moodycamel::ProducerToken& tx_producer,
             moodycamel::ProducerToken& notify_producer,
             std::vector<RxPacket>& rx_memory, uint8_t* const tx_memory);

  void Start();
  void Stop();
  void DoTxRx();

 private:
  int DequeueSend();
  void SendBeacon(size_t frame_id);
  Packet* RecvEnqueue(RxPacket& rx_placement, size_t interface_id);

  Config* const cfg_;
  const size_t tid_;
  const size_t core_offset_;
  const size_t num_interfaces_;
  const size_t interface_offset_;
  const size_t ant_per_cell_;

  size_t* const rx_frame_start_;

  std::vector<RxPacket>& rx_memory_;
  uint8_t* const tx_memory_;

  moodycamel::ConcurrentQueue<EventData>* event_notify_q_;
  moodycamel::ConcurrentQueue<EventData>* tx_pending_q_;
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

  std::thread thread_;
};
#endif  // TXRX_WORKER_H_
