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
        // Bits 4–7 reserved for future use.
        // Reliability, ordering, and priority are determined by channel_config,
        // not by per-packet flags.
    };

#pragma pack(push, 1)
    struct packet_header
    {
        uint16_t magic;        // Protocol magic identifier
        uint8_t version;       // Protocol version
        uint8_t flags;         // Packet flags
        uint16_t shard_id;     // Shard identifier
        uint8_t channel_id;    // Channel identifier
        uint8_t reserved;      // Reserved (alignment)
        uint64_t sequence;     // Sequence number
        uint64_t ack;          // Acknowledgment number
        uint32_t ack_bitmap;   // Selective ACK bitmap
        uint16_t payload_size; // Payload size in bytes
    };
#pragma pack(pop)

    static_assert(sizeof(packet_header) == 30, "packet_header must be 30 bytes");

} // namespace entanglement
