#pragma once

#include "udp_socket.h"
#include <string>
#include <atomic>
#include <functional>

namespace entanglement {

// Callback invoked when a response packet is received from the server
using on_response_received = std::function<void(
    const packet_header& header,
    const uint8_t* payload,
    size_t payload_size
)>;

class client {
public:
    client(const std::string& server_address, uint16_t server_port);
    ~client();

    // Non-copyable, non-movable
    client(const client&) = delete;
    client& operator=(const client&) = delete;

    // Initialize the client socket (binds to ephemeral port)
    bool connect();
    void disconnect();
    bool is_connected() const { return m_connected.load(); }

    // Send a packet to the server
    int send(const packet_header& header, const void* payload = nullptr);

    // Send raw payload with auto-filled header fields
    int send_payload(const void* data, size_t size, uint8_t flags = 0,
                     uint8_t channel_id = 0);

    // Process incoming packets from the server
    int poll();

    // Set the response callback
    void set_on_response(on_response_received callback);

    uint64_t next_sequence() { return m_sequence++; }

private:
    udp_socket m_socket;
    std::string m_server_address;
    uint16_t m_server_port;
    std::atomic<bool> m_connected{false};
    std::atomic<uint64_t> m_sequence{1};
    on_response_received m_on_response;
};

} // namespace entanglement
