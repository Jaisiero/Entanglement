// apps/net_bench.cpp — Cross-network echo throughput benchmark (C API)
// Mirrors the Rust net_bench exactly for apples-to-apples comparison.
//
// Usage:
//   net_bench server [port] [workers]
//   net_bench client <server_ip> [port] [clients] [duration] [payload] [burst]

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  pragma comment(lib, "ws2_32.lib")
#endif

#include "entanglement.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

// ================================================================
// Shared types
// ================================================================

struct Stats {
    std::atomic<uint64_t> recv{0};
    std::atomic<uint64_t> echo_ok{0};
    std::atomic<uint64_t> sent{0};
    std::atomic<uint64_t> echoes{0};
};

// Thread-safe fragment-buffer allocator (same pattern as Rust AllocMap)
struct AllocMap {
    std::mutex mtx;
    std::unordered_map<uintptr_t, size_t> map;

    uint8_t *alloc(size_t size) {
        auto *buf = new uint8_t[size];
        std::lock_guard<std::mutex> lk(mtx);
        map[reinterpret_cast<uintptr_t>(buf)] = size;
        return buf;
    }

    void free_buf(uint8_t *ptr) {
        {
            std::lock_guard<std::mutex> lk(mtx);
            map.erase(reinterpret_cast<uintptr_t>(ptr));
        }
        delete[] ptr;
    }
};

// ================================================================
// SERVER
// ================================================================

struct ServerCtx {
    ent_server_t *server;
    Stats         *stats;
    AllocMap       allocs;
};

// Simple-message callback: echo immediately on the same channel
static void srv_on_data(const ent_packet_header *hdr,
                        const uint8_t *payload, size_t payload_size,
                        ent_endpoint sender, void *ud)
{
    auto *ctx = static_cast<ServerCtx *>(ud);
    ctx->stats->recv.fetch_add(1, std::memory_order_relaxed);

    uint32_t mid = 0;
    int r = ent_server_send_to(ctx->server, payload, payload_size,
                               hdr->channel_id, sender, 0, &mid);
    if (r >= 0)
        ctx->stats->echo_ok.fetch_add(1, std::memory_order_relaxed);
}

static void srv_on_connected(ent_endpoint, const char *addr, uint16_t port, void *)
{
    std::printf("[server] +client %s:%u\n", addr, port);
}
static void srv_on_disconnected(ent_endpoint, const char *addr, uint16_t port, void *)
{
    std::printf("[server] -client %s:%u\n", addr, port);
}

// Fragment reassembly callbacks
static uint8_t *srv_alloc(ent_endpoint, uint32_t, uint8_t, uint8_t,
                           size_t max_size, void *ud)
{
    return static_cast<ServerCtx *>(ud)->allocs.alloc(max_size);
}

static void srv_complete(ent_endpoint sender, uint32_t, uint8_t ch,
                         uint8_t *data, size_t total, void *ud)
{
    auto *ctx = static_cast<ServerCtx *>(ud);
    ctx->stats->recv.fetch_add(1, std::memory_order_relaxed);

    uint32_t mid = 0;
    int r = ent_server_send_to(ctx->server, data, total, ch, sender, 0, &mid);
    if (r >= 0)
        ctx->stats->echo_ok.fetch_add(1, std::memory_order_relaxed);

    ctx->allocs.free_buf(data);
}

static void srv_failed(ent_endpoint, uint32_t, uint8_t,
                       uint8_t *buf, ent_message_fail_reason, uint8_t, uint8_t,
                       void *ud)
{
    static_cast<ServerCtx *>(ud)->allocs.free_buf(buf);
}

static void run_server(uint16_t port, int workers)
{
    std::printf("=== NET BENCH SERVER (C++) ===\n");
    std::printf("  Port: %u  Workers: %d\n", port, workers);
    std::printf("  Press Ctrl+C to stop\n\n");

    Stats stats;
    ent_server_t *srv = ent_server_create(port, "0.0.0.0");

    ServerCtx ctx{srv, &stats, {}};

    ent_server_set_worker_count(srv, workers);
    ent_server_register_default_channels(srv);
    ent_server_enable_auto_retransmit(srv);

    ent_server_set_on_client_data(srv, srv_on_data, &ctx);
    ent_server_set_on_client_connected(srv, srv_on_connected, nullptr);
    ent_server_set_on_client_disconnected(srv, srv_on_disconnected, nullptr);
    ent_server_set_on_allocate_message(srv, srv_alloc, &ctx);
    ent_server_set_on_message_complete(srv, srv_complete, &ctx);
    ent_server_set_on_message_failed(srv, srv_failed, &ctx);

    if (ent_server_start(srv) < 0) {
        std::fprintf(stderr, "Server start failed\n");
        return;
    }
    std::printf("[server] Listening on 0.0.0.0:%u\n\n", port);

    auto start = std::chrono::steady_clock::now();
    uint64_t last_recv = 0;

    for (;;) {
        ent_server_poll(srv, 8192);
        ent_server_update(srv);

        auto now = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now - start).count();
        if (ms % 1000 < 2) {
            uint64_t r = stats.recv.load(std::memory_order_relaxed);
            uint64_t e = stats.echo_ok.load(std::memory_order_relaxed);
            if (r != last_recv) {
                uint64_t dr = r - last_recv;
                std::printf("  [%.0fs] recv:%llu (%llu/s)  echo:%llu\n",
                            ms / 1000.0,
                            (unsigned long long)r,
                            (unsigned long long)dr,
                            (unsigned long long)e);
                last_recv = r;
            }
        }
        std::this_thread::yield();
    }
}

// ================================================================
// CLIENT
// ================================================================

struct ClientCtx {
    Stats    *stats;
    AllocMap  allocs;
};

static void cli_on_data(const ent_packet_header *, const uint8_t *, size_t,
                        void *ud)
{
    static_cast<ClientCtx *>(ud)->stats->echoes.fetch_add(
        1, std::memory_order_relaxed);
}

static uint8_t *cli_alloc(ent_endpoint, uint32_t, uint8_t, uint8_t,
                           size_t max_size, void *ud)
{
    return static_cast<ClientCtx *>(ud)->allocs.alloc(max_size);
}

static void cli_complete(ent_endpoint, uint32_t, uint8_t,
                         uint8_t *data, size_t, void *ud)
{
    auto *ctx = static_cast<ClientCtx *>(ud);
    ctx->stats->echoes.fetch_add(1, std::memory_order_relaxed);
    ctx->allocs.free_buf(data);
}

static void cli_failed(ent_endpoint, uint32_t, uint8_t,
                       uint8_t *buf, ent_message_fail_reason, uint8_t, uint8_t,
                       void *ud)
{
    static_cast<ClientCtx *>(ud)->allocs.free_buf(buf);
}

static void cli_connected(void *ud)
{
    static_cast<std::atomic<bool> *>(ud)->store(true,
                                                std::memory_order_release);
}

static void run_client(const char *server_ip, uint16_t port,
                       int num_clients, int duration_secs,
                       int payload_size, int burst, bool coalesced)
{
    std::printf("=== NET BENCH CLIENT (C++) ===\n");
    std::printf("  Server: %s:%u  Clients: %d  Duration: %ds\n",
                server_ip, port, num_clients, duration_secs);
    std::printf("  Payload: %dB  Burst: %d  Coalesced: %s\n",
                payload_size, burst, coalesced ? "YES" : "NO");
    std::printf("=============================================\n\n");

    // Choose channel: coalesced unreliable (ch 4) or plain unreliable (ch 1)
    const uint8_t send_channel = coalesced ? 4 : 1;

    Stats stats;
    std::atomic<bool> running{true};
    std::vector<std::thread> threads;

    for (int i = 0; i < num_clients; ++i) {
        threads.emplace_back([&stats, &running, i,
                              server_ip, port, payload_size, burst,
                              send_channel]()
        {
            // Each thread owns its client + context
            ClientCtx ctx{&stats, {}};

            ent_client_t *cli = ent_client_create(server_ip, port);
            ent_client_register_default_channels(cli);

            std::atomic<bool> connected{false};
            ent_client_set_on_connected(cli, cli_connected, &connected);
            ent_client_set_on_data_received(cli, cli_on_data, &ctx);
            ent_client_set_on_allocate_message(cli, cli_alloc, &ctx);
            ent_client_set_on_message_complete(cli, cli_complete, &ctx);
            ent_client_set_on_message_failed(cli, cli_failed, &ctx);

            if (ent_client_connect(cli) < 0) {
                std::fprintf(stderr, "[client %d] connect failed\n", i);
                ent_client_destroy(cli);
                return;
            }

            // Wait for handshake
            auto t0 = std::chrono::steady_clock::now();
            while (!connected.load(std::memory_order_acquire)) {
                ent_client_poll(cli, 256);
                ent_client_update(cli);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                if (std::chrono::steady_clock::now() - t0 >
                    std::chrono::seconds(5))
                {
                    std::fprintf(stderr, "[client %d] connection timeout\n", i);
                    ent_client_destroy(cli);
                    return;
                }
            }
            std::printf("[client %d] connected\n", i);

            std::vector<uint8_t> buf(payload_size, 0xAB);

            // Send loop (unreliable, channel 1)
            while (running.load(std::memory_order_relaxed)) {
                ent_client_poll(cli, 8192);
                ent_client_update(cli);

                if (ent_client_can_send(cli)) {
                    for (int b = 0; b < burst; ++b) {
                        if (!ent_client_can_send(cli)) break;
                        int r = ent_client_send(
                            cli, buf.data(), buf.size(),
                            send_channel, // channel_id
                            0,        // flags
                            nullptr,  // out_message_id
                            nullptr,  // out_sequence
                            0,        // channel_sequence
                            nullptr); // out_channel_sequence
                        if (r >= 0)
                            stats.sent.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                std::this_thread::yield();
            }

            ent_client_disconnect(cli);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            ent_client_destroy(cli);
        });
    }

    // ---- Per-second stats (main thread) ----
    auto start = std::chrono::steady_clock::now();
    uint64_t last_sent = 0, last_echo = 0;

    for (int sec = 0; sec < duration_secs; ++sec) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        uint64_t s = stats.sent.load(std::memory_order_relaxed);
        uint64_t e = stats.echoes.load(std::memory_order_relaxed);
        double t = std::chrono::duration<double>(
                       std::chrono::steady_clock::now() - start).count();
        std::printf("  [%.0fs] send:%llu/s  echo:%llu/s"
                    "  total_sent:%llu  total_echo:%llu\n",
                    t,
                    (unsigned long long)(s - last_sent),
                    (unsigned long long)(e - last_echo),
                    (unsigned long long)s,
                    (unsigned long long)e);
        last_sent = s;
        last_echo = e;
    }

    running.store(false, std::memory_order_relaxed);
    for (auto &t : threads) t.join();

    double elapsed = std::chrono::duration<double>(
                         std::chrono::steady_clock::now() - start).count();
    uint64_t total_sent = stats.sent.load(std::memory_order_relaxed);
    uint64_t total_echo = stats.echoes.load(std::memory_order_relaxed);
    double bw = total_sent * (double)payload_size * 8.0 / elapsed / 1e6;
    double rate = total_sent > 0
                      ? 100.0 * (double)total_echo / (double)total_sent
                      : 0.0;

    std::printf("\n=============================================\n");
    std::printf("  CLIENT RESULTS (%d clients, %dB payload)\n",
                num_clients, payload_size);
    std::printf("  Duration:    %.1fs\n", elapsed);
    std::printf("  Sent:        %llu (%.0f/s)\n",
                (unsigned long long)total_sent, total_sent / elapsed);
    std::printf("  Echoes:      %llu (%.0f/s)\n",
                (unsigned long long)total_echo, total_echo / elapsed);
    std::printf("  Echo rate:   %.1f%%\n", rate);
    std::printf("  BW send:     %.0f Mbps\n", bw);
    std::printf("=============================================\n");
}

// ================================================================
// main
// ================================================================

int main(int argc, char *argv[])
{
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }
#endif

    if (argc < 2) {
        std::fprintf(stderr,
            "Usage:\n"
            "  net_bench server [port] [workers]\n"
            "  net_bench client <server_ip> [port] [clients]"
            " [duration] [payload] [burst] [coalesced]\n"
            "\n  coalesced: 0 or 1 (default 0) — use coalesced channel\n");
        return 1;
    }

    if (std::strcmp(argv[1], "server") == 0) {
        uint16_t port  = argc > 2 ? (uint16_t)std::atoi(argv[2]) : 19877;
        int      workers = argc > 3 ? std::atoi(argv[3]) : 4;
        run_server(port, workers);
    }
    else if (std::strcmp(argv[1], "client") == 0) {
        const char *ip = argc > 2 ? argv[2] : "127.0.0.1";
        uint16_t port  = argc > 3 ? (uint16_t)std::atoi(argv[3]) : 19877;
        int clients    = argc > 4 ? std::atoi(argv[4]) : 4;
        int duration   = argc > 5 ? std::atoi(argv[5]) : 10;
        int payload    = argc > 6 ? std::atoi(argv[6]) : 1100;
        int burst      = argc > 7 ? std::atoi(argv[7]) : 32;
        bool coalesced = argc > 8 ? std::atoi(argv[8]) != 0 : false;
        run_client(ip, port, clients, duration, payload, burst, coalesced);
    }
    else {
        std::fprintf(stderr,
            "Unknown mode: %s. Use 'server' or 'client'\n", argv[1]);
        return 1;
    }

#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
