#pragma once

#include "constants.h"

namespace entanglement {

enum packet_flags : uint8_t {
    FLAG_NONE        = 0,
    FLAG_RELIABLE    = 1 << 0,  // Guaranteed delivery via retransmission
    FLAG_FRAGMENT    = 1 << 1,  // Fragmented packet (part of a larger message)
    FLAG_HAS_ACK     = 1 << 2,  // Packet carries acknowledgment data
    FLAG_CONTROL     = 1 << 3,  // Control packet (handshake, disconnect, heartbeat)
    FLAG_ORDERED     = 1 << 4,  // Ordered delivery within channel
    FLAG_PRIORITY    = 1 << 5,  // High priority (physics, combat)
    FLAG_COMPRESSED  = 1 << 6,  // Payload is compressed
    FLAG_SHARD_RELAY = 1 << 7,  // Cross-shard relay (halo region forwarding)
};

#pragma pack(push, 1)
struct packet_header {
    uint16_t magic;          // Protocol magic identifier
    uint8_t  version;        // Protocol version
    uint8_t  flags;          // Packet flags
    uint16_t shard_id;       // Shard identifier
    uint8_t  channel_id;     // Channel identifier
    uint8_t  reserved;       // Reserved (alignment)
    uint64_t sequence;       // Sequence number
    uint64_t ack;            // Acknowledgment number
    uint32_t ack_bitmap;     // Selective ACK bitmap
    uint16_t payload_size;   // Payload size in bytes
};
#pragma pack(pop)

static_assert(sizeof(packet_header) == 30, "packet_header must be 30 bytes");

} // namespace entanglement
