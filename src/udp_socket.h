#pragma once

#include "packet_header.h"
#include "platform.h"
#include <functional>
#include <string>
#include <vector>

namespace entanglement
{

    constexpr size_t MAX_PAYLOAD_SIZE = MAX_PACKET_SIZE - sizeof(packet_header);

    class udp_socket
    {
    public:
        udp_socket() = default;
        ~udp_socket();

        // Non-copyable
        udp_socket(const udp_socket &) = delete;
        udp_socket &operator=(const udp_socket &) = delete;

        // Move semantics
        udp_socket(udp_socket &&other) noexcept;
        udp_socket &operator=(udp_socket &&other) noexcept;

        // Create and bind to a local port (0 = any available port)
        bool bind(uint16_t port, const std::string &address = "0.0.0.0");

        // Send raw data to a remote address
        int send_to(const void *data, size_t size, const std::string &address, uint16_t port);

        // Receive raw data, fills sender address/port
        int recv_from(void *buffer, size_t buffer_size, std::string &sender_address, uint16_t &sender_port);

        // Send a packet with header + payload
        int send_packet(const packet_header &header, const void *payload, const std::string &address, uint16_t port);

        // Receive a packet, splits header and payload
        int recv_packet(packet_header &header, void *payload, size_t payload_capacity, std::string &sender_address,
                        uint16_t &sender_port);

        // Set non-blocking mode
        bool set_non_blocking(bool enabled);

        // Get the local port the socket is bound to
        uint16_t local_port() const;

        bool is_valid() const { return m_socket != INVALID_SOCK; }
        void close();

    private:
        socket_t m_socket = INVALID_SOCK;
    };

} // namespace entanglement
