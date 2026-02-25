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

    udp_socket::udp_socket(udp_socket &&other) noexcept
        : m_socket(other.m_socket)
#ifdef ENTANGLEMENT_SIMULATE_LOSS
          ,
          m_drop_rate(other.m_drop_rate),
          m_drop_count(other.m_drop_count),
          m_rng(std::move(other.m_rng))
#endif
    {
        other.m_socket = INVALID_SOCK;
#ifdef ENTANGLEMENT_SIMULATE_LOSS
        other.m_drop_rate = 0.0;
        other.m_drop_count = 0;
#endif
    }

    udp_socket &udp_socket::operator=(udp_socket &&other) noexcept
    {
        if (this != &other)
        {
            close();
            m_socket = other.m_socket;
            other.m_socket = INVALID_SOCK;
#ifdef ENTANGLEMENT_SIMULATE_LOSS
            m_drop_rate = other.m_drop_rate;
            m_drop_count = other.m_drop_count;
            m_rng = std::move(other.m_rng);
            other.m_drop_rate = 0.0;
            other.m_drop_count = 0;
#endif
        }
        return *this;
    }

    error_code udp_socket::bind(uint16_t port, const std::string &address)
    {
        m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (m_socket == INVALID_SOCK)
        {
            std::cerr << "[udp_socket] Failed to create socket: " << last_socket_error() << std::endl;
            return error_code::socket_error;
        }

        // Allow address reuse
        int opt = 1;
        setsockopt(m_socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&opt), sizeof(opt));

        // Increase receive buffer for high-throughput scenarios
        int rcvbuf = SOCKET_RECV_BUFFER_SIZE;
        setsockopt(m_socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char *>(&rcvbuf), sizeof(rcvbuf));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, address.c_str(), &addr.sin_addr);

        if (::bind(m_socket, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
        {
            std::cerr << "[udp_socket] Failed to bind on port " << port << ": " << last_socket_error() << std::endl;
            close();
            return error_code::socket_error;
        }

        return error_code::ok;
    }

    int udp_socket::send_to(const void *data, size_t size, const endpoint_key &dest)
    {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(dest.port);
        addr.sin_addr.s_addr = dest.address;

        int sent = sendto(m_socket, static_cast<const char *>(data), static_cast<int>(size), 0,
                          reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
        return sent;
    }

    int udp_socket::recv_from(void *buffer, size_t buffer_size, endpoint_key &sender)
    {
        sockaddr_in from{};
        socklen_t_ from_len = sizeof(from);

        int received = recvfrom(m_socket, static_cast<char *>(buffer), static_cast<int>(buffer_size), 0,
                                reinterpret_cast<sockaddr *>(&from), &from_len);

        if (received > 0)
        {
#ifdef ENTANGLEMENT_SIMULATE_LOSS
            if (should_drop())
                return -1; // simulate network loss
#endif
            sender.address = from.sin_addr.s_addr;
            sender.port = ntohs(from.sin_port);
        }

        return received;
    }

    int udp_socket::send_packet(const packet_header &header, const void *payload, const endpoint_key &dest)
    {
        // Delegate to scatter-gather with a single payload segment
        if (payload && header.payload_size > 0)
        {
            const void *seg = payload;
            size_t seg_size = header.payload_size;
            return send_packet_gather(header, &seg, &seg_size, 1, dest);
        }
        return send_packet_gather(header, nullptr, nullptr, 0, dest);
    }

    int udp_socket::send_packet_gather(const packet_header &header, const void *const *segments, const size_t *sizes,
                                       size_t count, const endpoint_key &dest)
    {
        // Validate total size
        size_t payload_total = 0;
        for (size_t i = 0; i < count; ++i)
            payload_total += sizes[i];

        if (count > MAX_GATHER_SEGMENTS)
        {
            std::cerr << "[udp_socket] Too many gather segments: " << count << " (max " << MAX_GATHER_SEGMENTS << ")"
                      << std::endl;
            return -1;
        }

        size_t total = sizeof(packet_header) + payload_total;
        if (total > MAX_PACKET_SIZE)
        {
            std::cerr << "[udp_socket] Packet too large: " << total << " bytes" << std::endl;
            return -1;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(dest.port);
        addr.sin_addr.s_addr = dest.address;

#ifdef ENTANGLEMENT_PLATFORM_WINDOWS
        // WSASend scatter-gather — no intermediate buffer copy
        WSABUF bufs[MAX_GATHER_SEGMENTS + 1]; // slot 0 = header, rest = payload segments
        bufs[0].buf = reinterpret_cast<char *>(const_cast<packet_header *>(&header));
        bufs[0].len = static_cast<ULONG>(sizeof(packet_header));

        for (size_t i = 0; i < count; ++i)
        {
            bufs[i + 1].buf = static_cast<char *>(const_cast<void *>(segments[i]));
            bufs[i + 1].len = static_cast<ULONG>(sizes[i]);
        }

        DWORD bytes_sent = 0;
        int result = WSASendTo(m_socket, bufs, static_cast<DWORD>(count + 1), &bytes_sent, 0,
                               reinterpret_cast<sockaddr *>(&addr), sizeof(addr), nullptr, nullptr);
        return (result == 0) ? static_cast<int>(bytes_sent) : -1;
#else
        // sendmsg scatter-gather — no intermediate buffer copy
        struct iovec iov[MAX_GATHER_SEGMENTS + 1]; // slot 0 = header, rest = payload segments
        iov[0].iov_base = const_cast<packet_header *>(&header);
        iov[0].iov_len = sizeof(packet_header);

        for (size_t i = 0; i < count; ++i)
        {
            iov[i + 1].iov_base = const_cast<void *>(segments[i]);
            iov[i + 1].iov_len = sizes[i];
        }

        struct msghdr msg{};
        msg.msg_name = &addr;
        msg.msg_namelen = sizeof(addr);
        msg.msg_iov = iov;
        msg.msg_iovlen = static_cast<int>(count + 1);

        return static_cast<int>(sendmsg(m_socket, &msg, 0));
#endif
    }

    int udp_socket::recv_packet(packet_header &header, void *payload, size_t payload_capacity, endpoint_key &sender)
    {
        uint8_t buffer[MAX_PACKET_SIZE];

        int received = recv_from(buffer, MAX_PACKET_SIZE, sender);
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

    error_code udp_socket::set_non_blocking(bool enabled)
    {
#ifdef ENTANGLEMENT_PLATFORM_WINDOWS
        u_long mode = enabled ? 1 : 0;
        return ioctlsocket(m_socket, FIONBIO, &mode) == 0 ? error_code::ok : error_code::socket_error;
#else
        int flags = fcntl(m_socket, F_GETFL, 0);
        if (flags == -1)
            return error_code::socket_error;
        flags = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
        return fcntl(m_socket, F_SETFL, flags) == 0 ? error_code::ok : error_code::socket_error;
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

    error_code udp_socket::set_recv_buffer_size(int size_bytes)
    {
        return setsockopt(m_socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char *>(&size_bytes),
                          sizeof(size_bytes)) == 0
                   ? error_code::ok
                   : error_code::socket_error;
    }

    void udp_socket::close()
    {
        if (m_socket != INVALID_SOCK)
        {
            close_socket(m_socket);
            m_socket = INVALID_SOCK;
        }
    }

#ifdef ENTANGLEMENT_SIMULATE_LOSS
    void udp_socket::set_drop_rate(double rate)
    {
        if (rate < 0.0)
            rate = 0.0;
        if (rate > 1.0)
            rate = 1.0;
        m_drop_rate = rate;
    }

    bool udp_socket::should_drop()
    {
        if (m_drop_rate <= 0.0)
            return false;
        bool drop = m_dist(m_rng) < m_drop_rate;
        if (drop)
            ++m_drop_count;
        return drop;
    }
#endif

} // namespace entanglement
