#include "server.h"
#include <iostream>

namespace entanglement {

server::server(uint16_t port, const std::string& bind_address)
    : m_port(port), m_bind_address(bind_address) {}

server::~server() {
    stop();
}

bool server::start() {
    if (!m_socket.bind(m_port, m_bind_address)) {
        return false;
    }

    if (!m_socket.set_non_blocking(true)) {
        std::cerr << "[server] Failed to set non-blocking mode" << std::endl;
        m_socket.close();
        return false;
    }

    m_running = true;
    std::cout << "[server] Listening on " << m_bind_address << ":" << m_port << std::endl;
    return true;
}

void server::stop() {
    if (!m_running) return;
    m_running = false;
    m_socket.close();
    std::cout << "[server] Stopped" << std::endl;
}

int server::poll() {
    if (!m_running) return 0;

    int count = 0;
    packet_header header{};
    uint8_t payload[MAX_PAYLOAD_SIZE];
    std::string sender_addr;
    uint16_t sender_port = 0;

    // Drain all pending packets
    while (true) {
        int result = m_socket.recv_packet(header, payload, MAX_PAYLOAD_SIZE,
                                          sender_addr, sender_port);
        if (result <= 0) break;

        if (m_on_packet_received) {
            m_on_packet_received(header, payload, header.payload_size,
                                 sender_addr, sender_port);
        }
        ++count;
    }

    return count;
}

void server::set_on_packet_received(on_packet_received callback) {
    m_on_packet_received = std::move(callback);
}

int server::send_to(const packet_header& header, const void* payload,
                    const std::string& address, uint16_t port) {
    return m_socket.send_packet(header, payload, address, port);
}

} // namespace entanglement
