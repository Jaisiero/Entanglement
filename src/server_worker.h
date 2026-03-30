#pragma once

#include "channel_manager.h"
#include "fragmentation.h"
#include "spsc_queue.h"
#include "udp_connection.h"
#include "udp_socket.h"
#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <thread>
#include <unordered_map>

namespace entanglement
{

    // --- Threading constants ---
    constexpr size_t WORKER_RECV_QUEUE_SIZE = 8192; // Per-worker inbound datagram queue
    constexpr size_t WORKER_SEND_QUEUE_SIZE = 1024; // Per-worker cross-thread send queue

    // --- Queued datagram (receiver thread → worker) ---
    struct queued_datagram
    {
        packet_header header{};
        endpoint_key sender{};
        uint8_t payload[MAX_PAYLOAD_SIZE]{};
    };

    // --- Send command (game thread → worker via queue) ---
    struct send_command
    {
        enum class kind : uint8_t
        {
            DATA,       // Unified send (auto-fragment)
            RAW,        // Pre-built header
            FRAGMENT,   // Single-fragment retransmit
            DISCONNECT, // Disconnect a client
        };

        kind type = kind::DATA;
        endpoint_key dest{};
        uint8_t channel_id = 0;
        uint8_t flags = 0;

        // Fragment-specific
        uint32_t message_id = 0;
        uint8_t fragment_index = 0;
        uint8_t fragment_count = 0;
        uint32_t channel_sequence = 0;

        // For RAW sends
        packet_header raw_header{};

        // Payload data (moved, not copied — handles both small and large messages)
        std::vector<uint8_t> data;
    };

    // Forward declarations for server callback types (duplicated from server.h to avoid circular include)
    using on_client_data_received = std::function<void(const packet_header &header, const uint8_t *payload,
                                                       size_t payload_size, const endpoint_key &sender)>;
    using on_client_connected = std::function<void(const endpoint_key &key, const std::string &address, uint16_t port)>;
    using on_client_disconnected =
        std::function<void(const endpoint_key &key, const std::string &address, uint16_t port)>;
    using on_channel_requested =
        std::function<bool(const endpoint_key &key, uint8_t channel_id, channel_mode mode, uint8_t priority)>;
    using on_server_packet_lost = std::function<void(const lost_packet_info &info, const endpoint_key &client)>;

    // -----------------------------------------------------------------------
    // server_worker — owns a subset of connections and processes them.
    //
    // In single-threaded mode the server drives the worker directly.
    // In multi-threaded mode each worker runs on its own thread and
    // receives datagrams through its SPSC recv queue.
    // -----------------------------------------------------------------------
    class server_worker
    {
    public:
        server_worker();
        ~server_worker() = default;

        // Non-copyable (contains atomics via queues). Movable by pointer only.
        server_worker(const server_worker &) = delete;
        server_worker &operator=(const server_worker &) = delete;

        // --- Initialisation (called once before start) ---
        void init(size_t pool_capacity, udp_socket *socket, channel_manager *channels,
                  const std::atomic<bool> *running_flag);

        // --- Receive queue (receiver thread → this worker) ---
        bool enqueue(queued_datagram &&dgram);
        bool enqueue(const queued_datagram &dgram);

        // --- Send command queue (any thread → this worker) ---
        bool enqueue_send(send_command &&cmd);

        // --- Processing (called from owning thread) ---

        // Dequeue and process up to max_packets from the recv queue.
        int poll_local(int max_packets = DEFAULT_MAX_POLL_PACKETS);

        // Process a single datagram directly (used in single-threaded mode, no queue).
        void process_datagram(const packet_header &header, const uint8_t *payload, const endpoint_key &sender);

        // Connection timeouts, heartbeats, loss detection, reassembly cleanup.
        int update(on_server_packet_lost loss_callback = nullptr);

        // --- Sending (must be called from owning thread for direct send) ---

        int send_raw_to(packet_header &header, const void *payload, const endpoint_key &dest);

        int send_to(const void *data, size_t size, uint8_t channel_id, const endpoint_key &dest, uint8_t flags = 0,
                    uint32_t *out_message_id = nullptr);

        int send_fragment_to(uint32_t message_id, uint8_t fragment_index, uint8_t fragment_count, const void *data,
                             size_t size, uint8_t flags, uint8_t channel_id, const endpoint_key &dest,
                             uint32_t channel_sequence = 0);

        // --- Connection management ---

        udp_connection *find(const endpoint_key &key);
        udp_connection *find_or_create(const endpoint_key &key);
        void disconnect_client(const endpoint_key &key);
        void disconnect_all();
        size_t connection_count() const { return m_index.size(); }

        bool is_fragment_throttled(const endpoint_key &key) const;

        // --- Callback setters (copies stored per worker) ---

        void set_on_client_data_received(on_client_data_received cb);
        void set_on_client_connected(on_client_connected cb) { m_on_client_connected = std::move(cb); }
        void set_on_client_disconnected(on_client_disconnected cb) { m_on_client_disconnected = std::move(cb); }
        void set_on_channel_requested(on_channel_requested cb) { m_on_channel_requested = std::move(cb); }
        void set_on_packet_lost(on_server_packet_lost cb) { m_on_packet_lost = std::move(cb); }

        // Fragmentation callbacks
        void set_on_allocate_message(on_allocate_message cb);
        void set_on_message_complete(on_message_complete cb);
        void set_on_message_failed(on_message_failed cb);
        void set_reassembly_timeout(int64_t timeout_us);

        // --- Configuration ---
        void set_auto_retransmit(bool enabled) { m_auto_retransmit = enabled; }
        bool auto_retransmit_enabled() const { return m_auto_retransmit; }

        void set_verbose(bool v) { m_verbose = v; }
        bool verbose() const { return m_verbose; }

        // --- Thread identity (for same-thread send optimisation) ---
        void set_thread_id(std::thread::id id) { m_thread_id = id; }
        std::thread::id thread_id() const { return m_thread_id; }

    private:
        // Shared resources (not owned)
        udp_socket *m_socket = nullptr;
        channel_manager *m_channels = nullptr;
        const std::atomic<bool> *m_running = nullptr;

        // Per-worker connection pool
        std::unique_ptr<udp_connection[]> m_pool;
        size_t m_pool_capacity = 0;
        std::unordered_map<endpoint_key, uint16_t, endpoint_key_hash> m_index;

        // Free-slot stack
        std::unique_ptr<uint16_t[]> m_free_stack;
        int m_free_top = -1;
        bool m_free_initialized = false;
        void init_freelist();
        int allocate_slot();

        // SPSC queues (heap-allocated to keep worker movable-by-pointer)
        std::unique_ptr<spsc_queue<queued_datagram, WORKER_RECV_QUEUE_SIZE>> m_recv_queue;
        std::unique_ptr<spsc_queue<send_command, WORKER_SEND_QUEUE_SIZE>> m_send_queue;

        // Flush pending send commands from the cross-thread queue
        void flush_send_queue();

        // Callbacks (copies — each worker invokes from its own thread)
        on_client_data_received m_on_client_data_received;
        on_client_connected m_on_client_connected;
        on_client_disconnected m_on_client_disconnected;
        on_channel_requested m_on_channel_requested;
        on_server_packet_lost m_on_packet_lost;

        // Fragmentation callback templates
        on_allocate_message m_frag_alloc_cb;
        on_message_complete m_app_on_message_complete;
        on_message_failed m_frag_failed_cb;
        int64_t m_reassembly_timeout_us = REASSEMBLY_TIMEOUT_US;

        bool m_auto_retransmit = false;
        bool m_verbose = true;

        std::thread::id m_thread_id{};

        // Tick counter for staggering expensive per-connection operations
        uint32_t m_tick_counter = 0;

        // Cached timestamp — set once per poll_local/update batch, used in send paths
        // to avoid redundant steady_clock::now() calls.
        std::chrono::steady_clock::time_point m_cached_now{};

        // --- Internal helpers ---
        void handle_control(const endpoint_key &key, const packet_header &header, const uint8_t *payload,
                            size_t payload_size);
        void send_control_to(udp_connection *conn, uint8_t control_type, const endpoint_key &dest);
        void send_control_payload_to(udp_connection *conn, const void *payload, size_t size, const endpoint_key &dest);
        void send_raw_control(uint8_t control_type, const endpoint_key &dest);
    };

} // namespace entanglement
