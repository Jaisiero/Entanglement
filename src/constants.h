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
    constexpr int64_t INITIAL_RTO_US = 200'000; // 200 ms initial retransmission timeout
    constexpr int64_t MIN_RTO_US = 50'000;      // 50 ms minimum RTO
    constexpr int64_t MAX_RTO_US = 2'000'000;   // 2 s maximum RTO
    constexpr int MAX_LOSSES_PER_UPDATE = 16;   // max loss notifications per update() call

    // --- Congestion control ---
    constexpr uint32_t INITIAL_CWND = 4;      // conservative initial window (packets)
    constexpr uint32_t MIN_CWND = 2;          // floor — never starve the connection
    constexpr uint32_t INITIAL_SSTHRESH = 64; // switch slow-start → congestion avoidance
    constexpr uint32_t MAX_CWND = 256;        // cap for gaming (low-latency priority)

    // --- Connection management ---
    constexpr int64_t HEARTBEAT_INTERVAL_US = 1'000'000;  // 1 s — send keepalive if idle
    constexpr int64_t CONNECTION_TIMEOUT_US = 10'000'000; // 10 s — disconnect if no recv

    // --- Control packet types (first byte of FLAG_CONTROL payload) ---
    enum control_type : uint8_t
    {
        CONTROL_CONNECTION_REQUEST = 0x01,
        CONTROL_CONNECTION_ACCEPTED = 0x02,
        CONTROL_CONNECTION_DENIED = 0x03,
        CONTROL_DISCONNECT = 0x04,
        CONTROL_HEARTBEAT = 0x05,
    };

} // namespace entanglement
