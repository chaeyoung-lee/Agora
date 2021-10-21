/**
 * @file packet_txrx_dpdk.h
 * @brief Implementation of PacketTxRxDpdk datapath functions for communicating
 * with DPDK
 */

#ifndef PACKETTXRX_DPDK_H_
#define PACKETTXRX_DPDK_H_

#if !defined(USE_DPDK)
static_assert(false, "Packet tx rx dpdk defined but DPDK is not enabled");
#endif

#include "dpdk_transport.h"

//Removed support for copy free dpdk memory due to slowdown issue.
//#define USE_DPDK_MEMORY

#if defined(USE_DPDK_MEMORY)
class DPDKRxPacket : public RxPacket {
 public:
  DPDKRxPacket() : RxPacket() { mem_ = nullptr; }
  explicit DPDKRxPacket(const DPDKRxPacket& copy) : RxPacket(copy) {
    mem_ = copy.mem_;
  }
  ~DPDKRxPacket() = default;
  inline bool Set(rte_mbuf* mem, Packet* in_pkt) {
    mem_ = mem;
    return RxPacket::Set(in_pkt);
  }

 private:
  rte_mbuf* mem_;
  inline void GcPacket() override {
    //std::printf("Garbage collecting the memory for DPDKRxPacket\n");
    rte_pktmbuf_free(mem_);
    this->Set(nullptr, nullptr);
  }
};
#endif  // defined(USE_DPDK_MEMORY)

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
class PacketTxRxDpdk {
 public:
  static const int kMaxSocketNum = 10;  // Max number of socket threads allowed

  explicit PacketTxRxDpdk(Config* cfg, size_t in_core_offset = 1);

  PacketTxRxDpdk(Config* cfg, size_t core_offset,
                 moodycamel::ConcurrentQueue<EventData>* queue_message,
                 moodycamel::ConcurrentQueue<EventData>* queue_task,
                 moodycamel::ProducerToken** rx_ptoks,
                 moodycamel::ProducerToken** tx_ptoks);
  ~PacketTxRxDpdk();

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
  void LoopTxRx(size_t tid);  // The thread function for thread [tid]

  // At thread [tid], receive packets from the NIC and enqueue them to the
  // master thread
  uint16_t DpdkRecv(int tid, uint16_t port_id, uint16_t queue_id,
                    size_t& prev_frame_id, size_t& rx_slot);
  int DequeueSend(int tid);

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

  //const size_t ant_per_cell_;
  uint32_t bs_rru_addr_;     // IPv4 address of the simulator sender
  uint32_t bs_server_addr_;  // IPv4 address of the Agora server
  struct rte_mempool* mbuf_pool_;

  // Dimension 1: socket_thread
  // Dimension 2: rx_packet
#if defined(USE_DPDK_MEMORY)
  std::vector<std::vector<DPDKRxPacket>> rx_packets_;
#else
  std::vector<std::vector<RxPacket>> rx_packets_;
#endif  // defined(USE_DPDK_MEMORY)
};

#endif  // PACKETTXRX_DPDK_H_