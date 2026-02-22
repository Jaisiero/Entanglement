#include "udp_socket.h"
#include <cstring>
#include <iostream>

#ifdef ENTANGLEMENT_PLATFORM_WINDOWS
#include <io.h>
#else
#include <fcntl.h>
#endif

namespace entanglement
{

    udp_socket::~udp_socket()
    {
        close();
    }

    udp_socket::udp_socket(udp_socket &&other) noexcept : m_socket(other.m_socket)
    {
        other.m_socket = INVALID_SOCK;
    }

    udp_socket &udp_socket::operator=(udp_socket &&other) noexcept
    {
        if (this != &other)
        {
            close();
            m_socket = other.m_socket;
            other.m_socket = INVALID_SOCK;
        }
        return *this;
    }

    bool udp_socket::bind(uint16_t port, const std::string &address)
    {
        m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (m_socket == INVALID_SOCK)
        {
            std::cerr << "[udp_socket] Failed to create socket: " << last_socket_error() << std::endl;
            return false;
        }

        // Allow address reuse
        int opt = 1;
        setsockopt(m_socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&opt), sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, address.c_str(), &addr.sin_addr);

        if (::bind(m_socket, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
        {
            std::cerr << "[udp_socket] Failed to bind on port " << port << ": " << last_socket_error() << std::endl;
            close();
            return false;
        }

        return true;
    }

    int udp_socket::send_to(const void *data, size_t size, const std::string &address, uint16_t port)
    {
        sockaddr_in dest{};
        dest.sin_family = AF_INET;
        dest.sin_port = htons(port);
        inet_pton(AF_INET, address.c_str(), &dest.sin_addr);

        int sent = sendto(m_socket, static_cast<const char *>(data), static_cast<int>(size), 0,
                          reinterpret_cast<sockaddr *>(&dest), sizeof(dest));
        return sent;
    }

    int udp_socket::recv_from(void *buffer, size_t buffer_size, std::string &sender_address, uint16_t &sender_port)
    {
        sockaddr_in from{};
        socklen_t_ from_len = sizeof(from);

        int received = recvfrom(m_socket, static_cast<char *>(buffer), static_cast<int>(buffer_size), 0,
                                reinterpret_cast<sockaddr *>(&from), &from_len);

        if (received > 0)
        {
            char addr_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &from.sin_addr, addr_str, sizeof(addr_str));
            sender_address = addr_str;
            sender_port = ntohs(from.sin_port);
        }

        return received;
    }

    int udp_socket::send_packet(const packet_header &header, const void *payload, const std::string &address,
                                uint16_t port)
    {
        uint8_t buffer[MAX_PACKET_SIZE];
        size_t total = sizeof(packet_header) + header.payload_size;

        if (total > MAX_PACKET_SIZE)
        {
            std::cerr << "[udp_socket] Packet too large: " << total << " bytes" << std::endl;
            return -1;
        }

        std::memcpy(buffer, &header, sizeof(packet_header));
        if (payload && header.payload_size > 0)
        {
            std::memcpy(buffer + sizeof(packet_header), payload, header.payload_size);
        }

        return send_to(buffer, total, address, port);
    }

    int udp_socket::recv_packet(packet_header &header, void *payload, size_t payload_capacity,
                                std::string &sender_address, uint16_t &sender_port)
    {
        uint8_t buffer[MAX_PACKET_SIZE];

        int received = recv_from(buffer, MAX_PACKET_SIZE, sender_address, sender_port);
        if (received < static_cast<int>(sizeof(packet_header)))
        {
            return -1;
        }

        std::memcpy(&header, buffer, sizeof(packet_header));

        // Validate magic
        if (header.magic != PROTOCOL_MAGIC)
        {
            std::cerr << "[udp_socket] Invalid magic: 0x" << std::hex << header.magic << std::dec << std::endl;
            return -1;
        }

        size_t payload_bytes = received - sizeof(packet_header);
        if (payload && payload_bytes > 0)
        {
            size_t copy_size = (payload_bytes < payload_capacity) ? payload_bytes : payload_capacity;
            std::memcpy(payload, buffer + sizeof(packet_header), copy_size);
        }

        return received;
    }

    bool udp_socket::set_non_blocking(bool enabled)
    {
#ifdef ENTANGLEMENT_PLATFORM_WINDOWS
        u_long mode = enabled ? 1 : 0;
        return ioctlsocket(m_socket, FIONBIO, &mode) == 0;
#else
        int flags = fcntl(m_socket, F_GETFL, 0);
        if (flags == -1)
            return false;
        flags = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
        return fcntl(m_socket, F_SETFL, flags) == 0;
#endif
    }

    uint16_t udp_socket::local_port() const
    {
        sockaddr_in addr{};
        socklen_t_ len = sizeof(addr);
        if (getsockname(m_socket, reinterpret_cast<sockaddr *>(&addr), &len) == 0)
        {
            return ntohs(addr.sin_port);
        }
        return 0;
    }

    void udp_socket::close()
    {
        if (m_socket != INVALID_SOCK)
        {
            close_socket(m_socket);
            m_socket = INVALID_SOCK;
        }
    }

} // namespace entanglement
