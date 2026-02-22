#include "client.h"
#include "packet_header.h"
#include "platform.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static std::mutex g_cout_mutex;

struct client_stats
{
    int id = 0;
    int packets_sent = 0;
    int responses_received = 0;
    int retransmissions = 0;
    uint32_t rtt_samples = 0;
    uint64_t local_seq = 0;
    uint64_t remote_seq = 0;
    double rtt_ms = 0.0;
    double rto_ms = 0.0;
    long long elapsed_ms = 0;
    bool passed_wrap = false;
    bool passed_echo = false;
};

static client_stats run_client(int id, const char *server_ip, uint16_t port, int total_packets)
{
    using namespace entanglement;

    client_stats stats;
    stats.id = id;
    stats.packets_sent = total_packets;

    client cli(server_ip, port);
    cli.set_verbose(false); // Suppress internal cout — we print under mutex

    std::atomic<int> responses{0};

    cli.set_on_response([&](const packet_header & /*hdr*/, const uint8_t * /*payload*/, size_t /*size*/)
                        { ++responses; });

    // Pre-generate all payloads before timing starts
    std::vector<std::string> messages(total_packets);
    for (int i = 0; i < total_packets; ++i)
    {
        messages[i] = "c" + std::to_string(id) + "#" + std::to_string(i);
    }

    // Payload provider callback — resolves sequence to pre-generated message (zero-copy)
    auto provider = [&](uint64_t seq) -> std::pair<const uint8_t *, uint16_t>
    {
        uint64_t idx = seq - 1; // sequences start at 1
        if (idx < static_cast<uint64_t>(total_packets))
        {
            const auto &m = messages[idx];
            return {reinterpret_cast<const uint8_t *>(m.data()), static_cast<uint16_t>(m.size())};
        }
        return {nullptr, 0};
    };

    if (!cli.connect())
    {
        std::lock_guard<std::mutex> lock(g_cout_mutex);
        std::cerr << "[client " << id << "] Failed to connect" << std::endl;
        return stats;
    }

    {
        std::lock_guard<std::mutex> lock(g_cout_mutex);
        std::cout << "[client " << id << "] Connected (port " << cli.local_port() << ")" << std::endl;
    }

    auto t0 = std::chrono::steady_clock::now();

    int total_retransmissions = 0;

    // Send packets, polling + retransmitting every 16
    for (int i = 0; i < total_packets; ++i)
    {
        const auto &msg = messages[i];

        packet_header hdr{};
        hdr.flags = FLAG_RELIABLE;
        hdr.payload_size = static_cast<uint16_t>(msg.size());
        cli.send(hdr, msg.data());

        if ((i & 0xF) == 0xF)
        {
            cli.poll();
            total_retransmissions += cli.update(provider);
        }
    }

    // Drain remaining responses with retransmission
    for (int attempt = 0; attempt < 500; ++attempt)
    {
        cli.poll();
        total_retransmissions += cli.update(provider);
        if (responses.load() >= total_packets)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    auto t1 = std::chrono::steady_clock::now();

    auto &conn = cli.connection();
    stats.responses_received = responses.load();
    stats.retransmissions = total_retransmissions;
    stats.rtt_samples = conn.rtt_sample_count();
    stats.local_seq = conn.local_sequence();
    stats.remote_seq = conn.remote_sequence();
    stats.rtt_ms = conn.srtt_ms();
    stats.rto_ms = conn.rto_ms();
    stats.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    stats.passed_wrap = (stats.local_seq - 1) >= SEQUENCE_BUFFER_SIZE;
    stats.passed_echo = (stats.responses_received == total_packets);

    {
        std::lock_guard<std::mutex> lock(g_cout_mutex);
        std::cout << "[client " << id << "] Done: " << stats.responses_received << "/" << total_packets << " echoes"
                  << std::endl;
    }

    cli.disconnect();
    return stats;
}

int main()
{
    using namespace entanglement;

    if (!platform_init())
    {
        std::cerr << "Failed to initialize platform" << std::endl;
        return 1;
    }

    constexpr int NUM_CLIENTS = 4;
    constexpr int TOTAL_PACKETS = 1100; // > SEQUENCE_BUFFER_SIZE to force wrap
    constexpr uint16_t PORT = DEFAULT_PORT;

    std::cout << "Entanglement Multi-Client Test" << std::endl;
    std::cout << "  Clients:            " << NUM_CLIENTS << std::endl;
    std::cout << "  Packets per client: " << TOTAL_PACKETS << std::endl;
    std::cout << "  Buffer size:        " << SEQUENCE_BUFFER_SIZE << std::endl;
    std::cout << "  Header size:        " << sizeof(packet_header) << " bytes" << std::endl;
    std::cout << std::endl;

    // Launch clients in parallel threads
    std::vector<std::thread> threads;
    std::vector<client_stats> results(NUM_CLIENTS);

    auto global_start = std::chrono::steady_clock::now();

    for (int i = 0; i < NUM_CLIENTS; ++i)
    {
        threads.emplace_back([&results, i]() { results[i] = run_client(i, "127.0.0.1", DEFAULT_PORT, TOTAL_PACKETS); });
    }

    for (auto &t : threads)
    {
        t.join();
    }

    auto global_elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - global_start).count();

    // --- Summary ---
    std::cout << "\n===== Multi-Client Test Results =====" << std::endl;

    int total_sent = 0;
    int total_recv = 0;
    bool all_wrap = true;
    bool all_echo = true;

    for (auto &s : results)
    {
        total_sent += s.packets_sent;
        total_recv += s.responses_received;
        all_wrap &= s.passed_wrap;
        all_echo &= s.passed_echo;

        int pct = s.packets_sent > 0 ? static_cast<int>(100LL * s.responses_received / s.packets_sent) : 0;
        std::cout << "  Client " << s.id << ": recv=" << s.responses_received << "/" << s.packets_sent << " (" << pct
                  << "%) retx=" << s.retransmissions << " rtt=" << std::fixed << std::setprecision(1) << s.rtt_ms
                  << "ms rto=" << s.rto_ms << "ms samples=" << s.rtt_samples << " " << s.elapsed_ms << "ms"
                  << (s.passed_echo ? " [OK]" : " [PARTIAL]") << std::endl;
    }

    std::cout << "  -----------------------------------" << std::endl;
    std::cout << "  Total packets:  " << total_sent << " sent, " << total_recv << " received" << std::endl;
    std::cout << "  Total elapsed:  " << global_elapsed << " ms" << std::endl;
    std::cout << "=====================================" << std::endl;

    if (all_wrap)
        std::cout << "[PASS] All clients wrapped the circular buffer." << std::endl;
    else
        std::cout << "[FAIL] Some clients did not wrap." << std::endl;

    if (all_echo)
        std::cout << "[PASS] All echoes received (reliability works!)." << std::endl;
    else
        std::cout << "[WARN] Some responses missing." << std::endl;

    platform_shutdown();
    return 0;
}
