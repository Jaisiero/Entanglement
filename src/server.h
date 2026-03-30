#pragma once

#include "server_worker.h"
#include <thread>
#include <vector>

namespace entanglement
{

    class server
    {
    public:
        explicit server(uint16_t port, const std::string &bind_address = "0.0.0.0");
        ~server();

        // Non-copyable, non-movable
        server(const server &) = delete;
        server &operator=(const server &) = delete;

        error_code start();
        void stop();
        bool is_running() const { return m_running.load(); }

        // Process pending packets (call from your game loop)
        int poll(int max_packets = DEFAULT_MAX_POLL_PACKETS);

        // Check connection timeouts and send heartbeats. Call from your game loop after poll().
        // Returns the number of connections that timed out.
        // If a loss_callback is provided it overrides the stored one for this call.
        int update(on_server_packet_lost loss_callback = nullptr);

        // Callbacks
        void set_on_client_data_received(on_client_data_received callback);
        void set_on_client_connected(on_client_connected callback);
        void set_on_client_disconnected(on_client_disconnected callback);
        void set_on_channel_requested(on_channel_requested callback);
        void set_on_packet_lost(on_server_packet_lost callback);

        // Send a data packet with a pre-built header (low-level).
        // Use send_to() instead for normal application data.
        int send_raw_to(packet_header &header, const void *payload, const endpoint_key &dest);
        int send_raw_to(packet_header &header, const void *payload, const std::string &address, uint16_t port);

        // Unified send: auto-handles simple and fragmented paths.
        // Messages <= MAX_PAYLOAD_SIZE are sent as a single packet;
        // larger messages are automatically fragmented.
        // Returns bytes of user data sent, or a negative error_code on failure.
        int send_to(const void *data, size_t size, uint8_t channel_id, const endpoint_key &dest, uint8_t flags = 0,
                    uint32_t *out_message_id = nullptr);
        int send_to(const void *data, size_t size, uint8_t channel_id, const std::string &address, uint16_t port,
                    uint8_t flags = 0, uint32_t *out_message_id = nullptr);

        // Retransmit a single fragment to a specific client.
        // For manual loss recovery only — prefer enable_auto_retransmit() instead.
        int send_fragment_to(uint32_t message_id, uint8_t fragment_index, uint8_t fragment_count, const void *data,
                             size_t size, uint8_t flags, uint8_t channel_id, const endpoint_key &dest,
                             uint32_t channel_sequence = 0);
        int send_fragment_to(uint32_t message_id, uint8_t fragment_index, uint8_t fragment_count, const void *data,
                             size_t size, uint8_t flags, uint8_t channel_id, const std::string &address, uint16_t port,
                             uint32_t channel_sequence = 0);

        // Disconnect a specific client
        void disconnect_client(const endpoint_key &key);

        // Disconnect all clients
        void disconnect_all();

        uint16_t port() const { return m_port; }

        size_t connection_count() const
        {
            size_t total = 0;
            for (auto &w : m_workers)
                total += w->connection_count();
            return total;
        }

        // Number of inbound datagrams silently dropped because a worker's
        // receive queue was full.  Non-zero means workers can't keep up
        // (e.g. application callbacks are too slow or queue is too small).
        uint64_t recv_queue_drops() const { return m_recv_queue_drops.load(std::memory_order_relaxed); }

        // --- Threading ---
        // Set the number of worker threads (call BEFORE start).
        // 0 (default) = single-threaded legacy mode driven by poll()/update().
        // >=1 = receiver thread + N worker threads; poll()/update() become no-ops.
        void set_worker_count(int count) { m_worker_count = count; }
        int worker_count() const { return m_worker_count; }

        // Enable automatic retransmission for all connections.
        // Reliable packets are auto-retransmitted from an internal buffer
        // instead of being reported through the loss callback.
        void enable_auto_retransmit() { m_auto_retransmit = true; }
        bool auto_retransmit_enabled() const { return m_auto_retransmit; }

        void set_verbose(bool verbose) { m_verbose = verbose; }
        bool verbose() const { return m_verbose; }

        // Enable platform-optimized async I/O for the receiver thread.
        // Windows: IOCP (overlapped WSARecvFrom + GetQueuedCompletionStatusEx)
        // Linux:   epoll + recvmmsg (batch kernel receive)
        // Call BEFORE start(). Only effective in multi-threaded mode (worker_count >= 1).
        void set_use_async_io(bool enabled) { m_use_async_io = enabled; }
        bool use_async_io() const { return m_use_async_io; }

        // Set the number of receive sockets (SO_REUSEPORT, Linux only).
        // When > 1, N sockets are bound to the same port and the kernel
        // distributes incoming datagrams by 5-tuple hash.  Each socket
        // gets its own receiver thread, parallelizing the receive path.
        // On Windows this setting is ignored (always 1 socket).
        // Call BEFORE start().
        void set_socket_count(int count) { m_socket_count = count; }
        int socket_count() const { return m_socket_count; }

#ifdef ENTANGLEMENT_SIMULATE_LOSS
        void set_simulated_drop_rate(double rate)
        {
            m_socket.set_drop_rate(rate);
            for (auto &s : m_extra_recv_sockets)
                s.set_drop_rate(rate);
        }
        double simulated_drop_rate() const { return m_socket.drop_rate(); }
#endif

        // Fragmentation: receiver callbacks
        void set_on_allocate_message(on_allocate_message cb);
        void set_on_message_complete(on_message_complete cb);
        void set_on_message_failed(on_message_failed cb);

        // Override the reassembly timeout (default: REASSEMBLY_TIMEOUT_US).
        void set_reassembly_timeout(int64_t timeout_us);

        // Fragment flow control: check if a specific client asked us to stop sending fragments
        bool is_fragment_throttled(const endpoint_key &dest) const;
        bool is_fragment_throttled(const std::string &address, uint16_t port) const;

        // Channel configuration
        channel_manager &channels() { return m_channels; }
        const channel_manager &channels() const { return m_channels; }

    private:
        udp_socket m_socket;
        uint16_t m_port;
        std::string m_bind_address;
        std::atomic<bool> m_running{false};
        bool m_verbose = true;
        channel_manager m_channels;
        int64_t m_reassembly_timeout_us = REASSEMBLY_TIMEOUT_US;

        // Stored callback templates (applied to each worker/connection)
        on_allocate_message m_frag_alloc_cb;
        on_message_complete m_app_on_message_complete;
        on_message_failed m_frag_failed_cb;

        on_client_data_received m_on_client_data_received;
        on_client_connected m_on_client_connected;
        on_client_disconnected m_on_client_disconnected;
        on_channel_requested m_on_channel_requested;
        on_server_packet_lost m_on_packet_lost;

        bool m_auto_retransmit = false;

        bool m_use_async_io = false;

        int m_socket_count = 1; // Number of receive sockets (SO_REUSEPORT on Linux)

        // Diagnostics: datagrams dropped by receiver thread due to full queues
        std::atomic<uint64_t> m_recv_queue_drops{0};

        // --- Worker infrastructure ---
        int m_worker_count = 0; // 0 = legacy single-threaded
        bool m_threaded = false;
        std::vector<std::unique_ptr<server_worker>> m_workers;

        // Extra receive sockets for SO_REUSEPORT multi-socket mode (Linux only).
        // m_socket is always the primary (index 0). These are indices 1..N-1.
        std::vector<udp_socket> m_extra_recv_sockets;

        // Threading (multi-threaded mode only)
        std::thread m_receiver_thread;                     // Primary receiver (socket index 0)
        std::vector<std::thread> m_extra_receiver_threads; // Extra receivers (socket indices 1..N-1)
        std::vector<std::thread> m_worker_threads;

        // Create and initialise workers (called from start())
        void create_workers();

        // Propagate stored callbacks to all workers
        void propagate_callbacks();

        // Determine which worker owns a given endpoint
        size_t worker_index(const endpoint_key &key) const
        {
            return (m_workers.size() <= 1) ? 0 : (endpoint_key_hash{}(key) % m_workers.size());
        }

        // Receiver thread loop (multi-threaded mode)
        void receiver_loop();

#if defined(ENTANGLEMENT_PLATFORM_WINDOWS) || defined(ENTANGLEMENT_PLATFORM_LINUX)
        // Platform-optimized async receiver loop (IOCP on Windows, epoll on Linux)
        // receiver_id identifies which recv queue to push to in each worker.
        void receiver_loop_async(int receiver_id = 0);

#ifdef ENTANGLEMENT_PLATFORM_LINUX
        // Async receiver loop for an extra SO_REUSEPORT socket.
        void receiver_loop_async_extra(int extra_index, int receiver_id);
#endif
#endif

        // Worker thread loop (multi-threaded mode)
        void worker_loop(size_t worker_idx);
    };

} // namespace entanglement
