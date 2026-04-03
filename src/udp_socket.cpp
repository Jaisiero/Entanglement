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

    error_code udp_socket::bind(uint16_t port, const std::string &address, bool reuse_port)
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

#ifdef ENTANGLEMENT_PLATFORM_LINUX
        // SO_REUSEPORT: allow multiple sockets on the same port (kernel distributes by 5-tuple hash)
        if (reuse_port)
        {
            int rp = 1;
            if (setsockopt(m_socket, SOL_SOCKET, SO_REUSEPORT, &rp, sizeof(rp)) != 0)
            {
                std::cerr << "[udp_socket] SO_REUSEPORT failed: " << last_socket_error() << std::endl;
                close();
                return error_code::socket_error;
            }
        }
#else
        (void)reuse_port; // SO_REUSEPORT not available on Windows
#endif

        // Increase receive buffer for high-throughput scenarios
        int rcvbuf = SOCKET_RECV_BUFFER_SIZE;
        setsockopt(m_socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char *>(&rcvbuf), sizeof(rcvbuf));

        // Increase send buffer to match (prevents burst drops on send side)
        int sndbuf = SOCKET_RECV_BUFFER_SIZE;
        setsockopt(m_socket, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char *>(&sndbuf), sizeof(sndbuf));

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
        // --- Linux: batch-mode or immediate sendmsg ---
        if (m_send_batch_mode)
        {
            // Flush if batch is full BEFORE accessing the slot array
            if (m_send_batch_count >= SEND_BATCH_MAX)
            {
                // Inline flush that preserves batch mode
                int remaining = m_send_batch_count;
                int offset = 0;
                while (remaining > 0)
                {
                    int sent = sendmmsg(m_socket, &m_send_mmsg[offset], remaining, 0);
                    if (sent <= 0)
                        break;
                    offset += sent;
                    remaining -= sent;
                }
                m_send_batch_count = 0;
            }

            // Linearise header + payload into the current batch slot
            auto &slot = m_send_slots[m_send_batch_count];
            slot.dest = addr;

            uint8_t *p = slot.buffer;
            std::memcpy(p, &header, sizeof(packet_header));
            p += sizeof(packet_header);
            for (size_t i = 0; i < count; ++i)
            {
                std::memcpy(p, segments[i], sizes[i]);
                p += sizes[i];
            }
            size_t pkt_size = static_cast<size_t>(p - slot.buffer);

            slot.iov.iov_base = slot.buffer;
            slot.iov.iov_len = pkt_size;

            auto &mh = m_send_mmsg[m_send_batch_count];
            std::memset(&mh, 0, sizeof(mh));
            mh.msg_hdr.msg_name = &slot.dest;
            mh.msg_hdr.msg_namelen = sizeof(sockaddr_in);
            mh.msg_hdr.msg_iov = &slot.iov;
            mh.msg_hdr.msg_iovlen = 1;

            ++m_send_batch_count;

            return static_cast<int>(total);
        }

        // Immediate sendmsg scatter-gather — no intermediate buffer copy
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
#ifdef ENTANGLEMENT_PLATFORM_WINDOWS
        if (m_iocp_enabled)
            shutdown_iocp();
#endif
#ifdef ENTANGLEMENT_PLATFORM_LINUX
        if (m_epoll_enabled)
            shutdown_epoll();
#endif
        if (m_socket != INVALID_SOCK)
        {
            close_socket(m_socket);
            m_socket = INVALID_SOCK;
        }
    }

    // -----------------------------------------------------------------------
    // IOCP batch receive (Windows only)
    // -----------------------------------------------------------------------

#ifdef ENTANGLEMENT_PLATFORM_WINDOWS

    bool udp_socket::post_recv(iocp_recv_op &op)
    {
        std::memset(&op.overlapped, 0, sizeof(OVERLAPPED));
        op.wsa_buf.buf = reinterpret_cast<char *>(op.buffer);
        op.wsa_buf.len = MAX_PACKET_SIZE;
        op.from_len = sizeof(sockaddr_in);

        DWORD flags = 0;
        DWORD bytes_received = 0;

        int result = WSARecvFrom(m_socket, &op.wsa_buf, 1, &bytes_received, &flags,
                                 reinterpret_cast<sockaddr *>(&op.from_addr), &op.from_len, &op.overlapped, nullptr);

        if (result == 0)
            return true; // Completed immediately — will appear in IOCP

        int err = WSAGetLastError();
        if (err == WSA_IO_PENDING)
            return true; // Normal async path

        // Real error — don't repost
        return false;
    }

    error_code udp_socket::init_iocp(int pool_size)
    {
        if (m_socket == INVALID_SOCK)
            return error_code::socket_error;

        // Create I/O Completion Port and associate the socket
        m_iocp = CreateIoCompletionPort(reinterpret_cast<HANDLE>(m_socket), nullptr, 0, 1);
        if (!m_iocp)
        {
            std::cerr << "[udp_socket] CreateIoCompletionPort failed: " << GetLastError() << std::endl;
            return error_code::socket_error;
        }

        // Allocate recv operation pool
        m_recv_pool_size = pool_size;
        m_recv_pool = std::make_unique<iocp_recv_op[]>(static_cast<size_t>(pool_size));

        // Post all initial receives
        int posted = 0;
        for (int i = 0; i < pool_size; ++i)
        {
            if (post_recv(m_recv_pool[i]))
                ++posted;
        }

        if (posted == 0)
        {
            std::cerr << "[udp_socket] Failed to post any IOCP recv operations" << std::endl;
            CloseHandle(m_iocp);
            m_iocp = nullptr;
            m_recv_pool.reset();
            return error_code::socket_error;
        }

        m_iocp_enabled = true;
        return error_code::ok;
    }

    int udp_socket::recv_batch_iocp(recv_completion *out, int max_count, DWORD timeout_ms)
    {
        OVERLAPPED_ENTRY entries[IOCP_MAX_COMPLETIONS];
        ULONG dequeued = 0;
        m_pending_repost_count = 0;

        int to_dequeue = (max_count < IOCP_MAX_COMPLETIONS) ? max_count : IOCP_MAX_COMPLETIONS;

        BOOL ok =
            GetQueuedCompletionStatusEx(m_iocp, entries, static_cast<ULONG>(to_dequeue), &dequeued, timeout_ms, FALSE);

        if (!ok || dequeued == 0)
            return 0;

        int count = 0;
        for (ULONG i = 0; i < dequeued; ++i)
        {
            auto *overlap = entries[i].lpOverlapped;
            DWORD bytes = entries[i].dwNumberOfBytesTransferred;

            // Recover the iocp_recv_op from the OVERLAPPED pointer
            auto *op = reinterpret_cast<iocp_recv_op *>(overlap);

            // Defer re-posting until caller has finished reading payload data
            m_pending_repost[m_pending_repost_count++] = op;

            // Validate
            if (bytes >= sizeof(packet_header))
            {
                packet_header hdr;
                std::memcpy(&hdr, op->buffer, sizeof(packet_header));

                if (hdr.magic == PROTOCOL_MAGIC)
                {
#ifdef ENTANGLEMENT_SIMULATE_LOSS
                    if (!should_drop())
#endif
                    {
                        auto &c = out[count];
                        c.header = hdr;
                        c.sender.address = op->from_addr.sin_addr.s_addr;
                        c.sender.port = ntohs(op->from_addr.sin_port);

                        // Zero-copy: point directly into the IOCP buffer (valid until repost)
                        c.payload_size = static_cast<uint16_t>(bytes - sizeof(packet_header));
                        c.payload_ptr = op->buffer + sizeof(packet_header);

                        ++count;
                    }
                }
            }
        }

        return count;
    }

    void udp_socket::repost_iocp_batch()
    {
        for (int i = 0; i < m_pending_repost_count; ++i)
            post_recv(*m_pending_repost[i]);
        m_pending_repost_count = 0;
    }

    void udp_socket::shutdown_iocp()
    {
        if (!m_iocp_enabled)
            return;

        m_iocp_enabled = false;

        // Cancel all pending overlapped I/O on this socket
        if (m_socket != INVALID_SOCK)
            CancelIoEx(reinterpret_cast<HANDLE>(m_socket), nullptr);

        // Drain any remaining completions
        if (m_iocp)
        {
            OVERLAPPED_ENTRY entries[IOCP_MAX_COMPLETIONS];
            ULONG dequeued = 0;
            // Brief wait to let cancellations complete
            GetQueuedCompletionStatusEx(m_iocp, entries, IOCP_MAX_COMPLETIONS, &dequeued, 100, FALSE);

            CloseHandle(m_iocp);
            m_iocp = nullptr;
        }

        m_recv_pool.reset();
        m_recv_pool_size = 0;
    }

#endif // ENTANGLEMENT_PLATFORM_WINDOWS

    // -----------------------------------------------------------------------
    // epoll + recvmmsg batch receive (Linux only)
    // -----------------------------------------------------------------------

#ifdef ENTANGLEMENT_PLATFORM_LINUX

    error_code udp_socket::init_epoll(int pool_size)
    {
        if (m_socket == INVALID_SOCK)
            return error_code::socket_error;

        // Create epoll instance
        m_epoll_fd = epoll_create1(0);
        if (m_epoll_fd < 0)
        {
            std::cerr << "[udp_socket] epoll_create1 failed: " << errno << std::endl;
            return error_code::socket_error;
        }

        // Register socket for level-triggered read events
        struct epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = m_socket;
        if (epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, m_socket, &ev) < 0)
        {
            std::cerr << "[udp_socket] epoll_ctl failed: " << errno << std::endl;
            ::close(m_epoll_fd);
            m_epoll_fd = -1;
            return error_code::socket_error;
        }

        // Allocate recvmmsg buffer pool
        m_epoll_pool_size = pool_size;
        m_recv_slots = std::make_unique<recvmmsg_slot[]>(static_cast<size_t>(pool_size));
        m_mmsg_hdrs = std::make_unique<struct mmsghdr[]>(static_cast<size_t>(pool_size));

        // Wire up the iovec → buffer and msghdr → slot pointers (one-time setup)
        for (int i = 0; i < pool_size; ++i)
        {
            auto &slot = m_recv_slots[i];
            slot.iov.iov_base = slot.buffer;
            slot.iov.iov_len = MAX_PACKET_SIZE;

            auto &mh = m_mmsg_hdrs[i];
            std::memset(&mh, 0, sizeof(mh));
            mh.msg_hdr.msg_name = &slot.from_addr;
            mh.msg_hdr.msg_namelen = sizeof(sockaddr_in);
            mh.msg_hdr.msg_iov = &slot.iov;
            mh.msg_hdr.msg_iovlen = 1;
        }

        m_epoll_enabled = true;
        return error_code::ok;
    }

    int udp_socket::recv_batch_epoll(recv_completion *out, int max_count, int timeout_ms)
    {
        // Wait for socket readability (avoids busy-spinning)
        struct epoll_event ev;
        int nfds = epoll_wait(m_epoll_fd, &ev, 1, timeout_ms);
        if (nfds <= 0)
            return 0;

        // Drain up to batch_size datagrams in a single syscall
        int batch = (max_count < m_epoll_pool_size) ? max_count : m_epoll_pool_size;

        // Reset msg_namelen for each slot (recvmmsg overwrites it)
        for (int i = 0; i < batch; ++i)
            m_mmsg_hdrs[i].msg_hdr.msg_namelen = sizeof(sockaddr_in);

        int n = recvmmsg(m_socket, m_mmsg_hdrs.get(), static_cast<unsigned int>(batch), MSG_DONTWAIT, nullptr);
        if (n <= 0)
            return 0;

        // Parse valid packets into completions
        int count = 0;
        for (int i = 0; i < n; ++i)
        {
            unsigned int bytes = m_mmsg_hdrs[i].msg_len;
            if (bytes < sizeof(packet_header))
                continue;

            auto &slot = m_recv_slots[i];
            packet_header hdr;
            std::memcpy(&hdr, slot.buffer, sizeof(packet_header));

            if (hdr.magic != PROTOCOL_MAGIC)
                continue;

#ifdef ENTANGLEMENT_SIMULATE_LOSS
            if (should_drop())
                continue;
#endif

            auto &c = out[count];
            c.header = hdr;
            c.sender.address = slot.from_addr.sin_addr.s_addr;
            c.sender.port = ntohs(slot.from_addr.sin_port);

            // Zero-copy: point directly into the recvmmsg buffer (valid until next batch call)
            c.payload_size = static_cast<uint16_t>(bytes - sizeof(packet_header));
            c.payload_ptr = slot.buffer + sizeof(packet_header);

            ++count;
        }

        return count;
    }

    void udp_socket::shutdown_epoll()
    {
        if (!m_epoll_enabled)
            return;

        m_epoll_enabled = false;

        if (m_epoll_fd >= 0)
        {
            ::close(m_epoll_fd);
            m_epoll_fd = -1;
        }

        m_recv_slots.reset();
        m_mmsg_hdrs.reset();
        m_epoll_pool_size = 0;
    }

    // --- sendmmsg batch send ---

    void udp_socket::begin_send_batch()
    {
        // Lazily allocate on first call
        if (!m_send_slots)
        {
            m_send_slots = std::make_unique<sendmmsg_slot[]>(SEND_BATCH_MAX);
            m_send_mmsg = std::make_unique<struct mmsghdr[]>(SEND_BATCH_MAX);
        }
        m_send_batch_mode = true;
        m_send_batch_count = 0;
    }

    int udp_socket::flush_send_batch()
    {
        m_send_batch_mode = false;

        if (m_send_batch_count == 0)
            return 0;

        int total_sent = 0;
        int remaining = m_send_batch_count;
        int offset = 0;

        while (remaining > 0)
        {
            int sent = sendmmsg(m_socket, &m_send_mmsg[offset], remaining, 0);
            if (sent <= 0)
                break;
            total_sent += sent;
            offset += sent;
            remaining -= sent;
        }

        m_send_batch_count = 0;
        return total_sent;
    }

#endif // ENTANGLEMENT_PLATFORM_LINUX

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
