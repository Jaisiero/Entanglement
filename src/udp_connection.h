#pragma once

#include "congestion_control.h"
#include "constants.h"
#include "endpoint_key.h"
#include "fragmentation.h"
#include "packet_header.h"
#include <array>
#include <chrono>
#include <cstdint>

namespace entanglement
{

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
        uint16_t message_id = 0;    // Non-zero if this packet is a fragment
        uint8_t fragment_index = 0; // Which fragment within the message
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
        uint16_t message_id = 0;    // Non-zero if this was a fragment
        uint8_t fragment_index = 0; // Which fragment within the message
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
        void prepare_header(packet_header &header, bool reliable = false);

        // --- Receiving side ---

        // Process an incoming packet header: update remote sequence, ack bitmap,
        // and process piggybacked ACKs from the remote side.
        // Returns false if the packet is a duplicate (already received).
        bool process_incoming(const packet_header &header);

        // --- Queries ---

        bool is_active() const { return m_active; }
        void set_active(bool active) { m_active = active; }

        endpoint_key endpoint() const { return m_endpoint; }
        void set_endpoint(const endpoint_key &ep) { m_endpoint = ep; }

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

        // RTT queries (milliseconds)
        double srtt_ms() const { return m_srtt / 1000.0; }
        double rttvar_ms() const { return m_rttvar / 1000.0; }
        double rto_ms() const { return static_cast<double>(m_rto) / 1000.0; }
        uint32_t rtt_sample_count() const { return m_rtt_sample_count; }

        // --- Fragmentation (sender side) ---

        // Get a new monotonic message_id for a fragmented send
        uint16_t next_message_id() { return m_next_message_id++; }

        // Register a fragmented send for ACK tracking.
        // Call after all fragments have been sent via prepare_header.
        // Returns false if the pending table is full.
        bool register_pending_message(uint16_t message_id, uint8_t fragment_count);

        // Query whether a fragmented message has been fully ACKed
        bool is_message_acked(uint16_t message_id) const;

        // Set callback for when all fragments of a message are ACKed
        void set_on_message_acked(on_message_acked cb) { m_on_message_acked = std::move(cb); }

        // --- Congestion control ---

        bool can_send() const { return m_cc.can_send(); }
        congestion_info congestion() const { return m_cc.info(); }
        congestion_control &cc() { return m_cc; }

    private:
        bool m_active = false;
        connection_state m_state = connection_state::DISCONNECTED;
        endpoint_key m_endpoint{};

        // Timestamps for heartbeat / timeout
        std::chrono::steady_clock::time_point m_last_recv_time{};
        std::chrono::steady_clock::time_point m_last_send_time{};

        // Sending: local sequence counter + buffer of sent packets
        uint64_t m_local_sequence = 1;
        std::array<sent_packet_entry, SEQUENCE_BUFFER_SIZE> m_send_buffer{};

        // Receiving: highest remote sequence seen + bitmap of previous 32
        uint64_t m_remote_sequence = 0;
        uint32_t m_recv_bitmap = 0;

        // RTT estimation (microseconds) — Jacobson/Karels (RFC 6298)
        bool m_rtt_initialized = false;
        double m_srtt = 0.0;            // smoothed RTT
        double m_rttvar = 0.0;          // RTT variance
        int64_t m_rto = INITIAL_RTO_US; // retransmission timeout
        uint32_t m_rtt_sample_count = 0;

        // Fragmentation (sender side)
        uint16_t m_next_message_id = 1; // 0 = not a fragment
        pending_message m_pending_messages[MAX_PENDING_FRAGMENTED_MESSAGES]{};
        on_message_acked m_on_message_acked;

        // Congestion control algorithm instance
        congestion_control m_cc;

        // Helper: mark a sent packet as acked (+ RTT sample)
        void ack_packet(uint64_t sequence);

        // Helper: update RTT estimates from a sample
        void update_rtt(int64_t sample_us);

        // Helper: check if we already received a sequence
        bool is_sequence_received(uint64_t sequence) const;

        // Helper: record a received sequence in the bitmap
        void record_received(uint64_t sequence);
    };

} // namespace entanglement
