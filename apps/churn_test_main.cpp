// ============================================================================
// Entanglement — Churn Soak Test v1.0
// ============================================================================
// Stress-tests the connection lifecycle by continuously creating clients
// that connect, exchange messages (simple + fragmented), disconnect, and
// reconnect. This exercises:
//   - Pool allocation / deallocation churn
//   - Slot reuse after disconnect
//   - Ordered delivery state reset across reconnections
//   - Server callback correctness under rapid connect/disconnect
//   - Resource cleanup (reassembler, ordered buffers)
//
// Usage:
//   EntanglementChurnTest [-t minutes] [-c concurrent] [-r rounds]
//                         [-d drop%] [-p port]
//
//   -t minutes     Total test duration (default: 5)
//   -c concurrent  Max concurrent clients (default: 8)
//   -r rounds      Disconnect/reconnect cycles per client (default: 0 = unlimited)
//   -d drop%       Simulated packet loss 0-100 (default: 0)
//   -p port        Server port (default: 9990)
//
// The test spawns a server + N client threads. Each client thread loops:
//   1. connect()
//   2. send M reliable + ordered messages (simple + fragmented)
//   3. wait for echoes
//   4. disconnect()
//   5. brief pause, then goto 1
//
// At the end, the test prints per-client stats and a PASS/FAIL verdict.
// ============================================================================

#include "channel_manager.h"
#include "client.h"
#include "endpoint_key.h"
#include "packet_header.h"
#include "platform.h"
#include "server.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace entanglement;

// --- Configuration ---
struct churn_config
{
    int duration_minutes = 5;
    int concurrent_clients = 8;
    int max_rounds = 0; // 0 = unlimited (time-based)
    double drop_rate = 0.0;
    uint16_t port = 9990;
    int workers = 0; // 0 = single-threaded, >0 = multi-threaded
};

// --- Per-client result ---
struct client_stats
{
    int id = 0;
    int rounds_completed = 0;
    int connect_failures = 0;
    int total_simple_sent = 0;
    int total_simple_echoes = 0;
    int total_frag_sent = 0;
    int total_frag_echoes = 0;
    int total_losses = 0;
    int total_retransmissions = 0;
    int order_violations = 0;
    long long elapsed_ms = 0;
    bool clean_exit = true;
};

// --- Server-side stats ---
struct server_stats
{
    std::atomic<int> total_connects{0};
    std::atomic<int> total_disconnects{0};
    std::atomic<int> total_data_packets{0};
    std::atomic<int> total_frag_messages{0};
    std::atomic<int> peak_connections{0};
};

static std::mutex g_cout_mutex;

// --- Soak message payload (simple: 9 bytes) ---
#pragma pack(push, 1)
struct churn_msg
{
    uint32_t msg_id;
    uint32_t round_id;
    uint8_t channel_id;
};
#pragma pack(pop)
static constexpr size_t CHURN_MSG_SIZE = sizeof(churn_msg);
static constexpr size_t FRAG_PAYLOAD_SIZE = 2500; // triggers fragmentation
static constexpr int MSGS_PER_ROUND = 10;         // simple messages per round per channel
static constexpr int FRAG_PER_ROUND = 3;          // fragmented messages per round

// ============================================================================
// Client thread: connect → send → wait echoes → disconnect → repeat
// ============================================================================
static client_stats run_churn_client(int id, const churn_config &cfg, std::atomic<bool> &global_stop)
{
    client_stats stats;
    stats.id = id;
    auto t0 = std::chrono::steady_clock::now();
    auto end_time = t0 + std::chrono::minutes(cfg.duration_minutes);

    int round = 0;

    while (!global_stop.load())
    {
        // Check time / round limit
        if (std::chrono::steady_clock::now() >= end_time)
            break;
        if (cfg.max_rounds > 0 && round >= cfg.max_rounds)
            break;

        // --- CREATE + CONNECT ---
        client c("127.0.0.1", cfg.port);
        c.set_verbose(false);
        c.channels().register_defaults();

#ifdef ENTANGLEMENT_SIMULATE_LOSS
        if (cfg.drop_rate > 0.0)
            c.set_simulated_drop_rate(cfg.drop_rate / 100.0);
#endif

        // Track echoes
        std::atomic<int> simple_echoes{0};
        std::atomic<int> frag_echoes{0};
        std::atomic<int> order_violations_round{0};
        uint32_t last_ordered_echo = UINT32_MAX;

        c.set_on_data_received(
            [&](const packet_header &hdr, const uint8_t *payload, size_t size)
            {
                if (size >= CHURN_MSG_SIZE)
                {
                    churn_msg m;
                    std::memcpy(&m, payload, CHURN_MSG_SIZE);
                    simple_echoes++;

                    if (hdr.channel_id == channels::ORDERED.id)
                    {
                        if (last_ordered_echo != UINT32_MAX && m.msg_id <= last_ordered_echo)
                            order_violations_round++;
                        last_ordered_echo = m.msg_id;
                    }
                }
            });

        c.set_on_allocate_message([](const endpoint_key &, uint32_t, uint8_t, uint8_t, size_t max_size) -> uint8_t *
                                  { return new uint8_t[max_size]; });

        c.set_on_message_complete(
            [&](const endpoint_key &, uint32_t, uint8_t, uint8_t *data, size_t total_size)
            {
                frag_echoes++;
                delete[] data;
            });

        c.set_on_message_failed([](const endpoint_key &, uint32_t, uint8_t, uint8_t *buf, message_fail_reason, uint8_t,
                                   uint8_t) { delete[] buf; });

        error_code ec = c.connect();
        if (failed(ec))
        {
            stats.connect_failures++;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        // --- SEND PHASE ---
        int simple_sent = 0;
        int frag_sent = 0;
        int losses = 0;
        int retransmissions = 0;

        // Store sent payloads for retransmission
        struct sent_info
        {
            uint16_t sequence;
            std::vector<uint8_t> payload;
            uint8_t channel_id;
        };
        std::vector<sent_info> retransmit_store;

        // Set up loss callback for retransmissions
        c.set_on_packet_lost(
            [&](const lost_packet_info &info)
            {
                losses++;
                // Find and retransmit
                for (auto &si : retransmit_store)
                {
                    if (si.sequence == info.sequence)
                    {
                        if (c.is_connected())
                        {
                            c.send(si.payload.data(), si.payload.size(), si.channel_id);
                            retransmissions++;
                        }
                        break;
                    }
                }
            });

        // Send simple messages on the ORDERED channel
        for (int i = 0; i < MSGS_PER_ROUND && !global_stop.load(); ++i)
        {
            churn_msg m;
            m.msg_id = static_cast<uint32_t>(i);
            m.round_id = static_cast<uint32_t>(round);
            m.channel_id = channels::ORDERED.id;

            int r = c.send(&m, CHURN_MSG_SIZE, channels::ORDERED.id);
            if (r > 0)
                simple_sent++;

            // Pump briefly
            c.poll();
            c.update(nullptr);
        }

        // Send fragmented messages on the RELIABLE channel
        for (int i = 0; i < FRAG_PER_ROUND && !global_stop.load(); ++i)
        {
            std::vector<uint8_t> buf(FRAG_PAYLOAD_SIZE, 0xCC);
            churn_msg m;
            m.msg_id = static_cast<uint32_t>(i);
            m.round_id = static_cast<uint32_t>(round);
            m.channel_id = channels::RELIABLE.id;
            std::memcpy(buf.data(), &m, CHURN_MSG_SIZE);

            int r = c.send(buf.data(), buf.size(), channels::RELIABLE.id);
            if (r > 0)
                frag_sent++;

            c.poll();
            c.update(nullptr);
        }

        // --- DRAIN PHASE: wait for echoes (up to 3 seconds) ---
        int expected_simple = simple_sent;
        int expected_frag = frag_sent;
        auto drain_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);

        while (std::chrono::steady_clock::now() < drain_deadline && !global_stop.load())
        {
            c.poll();
            c.update(nullptr);

            if (simple_echoes.load() >= expected_simple && frag_echoes.load() >= expected_frag)
                break;

            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        // --- DISCONNECT ---
        c.disconnect();

        // Accumulate stats
        stats.total_simple_sent += simple_sent;
        stats.total_simple_echoes += simple_echoes.load();
        stats.total_frag_sent += frag_sent;
        stats.total_frag_echoes += frag_echoes.load();
        stats.total_losses += losses;
        stats.total_retransmissions += retransmissions;
        stats.order_violations += order_violations_round.load();
        stats.rounds_completed++;

        // Brief pause between rounds
        std::this_thread::sleep_for(std::chrono::milliseconds(50 + (id * 7) % 50));

        round++;
    }

    auto tend = std::chrono::steady_clock::now();
    stats.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(tend - t0).count();

    return stats;
}

// ============================================================================
// Parse CLI
// ============================================================================
static churn_config parse_args(int argc, char *argv[])
{
    churn_config cfg;
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "-t" && i + 1 < argc)
            cfg.duration_minutes = std::atoi(argv[++i]);
        else if (arg == "-c" && i + 1 < argc)
            cfg.concurrent_clients = std::atoi(argv[++i]);
        else if (arg == "-r" && i + 1 < argc)
            cfg.max_rounds = std::atoi(argv[++i]);
        else if (arg == "-d" && i + 1 < argc)
            cfg.drop_rate = std::atof(argv[++i]);
        else if (arg == "-p" && i + 1 < argc)
            cfg.port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (arg == "-w" && i + 1 < argc)
        {
            cfg.workers = std::atoi(argv[++i]);
            if (cfg.workers < 0)
                cfg.workers = 0;
        }
    }

    // Cap worker count at logical CPU count
    if (cfg.workers > 0)
    {
        int hw = static_cast<int>(std::thread::hardware_concurrency());
        if (hw > 0)
            cfg.workers = (std::min)(cfg.workers, hw);
    }
    return cfg;
}

// ============================================================================
// Main
// ============================================================================
int main(int argc, char *argv[])
{
    platform_init();

    churn_config cfg = parse_args(argc, argv);

    std::cout << "========================================" << std::endl;
    std::cout << " Entanglement Churn Soak Test v1.0" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << " Duration:    " << cfg.duration_minutes << " min" << std::endl;
    std::cout << " Clients:     " << cfg.concurrent_clients << std::endl;
    std::cout << " Max rounds:  " << (cfg.max_rounds > 0 ? std::to_string(cfg.max_rounds) : "unlimited") << std::endl;
    std::cout << " Drop rate:   " << cfg.drop_rate << "%" << std::endl;
    std::cout << " Port:        " << cfg.port << std::endl;
    std::cout << " Workers:     " << (cfg.workers > 0 ? std::to_string(cfg.workers) + " threads" : "single-threaded")
              << std::endl;
    std::cout << "========================================" << std::endl;

    // --- Start server ---
    server srv(cfg.port);
    srv.set_verbose(false);
    srv.channels().register_defaults();

    if (cfg.workers > 0)
        srv.set_worker_count(cfg.workers);

    server_stats srv_stats;

    srv.set_on_client_connected(
        [&](const endpoint_key &, const std::string &, uint16_t)
        {
            srv_stats.total_connects++;
            int current = static_cast<int>(srv.connection_count());
            int peak = srv_stats.peak_connections.load();
            while (current > peak && !srv_stats.peak_connections.compare_exchange_weak(peak, current))
                ;
        });

    srv.set_on_client_disconnected([&](const endpoint_key &, const std::string &, uint16_t)
                                   { srv_stats.total_disconnects++; });

    // Echo simple packets back
    srv.set_on_client_data_received(
        [&](const packet_header &header, const uint8_t *payload, size_t payload_size, const endpoint_key &sender)
        {
            srv_stats.total_data_packets++;
            srv.send_to(payload, payload_size, header.channel_id, sender, header.flags);
        });

    // Echo fragmented messages back
    srv.set_on_allocate_message([](const endpoint_key &, uint32_t, uint8_t, uint8_t, size_t max_size) -> uint8_t *
                                { return new uint8_t[max_size]; });

    // Store connected client info for echo routing
    std::mutex echo_mutex;
    std::unordered_map<endpoint_key, std::pair<std::string, uint16_t>, endpoint_key_hash> echo_routes;

    srv.set_on_client_connected(
        [&](const endpoint_key &key, const std::string &addr, uint16_t port)
        {
            srv_stats.total_connects++;
            int current = static_cast<int>(srv.connection_count());
            int peak = srv_stats.peak_connections.load();
            while (current > peak && !srv_stats.peak_connections.compare_exchange_weak(peak, current))
                ;
            std::lock_guard<std::mutex> lk(echo_mutex);
            echo_routes[key] = {addr, port};
        });

    srv.set_on_client_disconnected(
        [&](const endpoint_key &key, const std::string &, uint16_t)
        {
            srv_stats.total_disconnects++;
            std::lock_guard<std::mutex> lk(echo_mutex);
            echo_routes.erase(key);
        });

    srv.set_on_message_complete(
        [&](const endpoint_key &key, uint32_t, uint8_t ch_id, uint8_t *data, size_t total)
        {
            srv_stats.total_frag_messages++;
            std::lock_guard<std::mutex> lk(echo_mutex);
            auto it = echo_routes.find(key);
            if (it != echo_routes.end())
            {
                srv.send_to(data, total, ch_id, it->second.first, it->second.second);
            }
            delete[] data;
        });

    srv.set_on_message_failed([](const endpoint_key &, uint32_t, uint8_t, uint8_t *buf, message_fail_reason, uint8_t,
                                 uint8_t) { delete[] buf; });

#ifdef ENTANGLEMENT_SIMULATE_LOSS
    if (cfg.drop_rate > 0.0)
        srv.set_simulated_drop_rate(cfg.drop_rate / 100.0);
#endif

    if (failed(srv.start()))
    {
        std::cerr << "FATAL: Server failed to start on port " << cfg.port << std::endl;
        platform_shutdown();
        return 1;
    }

    // Server loop thread
    std::atomic<bool> server_stop{false};
    std::thread server_thread(
        [&]()
        {
            while (!server_stop.load())
            {
                srv.poll();
                srv.update();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });

    // --- Launch client threads ---
    std::atomic<bool> global_stop{false};
    std::vector<std::thread> client_threads;
    std::vector<client_stats> results(cfg.concurrent_clients);

    auto test_start = std::chrono::steady_clock::now();

    for (int i = 0; i < cfg.concurrent_clients; ++i)
    {
        client_threads.emplace_back([&, i]() { results[i] = run_churn_client(i, cfg, global_stop); });
    }

    // Wait for all clients to finish
    for (auto &t : client_threads)
        t.join();

    auto test_end = std::chrono::steady_clock::now();
    auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(test_end - test_start).count();

    // Stop server
    server_stop = true;
    server_thread.join();
    srv.stop();

    // --- Print results ---
    std::cout << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << " CHURN TEST RESULTS" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << " Total time:           " << total_ms / 1000 << "." << (total_ms % 1000) / 100 << " s" << std::endl;
    std::cout << " Server connects:      " << srv_stats.total_connects.load() << std::endl;
    std::cout << " Server disconnects:   " << srv_stats.total_disconnects.load() << std::endl;
    std::cout << " Server data packets:  " << srv_stats.total_data_packets.load() << std::endl;
    std::cout << " Server frag messages: " << srv_stats.total_frag_messages.load() << std::endl;
    std::cout << " Peak connections:     " << srv_stats.peak_connections.load() << std::endl;
    std::cout << "========================================" << std::endl;

    int total_rounds = 0;
    int total_connect_failures = 0;
    int total_simple_sent = 0;
    int total_simple_echoes = 0;
    int total_frag_sent = 0;
    int total_frag_echoes = 0;
    int total_order_violations = 0;
    bool any_failure = false;

    std::cout << std::endl;
    std::cout << " Per-client breakdown:" << std::endl;
    std::cout << " " << std::setw(4) << "ID" << std::setw(8) << "Rounds" << std::setw(10) << "ConnFail" << std::setw(10)
              << "SimpSent" << std::setw(10) << "SimpEcho" << std::setw(10) << "FragSent" << std::setw(10) << "FragEcho"
              << std::setw(10) << "OrdViol" << std::setw(10) << "Losses" << std::endl;

    for (int i = 0; i < cfg.concurrent_clients; ++i)
    {
        auto &r = results[i];
        std::cout << " " << std::setw(4) << r.id << std::setw(8) << r.rounds_completed << std::setw(10)
                  << r.connect_failures << std::setw(10) << r.total_simple_sent << std::setw(10)
                  << r.total_simple_echoes << std::setw(10) << r.total_frag_sent << std::setw(10) << r.total_frag_echoes
                  << std::setw(10) << r.order_violations << std::setw(10) << r.total_losses << std::endl;

        total_rounds += r.rounds_completed;
        total_connect_failures += r.connect_failures;
        total_simple_sent += r.total_simple_sent;
        total_simple_echoes += r.total_simple_echoes;
        total_frag_sent += r.total_frag_sent;
        total_frag_echoes += r.total_frag_echoes;
        total_order_violations += r.order_violations;
    }

    std::cout << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << " TOTALS" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << " Total rounds:           " << total_rounds << std::endl;
    std::cout << " Connect failures:       " << total_connect_failures << std::endl;
    std::cout << " Simple sent/echoes:     " << total_simple_sent << " / " << total_simple_echoes << std::endl;
    std::cout << " Frag sent/echoes:       " << total_frag_sent << " / " << total_frag_echoes << std::endl;
    std::cout << " Order violations:       " << total_order_violations << std::endl;
    std::cout << "========================================" << std::endl;

    // --- VERDICT ---
    bool pass = true;
    std::vector<std::string> failures;

    if (total_rounds == 0)
    {
        pass = false;
        failures.push_back("No rounds completed");
    }
    if (total_connect_failures > total_rounds / 4)
    {
        pass = false;
        failures.push_back("Too many connect failures: " + std::to_string(total_connect_failures));
    }
    if (total_order_violations > 0)
    {
        pass = false;
        failures.push_back("Order violations detected: " + std::to_string(total_order_violations));
    }
    // Without loss simulation, we expect all echoes; with loss, allow some missing
    if (cfg.drop_rate == 0.0)
    {
        if (total_simple_echoes < total_simple_sent)
        {
            pass = false;
            failures.push_back("Missing simple echoes: " + std::to_string(total_simple_sent - total_simple_echoes) +
                               " / " + std::to_string(total_simple_sent));
        }
    }
    else
    {
        // With loss, at least 50% of echoes should arrive (3s drain per round)
        if (total_simple_sent > 0 && total_simple_echoes < total_simple_sent / 2)
        {
            pass = false;
            failures.push_back("Too few simple echoes under loss: " + std::to_string(total_simple_echoes) + " / " +
                               std::to_string(total_simple_sent));
        }
    }

    std::cout << std::endl;
    if (pass)
    {
        std::cout << " >>> VERDICT: ALL PASS <<<" << std::endl;
    }
    else
    {
        std::cout << " >>> VERDICT: FAIL <<<" << std::endl;
        for (auto &f : failures)
            std::cout << "   - " << f << std::endl;
    }
    std::cout << "========================================" << std::endl;

    platform_shutdown();
    return pass ? 0 : 1;
}
