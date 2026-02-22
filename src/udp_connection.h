#pragma once

#include "constants.h"
#include "packet_header.h"
#include <array>
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

    // --- Sent packet entry (stored in send buffer for potential retransmission) ---

    struct sent_packet_entry
    {
        uint64_t sequence = 0;
        bool acked = false;
        bool active = false; // slot in use
        // TODO: timestamp for RTT / retransmission
        // TODO: payload copy for retransmission
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

        // Fill header with current ack state before sending
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

    private:
        bool m_active = false;
        endpoint_key m_endpoint{};

        // Sending: local sequence counter + buffer of sent packets
        uint64_t m_local_sequence = 1;
        std::array<sent_packet_entry, SEQUENCE_BUFFER_SIZE> m_send_buffer{};

        // Receiving: highest remote sequence seen + bitmap of previous 32
        uint64_t m_remote_sequence = 0;
        uint32_t m_recv_bitmap = 0;

        // Helper: mark a sent packet as acked
        void ack_packet(uint64_t sequence);

        // Helper: check if we already received a sequence
        bool is_sequence_received(uint64_t sequence) const;

        // Helper: record a received sequence in the bitmap
        void record_received(uint64_t sequence);
    };

} // namespace entanglement
