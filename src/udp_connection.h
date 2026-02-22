#pragma once

#include "constants.h"
#include "packet_header.h"
#include <array>
#include <chrono>
#include <cstdint>

namespace entanglement
{

    // --- Endpoint identifier ---

    struct endpoint_key
    {
        uint32_t address = 0; // IPv4 in network byte order
        uint16_t port = 0;

        bool operator==(const endpoint_key &other) const { return address == other.address && port == other.port; }
    };

    struct endpoint_key_hash
    {
        size_t operator()(const endpoint_key &k) const
        {
            return std::hash<uint64_t>{}((static_cast<uint64_t>(k.address) << 16) | k.port);
        }
    };

    // --- Sent packet entry (metadata only, no payload copy) ---

    struct sent_packet_entry
    {
        uint64_t sequence = 0;
        bool acked = false;
        bool active = false;
        uint8_t flags = 0;
        uint8_t channel_id = 0;
        uint16_t shard_id = 0;
        uint16_t payload_size = 0;
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
    };

    // --- Connection state per peer ---

    constexpr size_t SEQUENCE_BUFFER_SIZE = 1024;

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
        void prepare_header(packet_header &header);

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

        uint64_t local_sequence() const { return m_local_sequence; }
        uint64_t remote_sequence() const { return m_remote_sequence; }

        // --- Reliability ---

        // Scan send buffer for timed-out reliable packets.
        // Marks them as inactive (gives up) and fills 'out' with loss metadata.
        // The application decides whether to resend as a new packet.
        // Returns how many losses were detected.
        int collect_losses(std::chrono::steady_clock::time_point now, lost_packet_info *out, int max_count);

        // RTT queries (milliseconds)
        double srtt_ms() const { return m_srtt / 1000.0; }
        double rttvar_ms() const { return m_rttvar / 1000.0; }
        double rto_ms() const { return static_cast<double>(m_rto) / 1000.0; }
        uint32_t rtt_sample_count() const { return m_rtt_sample_count; }

    private:
        bool m_active = false;
        endpoint_key m_endpoint{};

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
