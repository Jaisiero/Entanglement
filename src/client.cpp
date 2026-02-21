#include "client.h"
#include <iostream>
#include <cstring>

namespace entanglement {

client::client(const std::string& server_address, uint16_t server_port)
    : m_server_address(server_address), m_server_port(server_port) {}

client::~client() {
    disconnect();
}

bool client::connect() {
    // Bind to any available local port
    if (!m_socket.bind(0)) {
        return false;
    }

    if (!m_socket.set_non_blocking(true)) {
        std::cerr << "[client] Failed to set non-blocking mode" << std::endl;
        m_socket.close();
        return false;
    }

    m_connected = true;
    std::cout << "[client] Ready on local port " << m_socket.local_port()
              << " -> " << m_server_address << ":" << m_server_port << std::endl;
    return true;
}

void client::disconnect() {
    m_connected = false;
    m_socket.close();
    std::cout << "[client] Disconnected" << std::endl;
}

int client::send(const packet_header& header, const void* payload) {
    return m_socket.send_packet(header, payload, m_server_address, m_server_port);
}

int client::send_payload(const void* data, size_t size, uint8_t flags,
                          uint8_t channel_id) {
    packet_header header{};
    header.magic = PROTOCOL_MAGIC;
    header.version = PROTOCOL_VERSION;
    header.flags = flags;
    header.shard_id = 0;
    header.channel_id = channel_id;
    header.reserved = 0;
    header.sequence = next_sequence();
    header.ack = 0;
    header.ack_bitmap = 0;
    header.payload_size = static_cast<uint16_t>(size);

    return send(header, data);
}

int client::poll() {
    if (!m_connected) return 0;

    int count = 0;
    packet_header header{};
    uint8_t payload[MAX_PAYLOAD_SIZE];
    std::string sender_addr;
    uint16_t sender_port = 0;

    while (true) {
        int result = m_socket.recv_packet(header, payload, MAX_PAYLOAD_SIZE,
                                          sender_addr, sender_port);
        if (result <= 0) break;

        if (m_on_response) {
            m_on_response(header, payload, header.payload_size);
        }
        ++count;
    }

    return count;
}

void client::set_on_response(on_response_received callback) {
    m_on_response = std::move(callback);
}

} // namespace entanglement
