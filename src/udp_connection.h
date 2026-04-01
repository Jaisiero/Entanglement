#pragma once

#include "congestion_control.h"
#include "constants.h"
#include "endpoint_key.h"
#include "fragmentation.h"
#include "packet_header.h"
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>

namespace entanglement
{

    // Forward declarations for send helpers
    class udp_socket;
    class channel_manager;

    // --- Sent packet entry (metadata only, no payload copy) ---

    struct sent_packet_entry
    {
        uint64_t sequence = 0;
        bool acked = false;
        bool active = false;
        bool reliable = false; // Derived from channel_mode, used by collect_losses
        uint8_t flags = 0;
        uint8_t channel_id = 0;
        uint16_t shard_id = 0;
        uint16_t payload_size = 0;
        uint32_t channel_sequence = 0; // Per-channel ordering sequence (for retransmission)
        uint32_t message_id = 0;       // Non-zero if this packet is a fragment
        uint8_t fragment_index = 0;    // Which fragment within the message
        uint8_t fragment_count = 0;    // Total fragments in the message (0 if not a fragment)
        std::chrono::steady_clock::time_point send_time{};
    };

    // --- Lost packet info (returned by collect_losses for application-layer notification) ---

    struct lost_packet_info
    {
        uint64_t sequence = 0;
        uint8_t flags = 0;
        uint8_t channel_id = 0;
        uint16_t shard_id = 0;
        uint16_t payload_size = 0;
        uint32_t channel_sequence = 0; // Per-channel ordering sequence (pass back on retransmit)
        uint32_t message_id = 0;       // Non-zero if this was a fragment
        uint8_t fragment_index = 0;    // Which fragment within the message
        uint8_t fragment_count = 0;    // Total fragments in the message
    };

    // --- Ordered delivery buffer entry (receive-side hold-back for RELIABLE_ORDERED) ---

    struct ordered_pending_packet
    {
        bool active = false;
        uint32_t channel_sequence = 0;
        packet_header header{};
        uint8_t payload[MAX_ORDERED_PAYLOAD]{};
        uint16_t payload_size = 0;
    };

    // --- Ordered delivery buffer entry for completed fragmented messages ---

    struct ordered_pending_message
    {
        bool active = false;
        uint32_t channel_sequence = 0;
        uint8_t channel_id = 0;
        endpoint_key sender{};
        uint32_t message_id = 0;
        uint8_t *data = nullptr; // app-allocated buffer (ownership held until delivery)
        size_t total_size = 0;
    };

    // --- Automatic retransmission buffer entry ---

    struct retransmit_entry
    {
        bool active = false;
        uint64_t sequence = 0; // latest tracked sequence (updated on re-send)
        uint8_t flags = 0;
        uint8_t channel_id = 0;
        uint32_t channel_sequence = 0;
        uint32_t message_id = 0; // 0 for simple packets
        uint8_t fragment_index = 0;
        uint8_t fragment_count = 0;
        uint16_t data_size = 0;
        int attempts = 0;
        uint8_t data[MAX_ORDERED_PAYLOAD]{}; // user payload copy (MAX_ORDERED_PAYLOAD == MAX_PAYLOAD_SIZE)
    };

    struct retransmit_buffer
    {
        retransmit_entry entries[RETRANSMIT_BUFFER_SIZE]{};

        retransmit_entry *store(uint64_t seq, const void *payload, uint16_t size, uint8_t flags, uint8_t ch_id,
                                uint32_t ch_seq, uint32_t msg_id, uint8_t frag_idx, uint8_t frag_count)
        {
            // Find first free slot
            for (auto &e : entries)
            {
                if (!e.active)
                {
                    e.active = true;
                    e.sequence = seq;
                    e.flags = flags;
                    e.channel_id = ch_id;
                    e.channel_sequence = ch_seq;
                    e.message_id = msg_id;
                    e.fragment_index = frag_idx;
                    e.fragment_count = frag_count;
                    e.data_size = size;
                    e.attempts = 0;
                    if (size > 0 && payload)
                        std::memcpy(e.data, payload, size);
                    return &e;
                }
            }
            return nullptr; // full — this packet won't be auto-retransmitted
        }

        retransmit_entry *find(uint64_t seq)
        {
            for (auto &e : entries)
                if (e.active && e.sequence == seq)
                    return &e;
            return nullptr;
        }

        void remove(uint64_t seq)
        {
            for (auto &e : entries)
                if (e.active && e.sequence == seq)
                    e.active = false;
        }

        void clear()
        {
            for (auto &e : entries)
                e.active = false;
        }
    };

    // --- Connection state ---

    enum class connection_state : uint8_t
    {
        DISCONNECTED = 0,
        CONNECTING,
        CONNECTED,
    };

    // --- Connection state per peer ---

    class udp_connection
    {
    public:
        udp_connection() = default;

        // Reset to initial state (reuse pool slot)
        void reset();

        // --- Sending side ---

        // Get next sequence number and register it in the send buffer
        uint64_t next_sequence();

        // Fill header with current ack state before sending.
        // Registers the packet in the send buffer for ACK tracking / loss detection.
        // 'reliable' should be derived from channel_config (not a per-packet flag).
        // 'send_time' — if not epoch, used instead of calling steady_clock::now().
        void prepare_header(packet_header &header, bool reliable = false,
                            std::chrono::steady_clock::time_point send_time = {});

        // --- Receiving side ---

        // Process an incoming packet header: update remote sequence, ack bitmap,
        // and process piggybacked ACKs from the remote side.
        // Returns false if the packet is a duplicate (already received).
        // 'recv_time' — if not epoch, used instead of calling steady_clock::now().
        bool process_incoming(const packet_header &header, std::chrono::steady_clock::time_point recv_time = {});

        // --- Queries ---

        bool is_active() const { return m_active; }
        void set_active(bool active) { m_active = active; }

        endpoint_key endpoint() const { return m_endpoint; }
        void set_endpoint(const endpoint_key &ep) { m_endpoint = ep; }

        // Cache a timestamp for use by internal methods (process_incoming,
        // advance_ordered_seq, ack_packet RTT sample) to avoid repeated
        // calls to steady_clock::now() within the same processing batch.
        void set_cached_now(std::chrono::steady_clock::time_point t) { m_cached_now = t; }

        connection_state state() const { return m_state; }
        void set_state(connection_state s) { m_state = s; }

        uint64_t local_sequence() const { return m_local_sequence; }
        uint64_t remote_sequence() const { return m_remote_sequence; }

        // Direct access to a send buffer entry (for tagging fragment metadata after prepare_header)
        sent_packet_entry &send_buffer_entry(size_t index) { return m_send_buffer[index]; }
        const sent_packet_entry &send_buffer_entry(size_t index) const { return m_send_buffer[index]; }

        // --- Reliability ---

        // Scan send buffer for timed-out reliable packets.
        // Marks them as inactive (gives up) and fills 'out' with loss metadata.
        // The application decides whether to resend as a new packet.
        // Returns how many losses were detected.
        int collect_losses(std::chrono::steady_clock::time_point now, lost_packet_info *out, int max_count);

        // Connection liveness
        bool has_timed_out(std::chrono::steady_clock::time_point now) const;
        bool needs_heartbeat(std::chrono::steady_clock::time_point now) const;

        // Returns true if we have received data but haven't sent any packet
        // back within ACK_FLUSH_INTERVAL_US.  The caller should then call
        // send_ack_flush() so the remote gets timely ACK feedback.
        bool needs_ack_flush(std::chrono::steady_clock::time_point now) const;

        // Send a minimal ACK-only packet (sequence = 0) to the remote.
        // This piggybacks our latest ack + bitmap without allocating a sequence
        // or affecting the congestion controller.
        void send_ack_flush(udp_socket &socket, const endpoint_key &dest);

        // RTT queries (milliseconds)
        double srtt_ms() const { return m_srtt / 1000.0; }
        double rttvar_ms() const { return m_rttvar / 1000.0; }
        double rto_ms() const { return static_cast<double>(m_rto) / 1000.0; }
        uint32_t rtt_sample_count() const { return m_rtt_sample_count; }

        // --- Fragmentation (sender side) ---

        // Get a new monotonic message_id for a fragmented send
        uint32_t next_message_id() { return m_next_message_id++; }

        // Register a fragmented send for ACK tracking.
        // Call after all fragments have been sent via prepare_header.
        // Returns error_code::ok or error_code::pool_full.
        error_code register_pending_message(uint32_t message_id, uint8_t fragment_count);

        // Query whether a fragmented message has been fully ACKed
        bool is_message_acked(uint32_t message_id) const;

        // Set callback for when all fragments of a message are ACKed
        void set_on_message_acked(on_message_acked cb) { m_on_message_acked = std::move(cb); }

        // --- Congestion control ---

        bool can_send() const { return m_cc.can_send(); }
        congestion_info congestion() const { return m_cc.info(); }
        congestion_control &cc() { return m_cc; }

        // --- Automatic retransmission ---

        // Enable automatic retransmission for reliable channels.
        // Allocates the retransmit buffer (~74 KB).  Call once after reset.
        void enable_auto_retransmit();

        // Returns true if auto-retransmit is currently enabled.
        bool auto_retransmit_enabled() const { return m_retransmit != nullptr; }

        // Try to auto-retransmit a lost packet from the internal buffer.
        // If a stored copy exists and attempts < MAX, resends and returns true.
        // Otherwise returns false (caller should handle manually).
        bool try_auto_retransmit(const lost_packet_info &loss, udp_socket &socket, const channel_manager &channels,
                                 const endpoint_key &dest);

        // --- Ordered delivery diagnostics ---

        // Number of times skip_ordered_gap had to advance expected sequence.
        uint32_t ordered_gaps_skipped() const { return m_ordered_gaps_skipped; }

        // Check for ordered-delivery stalls across all channels.
        // If an ordered channel has buffered future messages but the expected
        // sequence hasn't advanced for the configured stall timeout, automatically
        // skip the gap and deliver the buffered messages.
        void check_ordered_stalls(const channel_manager &channels, std::chrono::steady_clock::time_point now);

        // Override the ordered stall timeout (default: ORDERED_STALL_TIMEOUT_US).
        // Lower values detect and recover stalls faster (useful during drain).
        void set_ordered_stall_timeout(int64_t timeout_us) { m_ordered_stall_timeout_us = timeout_us; }
        int64_t ordered_stall_timeout() const { return m_ordered_stall_timeout_us; }

        // --- Unified send (shared by client and server) ---

        // Send a user message (auto-fragments if needed).
        // Returns bytes of user data sent, or a negative error_code.
        // For coalesced channels, data is buffered and flushed later.
        int send_payload(udp_socket &socket, const channel_manager &channels, const void *data, size_t size,
                         uint8_t flags, uint8_t channel_id, const endpoint_key &dest,
                         uint32_t *out_message_id = nullptr, uint64_t *out_sequence = nullptr,
                         uint32_t channel_sequence = 0, uint32_t *out_channel_sequence = nullptr);

        // Send a single fragment (internal — used by send_payload and auto-retransmit).
        int send_fragment(udp_socket &socket, const channel_manager &channels, uint32_t message_id, uint8_t index,
                          uint8_t count, const void *data, size_t size, uint8_t flags, uint8_t channel_id,
                          const endpoint_key &dest, uint32_t channel_sequence = 0);

        // --- Message coalescing ---

        // Flush all pending coalesce buffers for this connection.
        // Called at the end of each update() tick.
        void flush_all_coalesce(udp_socket &socket, const channel_manager &channels, const endpoint_key &dest);

        // Flush a single channel's coalesce buffer.
        void flush_coalesce(uint8_t channel_id, udp_socket &socket, const channel_manager &channels,
                            const endpoint_key &dest);

        // Returns true if any channel has buffered coalesce data.
        bool has_pending_coalesce() const;

        // --- Fragment reassembly (receiver side, per-connection) ---

        fragment_reassembler &reassembler() { return m_reassembler; }
        const fragment_reassembler &reassembler() const { return m_reassembler; }

        // --- Fragment flow control (backpressure) ---

        // Sender side: remote peer told us to throttle fragmented sends
        bool is_fragment_backpressured() const { return m_fragment_backpressured; }
        void set_fragment_backpressured(bool bp) { m_fragment_backpressured = bp; }

        // Receiver side: we already sent CONTROL_BACKPRESSURE to sender
        bool backpressure_sent() const { return m_backpressure_sent; }
        void set_backpressure_sent(bool sent) { m_backpressure_sent = sent; }

        // --- Ordered delivery (receive-side hold-back for RELIABLE_ORDERED) ---

        // Expected next channel_sequence for a given channel (starts at 1)
        uint32_t expected_ordered_seq(uint8_t ch_id) const { return m_recv_channel_seq[ch_id]; }

        // Check if this channel_sequence is the expected next (i.e. deliverable now)
        bool is_ordered_next(uint8_t ch_id, uint32_t ch_seq) const { return ch_seq == m_recv_channel_seq[ch_id]; }

        // Advance the expected sequence after delivering a packet
        void advance_ordered_seq(uint8_t ch_id)
        {
            ++m_recv_channel_seq[ch_id];
            m_ordered_last_advance[ch_id] = m_cached_now;
        }

        // Buffer an out-of-order packet for later delivery. Returns true if buffered.
        bool buffer_ordered_packet(const packet_header &hdr, const uint8_t *data, uint16_t size);

        // Retrieve the next buffered packet matching the expected sequence.
        // Returns nullptr if none available.
        ordered_pending_packet *peek_next_ordered(uint8_t ch_id);

        // Release (deactivate) a buffered packet after delivery.
        void release_ordered_packet(ordered_pending_packet *pkt) { pkt->active = false; }

        // Force-skip: advance expected sequence past a gap when buffer is full.
        // Returns the number of packets delivered via skip (0 if nothing to skip to).
        int skip_ordered_gap(uint8_t ch_id);

        // --- Ordered delivery for fragmented messages (receive-side) ---

        // Buffer a completed fragmented message for later ordered delivery.
        bool buffer_ordered_message(const endpoint_key &sender, uint32_t message_id, uint8_t channel_id, uint8_t *data,
                                    size_t total_size, uint32_t channel_sequence);

        // Retrieve the next buffered message matching the expected sequence.
        ordered_pending_message *peek_next_ordered_message(uint8_t ch_id);

        // Release (deactivate) a buffered message after delivery.
        void release_ordered_message(ordered_pending_message *msg) { msg->active = false; }

        // --- Shared ordered-delivery helpers (used by both client and server) ---

        // Callback for delivering an ordered simple (non-fragmented) packet.
        using on_ordered_packet_deliver = std::function<void(const packet_header &, const uint8_t *, uint16_t)>;

        // Set the callback invoked when a buffered ordered packet is delivered.
        void set_on_ordered_packet_deliver(on_ordered_packet_deliver cb) { m_on_ordered_pkt_deliver = std::move(cb); }

        // Set the callback invoked when a buffered ordered fragmented message is delivered.
        void set_on_ordered_message_deliver(on_message_complete cb) { m_on_ordered_msg_deliver = std::move(cb); }

        // Drain ordered delivery buffers (simple + fragmented) for a channel,
        // invoking stored callbacks for each item delivered in sequence.
        void drain_ordered(uint8_t channel_id);

        // Process an incoming data packet through ordered delivery logic.
        // If the channel is ordered: delivers immediately or buffers, and drains. Returns true.
        // If the channel is not ordered: returns false (caller should deliver normally).
        bool deliver_ordered(const packet_header &header, const uint8_t *payload, uint16_t size,
                             const channel_manager &channels);

        // Install the ordered-delivery wrapper on the reassembler's on_complete callback.
        // The channels pointer must remain valid for the connection's lifetime.
        void install_ordered_complete_wrapper(const channel_manager *channels, on_message_complete app_cb);

    private:
        bool m_active = false;
        connection_state m_state = connection_state::DISCONNECTED;
        endpoint_key m_endpoint{};

        // Cached timestamp — set by the worker/client before processing a batch
        // to avoid repeated steady_clock::now() calls on the hot path.
        std::chrono::steady_clock::time_point m_cached_now{};

        // Timestamps for heartbeat / timeout
        std::chrono::steady_clock::time_point m_last_recv_time{};
        std::chrono::steady_clock::time_point m_last_send_time{};

        // Sending: local sequence counter + buffer of sent packets
        uint64_t m_local_sequence = 1;
        std::array<sent_packet_entry, SEQUENCE_BUFFER_SIZE> m_send_buffer{};

        // Receiving: highest remote sequence seen + bitmap of previous RECV_BITMAP_WIDTH
        uint64_t m_remote_sequence = 0;
        uint64_t m_recv_bitmap = 0;

        // RTT estimation (microseconds) — Jacobson/Karels (RFC 6298)
        bool m_rtt_initialized = false;
        double m_srtt = 0.0;            // smoothed RTT
        double m_rttvar = 0.0;          // RTT variance
        int64_t m_rto = INITIAL_RTO_US; // retransmission timeout
        uint32_t m_rtt_sample_count = 0;

        // Fragmentation (sender side)
        uint32_t m_next_message_id = 1; // 0 = not a fragment
        pending_message m_pending_messages[MAX_PENDING_FRAGMENTED_MESSAGES]{};
        on_message_acked m_on_message_acked;

        // Per-channel sequence counters (indexed by channel_id)
        uint32_t m_channel_sequences[MAX_CHANNELS]{};

        // Congestion control algorithm instance
        congestion_control m_cc;

        // Oldest un-acked sequence (optimization for collect_losses scan)
        uint64_t m_oldest_unacked_seq = 1;

        // Fragment reassembly (receiver side) — per-connection pool
        fragment_reassembler m_reassembler;

        // Fragment flow control (backpressure)
        bool m_fragment_backpressured = false; // sender side: remote told us to throttle
        bool m_backpressure_sent = false;      // receiver side: we sent throttle signal

        // Ordered delivery (receive-side hold-back for RELIABLE_ORDERED)
        uint32_t m_recv_channel_seq[MAX_CHANNELS]{}; // expected next per-channel seq (init 1 on first use)
        ordered_pending_packet m_ordered_buffer[ORDERED_BUFFER_SIZE]{};
        ordered_pending_message m_ordered_msg_buffer[ORDERED_MSG_BUFFER_SIZE]{};
        on_ordered_packet_deliver m_on_ordered_pkt_deliver; // stored callback for drain/deliver
        on_message_complete m_on_ordered_msg_deliver;       // stored callback for drain/deliver

        // Ordered delivery diagnostics
        uint32_t m_ordered_gaps_skipped = 0;

        // Ordered stall timeout: per-channel timestamp of when the expected
        // sequence last advanced.  Used to auto-skip gaps that block delivery.
        std::chrono::steady_clock::time_point m_ordered_last_advance[MAX_CHANNELS]{};
        int64_t m_ordered_stall_timeout_us = ORDERED_STALL_TIMEOUT_US;

        // Automatic retransmission (only allocated when enabled)
        std::unique_ptr<retransmit_buffer> m_retransmit;

        // Helper: mark a sent packet as acked.
        // take_rtt_sample: true for primary ACK (header.ack), false for bitmap entries
        // (bitmap ACKs can have inflated delays due to dropped responses).
        void ack_packet(uint64_t sequence, bool take_rtt_sample);

        // Helper: update RTT estimates from a sample
        void update_rtt(int64_t sample_us);

        // Helper: check if we already received a sequence
        bool is_sequence_received(uint64_t sequence) const;

        // Helper: record a received sequence in the bitmap
        void record_received(uint64_t sequence);

        // --- Message coalescing (per-channel send-side buffer) ---
        // Lazily-allocated: only channels with coalesce=true get a buffer.
        struct coalesce_buffer
        {
            uint8_t data[MAX_COALESCE_PAYLOAD]{};
            uint16_t used = 0;     // bytes written so far
            uint8_t msg_count = 0; // sub-messages in buffer
        };
        // Sparse map: index = channel_id, only populated for coalesced channels.
        // Using unique_ptr to avoid 256 × ~1.2 KB = ~300 KB per connection.
        std::unique_ptr<coalesce_buffer> m_coalesce[MAX_CHANNELS]{};

        // Ensure a coalesce buffer exists for a channel (lazily created)
        coalesce_buffer &ensure_coalesce(uint8_t channel_id);
    };

} // namespace entanglement
