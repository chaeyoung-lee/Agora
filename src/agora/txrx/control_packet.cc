#include <vector>

#include "buffer.h"
#include "concurrentqueue.h"
#include "config.h"
#include "txrx_worker.h"

ControlPacket::ControlPacket() {
    control_server_ = std::make_unique<UDPServer>(local_port_id, kSocketRxBufferSize);
    control_client_ = std::make_unique<UDPClient>();

    // receive packets
    ssize rbytes = control_server_->Recv(reinterpret_cast<uint8_t*>(pkt), packet_length);

    // send packets
    control_client_->Send(Configuration()->BsRruAddr(),
    Configuration()->BsRruPort() + global_interface_id,
    beacon_buffer_.data(), beacon_buffer_.size());
}