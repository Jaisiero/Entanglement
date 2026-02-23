// ============================================================================
// Entanglement — Soak Test Client (Multi-Client, Time-Based)
// ============================================================================
//
// Usage: EntanglementClient [server_ip] [port] [--t <minutes>] [--c <clients>]
//
// Sends messages continuously on three channels for the specified duration:
//   - RELIABLE:         All must arrive, order not guaranteed
//   - RELIABLE_ORDERED: All must arrive in order
//   - UNRELIABLE:       Best-effort, some loss is expected
//
// Tracks ACKs, losses, retransmissions, echoes, ordering, and per-channel
// sequence numbers. Prints comprehensive statistics with full precision.
// ============================================================================

#include "channel_manager.h"
#include "client.h"
#include "packet_header.h"
#include "platform.h"
#include <atomic>
#include <chrono>
#include <cstdio>
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

static std::mutex g_cout_mutex;

// --- Soak message payload (must match server) ---
#pragma pack(push, 1)
struct soak_msg
{
    uint32_t msg_id;
    uint32_t total_expected; // 0 = time-based (count unknown in advance)
    uint8_t channel_id;
};
#pragma pack(pop)

static constexpr size_t SOAK_MSG_SIZE = sizeof(soak_msg);

// --- Track which message a given sequence carried ---
struct sent_msg_info
{
    uint8_t channel_id;
    uint32_t msg_id;
};

// --- Per-channel send/receive statistics ---
struct channel_send_stats
{
    uint8_t channel_id = 0;
    std::string name;
    channel_mode mode = channel_mode::UNRELIABLE;

    int messages_sent = 0;
    int echoes_received = 0;
    int losses_detected = 0;
    int retransmissions = 0;
    int total_packets = 0; // messages_sent + retransmissions

    // Sequence tracking
    uint64_t min_sequence = UINT64_MAX;
    uint64_t max_sequence = 0;

    // Ordering verification (from echo arrival order)
    uint32_t last_echo_msg_id = UINT32_MAX;
    int echo_order_violations = 0;

    // Which msg_ids confirmed by server echo
    std::set<uint32_t> confirmed_ids;

    void record_send(uint64_t seq)
    {
        if (seq < min_sequence)
            min_sequence = seq;
        if (seq > max_sequence)
            max_sequence = seq;
        ++total_packets;
    }

    void record_echo(uint32_t mid)
    {
        if (confirmed_ids.count(mid))
            return;
        confirmed_ids.insert(mid);
        ++echoes_received;

        if (mode == channel_mode::RELIABLE_ORDERED)
        {
            if (last_echo_msg_id != UINT32_MAX && mid <= last_echo_msg_id)
                ++echo_order_violations;
            last_echo_msg_id = mid;
        }
    }

    std::vector<uint32_t> missing_ids() const
    {
        std::vector<uint32_t> missing;
        for (int i = 0; i < messages_sent; ++i)
        {
            if (!confirmed_ids.count(static_cast<uint32_t>(i)))
                missing.push_back(static_cast<uint32_t>(i));
        }
        return missing;
    }
};

// --- Per-client aggregate results ---
struct client_result
{
    int id = 0;
    bool connected = false;
    channel_send_stats reliable{}, ordered{}, unreliable{};
    int total_data_packets_sent = 0;
    int total_retransmissions = 0;
    int total_losses_detected = 0;
    int total_echoes = 0;
    int pacing_waits = 0;
    uint64_t final_local_seq = 0;
    uint64_t final_remote_seq = 0;
    uint32_t rtt_samples = 0;
    double srtt_ms = 0, rttvar_ms = 0, rto_ms = 0;
    uint32_t final_cwnd = 0;
    uint32_t final_ssthresh = 0;
    uint32_t final_in_flight = 0;
    long long elapsed_ms = 0;
    long long send_phase_ms = 0;
    int drain_iterations = 0;
};

// ============================================================================
// Helper: format percentage with 4 decimal places
// ============================================================================
static std::string fmt_pct(int64_t num, int64_t den)
{
    if (den == 0)
        return "N/A";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.4f%%", 100.0 * static_cast<double>(num) / static_cast<double>(den));
    return buf;
}

// ============================================================================
// Single client soak test run
// ============================================================================
static client_result run_client(int id, const char *server_ip, uint16_t port, int duration_seconds)
{
    client_result result;
    result.id = id;

    // Init channel stats
    result.reliable.channel_id = channels::RELIABLE.id;
    result.reliable.name = "reliable";
    result.reliable.mode = channel_mode::RELIABLE;

    result.ordered.channel_id = channels::ORDERED.id;
    result.ordered.name = "ordered";
    result.ordered.mode = channel_mode::RELIABLE_ORDERED;

    result.unreliable.channel_id = channels::UNRELIABLE.id;
    result.unreliable.name = "unreliable";
    result.unreliable.mode = channel_mode::UNRELIABLE;

    std::unordered_map<uint8_t, channel_send_stats *> ch_map{
        {channels::RELIABLE.id, &result.reliable},
        {channels::ORDERED.id, &result.ordered},
        {channels::UNRELIABLE.id, &result.unreliable},
    };

    std::unordered_map<uint64_t, sent_msg_info> seq_to_msg;

    client cli(server_ip, port);
    cli.set_verbose(false);
    cli.channels().register_defaults();

    int echoes = 0;

    // --- Echo callback ---
    cli.set_on_response(
        [&](const packet_header & /*hdr*/, const uint8_t *payload, size_t size)
        {
            if (size >= SOAK_MSG_SIZE)
            {
                soak_msg msg;
                std::memcpy(&msg, payload, SOAK_MSG_SIZE);

                auto it = ch_map.find(msg.channel_id);
                if (it != ch_map.end())
                    it->second->record_echo(msg.msg_id);
            }
            ++echoes;
        });

    // --- Loss callback ---
    auto on_loss = [&](const lost_packet_info &info)
    {
        ++result.total_losses_detected;

        auto it = seq_to_msg.find(info.sequence);
        if (it == seq_to_msg.end())
            return;

        sent_msg_info mi = it->second;
        seq_to_msg.erase(it);

        auto cit = ch_map.find(mi.channel_id);
        if (cit != ch_map.end())
            ++cit->second->losses_detected;

        if (!cli.channels().is_reliable(mi.channel_id))
            return;

        // Retransmit
        soak_msg payload;
        payload.msg_id = mi.msg_id;
        payload.total_expected = 0;
        payload.channel_id = mi.channel_id;

        packet_header hdr{};
        hdr.channel_id = mi.channel_id;
        hdr.payload_size = static_cast<uint16_t>(SOAK_MSG_SIZE);
        cli.send(hdr, &payload);

        uint64_t new_seq = hdr.sequence;
        seq_to_msg[new_seq] = mi;
        if (cit != ch_map.end())
        {
            cit->second->record_send(new_seq);
            ++cit->second->retransmissions;
        }
        ++result.total_retransmissions;
        ++result.total_data_packets_sent;
    };

    // --- Connect ---
    {
        std::lock_guard<std::mutex> lk(g_cout_mutex);
        std::cout << "[client " << id << "] Connecting to " << server_ip << ":" << port << "..." << std::endl;
    }

    if (!cli.connect())
    {
        std::lock_guard<std::mutex> lk(g_cout_mutex);
        std::cerr << "[client " << id << "] FAILED to connect!" << std::endl;
        return result;
    }

    result.connected = true;
    {
        std::lock_guard<std::mutex> lk(g_cout_mutex);
        std::cout << "[client " << id << "] Connected (port " << cli.local_port() << ")" << std::endl;
    }

    // --- Send loop (time-based) ---
    uint8_t send_ch[3] = {channels::RELIABLE.id, channels::ORDERED.id, channels::UNRELIABLE.id};
    int sent_per_ch[3] = {0, 0, 0};

    auto t_start = std::chrono::steady_clock::now();
    auto deadline = t_start + std::chrono::seconds(duration_seconds);
    auto next_send = t_start;
    auto last_progress = t_start;
    constexpr auto PROGRESS_INTERVAL = std::chrono::seconds(10);

    while (std::chrono::steady_clock::now() < deadline)
    {
        cli.poll();
        cli.update(on_loss);

        if (!cli.can_send())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            ++result.pacing_waits;
            continue;
        }

        auto now = std::chrono::steady_clock::now();
        if (now < next_send)
        {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            ++result.pacing_waits;
            continue;
        }

        // Send one message on each channel
        for (int c = 0; c < 3; ++c)
        {
            uint8_t ch_id = send_ch[c];
            uint32_t msg_id = static_cast<uint32_t>(sent_per_ch[c]);

            soak_msg payload;
            payload.msg_id = msg_id;
            payload.total_expected = 0;
            payload.channel_id = ch_id;

            packet_header hdr{};
            hdr.channel_id = ch_id;
            hdr.payload_size = static_cast<uint16_t>(SOAK_MSG_SIZE);
            cli.send(hdr, &payload);

            uint64_t seq = hdr.sequence;
            seq_to_msg[seq] = {ch_id, msg_id};

            auto cit = ch_map.find(ch_id);
            if (cit != ch_map.end())
                cit->second->record_send(seq);

            ++sent_per_ch[c];
            ++result.total_data_packets_sent;
        }

        result.reliable.messages_sent = sent_per_ch[0];
        result.ordered.messages_sent = sent_per_ch[1];
        result.unreliable.messages_sent = sent_per_ch[2];

        // Pacing
        auto ci = cli.congestion();
        next_send = (ci.pacing_interval_us > 0) ? now + std::chrono::microseconds(ci.pacing_interval_us) : now;

        // Progress report every 10s
        now = std::chrono::steady_clock::now();
        if (now - last_progress >= PROGRESS_INTERVAL)
        {
            auto elapsed_s = std::chrono::duration_cast<std::chrono::seconds>(now - t_start).count();
            auto remain_s = std::chrono::duration_cast<std::chrono::seconds>(deadline - now).count();
            std::lock_guard<std::mutex> lk(g_cout_mutex);
            std::cout << "[client " << id << "] @" << elapsed_s << "s (" << remain_s << "s left)"
                      << " sent=" << result.total_data_packets_sent << " echo=" << echoes
                      << " loss=" << result.total_losses_detected << " retx=" << result.total_retransmissions
                      << " seq_rel=[" << result.reliable.min_sequence << ".." << result.reliable.max_sequence << "]"
                      << " seq_ord=[" << result.ordered.min_sequence << ".." << result.ordered.max_sequence << "]"
                      << " seq_unr=[" << result.unreliable.min_sequence << ".." << result.unreliable.max_sequence << "]"
                      << std::endl;
            last_progress = now;
        }
    }

    auto t_send_done = std::chrono::steady_clock::now();
    result.send_phase_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_send_done - t_start).count();

    {
        std::lock_guard<std::mutex> lk(g_cout_mutex);
        std::cout << "[client " << id << "] Send done: " << result.total_data_packets_sent << " pkts in "
                  << result.send_phase_ms << " ms" << std::endl;
    }

    // --- Drain echoes + detect remaining losses ---
    int expected_reliable = result.reliable.messages_sent + result.ordered.messages_sent;

    auto drain_start = std::chrono::steady_clock::now();
    constexpr auto MAX_DRAIN = std::chrono::seconds(15);

    while (true)
    {
        cli.poll();
        cli.update(on_loss);

        auto now = std::chrono::steady_clock::now();
        if (now - drain_start > MAX_DRAIN)
            break;

        int confirmed = result.reliable.echoes_received + result.ordered.echoes_received;
        if (confirmed >= expected_reliable)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            cli.poll();
            cli.update(on_loss);
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        ++result.drain_iterations;
    }

    auto t_end = std::chrono::steady_clock::now();
    result.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count();
    result.total_echoes = echoes;

    // Connection stats
    auto &conn = cli.connection();
    result.final_local_seq = conn.local_sequence();
    result.final_remote_seq = conn.remote_sequence();
    result.rtt_samples = conn.rtt_sample_count();
    result.srtt_ms = conn.srtt_ms();
    result.rttvar_ms = conn.rttvar_ms();
    result.rto_ms = conn.rto_ms();
    auto ci_final = cli.congestion();
    result.final_cwnd = ci_final.cwnd;
    result.final_ssthresh = ci_final.ssthresh;
    result.final_in_flight = ci_final.in_flight;

    cli.disconnect();

    {
        std::lock_guard<std::mutex> lk(g_cout_mutex);
        std::cout << "[client " << id << "] Finished: echoes=" << echoes << " loss=" << result.total_losses_detected
                  << " retx=" << result.total_retransmissions << " drain=" << result.drain_iterations << std::endl;
    }

    return result;
}

// ============================================================================
// Print per-channel stats
// ============================================================================
static void print_channel(const channel_send_stats &cs)
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

    std::cout << "\n  --- Channel: " << cs.name << " (id=" << static_cast<int>(cs.channel_id) << ", " << mode_str
              << ") ---" << std::endl;
    std::cout << "    Messages sent:          " << cs.messages_sent << std::endl;
    std::cout << "    Total packets (w/retx): " << cs.total_packets << std::endl;
    std::cout << "    Server echoes received: " << cs.echoes_received << " / " << cs.messages_sent << " ("
              << fmt_pct(cs.echoes_received, cs.messages_sent) << ")" << std::endl;

    if (cs.min_sequence != UINT64_MAX)
    {
        std::cout << "    Sequence range:         [" << cs.min_sequence << " .. " << cs.max_sequence
                  << "] (span=" << (cs.max_sequence - cs.min_sequence + 1) << ")" << std::endl;
    }

    std::cout << "    Losses detected:        " << cs.losses_detected << std::endl;
    std::cout << "    Retransmissions:        " << cs.retransmissions << std::endl;

    if (cs.mode == channel_mode::RELIABLE || cs.mode == channel_mode::RELIABLE_ORDERED)
    {
        bool all_ok = (cs.echoes_received == cs.messages_sent);
        std::cout << "    All delivered:          " << (all_ok ? "YES" : "NO") << std::endl;

        auto missing = cs.missing_ids();
        if (!missing.empty())
        {
            std::cout << "    Missing msg_ids:        " << missing.size();
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
            std::cout << std::endl;
        }
    }

    if (cs.mode == channel_mode::RELIABLE_ORDERED)
    {
        std::cout << "    Ordering violations:    " << cs.echo_order_violations << std::endl;
        std::cout << "    Order intact:           " << (cs.echo_order_violations == 0 ? "YES" : "NO") << std::endl;
    }
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char *argv[])
{
    if (!platform_init())
    {
        std::cerr << "Failed to initialize platform" << std::endl;
        return 1;
    }

    // --- Defaults ---
    const char *server_ip = "127.0.0.1";
    uint16_t server_port = DEFAULT_PORT;
    int duration_minutes = 1;
    int num_clients = 1;

    // --- Parse args: positional [ip] [port] + named -t/--t <min> -c/--c <count> ---
    bool got_ip = false, got_port = false;
    for (int i = 1; i < argc; ++i)
    {
        if ((std::strcmp(argv[i], "-t") == 0 || std::strcmp(argv[i], "--t") == 0) && i + 1 < argc)
        {
            duration_minutes = std::atoi(argv[++i]);
            if (duration_minutes < 1)
                duration_minutes = 1;
        }
        else if ((std::strcmp(argv[i], "-c") == 0 || std::strcmp(argv[i], "--c") == 0) && i + 1 < argc)
        {
            num_clients = std::atoi(argv[++i]);
            if (num_clients < 1)
                num_clients = 1;
        }
        else if (!got_ip)
        {
            server_ip = argv[i];
            got_ip = true;
        }
        else if (!got_port)
        {
            server_port = static_cast<uint16_t>(std::atoi(argv[i]));
            got_port = true;
        }
    }

    int duration_seconds = duration_minutes * 60;

    std::cout << "=============================================" << std::endl;
    std::cout << " Entanglement Soak Test Client v2.0" << std::endl;
    std::cout << " Server:    " << server_ip << ":" << server_port << std::endl;
    std::cout << " Duration:  " << duration_minutes << " min (" << duration_seconds << "s)" << std::endl;
    std::cout << " Clients:   " << num_clients << std::endl;
    std::cout << " Channels:  reliable, ordered, unreliable" << std::endl;
    std::cout << " Payload:   " << SOAK_MSG_SIZE << " bytes" << std::endl;
    std::cout << "=============================================" << std::endl;

    // --- Launch client threads ---
    std::vector<std::thread> threads;
    std::vector<client_result> results(num_clients);

    auto global_start = std::chrono::steady_clock::now();

    for (int i = 0; i < num_clients; ++i)
    {
        threads.emplace_back([&results, i, server_ip, server_port, duration_seconds]()
                             { results[i] = run_client(i, server_ip, server_port, duration_seconds); });
    }

    for (auto &t : threads)
        t.join();

    auto global_elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - global_start);

    // =========================================================================
    // RESULTS
    // =========================================================================

    std::cout << "\n=============================================" << std::endl;
    std::cout << " ENTANGLEMENT SOAK TEST RESULTS" << std::endl;
    std::cout << " " << num_clients << " client(s), " << duration_minutes << " min, wall=" << global_elapsed.count()
              << " ms" << std::endl;
    std::cout << "=============================================" << std::endl;

    // Aggregates
    int64_t agg_sent = 0, agg_retx = 0, agg_losses = 0, agg_echoes = 0;
    int64_t agg_rel_sent = 0, agg_rel_echo = 0;
    int64_t agg_ord_sent = 0, agg_ord_echo = 0, agg_ord_violations = 0;
    int64_t agg_unrel_sent = 0, agg_unrel_echo = 0;

    for (auto &r : results)
    {
        if (!r.connected)
        {
            std::cerr << "\n  Client " << r.id << ": FAILED TO CONNECT" << std::endl;
            continue;
        }

        std::cout << "\n--- Client " << r.id << " ---" << std::endl;
        std::cout << "  Duration:   " << r.elapsed_ms << " ms (send=" << r.send_phase_ms << " ms)" << std::endl;
        std::cout << "  Sent:       " << r.total_data_packets_sent << " (retx=" << r.total_retransmissions << ")"
                  << std::endl;
        std::cout << "  Echoes:     " << r.total_echoes << std::endl;
        std::cout << "  Losses:     " << r.total_losses_detected << std::endl;
        std::cout << "  Pacing:     " << r.pacing_waits << " waits" << std::endl;
        std::cout << "  Drain:      " << r.drain_iterations << " iterations" << std::endl;
        std::cout << "  Sequence:   local=" << (r.final_local_seq - 1) << " remote=" << r.final_remote_seq << std::endl;
        std::cout << "  RTT:        " << std::fixed << std::setprecision(3) << r.srtt_ms << "ms var=" << r.rttvar_ms
                  << "ms rto=" << r.rto_ms << "ms" << std::endl;
        std::cout << "  Congestion: cwnd=" << r.final_cwnd << " ssthresh=" << r.final_ssthresh
                  << " inflight=" << r.final_in_flight << std::endl;
        std::cout << "  ACK:        " << r.rtt_samples << " samples";
        if (r.final_local_seq > 1)
            std::cout << " (" << fmt_pct(r.rtt_samples, static_cast<int64_t>(r.final_local_seq - 1)) << ")";
        std::cout << std::endl;

        if (r.elapsed_ms > 0)
        {
            double tp = 1000.0 * r.total_data_packets_sent / r.elapsed_ms;
            std::cout << "  Throughput: " << std::fixed << std::setprecision(1) << tp << " pkt/s" << std::endl;
        }

        print_channel(r.reliable);
        print_channel(r.ordered);
        print_channel(r.unreliable);

        agg_sent += r.total_data_packets_sent;
        agg_retx += r.total_retransmissions;
        agg_losses += r.total_losses_detected;
        agg_echoes += r.total_echoes;
        agg_rel_sent += r.reliable.messages_sent;
        agg_rel_echo += r.reliable.echoes_received;
        agg_ord_sent += r.ordered.messages_sent;
        agg_ord_echo += r.ordered.echoes_received;
        agg_ord_violations += r.ordered.echo_order_violations;
        agg_unrel_sent += r.unreliable.messages_sent;
        agg_unrel_echo += r.unreliable.echoes_received;
    }

    int64_t total_rel_sent = agg_rel_sent + agg_ord_sent;
    int64_t total_rel_echo = agg_rel_echo + agg_ord_echo;

    std::cout << "\n=============================================" << std::endl;
    std::cout << " AGGREGATE" << std::endl;
    std::cout << "=============================================" << std::endl;
    std::cout << "  Packets sent:       " << agg_sent << " (retx=" << agg_retx << ")" << std::endl;
    std::cout << "  Total echoes:       " << agg_echoes << std::endl;
    std::cout << "  Total losses:       " << agg_losses << std::endl;
    std::cout << "  Reliable:           " << agg_rel_echo << " / " << agg_rel_sent << " ("
              << fmt_pct(agg_rel_echo, agg_rel_sent) << ")" << std::endl;
    std::cout << "  Ordered:            " << agg_ord_echo << " / " << agg_ord_sent << " ("
              << fmt_pct(agg_ord_echo, agg_ord_sent) << ")" << std::endl;
    std::cout << "  Ord. violations:    " << agg_ord_violations << std::endl;
    std::cout << "  Unreliable:         " << agg_unrel_echo << " / " << agg_unrel_sent << " ("
              << fmt_pct(agg_unrel_echo, agg_unrel_sent) << ")" << std::endl;
    std::cout << "  Total reliable:     " << total_rel_echo << " / " << total_rel_sent << " ("
              << fmt_pct(total_rel_echo, total_rel_sent) << ")" << std::endl;

    if (global_elapsed.count() > 0)
    {
        double tp = 1000.0 * agg_sent / global_elapsed.count();
        std::cout << "  Throughput:         " << std::fixed << std::setprecision(1) << tp << " pkt/s" << std::endl;
    }

    // --- Verdicts ---
    std::cout << "\n=============================================\n" << std::endl;

    bool rel_pass = (total_rel_echo == total_rel_sent);
    bool ord_pass = (agg_ord_violations == 0);
    bool ord_del_pass = (agg_ord_echo == agg_ord_sent);

    if (rel_pass)
        std::cout << "  VERDICT: ALL RELIABLE DELIVERED              [PASS]" << std::endl;
    else
        std::cout << "  VERDICT: RELIABLE MISSING                    [FAIL] (" << (total_rel_sent - total_rel_echo)
                  << " missing)" << std::endl;

    if (ord_del_pass && ord_pass)
        std::cout << "  VERDICT: ORDERED DELIVERY + ORDER            [PASS]" << std::endl;
    else if (ord_del_pass && !ord_pass)
        std::cout << "  VERDICT: ORDERED DELIVERED BUT OUT-OF-ORDER  [FAIL] (" << agg_ord_violations << " violations)"
                  << std::endl;
    else
        std::cout << "  VERDICT: ORDERED INCOMPLETE                  [FAIL]" << std::endl;

    std::cout << "  VERDICT: UNRELIABLE DELIVERY                 " << fmt_pct(agg_unrel_echo, agg_unrel_sent)
              << std::endl;

    std::cout << "\n=============================================" << std::endl;

    platform_shutdown();
    return (rel_pass && ord_pass) ? 0 : 1;
}
