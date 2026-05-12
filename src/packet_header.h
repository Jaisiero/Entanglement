#pragma once

#include "constants.h"

namespace entanglement
{

    enum packet_flags : uint8_t
    {
        FLAG_NONE = 0,
        FLAG_FRAGMENT = 1 << 0,    // Fragmented packet (part of a larger message)
        FLAG_CONTROL = 1 << 1,     // Control packet (handshake, disconnect, heartbeat)
        FLAG_COMPRESSED = 1 << 2,  // Payload is compressed
        FLAG_SHARD_RELAY = 1 << 3, // Cross-shard relay (halo region forwarding)
        FLAG_COALESCED = 1 << 4,   // Payload contains multiple length-prefixed sub-messages
        // FLAG_RESET (2026-05-12): "session-reset" marker. The receiver
        // resets its `udp_connection` state (local_sequence,
        // remote_sequence, channel sequences, ack bitmap, fragmentation
        // state, RTT, congestion) IMMEDIATELY upon receipt — before the
        // sequence check that would otherwise drop this packet as
        // out-of-order. The sender must reset its own outbound state
        // BEFORE emitting (so the packet itself is seq=1).
        //
        // Use case: socket pre-warm / backup-handle promotion in the
        // client. The bot keeps a UDP socket alive to a previously-used
        // shard via heartbeats; when a later SHARD_HANDOFF brings it
        // back, the bot reuses that socket instead of paying the ~500 ms
        // setup of a fresh connection. Without FLAG_RESET, the
        // application-layer HANDOFF_AUTH/SessionOpen reply pair race
        // against stale sequence state on either side and the session
        // never activates (server's "stale Pending" watchdog observed
        // 2026-05-12). With FLAG_RESET on both directions of the
        // HANDOFF_AUTH/SessionOpen exchange, both endpoints converge
        // on a clean (seq=1, ack=0, bitmap=0) state and the bind
        // succeeds within RTT (~1-5 ms LAN).
        FLAG_RESET = 1 << 5,
        // Bits 6–7 reserved for future use.
        // Reliability, ordering, and priority are determined by channel_config,
        // not by per-packet flags.
    };

#pragma pack(push, 1)
    struct packet_header
    {
        uint16_t magic;            // Protocol magic identifier
        uint8_t version;           // Protocol version
        uint8_t flags;             // Packet flags
        uint16_t shard_id;         // Shard identifier
        uint8_t channel_id;        // Channel identifier
        uint8_t reserved;          // Reserved for future use
        uint64_t sequence;         // Global sequence number (ACK / loss / RTT)
        uint64_t ack;              // Acknowledgment number
        uint32_t ack_bitmap;       // Selective ACK bitmap
        uint32_t channel_sequence; // Per-channel sequence (ordering / dedup)
        uint16_t payload_size;     // Payload size in bytes
    };
#pragma pack(pop)

    static_assert(sizeof(packet_header) == 34, "packet_header must be 34 bytes");

} // namespace entanglement
