#pragma once

#include "channel_manager.h"
#include "fragmentation.h"
#include "mpsc_queue.h"
#include "send_pool.h"
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
    constexpr size_t WORKER_SEND_QUEUE_SIZE = 4096; // Per-worker cross-thread send queue

    // --- Queued datagram (receiver thread → worker) ---
    struct queued_datagram
    {
        packet_header header{};
        endpoint_key sender{};
        uint8_t payload[MAX_PAYLOAD_SIZE]{};
    };

    // Per-receiver SPSC queue type (zero-copy via write_slot/read_slot)
    using recv_queue_t = spsc_queue<queued_datagram, WORKER_RECV_QUEUE_SIZE>;

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

        // Payload reference into the shared send_pool (replaces inline buffer).
        // Encoded offset: bit 31 = pool index, bits 0-30 = byte offset.
        uint32_t pool_offset = UINT32_MAX;
        uint16_t data_size = 0;
    };

    // Forward declarations for server callback types (duplicated from server.h to avoid circular include)
    using on_client_data_received = std::function<void(const packet_header &header, const uint8_t *payload,
                                                       size_t payload_size, const endpoint_key &sender)>;
    using on_client_coalesced_data =
        std::function<void(const packet_header &header, const uint8_t *raw_payload, size_t payload_size,
                           int message_count, const endpoint_key &sender)>;
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
        // recv_queue_count: number of SPSC recv queues (one per receiver thread).
        // Default 1 for single-socket mode. Set > 1 for SO_REUSEPORT multi-socket.
        void init(size_t pool_capacity, udp_socket *socket, channel_manager *channels,
                  const std::atomic<bool> *running_flag, send_pool *spool = nullptr, int recv_queue_count = 1);

        // --- Receive queue (receiver thread → this worker, zero-copy) ---
        // Acquires a pool slot, copies payload once from source, pushes slot index.
        bool enqueue_packet(const packet_header &hdr, const uint8_t *payload, uint16_t payload_size,
                            const endpoint_key &sender, int queue_id = 0);

        // --- Send command queue (any thread → this worker, multi-producer safe) ---
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

        // Send multiple payloads to the same destination via UDP GSO.
        int send_to_multi(const void *const *payloads, const uint16_t *sizes,
                          uint32_t count, uint8_t channel_id, const endpoint_key &dest, uint8_t flags = 0);

        // Zero-copy GSO builder: returns writable buffer for direct payload writes.
        // Layout: seg N payload starts at N * (sizeof(packet_header) + max_payload) + sizeof(packet_header).
        // In batch mode, returns the current (un-queued) slot.
        uint8_t *gso_buf();

        // Flush GSO builder: fill headers in-place, pad segments, send via GSO.
        // Payloads must already be written at correct offsets in gso_buf().
        // In batch mode: queues instead of sending; call gso_batch_flush() later.
        int gso_send(uint32_t count, const uint16_t *payload_sizes, uint16_t max_payload,
                     uint8_t channel_id, const endpoint_key &dest, uint8_t flags);

        // --- GSO sendmmsg batching ---
        // Begin batch mode: subsequent gso_send() calls queue instead of sending.
        void gso_batch_begin();
        // Flush all queued GSO sends via a single sendmmsg() syscall.
        int gso_batch_flush();

        int send_fragment_to(uint32_t message_id, uint8_t fragment_index, uint8_t fragment_count, const void *data,
                             size_t size, uint8_t flags, uint8_t channel_id, const endpoint_key &dest,
                             uint32_t channel_sequence = 0);

        // Flush coalesced message buffers for a specific client.
        void flush_coalesce_for(const endpoint_key &dest);

        // --- Connection management ---

        udp_connection *find(const endpoint_key &key);
        udp_connection *find_or_create(const endpoint_key &key);
        void disconnect_client(const endpoint_key &key);
        void disconnect_all();
        size_t connection_count() const { return m_index.size(); }

        bool is_fragment_throttled(const endpoint_key &key) const;

        // --- Callback setters (copies stored per worker) ---

        void set_on_client_data_received(on_client_data_received cb);
        void set_on_coalesced_data(on_client_coalesced_data cb) { m_on_coalesced_data = std::move(cb); }
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

        // Assign a dedicated send socket for this worker (eliminates socket-lock
        // contention when multiple workers send concurrently).  When not set,
        // the shared receive socket is used for both send and receive.
        void set_send_socket(udp_socket *s) { m_send_socket = s ? s : m_socket; }
        udp_socket *send_socket() const { return m_send_socket; }

    private:
        // Shared resources (not owned)
        udp_socket *m_socket = nullptr;      // receive (and default send) socket
        udp_socket *m_send_socket = nullptr; // per-worker send socket (may == m_socket)
        channel_manager *m_channels = nullptr;
        const std::atomic<bool> *m_running = nullptr;
        send_pool *m_send_pool = nullptr; // shared send data pool (for cross-thread commands)

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
        // One recv queue per receiver thread (size 1 in single-socket mode).
        // Uses write_slot/read_slot zero-copy API (no full-struct copies).
        std::vector<std::unique_ptr<recv_queue_t>> m_recv_queues;
        // MPSC send queue (multi-producer safe — game threads enqueue concurrently,
        // single worker thread drains in flush_send_queue).
        std::unique_ptr<mpsc_queue<send_command, WORKER_SEND_QUEUE_SIZE>> m_send_queue;

        // Flush pending send commands from the cross-thread queue
        void flush_send_queue();

        // Callbacks (copies — each worker invokes from its own thread)
        on_client_data_received m_on_client_data_received;
        on_client_coalesced_data m_on_coalesced_data;
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

        // Per-worker GSO pool — heap-allocated to support sendmmsg batching.
        // In non-batch mode, only slot 0 is used (same as before).
        // In batch mode, each gso_send() advances to the next slot.
        static constexpr size_t GSO_BUF_SIZE = 32768;
        static constexpr int GSO_POOL_SLOTS = 128;
        std::unique_ptr<uint8_t[]> m_gso_pool;

        // GSO batch state
        bool m_gso_batch_mode = false;
        int m_gso_batch_count = 0;
        std::unique_ptr<udp_socket::gso_batch_entry[]> m_gso_entries;

        // --- Internal helpers ---
        void handle_control(const endpoint_key &key, const packet_header &header, const uint8_t *payload,
                            size_t payload_size);
        void send_control_to(udp_connection *conn, uint8_t control_type, const endpoint_key &dest);
        void send_control_payload_to(udp_connection *conn, const void *payload, size_t size, const endpoint_key &dest);
        void send_raw_control(uint8_t control_type, const endpoint_key &dest);
    };

} // namespace entanglement
