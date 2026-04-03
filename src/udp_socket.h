#pragma once

#include "endpoint_key.h"
#include "packet_header.h"
#include "platform.h"
#include <functional>
#include <memory>
#include <string>

#ifdef ENTANGLEMENT_SIMULATE_LOSS
#include <random>
#endif

namespace entanglement
{

    constexpr size_t MAX_PAYLOAD_SIZE = MAX_PACKET_SIZE - sizeof(packet_header);
    static_assert(MAX_PAYLOAD_SIZE == MAX_PACKET_SIZE - PACKET_HEADER_SIZE,
                  "PACKET_HEADER_SIZE constant must match sizeof(packet_header)");

    // --- Async batch-receive constants (shared across platforms) ---
    constexpr int ASYNC_RECV_POOL_SIZE = 128; // Pre-allocated recv operations / recvmmsg slots
    constexpr int ASYNC_MAX_COMPLETIONS = 64; // Max completions per batch dequeue

    // Legacy aliases
    constexpr int IOCP_RECV_POOL_SIZE = ASYNC_RECV_POOL_SIZE;
    constexpr int IOCP_MAX_COMPLETIONS = ASYNC_MAX_COMPLETIONS;

#ifdef ENTANGLEMENT_PLATFORM_WINDOWS
    // --- IOCP receive operation (one per pre-posted WSARecvFrom) ---
    struct iocp_recv_op
    {
        OVERLAPPED overlapped{}; // Must be first for CONTAINING_RECORD
        WSABUF wsa_buf{};
        uint8_t buffer[MAX_PACKET_SIZE]{};
        sockaddr_in from_addr{};
        INT from_len = sizeof(sockaddr_in);
    };
#endif

    // --- Zero-copy parsed packet from async batch receive ---
    // payload_ptr points into internal receive buffer and is valid until
    // the next recv_batch call (epoll) or until repost_iocp_batch (IOCP).
    struct recv_completion
    {
        packet_header header{};
        const uint8_t *payload_ptr = nullptr;
        uint16_t payload_size = 0;
        endpoint_key sender{};
    };

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
        // reuse_port: on Linux, set SO_REUSEPORT to allow multiple sockets on the same port.
        error_code bind(uint16_t port, const std::string &address = "0.0.0.0", bool reuse_port = false);

        // Send raw data to a remote endpoint
        int send_to(const void *data, size_t size, const endpoint_key &dest);

        // Receive raw data, fills sender endpoint
        int recv_from(void *buffer, size_t buffer_size, endpoint_key &sender);

        // Send a packet with header + payload
        int send_packet(const packet_header &header, const void *payload, const endpoint_key &dest);

        // Send a packet with header + multiple payload segments (scatter-gather, zero-copy).
        // Segments are sent contiguously after the header without intermediate buffer copies.
        // segments/sizes arrays must have 'count' elements (max MAX_GATHER_SEGMENTS).
        int send_packet_gather(const packet_header &header, const void *const *segments, const size_t *sizes,
                               size_t count, const endpoint_key &dest);

        // Receive a packet, splits header and payload
        int recv_packet(packet_header &header, void *payload, size_t payload_capacity, endpoint_key &sender);

        // Set non-blocking mode
        error_code set_non_blocking(bool enabled);

        // Set socket receive buffer size (SO_RCVBUF)
        error_code set_recv_buffer_size(int size_bytes);

        // Get the local port the socket is bound to
        uint16_t local_port() const;

        bool is_valid() const { return m_socket != INVALID_SOCK; }
        void close();

#ifdef ENTANGLEMENT_PLATFORM_WINDOWS
        // --- IOCP batch receive ---

        // Initialise IOCP: create completion port, allocate recv pool, post initial recvs.
        // Call AFTER bind() and BEFORE entering the receiver loop.
        // The socket is automatically put into overlapped mode (non-blocking is not required).
        error_code init_iocp(int pool_size = ASYNC_RECV_POOL_SIZE);

        // Dequeue up to max_count completed recv operations (zero-copy).
        // Returns number of completions (0 if none ready within timeout_ms).
        // payload_ptr in each completion points into the IOCP buffer pool.
        // Caller MUST call repost_iocp_batch() after processing all payloads.
        int recv_batch_iocp(recv_completion *out, int max_count, DWORD timeout_ms = 0);

        // Re-post all IOCP receive operations from the last recv_batch_iocp call.
        // Must be called after the caller has finished reading payload_ptr data.
        void repost_iocp_batch();

        // Shut down IOCP: cancel pending I/O, close completion port.
        void shutdown_iocp();

        bool iocp_enabled() const { return m_iocp_enabled; }

        // No-op batch send on Windows – individual WSASendTo is already efficient.
        // Re-entrant: nested begin/flush pairs are harmless no-ops.
        void begin_send_batch() {}
        int flush_send_batch() { return 0; }
#endif

#ifdef ENTANGLEMENT_PLATFORM_LINUX
        // --- epoll + recvmmsg batch receive ---

        // Initialise epoll: create epoll fd, register socket, allocate recvmmsg pool.
        // Call AFTER bind() and BEFORE entering the receiver loop.
        error_code init_epoll(int pool_size = ASYNC_RECV_POOL_SIZE);

        // Receive up to max_count datagrams using recvmmsg (batch kernel receive).
        // Uses epoll_wait for efficient timeout, then drains all available data.
        // Returns number of valid parsed completions (0 if none ready within timeout_ms).
        int recv_batch_epoll(recv_completion *out, int max_count, int timeout_ms = 0);

        // Shut down epoll: close epoll fd, free buffers.
        void shutdown_epoll();

        bool epoll_enabled() const { return m_epoll_enabled; }

        // --- sendmmsg batch send ---

        // Enable send batching: subsequent send_packet / send_packet_gather calls
        // buffer packets instead of sending immediately.  Call flush_send_batch()
        // to send all buffered packets in a single sendmmsg() syscall.
        void begin_send_batch();

        // Flush all buffered sends via sendmmsg. Returns number of packets sent.
        // Automatically exits batch mode.
        int flush_send_batch();

        // --- UDP GSO (Generic Segmentation Offload) ---

        // Send a pre-built GSO buffer as a single sendmsg with UDP_SEGMENT cmsg.
        // The kernel splits it into datagrams of segment_size bytes (last may be shorter).
        // Returns total bytes submitted, or -1 on error.
        int send_gso(const void *buffer, size_t total_size, uint16_t segment_size,
                     const endpoint_key &dest);
#endif

#ifdef ENTANGLEMENT_SIMULATE_LOSS
        // Set the probability [0.0, 1.0] that an inbound packet is silently dropped.
        void set_drop_rate(double rate);
        double drop_rate() const { return m_drop_rate; }
        uint64_t drop_count() const { return m_drop_count; }
#endif

    private:
        socket_t m_socket = INVALID_SOCK;

#ifdef ENTANGLEMENT_PLATFORM_WINDOWS
        // IOCP state
        HANDLE m_iocp = nullptr;
        std::unique_ptr<iocp_recv_op[]> m_recv_pool;
        int m_recv_pool_size = 0;
        bool m_iocp_enabled = false;

        // Post a single overlapped WSARecvFrom
        bool post_recv(iocp_recv_op &op);

        // Deferred re-post tracking for zero-copy batch receive
        iocp_recv_op *m_pending_repost[IOCP_MAX_COMPLETIONS]{};
        int m_pending_repost_count = 0;
#endif

#ifdef ENTANGLEMENT_PLATFORM_LINUX
        // epoll + recvmmsg state
        int m_epoll_fd = -1;
        int m_epoll_pool_size = 0;
        bool m_epoll_enabled = false;

        // Pre-allocated arrays for recvmmsg batch receive.
        // m_recv_slots holds per-datagram buffers + sender addresses.
        // m_mmsg_hdrs is the contiguous mmsghdr array passed to recvmmsg.
        struct recvmmsg_slot
        {
            uint8_t buffer[MAX_PACKET_SIZE]{};
            sockaddr_in from_addr{};
            struct iovec iov{};
        };
        std::unique_ptr<recvmmsg_slot[]> m_recv_slots;
        std::unique_ptr<struct mmsghdr[]> m_mmsg_hdrs;

        // --- sendmmsg batch send ---
        static constexpr int SEND_BATCH_MAX = 256;

        struct sendmmsg_slot
        {
            uint8_t buffer[MAX_PACKET_SIZE]{};
            sockaddr_in dest{};
            struct iovec iov{};
        };

        bool m_send_batch_mode = false;
        int m_send_batch_count = 0;
        int m_send_batch_nesting = 0; // >0 means nested begin — flush is a no-op
        std::unique_ptr<sendmmsg_slot[]> m_send_slots;
        std::unique_ptr<struct mmsghdr[]> m_send_mmsg;
#endif

#ifdef ENTANGLEMENT_SIMULATE_LOSS
        double m_drop_rate = 0.0;
        uint64_t m_drop_count = 0;
        std::mt19937 m_rng{std::random_device{}()};
        std::uniform_real_distribution<double> m_dist{0.0, 1.0};
        bool should_drop();
#endif
    };

} // namespace entanglement
