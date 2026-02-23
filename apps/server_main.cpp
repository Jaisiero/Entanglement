// ============================================================================
// Entanglement — Soak Test Server
// ============================================================================
// Receives messages from soak-test clients, tracks per-channel statistics,
// verifies ordering for RELIABLE_ORDERED channels, detects gaps and
// duplicates, echoes every message back, and prints a comprehensive
// summary when clients disconnect or on Ctrl+C.
// ============================================================================

#include "channel_manager.h"
#include "packet_header.h"
#include "platform.h"
#include "server.h"
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace entanglement;

// --- Soak message payload (must match client) ---
#pragma pack(push, 1)
struct soak_msg
{
    uint32_t msg_id;         // Sequential per-channel message ID (0-based)
    uint32_t total_expected; // Total messages the client will send on this channel
    uint8_t channel_id;      // Channel this was sent on (for cross-check)
};
#pragma pack(pop)

static constexpr size_t SOAK_MSG_SIZE = sizeof(soak_msg);

// --- Per-channel receive statistics ---
struct channel_recv_stats
{
    uint8_t channel_id = 0;
    channel_mode mode = channel_mode::UNRELIABLE;
    std::string name;
    uint32_t total_expected = 0;

    int unique_received = 0;
    int duplicate_count = 0;
    int out_of_order_count = 0;
    uint32_t highest_msg_id = 0;
    uint32_t last_ordered_id = UINT32_MAX;

    // Sequence tracking
    uint64_t min_sequence = UINT64_MAX;
    uint64_t max_sequence = 0;

    std::set<uint32_t> received_ids;

    void record(uint32_t mid, uint64_t seq)
    {
        if (received_ids.count(mid))
        {
            ++duplicate_count;
            return;
        }
        received_ids.insert(mid);
        ++unique_received;
        if (mid > highest_msg_id)
            highest_msg_id = mid;
        if (seq < min_sequence)
            min_sequence = seq;
        if (seq > max_sequence)
            max_sequence = seq;

        if (mode == channel_mode::RELIABLE_ORDERED)
        {
            if (last_ordered_id != UINT32_MAX && mid <= last_ordered_id)
            {
                ++out_of_order_count;
            }
            last_ordered_id = mid;
        }
    }

    // Effective expected: use total_expected if known, else highest_msg_id+1
    uint32_t effective_expected() const
    {
        if (total_expected > 0)
            return total_expected;
        return unique_received > 0 ? highest_msg_id + 1 : 0;
    }

    std::vector<uint32_t> missing_ids() const
    {
        std::vector<uint32_t> missing;
        uint32_t expected = effective_expected();
        if (expected == 0)
            return missing;
        for (uint32_t i = 0; i < expected; ++i)
        {
            if (!received_ids.count(i))
                missing.push_back(i);
        }
        return missing;
    }
};

// --- Per-client statistics ---
struct client_recv_stats
{
    std::string address;
    uint16_t port = 0;
    int total_data_packets = 0;
    int total_invalid_format = 0;
    int total_echoes_sent = 0;
    std::unordered_map<uint8_t, channel_recv_stats> channels;
    std::chrono::steady_clock::time_point first_packet_time;
    std::chrono::steady_clock::time_point last_packet_time;
    bool has_packets = false;
};

// --- Global state ---
static std::atomic<bool> g_running{true};
static std::unordered_map<std::string, client_recv_stats> g_client_stats;
static int g_total_data_packets = 0;
static int g_total_echoes_sent = 0;
static bool g_had_clients = false;

static std::string make_client_key(const std::string &addr, uint16_t p)
{
    return addr + ":" + std::to_string(p);
}

// --- Signal handler ---
static void signal_handler(int /*sig*/)
{
    g_running = false;
}

// --- Print per-channel stats block ---
static void print_channel_stats(const channel_recv_stats &cs)
{
    const char *mode_str = "UNKNOWN";
    switch (cs.mode)
    {
        case channel_mode::UNRELIABLE:
            mode_str = "UNRELIABLE";
            break;
        case channel_mode::RELIABLE:
            mode_str = "RELIABLE";
            break;
        case channel_mode::RELIABLE_ORDERED:
            mode_str = "RELIABLE_ORDERED";
            break;
    }

    uint32_t eff_expected = cs.effective_expected();
    char pct_buf[32] = "N/A";
    if (eff_expected > 0)
        std::snprintf(pct_buf, sizeof(pct_buf), "%.4f%%", 100.0 * cs.unique_received / eff_expected);

    std::cout << "  Channel: " << cs.name << " (id=" << static_cast<int>(cs.channel_id) << ", " << mode_str << ")"
              << std::endl;
    std::cout << "    Unique messages received: " << cs.unique_received << " / " << eff_expected << " (" << pct_buf
              << ")" << std::endl;
    std::cout << "    Duplicates received:      " << cs.duplicate_count << std::endl;
    if (cs.min_sequence != UINT64_MAX)
    {
        std::cout << "    Sequence range:           [" << cs.min_sequence << " .. " << cs.max_sequence
                  << "] (span=" << (cs.max_sequence - cs.min_sequence + 1) << ")" << std::endl;
    }

    if (cs.mode == channel_mode::RELIABLE_ORDERED)
    {
        std::cout << "    Ordering violations:      " << cs.out_of_order_count << std::endl;
    }

    auto missing = cs.missing_ids();
    if (missing.empty())
    {
        std::cout << "    Missing IDs:              none" << std::endl;
    }
    else
    {
        std::cout << "    Missing IDs:              " << missing.size() << " total";
        if (missing.size() <= 20)
        {
            std::cout << " [";
            for (size_t i = 0; i < missing.size(); ++i)
            {
                if (i > 0)
                    std::cout << ", ";
                std::cout << missing[i];
            }
            std::cout << "]";
        }
        else
        {
            std::cout << " (first 20: [";
            for (size_t i = 0; i < 20; ++i)
            {
                if (i > 0)
                    std::cout << ", ";
                std::cout << missing[i];
            }
            std::cout << ", ...])";
        }
        std::cout << std::endl;
    }
}

// --- Print stats for a single client ---
static void print_client_stats(const client_recv_stats &cs)
{
    std::cout << "\n--- Client: " << cs.address << ":" << cs.port << " ---" << std::endl;
    std::cout << "  Total data packets received: " << cs.total_data_packets << std::endl;
    std::cout << "  Invalid format packets:      " << cs.total_invalid_format << std::endl;
    std::cout << "  Echoes sent back:            " << cs.total_echoes_sent << std::endl;

    if (cs.has_packets)
    {
        auto duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(cs.last_packet_time - cs.first_packet_time);
        std::cout << "  Duration (first..last pkt):  " << duration.count() << " ms" << std::endl;

        if (duration.count() > 0)
        {
            double rate = 1000.0 * cs.total_data_packets / duration.count();
            std::cout << "  Receive throughput:          " << std::fixed << std::setprecision(1) << rate << " pkt/s"
                      << std::endl;
        }
    }

    std::cout << std::endl;

    // Print channels sorted by id
    std::vector<uint8_t> ch_ids;
    for (auto &[id, _] : cs.channels)
        ch_ids.push_back(id);
    std::sort(ch_ids.begin(), ch_ids.end());

    for (uint8_t id : ch_ids)
    {
        print_channel_stats(cs.channels.at(id));
        std::cout << std::endl;
    }
}

// --- Print aggregate verdict ---
static void print_aggregate_stats()
{
    std::cout << "\n=============================================" << std::endl;
    std::cout << " ENTANGLEMENT SOAK TEST — SERVER SUMMARY" << std::endl;
    std::cout << "=============================================" << std::endl;

    std::cout << "Total data packets received (all clients): " << g_total_data_packets << std::endl;
    std::cout << "Total echoes sent back:                     " << g_total_echoes_sent << std::endl;

    bool all_reliable_ok = true;
    bool all_ordered_ok = true;

    for (auto &[key, cs] : g_client_stats)
    {
        print_client_stats(cs);

        for (auto &[ch_id, ch] : cs.channels)
        {
            if (ch.mode == channel_mode::RELIABLE || ch.mode == channel_mode::RELIABLE_ORDERED)
            {
                if (ch.unique_received < static_cast<int>(ch.total_expected))
                {
                    all_reliable_ok = false;
                }
            }
            if (ch.mode == channel_mode::RELIABLE_ORDERED && ch.out_of_order_count > 0)
            {
                all_ordered_ok = false;
            }
        }
    }

    std::cout << "=============================================\n" << std::endl;

    if (all_reliable_ok)
        std::cout << "  VERDICT: ALL RELIABLE MESSAGES RECEIVED   [PASS]" << std::endl;
    else
        std::cout << "  VERDICT: SOME RELIABLE MESSAGES MISSING   [FAIL]" << std::endl;

    if (all_ordered_ok)
        std::cout << "  VERDICT: ORDERED DELIVERY INTACT          [PASS]" << std::endl;
    else
        std::cout << "  VERDICT: ORDERING VIOLATIONS DETECTED     [FAIL]" << std::endl;

    std::cout << "\n=============================================" << std::endl;
}

// ============================================================================

int main(int argc, char *argv[])
{
    if (!platform_init())
    {
        std::cerr << "Failed to initialize platform" << std::endl;
        return 1;
    }

    uint16_t port = DEFAULT_PORT;
    if (argc >= 2)
        port = static_cast<uint16_t>(std::atoi(argv[1]));

    std::signal(SIGINT, signal_handler);

    std::cout << "=============================================" << std::endl;
    std::cout << " Entanglement Soak Test Server v1.0" << std::endl;
    std::cout << " Port: " << port << std::endl;
    std::cout << " Header size: " << sizeof(packet_header) << " bytes" << std::endl;
    std::cout << " Soak payload: " << SOAK_MSG_SIZE << " bytes" << std::endl;
    std::cout << "=============================================" << std::endl;

    server srv(port);
    srv.channels().register_defaults();

    // --- Client connected ---
    srv.set_on_client_connected(
        [&](const endpoint_key & /*key*/, const std::string &addr, uint16_t p)
        {
            std::string ck = make_client_key(addr, p);
            auto &cs = g_client_stats[ck];
            cs.address = addr;
            cs.port = p;
            g_had_clients = true;
            std::cout << "[server] Client connected: " << addr << ":" << p << std::endl;
        });

    // --- Client disconnected ---
    srv.set_on_client_disconnected(
        [&](const endpoint_key & /*key*/, const std::string &addr, uint16_t p)
        {
            std::string ck = make_client_key(addr, p);
            std::cout << "[server] Client disconnected: " << addr << ":" << p << std::endl;
            auto it = g_client_stats.find(ck);
            if (it != g_client_stats.end())
            {
                print_client_stats(it->second);
            }
        });

    // --- Packet received ---
    srv.set_on_packet_received(
        [&](const packet_header &hdr, const uint8_t *payload, size_t size, const std::string &addr, uint16_t p)
        {
            ++g_total_data_packets;

            std::string ck = make_client_key(addr, p);
            auto &cs = g_client_stats[ck];
            ++cs.total_data_packets;

            auto now = std::chrono::steady_clock::now();
            if (!cs.has_packets)
            {
                cs.first_packet_time = now;
                cs.has_packets = true;
            }
            cs.last_packet_time = now;

            // Parse soak message
            if (size >= SOAK_MSG_SIZE)
            {
                soak_msg msg;
                std::memcpy(&msg, payload, SOAK_MSG_SIZE);

                uint8_t ch_id = hdr.channel_id;

                // Initialize channel stats if needed
                if (cs.channels.find(ch_id) == cs.channels.end())
                {
                    auto &ch = cs.channels[ch_id];
                    ch.channel_id = ch_id;

                    const auto *cfg = srv.channels().get_channel(ch_id);
                    if (cfg)
                    {
                        ch.mode = cfg->mode;
                        ch.name = cfg->name;
                    }
                    else
                    {
                        ch.name = "ch" + std::to_string(ch_id);
                    }
                }

                auto &ch = cs.channels[ch_id];
                if (ch.total_expected == 0)
                    ch.total_expected = msg.total_expected;

                ch.record(msg.msg_id, hdr.sequence);

                // Verbose logging: first 5, every 1000, last 5
                bool verbose = false;
                if (ch.unique_received <= 5)
                    verbose = true;
                else if (ch.unique_received % 1000 == 0)
                    verbose = true;
                else if (msg.total_expected > 0 && msg.msg_id >= msg.total_expected - 5)
                    verbose = true;

                if (verbose)
                {
                    std::cout << "[recv] " << addr << ":" << p << " ch=" << ch.name << " msg_id=" << msg.msg_id << "/"
                              << msg.total_expected << " seq=" << hdr.sequence << " ack=" << hdr.ack
                              << " (unique=" << ch.unique_received << ")" << std::endl;
                }
            }
            else
            {
                ++cs.total_invalid_format;
            }

            // Echo back on same channel
            packet_header reply{};
            reply.flags = hdr.flags;
            reply.shard_id = hdr.shard_id;
            reply.channel_id = hdr.channel_id;
            reply.payload_size = static_cast<uint16_t>(size);
            srv.send_to(reply, payload, addr, p);
            ++cs.total_echoes_sent;
            ++g_total_echoes_sent;
        });

    if (!srv.start())
    {
        platform_shutdown();
        return 1;
    }

    std::cout << "[server] Waiting for clients... (Ctrl+C to stop)" << std::endl;

    // Main loop with auto-stop on idle
    auto idle_start = std::chrono::steady_clock::now();
    constexpr auto IDLE_TIMEOUT = std::chrono::seconds(5);

    while (g_running.load())
    {
        srv.poll();
        srv.update();

        auto now = std::chrono::steady_clock::now();

        if (srv.connection_count() > 0)
        {
            idle_start = now;
        }

        // Auto-stop: if we had clients and all disconnected, wait IDLE_TIMEOUT then stop
        if (g_had_clients && srv.connection_count() == 0)
        {
            if (now - idle_start > IDLE_TIMEOUT)
            {
                std::cout << "\n[server] All clients disconnected. Auto-stopping after "
                          << std::chrono::duration_cast<std::chrono::seconds>(IDLE_TIMEOUT).count() << "s idle."
                          << std::endl;
                break;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    srv.stop();
    print_aggregate_stats();

    platform_shutdown();
    return 0;
}
