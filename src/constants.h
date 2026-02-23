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
    constexpr int SOCKET_RECV_BUFFER_SIZE = 256 * 1024; // 256 KB SO_RCVBUF

    // --- Poll defaults ---
    constexpr int DEFAULT_MAX_POLL_PACKETS = 64;

    // --- Sequence / ACK tracking ---
    constexpr size_t SEQUENCE_BUFFER_SIZE = 1024; // Circular send buffer entries
    constexpr int ACK_BITMAP_WIDTH = 32;          // Selective ACK bitmap width (bits)

    // --- Reliability ---
    constexpr int64_t INITIAL_RTO_US = 200'000; // 200 ms initial retransmission timeout
    constexpr int64_t MIN_RTO_US = 50'000;      // 50 ms minimum RTO
    constexpr int64_t MAX_RTO_US = 2'000'000;   // 2 s maximum RTO
    constexpr int MAX_LOSSES_PER_UPDATE = 64;   // max loss notifications per update() call

    // --- RTT estimation (RFC 6298 / Jacobson-Karels) ---
    constexpr double RTT_ALPHA = 0.125;             // SRTT smoothing factor (1/8)
    constexpr double RTT_BETA = 0.25;               // RTTVAR smoothing factor (1/4)
    constexpr double RTT_VARIANCE_MULTIPLIER = 4.0; // K in RTO = SRTT + K * RTTVAR
    constexpr int64_t CLOCK_GRANULARITY_US = 1000;  // 1 ms minimum granularity (G)

    // --- Congestion control ---
    constexpr uint32_t INITIAL_CWND = 4;      // conservative initial window (packets)
    constexpr uint32_t MIN_CWND = 4;          // floor — never starve the connection
    constexpr uint32_t INITIAL_SSTHRESH = 64; // switch slow-start → congestion avoidance
    constexpr uint32_t MAX_CWND = 256;        // cap for gaming (low-latency priority)

    // --- Connection management ---
    constexpr int64_t HEARTBEAT_INTERVAL_US = 1'000'000;  // 1 s — send keepalive if idle
    constexpr int64_t CONNECTION_TIMEOUT_US = 10'000'000; // 10 s — disconnect if no recv
    constexpr int HANDSHAKE_MAX_ATTEMPTS = 10;            // connection request retries
    constexpr int64_t HANDSHAKE_RETRY_INTERVAL_MS = 500;  // ms between retries
    constexpr int MAX_TIMEOUTS_PER_UPDATE = 32;           // max timeouts processed per server update

    // --- Channel limits ---
    constexpr size_t MAX_CHANNELS = 256;    // Matches uint8_t channel_id range (0–255)
    constexpr size_t MAX_CHANNEL_NAME = 32; // Max channel name length (including null terminator)

    // --- Control packet types (first byte of FLAG_CONTROL payload) ---
    enum control_type : uint8_t
    {
        CONTROL_CONNECTION_REQUEST = 0x01,
        CONTROL_CONNECTION_ACCEPTED = 0x02,
        CONTROL_CONNECTION_DENIED = 0x03,
        CONTROL_DISCONNECT = 0x04,
        CONTROL_HEARTBEAT = 0x05,
        CONTROL_CHANNEL_OPEN = 0x06, // Client requests opening a channel
        CONTROL_CHANNEL_ACK = 0x07,  // Server responds to channel open
        CONTROL_BACKPRESSURE = 0x08, // Flow control: receiver tells sender to throttle/resume
    };

    // --- Channel negotiation ---
    constexpr uint8_t CHANNEL_STATUS_ACCEPTED = 0x00;
    constexpr uint8_t CHANNEL_STATUS_REJECTED = 0x01;
    constexpr int CHANNEL_OPEN_MAX_ATTEMPTS = 5;            // retries before giving up
    constexpr int64_t CHANNEL_OPEN_RETRY_INTERVAL_MS = 200; // ms between retries

    // --- Fragment flow control (backpressure) ---
    constexpr int BACKPRESSURE_HIGH_WATERMARK = 75; // percent: send throttle signal to sender
    constexpr int BACKPRESSURE_LOW_WATERMARK = 50;  // percent: send relief signal to sender

    // --- Scatter-gather I/O ---
    constexpr size_t MAX_GATHER_SEGMENTS = 2; // Max payload segments per send_packet_gather (header excluded)

    // --- Fragmentation ---
    constexpr size_t FRAGMENT_HEADER_SIZE = 6; // [message_id(4)][index(1)][count(1)]
    constexpr size_t PACKET_HEADER_SIZE = 34;  // Must match sizeof(packet_header)
    constexpr size_t MAX_FRAGMENT_PAYLOAD =
        MAX_PACKET_SIZE - PACKET_HEADER_SIZE - FRAGMENT_HEADER_SIZE; // ~1166 bytes user data per fragment
    constexpr size_t MAX_PENDING_FRAGMENTED_MESSAGES = 64;           // sender: concurrent fragmented sends tracked
    constexpr size_t MAX_INCOMING_FRAGMENTED_MESSAGES = 64;          // receiver: per-connection concurrent reassemblies
    constexpr int64_t REASSEMBLY_TIMEOUT_US = 15'000'000;            // 15 s — accommodate fragment retransmissions

} // namespace entanglement
