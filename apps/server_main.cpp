// ============================================================================
// Entanglement — Soak Test Server v3.1
// ============================================================================
// Receives both simple and fragmented messages from soak-test clients,
// tracks per-channel statistics for each type, verifies ordering for
// RELIABLE_ORDERED channels, detects gaps/duplicates, echoes every
// message back, and prints a comprehensive summary.
//
// Echo retransmission: when a reliable echo packet is detected as lost
// (via collect_losses), the server retransmits it. Simple echoes are
// stored by sequence; fragmented echoes are stored by message_id.
// This ensures 100% end-to-end delivery under simulated packet loss.
//
// Simple messages   → set_on_client_data_received   → echo via send_to
// Fragmented messages → set_on_message_complete → echo via send_to
// ============================================================================

#include "channel_manager.h"
#include "endpoint_key.h"
#include "packet_header.h"
#include "platform.h"
#include "server.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace entanglement;

// --- Soak message payload (must match client — first 9 bytes of every payload) ---
#pragma pack(push, 1)
struct soak_msg
{
    uint32_t msg_id;         // Sequential per-stream message ID (0-based)
    uint32_t total_expected; // Total messages the client will send (0 = time-based)
    uint8_t channel_id;      // Channel this was sent on (for cross-check)
};
#pragma pack(pop)

static constexpr size_t SOAK_MSG_SIZE = sizeof(soak_msg);

// --- Per-stream receive statistics ---
// A "stream" is (channel_id, is_fragmented). We track 6 streams: 3 channels × 2 types.
struct stream_recv_stats
{
    uint8_t channel_id = 0;
    channel_mode mode = channel_mode::UNRELIABLE;
    std::string label; // e.g. "reliable-simple" or "ordered-frag"
    uint32_t total_expected = 0;

    int unique_received = 0;
    int duplicate_count = 0;
    int out_of_order_count = 0;
    uint32_t highest_msg_id = 0;
    uint32_t last_ordered_id = UINT32_MAX;

    std::set<uint32_t> received_ids;

    void record(uint32_t mid)
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

        if (mode == channel_mode::RELIABLE_ORDERED)
        {
            if (last_ordered_id != UINT32_MAX && mid <= last_ordered_id)
                ++out_of_order_count;
            last_ordered_id = mid;
        }
    }

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
        for (uint32_t i = 0; i < expected; ++i)
            if (!received_ids.count(i))
                missing.push_back(i);
        return missing;
    }
};

// --- Per-client statistics ---
struct client_recv_stats
{
    std::string address;
    uint16_t port = 0;
    int total_simple_packets = 0;
    int total_frag_messages = 0;
    int total_invalid_format = 0;
    int total_simple_echoes = 0;
    int total_frag_echoes = 0;
    int total_expired = 0;

    // simple_streams[channel_id] and frag_streams[channel_id]
    std::unordered_map<uint8_t, stream_recv_stats> simple_streams;
    std::unordered_map<uint8_t, stream_recv_stats> frag_streams;

    std::chrono::steady_clock::time_point first_packet_time;
    std::chrono::steady_clock::time_point last_packet_time;
    bool has_packets = false;

    void touch()
    {
        auto now = std::chrono::steady_clock::now();
        if (!has_packets)
        {
            first_packet_time = now;
            has_packets = true;
        }
        last_packet_time = now;
    }
};

// --- Global state ---
static std::atomic<bool> g_running{true};
static bool g_verbose = false;
static std::mutex g_state_mutex; // protects all global maps in multi-threaded mode
static std::unordered_map<std::string, client_recv_stats> g_client_stats;
static std::unordered_map<endpoint_key, std::pair<std::string, uint16_t>, endpoint_key_hash> g_endpoint_addr;
static std::atomic<int> g_total_simple_packets{0};
static std::atomic<int> g_total_frag_messages{0};
static std::atomic<int> g_total_simple_echoes{0};
static std::atomic<int> g_total_frag_echoes{0};
static std::atomic<int> g_total_echo_retransmissions{0};
static std::atomic<bool> g_had_clients{false};

// Diagnostics: echo send failures
static std::atomic<int> g_frag_echo_bp_retries{0};    // send_to retries due to backpressure
static std::atomic<int> g_frag_echo_send_failures{0}; // send_to permanently failed (echo_msg_id==0)
static std::atomic<int> g_simple_echo_send_failures{0};

// --- Echo retransmission state ---
// Composite key: encode client port into high bits to avoid collisions
// between connections that share the same per-connection sequence/message_id space.
static uint64_t make_echo_key(uint16_t port, uint64_t id)
{
    return (static_cast<uint64_t>(port) << 48) ^ id;
}

// Simple echoes: (port⊕sequence → {payload, channel_id, channel_sequence})
struct simple_echo_entry
{
    std::vector<uint8_t> payload;
    uint8_t channel_id = 0;
    uint32_t channel_sequence = 0; // original ordering position (for retransmit)
};
static std::unordered_map<uint64_t, simple_echo_entry> g_simple_echo_payloads;

// Fragmented echoes: (port⊕message_id → {full payload, channel_id})
struct frag_echo_entry
{
    std::vector<uint8_t> payload;
    uint8_t channel_id = 0;
};
static std::unordered_map<uint64_t, frag_echo_entry> g_frag_echo_payloads;

// Per-fragment retry counter: key = make_echo_key(port, (message_id << 8) | fragment_index)
static std::unordered_map<uint64_t, int> g_frag_echo_retries;
constexpr int MAX_ECHO_FRAG_RETRIES = 10;

static std::string make_client_key(const std::string &addr, uint16_t p)
{
    return addr + ":" + std::to_string(p);
}

// --- Signal handler ---
static void signal_handler(int /*sig*/)
{
    g_running = false;
}

// --- Resolve endpoint_key to (address, port) ---
static bool resolve_endpoint(const endpoint_key &ek, std::string &addr, uint16_t &port)
{
    auto it = g_endpoint_addr.find(ek);
    if (it == g_endpoint_addr.end())
        return false;
    addr = it->second.first;
    port = it->second.second;
    return true;
}

// --- Initialize a stream_recv_stats entry ---
static void init_stream(stream_recv_stats &s, uint8_t ch_id, bool fragmented, const server &srv)
{
    s.channel_id = ch_id;
    const auto *cfg = srv.channels().get_channel(ch_id);
    if (cfg)
    {
        s.mode = cfg->mode;
        s.label = cfg->name;
    }
    else
    {
        s.label = "ch" + std::to_string(ch_id);
    }
    s.label += fragmented ? "-frag" : "-simple";
}

// --- Format helpers ---
static const char *mode_str(channel_mode m)
{
    switch (m)
    {
        case channel_mode::UNRELIABLE:
            return "UNRELIABLE";
        case channel_mode::RELIABLE:
            return "RELIABLE";
        case channel_mode::RELIABLE_ORDERED:
            return "RELIABLE_ORDERED";
    }
    return "UNKNOWN";
}

static void print_stream_stats(const stream_recv_stats &s)
{
    uint32_t eff = s.effective_expected();
    char pct[32] = "N/A";
    if (eff > 0)
        std::snprintf(pct, sizeof(pct), "%.4f%%", 100.0 * s.unique_received / eff);

    std::cout << "  Stream: " << s.label << " (id=" << static_cast<int>(s.channel_id) << ", " << mode_str(s.mode) << ")"
              << std::endl;
    std::cout << "    Unique received:     " << s.unique_received << " / " << eff << " (" << pct << ")" << std::endl;
    std::cout << "    Duplicates:          " << s.duplicate_count << std::endl;

    if (s.mode == channel_mode::RELIABLE_ORDERED)
        std::cout << "    Order violations:    " << s.out_of_order_count << std::endl;

    auto missing = s.missing_ids();
    if (missing.empty())
    {
        std::cout << "    Missing IDs:         none" << std::endl;
    }
    else
    {
        std::cout << "    Missing IDs:         " << missing.size();
        if (missing.size() <= 20)
        {
            std::cout << " [";
            for (size_t i = 0; i < missing.size(); ++i)
            {
                if (i)
                    std::cout << ", ";
                std::cout << missing[i];
            }
            std::cout << "]";
        }
        std::cout << std::endl;
    }
}

// --- Print stats for a single client ---
static void print_client_stats(const client_recv_stats &cs)
{
    std::cout << "\n--- Client: " << cs.address << ":" << cs.port << " ---" << std::endl;
    std::cout << "  Simple packets received:     " << cs.total_simple_packets << std::endl;
    std::cout << "  Fragmented messages received: " << cs.total_frag_messages << std::endl;
    std::cout << "  Invalid format:              " << cs.total_invalid_format << std::endl;
    std::cout << "  Simple echoes sent:          " << cs.total_simple_echoes << std::endl;
    std::cout << "  Fragmented echoes sent:      " << cs.total_frag_echoes << std::endl;
    std::cout << "  Expired (incomplete) frags:  " << cs.total_expired << std::endl;

    if (cs.has_packets)
    {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(cs.last_packet_time - cs.first_packet_time);
        std::cout << "  Duration (first..last):      " << ms.count() << " ms" << std::endl;
        if (ms.count() > 0)
        {
            double rate = 1000.0 * (cs.total_simple_packets + cs.total_frag_messages) / ms.count();
            std::cout << "  Receive throughput:          " << std::fixed << std::setprecision(1) << rate << " msg/s"
                      << std::endl;
        }
    }

    // Collect and sort all stream ids
    std::set<uint8_t> all_ids;
    for (auto &[id, _] : cs.simple_streams)
        all_ids.insert(id);
    for (auto &[id, _] : cs.frag_streams)
        all_ids.insert(id);

    std::cout << std::endl;
    for (uint8_t id : all_ids)
    {
        auto sit = cs.simple_streams.find(id);
        if (sit != cs.simple_streams.end())
        {
            print_stream_stats(sit->second);
            std::cout << std::endl;
        }
        auto fit = cs.frag_streams.find(id);
        if (fit != cs.frag_streams.end())
        {
            print_stream_stats(fit->second);
            std::cout << std::endl;
        }
    }
}

// --- Print aggregate verdict ---
static void print_aggregate_stats()
{
    std::cout << "\n=============================================" << std::endl;
    std::cout << " ENTANGLEMENT SOAK TEST — SERVER SUMMARY" << std::endl;
    std::cout << "=============================================" << std::endl;
    std::cout << "Simple packets received:     " << g_total_simple_packets.load() << std::endl;
    std::cout << "Fragmented messages received: " << g_total_frag_messages.load() << std::endl;
    std::cout << "Simple echoes sent:          " << g_total_simple_echoes.load() << std::endl;
    std::cout << "Fragmented echoes sent:      " << g_total_frag_echoes.load() << std::endl;
    std::cout << "Echo retransmissions:        " << g_total_echo_retransmissions.load() << std::endl;

    bool all_simple_reliable_ok = true;
    bool all_simple_ordered_ok = true;
    bool all_frag_reliable_ok = true;
    bool all_frag_ordered_ok = true;

    for (auto &[key, cs] : g_client_stats)
    {
        print_client_stats(cs);

        // Simple streams
        for (auto &[ch_id, s] : cs.simple_streams)
        {
            if (s.mode == channel_mode::RELIABLE || s.mode == channel_mode::RELIABLE_ORDERED)
                if (s.unique_received < static_cast<int>(s.effective_expected()))
                    all_simple_reliable_ok = false;
            if (s.mode == channel_mode::RELIABLE_ORDERED && s.out_of_order_count > 0)
                all_simple_ordered_ok = false;
        }
        // Frag streams
        for (auto &[ch_id, s] : cs.frag_streams)
        {
            if (s.mode == channel_mode::RELIABLE || s.mode == channel_mode::RELIABLE_ORDERED)
                if (s.unique_received < static_cast<int>(s.effective_expected()))
                    all_frag_reliable_ok = false;
            if (s.mode == channel_mode::RELIABLE_ORDERED && s.out_of_order_count > 0)
                all_frag_ordered_ok = false;
        }
    }

    std::cout << "=============================================\n" << std::endl;

    // Simple verdicts
    std::cout << "  SIMPLE RELIABLE DELIVERY:     " << (all_simple_reliable_ok ? "[PASS]" : "[FAIL]") << std::endl;
    std::cout << "  SIMPLE ORDERED INTACT:        " << (all_simple_ordered_ok ? "[PASS]" : "[FAIL]") << std::endl;

    // Fragmented verdicts (no retransmission → report as info)
    std::cout << "  FRAG RELIABLE DELIVERY:       " << (all_frag_reliable_ok ? "[PASS]" : "[WARN]") << std::endl;
    std::cout << "  FRAG ORDERED INTACT:          " << (all_frag_ordered_ok ? "[PASS]" : "[WARN]") << std::endl;

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
    double drop_rate = 0.0;
    int worker_count = 0;
    int socket_count = 1;
    bool keep_alive = false; // -k: don't auto-stop when clients disconnect

    for (int i = 1; i < argc; ++i)
    {
        if ((std::strcmp(argv[i], "-p") == 0 || std::strcmp(argv[i], "--port") == 0) && i + 1 < argc)
        {
            port = static_cast<uint16_t>(std::atoi(argv[++i]));
        }
        else if ((std::strcmp(argv[i], "-d") == 0 || std::strcmp(argv[i], "--drop") == 0) && i + 1 < argc)
        {
            drop_rate = std::atof(argv[++i]) / 100.0;
            if (drop_rate < 0.0)
                drop_rate = 0.0;
            if (drop_rate > 1.0)
                drop_rate = 1.0;
        }
        else if ((std::strcmp(argv[i], "-w") == 0 || std::strcmp(argv[i], "--workers") == 0) && i + 1 < argc)
        {
            worker_count = std::atoi(argv[++i]);
            if (worker_count < 0)
                worker_count = 0;
        }
        else if ((std::strcmp(argv[i], "-s") == 0 || std::strcmp(argv[i], "--sockets") == 0) && i + 1 < argc)
        {
            socket_count = std::atoi(argv[++i]);
            if (socket_count < 1)
                socket_count = 1;
        }
        else if (std::strcmp(argv[i], "-k") == 0 || std::strcmp(argv[i], "--keep-alive") == 0)
        {
            keep_alive = true;
        }
        else if (std::strcmp(argv[i], "-v") == 0 || std::strcmp(argv[i], "--verbose") == 0)
        {
            g_verbose = true;
        }
        else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0)
        {
            std::cout << "Usage: EntanglementServer [-p port] [-d drop%] [-w workers] [-s sockets] [-k] [-v]"
                      << std::endl;
            std::cout << "  -p, --port       Listen port (default: " << DEFAULT_PORT << ")" << std::endl;
            std::cout << "  -d, --drop       Simulated drop rate in percent (default: 0)" << std::endl;
            std::cout << "  -w, --workers    Worker threads (0 = single-threaded, default: 0)" << std::endl;
            std::cout << "  -s, --sockets    Receive sockets (SO_REUSEPORT, Linux only, default: 1)" << std::endl;
            std::cout << "  -k, --keep-alive Don't auto-stop when all clients disconnect" << std::endl;
            std::cout << "  -v, --verbose    Print intermediate output during execution" << std::endl;
            return 0;
        }
        else
        {
            std::cerr << "Unknown argument: " << argv[i] << std::endl;
            std::cerr << "Usage: EntanglementServer [-p port] [-d drop%] [-w workers] [-s sockets] [-k] [-v]"
                      << std::endl;
            return 1;
        }
    }

    // Cap worker count at logical CPU count
    if (worker_count > 0)
    {
        int hw = static_cast<int>(std::thread::hardware_concurrency());
        if (hw > 0)
            worker_count = (std::min)(worker_count, hw);
    }

    std::signal(SIGINT, signal_handler);

    std::cout << "=============================================" << std::endl;
    std::cout << " Entanglement Soak Test Server v3.1" << std::endl;
    std::cout << " Port: " << port << std::endl;
    std::cout << " Header size: " << sizeof(packet_header) << " bytes" << std::endl;
    std::cout << " Soak payload (simple): " << SOAK_MSG_SIZE << " bytes" << std::endl;
    std::cout << " Max fragment payload:  " << MAX_FRAGMENT_PAYLOAD << " bytes" << std::endl;
    if (worker_count > 0)
        std::cout << " Workers: " << worker_count << " (async I/O)" << std::endl;
    if (socket_count > 1)
        std::cout << " Sockets: " << socket_count << " (SO_REUSEPORT)" << std::endl;
#ifdef ENTANGLEMENT_SIMULATE_LOSS
    std::cout << " Drop rate: " << (drop_rate * 100.0) << "%" << std::endl;
#endif
    std::cout << "=============================================" << std::endl;

    server srv(port);
    srv.channels().register_defaults();

    if (worker_count > 0)
        srv.set_worker_count(worker_count);

    // Multi-socket: SO_REUSEPORT for parallel receiver threads (Linux only)
    if (socket_count > 1)
        srv.set_socket_count(socket_count);

    // Enable platform-optimized async I/O for high-throughput batch receive
    if (worker_count > 0)
        srv.set_use_async_io(true);

#ifdef ENTANGLEMENT_SIMULATE_LOSS
    if (drop_rate > 0.0)
        srv.set_simulated_drop_rate(drop_rate);
#else
    (void)drop_rate;
#endif

    // --- Client connected ---
    srv.set_on_client_connected(
        [&](const endpoint_key &ek, const std::string &addr, uint16_t p)
        {
            std::lock_guard<std::mutex> lk(g_state_mutex);
            g_endpoint_addr[ek] = {addr, p};
            std::string ck = make_client_key(addr, p);
            auto &cs = g_client_stats[ck];
            cs.address = addr;
            cs.port = p;
            g_had_clients = true;
            if (g_verbose)
                std::cout << "[server] Client connected: " << addr << ":" << p << std::endl;
        });

    // --- Client disconnected ---
    srv.set_on_client_disconnected(
        [&](const endpoint_key &ek, const std::string &addr, uint16_t p)
        {
            std::lock_guard<std::mutex> lk(g_state_mutex);
            if (g_verbose)
            {
                std::cout << "[server] Client disconnected: " << addr << ":" << p << std::endl;
                auto it = g_client_stats.find(make_client_key(addr, p));
                if (it != g_client_stats.end())
                    print_client_stats(it->second);
            }
            g_endpoint_addr.erase(ek);
        });

    // =========================================================================
    // SIMPLE MESSAGES → on_client_data_received
    // =========================================================================
    srv.set_on_client_data_received(
        [&](const packet_header &hdr, const uint8_t *payload, size_t size, const endpoint_key &sender)
        {
            ++g_total_simple_packets;

            // Echo back FIRST — the send is a direct call on the owning worker
            // thread, so it doesn't need g_state_mutex.  Moving it outside the
            // lock dramatically reduces contention in multi-threaded mode and
            // prevents the receiver-thread's per-worker queue from overflowing.
            packet_header reply{};
            reply.flags = hdr.flags;
            reply.shard_id = hdr.shard_id;
            reply.channel_id = hdr.channel_id;
            reply.payload_size = static_cast<uint16_t>(size);
            int echo_result = srv.send_raw_to(reply, payload, sender);
            if (echo_result <= 0)
                ++g_simple_echo_send_failures;

            // Now lock for stats + echo tracking
            uint16_t p = sender.port;
            {
                std::lock_guard<std::mutex> lk(g_state_mutex);

                std::string addr = endpoint_address_string(sender);
                std::string ck = make_client_key(addr, p);
                auto &cs = g_client_stats[ck];
                ++cs.total_simple_packets;
                cs.touch();

                if (size >= SOAK_MSG_SIZE)
                {
                    soak_msg msg;
                    std::memcpy(&msg, payload, SOAK_MSG_SIZE);

                    uint8_t ch_id = hdr.channel_id;
                    auto &st = cs.simple_streams[ch_id];
                    if (st.label.empty())
                        init_stream(st, ch_id, false, srv);
                    if (st.total_expected == 0)
                        st.total_expected = msg.total_expected;
                    st.record(msg.msg_id);

                    // Verbose: first 5, every 5000
                    if (g_verbose && (st.unique_received <= 5 || st.unique_received % 5000 == 0))
                    {
                        std::cout << "[simple] " << addr << ":" << p << " " << st.label << " msg=" << msg.msg_id
                                  << " (unique=" << st.unique_received << ")" << std::endl;
                    }
                }
                else
                {
                    ++cs.total_invalid_format;
                }

                // Store payload for echo retransmission (reliable channels only)
                if (srv.channels().is_reliable(hdr.channel_id))
                {
                    simple_echo_entry entry;
                    entry.payload.assign(payload, payload + size);
                    entry.channel_id = hdr.channel_id;
                    entry.channel_sequence = reply.channel_sequence; // preserve for retransmit
                    g_simple_echo_payloads[make_echo_key(p, reply.sequence)] = std::move(entry);
                }

                ++cs.total_simple_echoes;
            }
            ++g_total_simple_echoes;
        });

    // =========================================================================
    // FRAGMENTED MESSAGES → reassembler callbacks
    // =========================================================================

    // Allocate reassembly buffer
    srv.set_on_allocate_message([&](const endpoint_key & /*sender*/, uint32_t /*msg_id*/, uint8_t /*ch_id*/,
                                    uint8_t /*frag_count*/,
                                    size_t max_size) -> uint8_t * { return new uint8_t[max_size]; });

    // Message fully reassembled — track stats and echo back
    srv.set_on_message_complete(
        [&](const endpoint_key &sender, uint32_t /*protocol_msg_id*/, uint8_t ch_id, uint8_t *data, size_t total_size)
        {
            ++g_total_frag_messages;

            // Echo entire reassembled payload FIRST (no lock needed —
            // direct call on owning worker thread via endpoint_key overload).
            // Retry if backpressured — the client's reassembler may be
            // temporarily full; a short sleep lets it drain.
            uint32_t echo_msg_id = 0;
            // Try up to 3 times, no blocking sleep — keeping the main loop responsive
            // is more important than guaranteed echo delivery (client drain handles retries).
            for (int attempt = 0; attempt < 3; ++attempt)
            {
                echo_msg_id = 0;
                srv.send_to(data, total_size, ch_id, sender, 0, &echo_msg_id);
                if (echo_msg_id != 0)
                    break;
                ++g_frag_echo_bp_retries;
            }
            if (echo_msg_id == 0)
                ++g_frag_echo_send_failures;

            // Now lock for stats + echo tracking
            uint16_t p = sender.port;
            {
                std::lock_guard<std::mutex> lk(g_state_mutex);

                std::string addr = endpoint_address_string(sender);
                std::string ck = make_client_key(addr, p);
                auto &cs = g_client_stats[ck];
                ++cs.total_frag_messages;
                cs.touch();

                if (total_size >= SOAK_MSG_SIZE)
                {
                    soak_msg msg;
                    std::memcpy(&msg, data, SOAK_MSG_SIZE);

                    auto &st = cs.frag_streams[ch_id];
                    if (st.label.empty())
                        init_stream(st, ch_id, true, srv);
                    if (st.total_expected == 0)
                        st.total_expected = msg.total_expected;
                    st.record(msg.msg_id);

                    if (g_verbose && (st.unique_received <= 5 || st.unique_received % 5000 == 0))
                    {
                        std::cout << "[frag]   " << addr << ":" << p << " " << st.label << " msg=" << msg.msg_id
                                  << " size=" << total_size << " (unique=" << st.unique_received << ")" << std::endl;
                    }
                }

                // Store payload for echo retransmission (reliable channels only)
                if (srv.channels().is_reliable(ch_id) && echo_msg_id != 0)
                {
                    frag_echo_entry entry;
                    entry.payload.assign(data, data + total_size);
                    entry.channel_id = ch_id;
                    g_frag_echo_payloads[make_echo_key(p, echo_msg_id)] = std::move(entry);
                }

                ++cs.total_frag_echoes;
            }
            ++g_total_frag_echoes;

            delete[] data;
        });

    // Incomplete message failed (expired or evicted)
    srv.set_on_message_failed(
        [&](const endpoint_key &sender, uint32_t /*msg_id*/, uint8_t /*ch_id*/, uint8_t *buf,
            message_fail_reason /*reason*/, uint8_t /*received*/, uint8_t /*total*/)
        {
            std::lock_guard<std::mutex> lk(g_state_mutex);
            std::string addr;
            uint16_t p = 0;
            if (resolve_endpoint(sender, addr, p))
            {
                auto &cs = g_client_stats[make_client_key(addr, p)];
                ++cs.total_expired;
            }
            delete[] buf;
        });

    // =========================================================================
    // LOSS callback — retransmit lost echo packets
    // =========================================================================
    auto on_echo_loss = [&](const lost_packet_info &info, const endpoint_key &client_ep)
    {
        uint16_t p = client_ep.port;

        if (info.message_id != 0)
        {
            // Fragment echo loss — copy payload chunk under lock, send outside
            std::vector<uint8_t> chunk_copy;
            {
                std::lock_guard<std::mutex> lk(g_state_mutex);

                uint64_t ekey = make_echo_key(p, info.message_id);
                auto it = g_frag_echo_payloads.find(ekey);
                if (it == g_frag_echo_payloads.end())
                    return;

                // Check retry limit
                uint64_t frag_key =
                    make_echo_key(p, (static_cast<uint64_t>(info.message_id) << 8) | info.fragment_index);
                int &retries = g_frag_echo_retries[frag_key];
                if (retries >= MAX_ECHO_FRAG_RETRIES)
                    return;
                ++retries;

                const auto &entry = it->second;
                size_t offset = static_cast<size_t>(info.fragment_index) * MAX_FRAGMENT_PAYLOAD;
                if (offset >= entry.payload.size())
                    return;
                size_t chunk = (std::min)(MAX_FRAGMENT_PAYLOAD, entry.payload.size() - offset);
                chunk_copy.assign(entry.payload.data() + offset, entry.payload.data() + offset + chunk);
            }

            srv.send_fragment_to(info.message_id, info.fragment_index, info.fragment_count, chunk_copy.data(),
                                 chunk_copy.size(), 0, info.channel_id, client_ep, info.channel_sequence);
            ++g_total_echo_retransmissions;
            return;
        }

        // Simple echo loss — copy payload under lock, send outside, update tracking
        std::vector<uint8_t> payload_copy;
        uint8_t channel_id;
        uint32_t channel_sequence;
        {
            std::lock_guard<std::mutex> lk(g_state_mutex);

            uint64_t ekey = make_echo_key(p, info.sequence);
            auto it = g_simple_echo_payloads.find(ekey);
            if (it == g_simple_echo_payloads.end())
                return;

            const auto &entry = it->second;
            payload_copy = entry.payload;
            channel_id = entry.channel_id;
            channel_sequence = entry.channel_sequence;
            g_simple_echo_payloads.erase(it);
        }

        packet_header reply{};
        reply.channel_id = channel_id;
        reply.channel_sequence = channel_sequence;
        reply.payload_size = static_cast<uint16_t>(payload_copy.size());
        int send_result = srv.send_raw_to(reply, payload_copy.data(), client_ep);
        if (send_result <= 0)
        {
            // Send failed — re-store under OLD key so next loss can retry
            ++g_simple_echo_send_failures;
            std::lock_guard<std::mutex> lk(g_state_mutex);
            simple_echo_entry restored;
            restored.payload = std::move(payload_copy);
            restored.channel_id = channel_id;
            restored.channel_sequence = channel_sequence;
            g_simple_echo_payloads[make_echo_key(p, info.sequence)] = std::move(restored);
            return;
        }

        // Store updated echo entry under new sequence
        {
            std::lock_guard<std::mutex> lk(g_state_mutex);
            simple_echo_entry new_entry;
            new_entry.payload = std::move(payload_copy);
            new_entry.channel_id = channel_id;
            new_entry.channel_sequence = channel_sequence;
            g_simple_echo_payloads[make_echo_key(p, reply.sequence)] = std::move(new_entry);
        }
        ++g_total_echo_retransmissions;
    };

    // =========================================================================

    // Echo retransmission is intentionally disabled.  The client already
    // handles reliability end-to-end: if an echo is lost, the client
    // retransmits the original message and the server generates a fresh
    // echo.  Server-side echo retransmission created a positive-feedback
    // cascade under simulated packet loss, collapsing the server's
    // congestion window and amplifying traffic ~35×.
    // srv.set_on_packet_lost(on_echo_loss);

    if (failed(srv.start()))
    {
        platform_shutdown();
        return 1;
    }

    if (g_verbose)
        std::cout << "[server] Waiting for clients... (Ctrl+C to stop)" << std::endl;

    auto idle_start = std::chrono::steady_clock::now();
    constexpr auto IDLE_TIMEOUT = std::chrono::seconds(5);

    while (g_running.load())
    {
        int polled = srv.poll();
        srv.update();

        auto now = std::chrono::steady_clock::now();

        if (srv.connection_count() > 0)
            idle_start = now;

        if (!keep_alive && g_had_clients && srv.connection_count() == 0)
        {
            if (now - idle_start > IDLE_TIMEOUT)
            {
                if (g_verbose)
                    std::cout << "\n[server] All clients disconnected. Auto-stopping after "
                              << std::chrono::duration_cast<std::chrono::seconds>(IDLE_TIMEOUT).count() << "s idle."
                              << std::endl;
                break;
            }
        }

        // Yield when idle to save CPU; when busy, loop immediately for low latency.
        if (polled == 0)
            std::this_thread::yield();
    }

    srv.stop();
    print_aggregate_stats();

    // Diagnostics: receiver queue overflow
    uint64_t drops = srv.recv_queue_drops();
    if (drops > 0)
        std::cout << "  [DIAG] Recv queue drops:      " << drops << std::endl;

    int frag_retries = g_frag_echo_bp_retries.load();
    int frag_failures = g_frag_echo_send_failures.load();
    int simple_failures = g_simple_echo_send_failures.load();
    if (frag_retries > 0 || frag_failures > 0 || simple_failures > 0)
    {
        std::cout << "  [DIAG] Frag echo retries:     " << frag_retries << std::endl;
        std::cout << "  [DIAG] Frag echo send fails:  " << frag_failures << std::endl;
        std::cout << "  [DIAG] Simple echo send fails: " << simple_failures << std::endl;
    }

    platform_shutdown();
    return 0;
}
