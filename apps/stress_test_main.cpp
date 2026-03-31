// ============================================================================
// Entanglement — Stress Test  (N virtual clients in a single process)
// ============================================================================
//
// Usage: EntanglementStress [-s ip] [-p port] [-c clients] [-t seconds]
//                           [-r rate_hz] [-w workers] [-v]
//
// Spawns N lightweight virtual clients inside one process. Each client:
//   1. Connects to the server
//   2. Opens RELIABLE channel
//   3. Sends a small heartbeat-sized packet at 'rate_hz' per second
//   4. Polls for echoes
//   5. Disconnects
//
// Reports:
//   - Connection success/failure counts
//   - Aggregate throughput (total pkts sent / elapsed)
//   - Per-second avg RTT across all connections
//   - Delivery rate (echoes received / sent)
//   - Server-side recv queue drops (if any printed by server)
//
// This is NOT a bandwidth test — it measures how many concurrent connections
// the server can handle at a given tick rate with minimal per-client load.
// ============================================================================

#define NOMINMAX
#include "client.h"
#include "constants.h"
#include "platform.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace entanglement;

static std::atomic<bool> g_running{true};

static void signal_handler(int)
{
    g_running = false;
}

// --- Per-client lightweight state ---
struct virtual_client
{
    std::unique_ptr<client> cli;
    bool connected = false;
    uint32_t sent = 0;
    uint32_t echoes = 0;
    uint32_t losses = 0;
    int channel_id = -1;
};

// --- Small payload (just an ID + counter) ---
#pragma pack(push, 1)
struct stress_msg
{
    uint32_t client_index;
    uint32_t sequence;
};
#pragma pack(pop)

int main(int argc, char *argv[])
{
    std::string server_addr = "127.0.0.1";
    uint16_t port = 9876;
    int num_clients = 100;
    int duration_s = 30;
    int rate_hz = 10;       // sends per second per client
    int server_workers = 4; // hint: print in banner
    bool verbose = false;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "-s" && i + 1 < argc)
            server_addr = argv[++i];
        else if (arg == "-p" && i + 1 < argc)
            port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (arg == "-c" && i + 1 < argc)
            num_clients = std::atoi(argv[++i]);
        else if (arg == "-t" && i + 1 < argc)
            duration_s = std::atoi(argv[++i]);
        else if (arg == "-r" && i + 1 < argc)
            rate_hz = std::atoi(argv[++i]);
        else if (arg == "-w" && i + 1 < argc)
            server_workers = std::atoi(argv[++i]);
        else if (arg == "-v")
            verbose = true;
    }

    platform_init();
    std::signal(SIGINT, signal_handler);

    std::cout << "=============================================" << std::endl;
    std::cout << " Entanglement Stress Test" << std::endl;
    std::cout << " Server:   " << server_addr << ":" << port << std::endl;
    std::cout << " Clients:  " << num_clients << std::endl;
    std::cout << " Duration: " << duration_s << "s" << std::endl;
    std::cout << " Rate:     " << rate_hz << " Hz per client" << std::endl;
    std::cout << " Target:   " << (num_clients * rate_hz) << " pkt/s total" << std::endl;
    std::cout << "=============================================" << std::endl;

    // --- Phase 1: Connect all clients (batched to avoid overwhelming) ---
    std::vector<virtual_client> clients(static_cast<size_t>(num_clients));

    int connect_ok = 0;
    int connect_fail = 0;

    constexpr int CONNECT_BATCH = 50; // connect N at a time
    auto connect_start = std::chrono::steady_clock::now();

    for (int base = 0; base < num_clients && g_running; base += CONNECT_BATCH)
    {
        int batch_end = std::min(base + CONNECT_BATCH, num_clients);

        // Create and connect batch
        for (int i = base; i < batch_end && g_running; ++i)
        {
            auto &vc = clients[static_cast<size_t>(i)];
            vc.cli = std::make_unique<client>(server_addr, port);
            vc.cli->set_verbose(false);
            vc.cli->channels().register_defaults();
            vc.cli->enable_auto_retransmit();

            auto ec = vc.cli->connect();
            if (succeeded(ec))
            {
                // Open a reliable channel
                vc.channel_id = vc.cli->open_channel(channel_mode::RELIABLE, 128, "stress");
                if (vc.channel_id >= 0)
                {
                    vc.connected = true;
                    ++connect_ok;

                    // Count echoes via data callback
                    auto *vcp = &vc;
                    vc.cli->set_on_data_received([vcp](const packet_header &, const uint8_t *, size_t)
                                                 { ++vcp->echoes; });

                    vc.cli->set_on_packet_lost([vcp](const lost_packet_info &) { ++vcp->losses; });
                }
                else
                {
                    ++connect_fail;
                }
            }
            else
            {
                ++connect_fail;
            }
        }

        if (verbose || (base + CONNECT_BATCH) % 100 == 0 || batch_end == num_clients)
        {
            std::cout << "  Connected: " << connect_ok << " / " << batch_end << " (fail=" << connect_fail << ")"
                      << std::endl;
        }

        // Poll already-connected clients to keep them alive while we
        // connect more. Without this, earlier clients may time out or
        // the server gets overwhelmed with stale heartbeat retries.
        for (int j = 0; j < base; ++j)
        {
            auto &prev = clients[static_cast<size_t>(j)];
            if (prev.connected && prev.cli->is_connected())
            {
                prev.cli->poll();
                prev.cli->update();
            }
        }

        // Small delay between batches to let the server process
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    auto connect_elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - connect_start).count();

    std::cout << std::endl;
    std::cout << " Connected: " << connect_ok << " / " << num_clients << " (fail=" << connect_fail << ") in "
              << connect_elapsed << " ms" << std::endl;

    if (connect_ok == 0)
    {
        std::cerr << " No clients connected — aborting." << std::endl;
        platform_shutdown();
        return 1;
    }

    // --- Phase 2: Steady-state send/recv loop ---
    std::cout << " Running for " << duration_s << "s..." << std::endl;

    auto test_start = std::chrono::steady_clock::now();
    auto test_end = test_start + std::chrono::seconds(duration_s);

    // Compute per-client send interval
    auto send_interval = std::chrono::microseconds(1'000'000 / std::max(rate_hz, 1));

    // Stagger sends: divide clients into rate_hz groups
    // so we don't blast all at the same instant
    int report_interval_s = 5;
    auto next_report = test_start + std::chrono::seconds(report_interval_s);

    uint64_t total_sent = 0;
    uint64_t total_losses = 0;
    uint64_t poll_cycles = 0;

    // Track when each client last sent
    std::vector<std::chrono::steady_clock::time_point> last_send(static_cast<size_t>(num_clients), test_start);

    // Stagger initial sends across the first interval
    for (int i = 0; i < num_clients; ++i)
    {
        auto stagger = std::chrono::microseconds((send_interval.count() * i) / std::max(num_clients, 1));
        last_send[static_cast<size_t>(i)] = test_start + stagger;
    }

    while (g_running && std::chrono::steady_clock::now() < test_end)
    {
        auto now = std::chrono::steady_clock::now();
        int sends_this_cycle = 0;

        for (int i = 0; i < num_clients; ++i)
        {
            auto &vc = clients[static_cast<size_t>(i)];
            if (!vc.connected || !vc.cli->is_connected())
                continue;

            // Poll for incoming (echoes, acks, heartbeats)
            vc.cli->poll();
            vc.cli->update();

            // Send if it's time
            if (now >= last_send[static_cast<size_t>(i)])
            {
                if (vc.cli->can_send())
                {
                    stress_msg msg;
                    msg.client_index = static_cast<uint32_t>(i);
                    msg.sequence = vc.sent;

                    int result = vc.cli->send(&msg, sizeof(msg), static_cast<uint8_t>(vc.channel_id));
                    if (result > 0)
                    {
                        ++vc.sent;
                        ++sends_this_cycle;
                    }
                }
                last_send[static_cast<size_t>(i)] = now + send_interval;
            }
        }

        total_sent += static_cast<uint64_t>(sends_this_cycle);
        ++poll_cycles;

        // Periodic report
        if (now >= next_report)
        {
            auto elapsed_s = std::chrono::duration_cast<std::chrono::seconds>(now - test_start).count();
            uint64_t echo_count = 0;
            uint64_t loss_count = 0;

            for (auto &vc : clients)
            {
                if (!vc.connected)
                    continue;
                echo_count += vc.echoes;
                loss_count += vc.losses;
            }

            auto rate_denom = elapsed_s > 0 ? elapsed_s : 1LL;
            std::cout << "  [" << elapsed_s << "s] sent=" << total_sent << " echoes=" << echo_count
                      << " rate=" << (total_sent / rate_denom) << " pkt/s"
                      << " losses=" << loss_count << " cycles=" << poll_cycles << std::endl;

            next_report = now + std::chrono::seconds(report_interval_s);
        }

        // Yield briefly to avoid 100% CPU spin
        // With many clients the poll loop itself takes enough time
        if (sends_this_cycle == 0)
            std::this_thread::yield();
    }

    // --- Final poll/drain ---
    std::cout << " Draining..." << std::endl;
    auto drain_end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < drain_end)
    {
        for (auto &vc : clients)
        {
            if (!vc.connected || !vc.cli->is_connected())
                continue;
            vc.cli->poll();
            vc.cli->update();
        }
        std::this_thread::yield();
    }

    // --- Collect final stats ---
    total_sent = 0;
    total_losses = 0;
    int still_connected = 0;

    for (auto &vc : clients)
    {
        if (!vc.connected)
            continue;
        total_sent += vc.sent;
        total_losses += vc.losses;
        if (vc.cli->is_connected())
            ++still_connected;
    }

    // --- Phase 3: Disconnect ---
    std::cout << " Disconnecting..." << std::endl;
    for (auto &vc : clients)
    {
        if (vc.connected && vc.cli)
            vc.cli->disconnect();
    }

    // --- Report ---
    // Use the actual send duration (not including drain) for rate calculation
    double send_elapsed_s = static_cast<double>(duration_s);
    double send_rate = total_sent / (send_elapsed_s > 0.1 ? send_elapsed_s : 0.1);

    std::cout << std::endl;
    std::cout << "=============================================" << std::endl;
    std::cout << " STRESS TEST RESULTS" << std::endl;
    std::cout << "=============================================" << std::endl;
    std::cout << " Clients attempted:  " << num_clients << std::endl;
    std::cout << " Connected:          " << connect_ok << std::endl;
    std::cout << " Still connected:    " << still_connected << std::endl;
    std::cout << " Connection failures:" << connect_fail << std::endl;
    std::cout << " Send duration:      " << duration_s << "s" << std::endl;
    std::cout << " Packets sent:       " << total_sent << std::endl;
    std::cout << " Losses detected:    " << total_losses << std::endl;
    std::cout << " Avg send rate:      " << static_cast<uint64_t>(send_rate) << " pkt/s" << std::endl;
    std::cout << " Target rate:        " << (connect_ok * rate_hz) << " pkt/s" << std::endl;
    std::cout << " Poll cycles:        " << poll_cycles << std::endl;
    std::cout << "=============================================" << std::endl;

    // Pass/fail: did we sustain close to target rate?
    double target_rate = static_cast<double>(connect_ok * rate_hz);
    double ratio = target_rate > 0 ? send_rate / target_rate : 0;

    if (connect_ok >= num_clients * 0.95 && ratio >= 0.90)
    {
        std::cout << " RESULT: PASS (" << std::fixed << std::setprecision(1) << (ratio * 100) << "% of target rate)"
                  << std::endl;
    }
    else
    {
        std::cout << " RESULT: DEGRADED (" << std::fixed << std::setprecision(1) << (ratio * 100)
                  << "% of target rate, " << connect_ok << "/" << num_clients << " connected)" << std::endl;
    }
    std::cout << "=============================================" << std::endl;

    clients.clear();
    platform_shutdown();
    return 0;
}
