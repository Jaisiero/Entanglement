#pragma once

#include "udp_socket.h"
#include <atomic>
#include <functional>
#include <string>

namespace entanglement {

// Callback invoked when a valid packet is received
using on_packet_received = std::function<void(
    const packet_header& header,
    const uint8_t* payload,
    size_t payload_size,
    const std::string& sender_address,
    uint16_t sender_port
)>;

class server {
public:
    explicit server(uint16_t port, const std::string& bind_address = "0.0.0.0");
    ~server();

    // Non-copyable, non-movable
    server(const server&) = delete;
    server& operator=(const server&) = delete;

    bool start();
    void stop();
    bool is_running() const { return m_running.load(); }

    // Process pending packets (call from your game loop or a dedicated thread)
    // Returns the number of packets processed (up to max_packets per call)
    int poll(int max_packets = DEFAULT_MAX_POLL_PACKETS);

    // Set the packet received callback
    void set_on_packet_received(on_packet_received callback);

    // Send a response back to a client
    int send_to(const packet_header& header, const void* payload,
                const std::string& address, uint16_t port);

    uint16_t port() const { return m_port; }

private:
    udp_socket m_socket;
    uint16_t m_port;
    std::string m_bind_address;
    std::atomic<bool> m_running{false};
    on_packet_received m_on_packet_received;
};

} // namespace entanglement
