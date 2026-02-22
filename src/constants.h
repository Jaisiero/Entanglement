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

    // --- Reliability ---
    constexpr int64_t INITIAL_RTO_US = 200'000;   // 200 ms initial retransmission timeout
    constexpr int64_t MIN_RTO_US = 50'000;        // 50 ms minimum RTO
    constexpr int64_t MAX_RTO_US = 2'000'000;     // 2 s maximum RTO
    constexpr int MAX_RETRANSMISSIONS = 10;       // give up after this many retries
    constexpr int MAX_RETRANSMIT_PER_UPDATE = 16; // max retransmissions per update() call

} // namespace entanglement
