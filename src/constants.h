#pragma once

#include <cstddef>
#include <cstdint>

namespace entanglement
{

    // --- Protocol identity ---
    constexpr uint16_t PROTOCOL_MAGIC = 0xE7A9;
    constexpr uint8_t PROTOCOL_VERSION = 1;

    // --- Network limits ---
    constexpr size_t MAX_PACKET_SIZE = 1200; // Safe MTU minus IP/UDP headers
    constexpr uint16_t DEFAULT_PORT = 9876;
    constexpr size_t MAX_CONNECTIONS = 1024;

    // --- Poll defaults ---
    constexpr int DEFAULT_MAX_POLL_PACKETS = 64;

} // namespace entanglement
