// ============================================================================
// Entanglement — Soak Test Client v6.0  (Simple + Fragmented, Full Retransmit)
// ============================================================================
//
// Usage: EntanglementClient [-s ip] [-p port] [-t min] [-c clients] [-d drop%] [-m modes]
//
// Sends both SIMPLE (9-byte) and FRAGMENTED (2500-byte) messages on all
// three default channels, giving 6 independent streams:
//
//   Stream                 Channel           Type
//   ──────────────────────────────────────────────
//   reliable-simple        RELIABLE          simple
//   ordered-simple         RELIABLE_ORDERED  simple
//   unreliable-simple      UNRELIABLE        simple
//   reliable-frag          RELIABLE          fragmented
//   ordered-frag           RELIABLE_ORDERED  fragmented
//   unreliable-frag        UNRELIABLE        fragmented
//
// Both simple and fragmented reliable messages are retransmitted on loss.
// The library reports per-fragment loss metadata (message_id, fragment_index,
// fragment_count) and the app retransmits individual fragments using stored
// payloads — no intermediate buffers inside the library.
//
// Tracks ACKs, losses, echoes, ordering, and per-stream sequence numbers.
// ============================================================================

#include "channel_manager.h"
#include "client.h"
#include "endpoint_key.h"
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

// --- Soak message payload (must match server — first 9 bytes of every payload) ---
#pragma pack(push, 1)
struct soak_msg
{
    uint32_t msg_id;
    uint32_t total_expected; // 0 = time-based (count unknown in advance)
    uint8_t channel_id;
};
#pragma pack(pop)

static constexpr size_t SOAK_MSG_SIZE = sizeof(soak_msg);
static constexpr size_t FRAG_PAYLOAD_SIZE = 2500; // >MAX_PAYLOAD_SIZE → triggers fragmentation

// --- Track which message a given sequence carried (simple only) ---
struct sent_msg_info
{
    uint8_t channel_id;
    uint32_t msg_id;
};

// --- Per-stream send/receive statistics ---
struct stream_send_stats
{
    uint8_t channel_id = 0;
    std::string name;
    channel_mode mode = channel_mode::UNRELIABLE;
    bool fragmented = false;

    int messages_sent = 0;
    int echoes_received = 0;
    int losses_detected = 0;
    int retransmissions = 0;
    int total_packets = 0; // messages_sent + retransmissions (simple only)

    // Ordering verification (from echo arrival order)
    uint32_t last_echo_msg_id = UINT32_MAX;
    int echo_order_violations = 0;

    std::set<uint32_t> confirmed_ids;

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
            if (!confirmed_ids.count(static_cast<uint32_t>(i)))
                missing.push_back(static_cast<uint32_t>(i));
        return missing;
    }
};

// --- Per-client aggregate results ---
struct client_result
{
    int id = 0;
    bool connected = false;

    // 6 streams: [0..2] = simple (reliable, ordered, unreliable)
    //            [3..5] = fragmented (reliable, ordered, unreliable)
    stream_send_stats streams[6];

    int total_data_packets_sent = 0;
    int total_retransmissions = 0;
    int total_losses_detected = 0;
    int total_simple_echoes = 0;
    int total_frag_echoes = 0;
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
static std::string fmt_pct(int64_t num, int64_t den)
{
    if (den == 0)
        return "N/A";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.4f%%", 100.0 * static_cast<double>(num) / static_cast<double>(den));
    return buf;
}

// ============================================================================
// Build the fragmented payload: soak_msg header + fill pattern
// ============================================================================
static void build_frag_payload(std::vector<uint8_t> &buf, uint32_t msg_id, uint32_t total_expected, uint8_t ch_id)
{
    buf.resize(FRAG_PAYLOAD_SIZE, 0xAB);
    soak_msg hdr;
    hdr.msg_id = msg_id;
    hdr.total_expected = total_expected;
    hdr.channel_id = ch_id;
    std::memcpy(buf.data(), &hdr, SOAK_MSG_SIZE);
}

// ============================================================================
// Single client soak test run
// ============================================================================
static client_result run_client(int id, const char *server_ip, uint16_t port, int duration_seconds, double drop_rate,
                                bool enable_reliable, bool enable_ordered, bool enable_unreliable)
{
    client_result result;
    result.id = id;

    // Stream indices
    constexpr int S_REL = 0, S_ORD = 1, S_UNR = 2; // simple
    constexpr int F_REL = 3, F_ORD = 4, F_UNR = 5; // fragmented

    // Init stream stats
    auto init_s = [&](int idx, uint8_t ch_id, const char *name, channel_mode m, bool frag)
    {
        auto &s = result.streams[idx];
        s.channel_id = ch_id;
        s.name = name;
        s.mode = m;
        s.fragmented = frag;
    };
    init_s(S_REL, channels::RELIABLE.id, "reliable-simple", channel_mode::RELIABLE, false);
    init_s(S_ORD, channels::ORDERED.id, "ordered-simple", channel_mode::RELIABLE_ORDERED, false);
    init_s(S_UNR, channels::UNRELIABLE.id, "unreliable-simple", channel_mode::UNRELIABLE, false);
    init_s(F_REL, channels::RELIABLE.id, "reliable-frag", channel_mode::RELIABLE, true);
    init_s(F_ORD, channels::ORDERED.id, "ordered-frag", channel_mode::RELIABLE_ORDERED, true);
    init_s(F_UNR, channels::UNRELIABLE.id, "unreliable-frag", channel_mode::UNRELIABLE, true);

    // Map channel_id → simple stream index
    std::unordered_map<uint8_t, int> simple_idx{
        {channels::RELIABLE.id, S_REL},
        {channels::ORDERED.id, S_ORD},
        {channels::UNRELIABLE.id, S_UNR},
    };
    // Map channel_id → frag stream index
    std::unordered_map<uint8_t, int> frag_idx{
        {channels::RELIABLE.id, F_REL},
        {channels::ORDERED.id, F_ORD},
        {channels::UNRELIABLE.id, F_UNR},
    };

    std::unordered_map<uint64_t, sent_msg_info> seq_to_msg; // simple retransmission tracking

    // --- Fragment retransmission state ---
    // Library message_id → full payload (app stores for retransmit)
    std::unordered_map<uint32_t, std::vector<uint8_t>> frag_payloads;
    // Per-fragment retry counter: key = (message_id << 8) | fragment_index
    std::unordered_map<uint64_t, int> frag_retries;
    constexpr int MAX_FRAG_RETRIES = 10;

    // --- Ordered channel gating with timeout recovery ---
    // One ordered message per type (simple/frag) in flight at a time.
    // If the echo doesn't arrive within ordered_retry_timeout, the message
    // is retransmitted to ensure progress on both nodes with the same msg_id.
    auto ordered_retry_timeout = std::chrono::seconds(3);

    bool ordered_simple_pending = false;
    uint32_t ordered_simple_pending_msg_id = 0;
    std::chrono::steady_clock::time_point ordered_simple_send_time{};

    bool ordered_frag_pending = false;
    uint32_t ordered_frag_pending_msg_id = 0;
    uint32_t ordered_frag_lib_msg_id = 0;
    std::vector<uint8_t> ordered_frag_pending_payload;
    std::chrono::steady_clock::time_point ordered_frag_send_time{};

    client cli(server_ip, port);
    cli.set_verbose(false);
    cli.channels().register_defaults();

    // Shorten reassembly timeout so stale echo entries are evicted faster.
    // With RTO ~50ms, even multiply-retransmitted fragments complete well
    // within 5s. This keeps reassembler slot pressure low during drain.
    cli.set_reassembly_timeout(5'000'000); // 5 seconds

#ifdef ENTANGLEMENT_SIMULATE_LOSS
    if (drop_rate > 0.0)
        cli.set_simulated_drop_rate(drop_rate);
#else
    (void)drop_rate;
#endif

    // =========================================================================
    // SIMPLE ECHO callback (non-fragmented responses from server)
    // =========================================================================
    cli.set_on_data_received(
        [&](const packet_header & /*hdr*/, const uint8_t *payload, size_t size)
        {
            if (size >= SOAK_MSG_SIZE)
            {
                soak_msg msg;
                std::memcpy(&msg, payload, SOAK_MSG_SIZE);
                auto it = simple_idx.find(msg.channel_id);
                if (it != simple_idx.end())
                    result.streams[it->second].record_echo(msg.msg_id);

                // Ordered gating: unblock only if this echo matches the pending msg_id.
                // Stale/duplicate echoes from retransmissions must NOT open the gate
                // for a different message, as that would violate ordering.
                if (msg.channel_id == channels::ORDERED.id && msg.msg_id == ordered_simple_pending_msg_id)
                    ordered_simple_pending = false;
            }
            ++result.total_simple_echoes;
        });

    // =========================================================================
    // FRAGMENTED ECHO callbacks (reassembled responses from server)
    // =========================================================================
    cli.set_on_allocate_message([&](const endpoint_key &, uint32_t, uint8_t, uint8_t, size_t max_size) -> uint8_t *
                                { return new uint8_t[max_size]; });

    cli.set_on_message_complete(
        [&](const endpoint_key &, uint32_t, uint8_t ch_id, uint8_t *data, size_t total_size)
        {
            if (total_size >= SOAK_MSG_SIZE)
            {
                soak_msg msg;
                std::memcpy(&msg, data, SOAK_MSG_SIZE);
                auto it = frag_idx.find(msg.channel_id);
                if (it != frag_idx.end())
                    result.streams[it->second].record_echo(msg.msg_id);

                // Ordered gating: unblock only if this echo matches the pending msg_id.
                // Stale/duplicate echoes from retransmissions must NOT open the gate
                // for a different message, as that would violate ordering.
                if (msg.channel_id == channels::ORDERED.id && msg.msg_id == ordered_frag_pending_msg_id)
                {
                    ordered_frag_pending = false;
                    if (ordered_frag_lib_msg_id != 0)
                    {
                        frag_payloads.erase(ordered_frag_lib_msg_id);
                        ordered_frag_lib_msg_id = 0;
                    }
                }
            }
            ++result.total_frag_echoes;
            delete[] data;
        });

    cli.set_on_message_failed(
        [&](const endpoint_key &, uint32_t, uint8_t ch_id, uint8_t *buf, message_fail_reason, uint8_t, uint8_t)
        {
            // If a fragmented echo on the ordered channel expired/was evicted,
            // force an immediate retry by backdating the send timestamp so the
            // timeout-recovery lambda fires on the very next loop iteration.
            if (ch_id == channels::ORDERED.id && ordered_frag_pending)
                ordered_frag_send_time = std::chrono::steady_clock::time_point{};
            delete[] buf;
        });

    // =========================================================================
    // LOSS callback (simple retransmission only)
    // =========================================================================
    auto on_loss = [&](const lost_packet_info &info)
    {
        ++result.total_losses_detected;

        if (info.message_id != 0)
        {
            // Fragment loss — find the frag stream and count it
            auto it = frag_idx.find(info.channel_id);
            if (it != frag_idx.end())
                ++result.streams[it->second].losses_detected;

            if (!cli.channels().is_reliable(info.channel_id))
                return;

            // Check retry limit
            uint64_t frag_key = (static_cast<uint64_t>(info.message_id) << 8) | info.fragment_index;
            int &retries = frag_retries[frag_key];
            if (retries >= MAX_FRAG_RETRIES)
                return;
            ++retries;

            // Look up stored payload
            auto pit = frag_payloads.find(info.message_id);
            if (pit == frag_payloads.end())
                return; // payload already cleaned up or not found

            const auto &payload = pit->second;

            // Skip retransmission if echo already received for this app-level message.
            // Without this, re-sent fragments spawn new sequences that also time out
            // and chain into an unbounded retransmission spiral.
            if (payload.size() >= SOAK_MSG_SIZE)
            {
                soak_msg app_hdr;
                std::memcpy(&app_hdr, payload.data(), SOAK_MSG_SIZE);
                auto fit = frag_idx.find(app_hdr.channel_id);
                if (fit != frag_idx.end() && result.streams[fit->second].confirmed_ids.count(app_hdr.msg_id))
                {
                    return; // echo confirmed — skip retransmission (payload kept for drain sweep)
                }
            }

            size_t offset = static_cast<size_t>(info.fragment_index) * MAX_FRAGMENT_PAYLOAD;
            if (offset >= payload.size())
                return; // safety check
            size_t chunk = (std::min)(MAX_FRAGMENT_PAYLOAD, payload.size() - offset);

            cli.send_fragment(info.message_id, info.fragment_index, info.fragment_count, payload.data() + offset, chunk,
                              0, info.channel_id);

            // Reset ordered gating timer so the timeout recovery knows we just retried
            if (info.channel_id == channels::ORDERED.id)
                ordered_frag_send_time = std::chrono::steady_clock::now();

            if (it != frag_idx.end())
            {
                ++result.streams[it->second].retransmissions;
                ++result.streams[it->second].total_packets;
            }
            ++result.total_retransmissions;
            ++result.total_data_packets_sent;
            return;
        }

        // Simple packet loss
        auto it = seq_to_msg.find(info.sequence);
        if (it == seq_to_msg.end())
            return;

        sent_msg_info mi = it->second;
        seq_to_msg.erase(it);

        auto sit = simple_idx.find(mi.channel_id);
        if (sit != simple_idx.end())
            ++result.streams[sit->second].losses_detected;

        if (!cli.channels().is_reliable(mi.channel_id))
            return;

        // Skip retransmission if echo already received — prevents unbounded
        // retransmission chains where each re-sent packet times out and
        // spawns yet another retransmission, starving real traffic.
        if (sit != simple_idx.end() && result.streams[sit->second].confirmed_ids.count(mi.msg_id))
            return;

        // Retransmit simple message
        soak_msg payload;
        payload.msg_id = mi.msg_id;
        payload.total_expected = 0;
        payload.channel_id = mi.channel_id;

        uint64_t seq = 0;
        cli.send(&payload, SOAK_MSG_SIZE, mi.channel_id, 0, nullptr, &seq);

        seq_to_msg[seq] = mi;

        // Reset ordered gating timer so the timeout recovery knows we just retried
        if (mi.channel_id == channels::ORDERED.id)
            ordered_simple_send_time = std::chrono::steady_clock::now();

        if (sit != simple_idx.end())
        {
            ++result.streams[sit->second].retransmissions;
            ++result.streams[sit->second].total_packets;
        }
        ++result.total_retransmissions;
        ++result.total_data_packets_sent;
    };

    // --- Connect ---
    {
        std::lock_guard<std::mutex> lk(g_cout_mutex);
        std::cout << "[client " << id << "] Connecting to " << server_ip << ":" << port << "..." << std::endl;
    }

    if (failed(cli.connect()))
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
    uint8_t ch_ids[3] = {channels::RELIABLE.id, channels::ORDERED.id, channels::UNRELIABLE.id};
    int simple_sent[3] = {0, 0, 0};
    int frag_sent[3] = {0, 0, 0};

    std::vector<uint8_t> frag_buf;
    frag_buf.reserve(FRAG_PAYLOAD_SIZE);

    auto t_start = std::chrono::steady_clock::now();
    auto deadline = t_start + std::chrono::seconds(duration_seconds);
    auto next_send = t_start;
    auto last_progress = t_start;
    constexpr auto PROGRESS_INTERVAL = std::chrono::seconds(10);
    int cycle = 0;
    const bool ch_enabled[3] = {enable_reliable, enable_ordered, enable_unreliable};

    // Stream rotation: cycle through 6 streams one message at a time.
    // 0=simple-rel, 1=frag-rel, 2=simple-ord, 3=frag-ord, 4=simple-unr, 5=frag-unr
    // This avoids bursting 9 packets (3 frag messages) which overwhelms the cwnd.

    // Lambda: retransmit pending ordered messages on timeout.
    // Ensures forward progress even if the normal loss-detection chain breaks
    // (e.g. send-buffer wrap before RTO fires).
    auto retry_ordered_timeouts = [&]()
    {
        auto now = std::chrono::steady_clock::now();

        if (ordered_simple_pending && (now - ordered_simple_send_time) > ordered_retry_timeout)
        {
            soak_msg payload;
            payload.msg_id = ordered_simple_pending_msg_id;
            payload.total_expected = 0;
            payload.channel_id = channels::ORDERED.id;

            cli.send(&payload, SOAK_MSG_SIZE, channels::ORDERED.id);

            // Do NOT add to seq_to_msg — the timeout mechanism handles ordered
            // retries independently. Adding would let the loss handler also
            // retransmit, creating a duplicate chain that amplifies traffic.
            ++result.streams[S_ORD].retransmissions;
            ++result.streams[S_ORD].total_packets;
            ++result.total_retransmissions;
            ++result.total_data_packets_sent;

            ordered_simple_send_time = now;
        }

        if (ordered_frag_pending && (now - ordered_frag_send_time) > ordered_retry_timeout)
        {
            // Always create a NEW library message_id for each retry.
            // Reusing the old message_id would be blocked by the server's
            // was_recently_completed() check if the previous attempt already
            // completed reassembly (but the echo was lost on the way back).
            // A fresh message_id guarantees a new reassembly entry and a new echo.
            uint32_t lib_message_id = 0;
            int sent = cli.send(ordered_frag_pending_payload.data(), ordered_frag_pending_payload.size(),
                                channels::ORDERED.id, 0, &lib_message_id);
            if (sent > 0)
            {
                if (lib_message_id != 0)
                    frag_payloads[lib_message_id] = ordered_frag_pending_payload;
                ordered_frag_lib_msg_id = lib_message_id;
                ++result.streams[F_ORD].retransmissions;
                result.total_data_packets_sent += 3;
                ++result.total_retransmissions;
            }

            ordered_frag_send_time = now;
        }
    };

    while (std::chrono::steady_clock::now() < deadline)
    {
        cli.poll();
        cli.update(on_loss);

        // Ordered gating timeout recovery — retransmit same msg_id if echo is overdue
        retry_ordered_timeouts();

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

        int stream = cycle % 6;
        int ch_index = stream / 2;            // 0, 0, 1, 1, 2, 2
        bool send_simple = (stream % 2 == 0); // even=simple, odd=frag
        ++cycle;

        // Skip disabled channel types
        if (!ch_enabled[ch_index])
            continue;

        uint8_t ch_id = ch_ids[ch_index];

        // Ordered gating: skip if previous ordered message is still pending echo
        if (ch_id == channels::ORDERED.id)
        {
            if ((send_simple && ordered_simple_pending) || (!send_simple && ordered_frag_pending))
                continue; // cycle advanced — poll/update will run next iteration
        }

        if (send_simple)
        {
            // ── Send 1 simple message ──
            uint32_t msg_id = static_cast<uint32_t>(simple_sent[ch_index]);

            soak_msg payload;
            payload.msg_id = msg_id;
            payload.total_expected = 0;
            payload.channel_id = ch_id;

            uint64_t seq = 0;
            cli.send(&payload, SOAK_MSG_SIZE, ch_id, 0, nullptr, &seq);

            seq_to_msg[seq] = {ch_id, msg_id};
            auto sit = simple_idx.find(ch_id);
            if (sit != simple_idx.end())
                ++result.streams[sit->second].total_packets;

            // Mark ordered channel as pending (wait for echo before sending next)
            if (ch_id == channels::ORDERED.id)
            {
                ordered_simple_pending = true;
                ordered_simple_pending_msg_id = msg_id;
                ordered_simple_send_time = std::chrono::steady_clock::now();
            }

            ++simple_sent[ch_index];
            ++result.total_data_packets_sent;

            result.streams[S_REL].messages_sent = simple_sent[0];
            result.streams[S_ORD].messages_sent = simple_sent[1];
            result.streams[S_UNR].messages_sent = simple_sent[2];
        }
        else
        {
            // ── Send 1 fragmented message ──
            uint32_t msg_id = static_cast<uint32_t>(frag_sent[ch_index]);

            build_frag_payload(frag_buf, msg_id, 0, ch_id);

            uint32_t lib_message_id = 0;
            int sent_bytes = cli.send(frag_buf.data(), frag_buf.size(), ch_id, 0, &lib_message_id);

            if (sent_bytes > 0)
            {
                // Store payload for potential fragment retransmission
                if (lib_message_id != 0)
                    frag_payloads[lib_message_id] = frag_buf;

                // Mark ordered channel as pending (wait for echo before sending next)
                if (ch_id == channels::ORDERED.id)
                {
                    ordered_frag_pending = true;
                    ordered_frag_pending_msg_id = msg_id;
                    ordered_frag_lib_msg_id = lib_message_id;
                    ordered_frag_pending_payload = frag_buf;
                    ordered_frag_send_time = std::chrono::steady_clock::now();
                }

                ++frag_sent[ch_index];
                // Each fragmented message generates ceil(2500/1160)=3 packets
                result.total_data_packets_sent += 3;
            }

            result.streams[F_REL].messages_sent = frag_sent[0];
            result.streams[F_ORD].messages_sent = frag_sent[1];
            result.streams[F_UNR].messages_sent = frag_sent[2];
        }

        // Pacing
        auto ci = cli.congestion();
        now = std::chrono::steady_clock::now();
        next_send = (ci.pacing_interval_us > 0) ? now + std::chrono::microseconds(ci.pacing_interval_us) : now;

        // Progress report every 10s
        if (now - last_progress >= PROGRESS_INTERVAL)
        {
            auto elapsed_s = std::chrono::duration_cast<std::chrono::seconds>(now - t_start).count();
            auto remain_s = std::chrono::duration_cast<std::chrono::seconds>(deadline - now).count();
            std::lock_guard<std::mutex> lk(g_cout_mutex);
            std::cout << "[client " << id << "] @" << elapsed_s << "s (" << remain_s << "s left)"
                      << " pkts=" << result.total_data_packets_sent << " s_echo=" << result.total_simple_echoes
                      << " f_echo=" << result.total_frag_echoes << " loss=" << result.total_losses_detected
                      << " retx=" << result.total_retransmissions << std::endl;
            std::cout << "           simple: rel=" << simple_sent[0] << " ord=" << simple_sent[1]
                      << " unr=" << simple_sent[2] << "  frag: rel=" << frag_sent[0] << " ord=" << frag_sent[1]
                      << " unr=" << frag_sent[2] << std::endl;
            last_progress = now;
        }
    }

    auto t_send_done = std::chrono::steady_clock::now();
    result.send_phase_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_send_done - t_start).count();

    {
        std::lock_guard<std::mutex> lk(g_cout_mutex);
        std::cout << "[client " << id << "] Send done: " << result.total_data_packets_sent << " pkts in "
                  << result.send_phase_ms << " ms"
                  << " (simple=" << (simple_sent[0] + simple_sent[1] + simple_sent[2])
                  << " frag=" << (frag_sent[0] + frag_sent[1] + frag_sent[2]) << ")" << std::endl;
    }

    // --- Drain echoes + detect remaining losses ---
    // Active-sweep drain: when progress stalls, retransmit ALL fragments of
    // every unconfirmed reliable message. This handles the case where the
    // server's reassembler evicted a partial message (timeout/slot pressure)
    // and single-fragment retransmissions can never complete it.
    int expected_reliable = 0;
    if (enable_reliable)
        expected_reliable += result.streams[S_REL].messages_sent + result.streams[F_REL].messages_sent;
    if (enable_ordered)
        expected_reliable += result.streams[S_ORD].messages_sent + result.streams[F_ORD].messages_sent;

    auto drain_start = std::chrono::steady_clock::now();
    constexpr auto MAX_DRAIN = std::chrono::seconds(120);
    constexpr auto DRAIN_SWEEP_INTERVAL = std::chrono::seconds(2); // aggressive: 2s (was 10s)
    auto last_drain_progress = drain_start;
    auto next_sweep_time = drain_start + DRAIN_SWEEP_INTERVAL;
    int last_drain_confirmed = 0;
    int drain_sweeps = 0;

    // --- Aggressive drain timeouts ---
    // During drain no new data is generated, so shorter timeouts are safe
    // and dramatically reduce wait time when echoes are lost.
    ordered_retry_timeout = std::chrono::seconds(1);
    cli.connection().set_ordered_stall_timeout(1'000'000); // 1s (was 5s)

    // Lambda: sweep all unconfirmed reliable messages, retransmitting them fully.
    auto drain_sweep = [&]()
    {
        int sweep_frag = 0, sweep_simple = 0;

        // 1) Unconfirmed frag reliable — retransmit ALL fragments from stored payloads
        for (auto &[lib_msg_id, payload] : frag_payloads)
        {
            if (payload.size() < SOAK_MSG_SIZE)
                continue;
            soak_msg app_hdr;
            std::memcpy(&app_hdr, payload.data(), SOAK_MSG_SIZE);

            // Only sweep non-ordered reliable (ordered has its own retry mechanism)
            if (app_hdr.channel_id == channels::ORDERED.id)
                continue;
            if (!cli.channels().is_reliable(app_hdr.channel_id))
                continue;
            auto fit = frag_idx.find(app_hdr.channel_id);
            if (fit == frag_idx.end())
                continue;
            if (result.streams[fit->second].confirmed_ids.count(app_hdr.msg_id))
                continue;

            // Retransmit all fragments of this unconfirmed message
            uint8_t frag_count =
                static_cast<uint8_t>((payload.size() + MAX_FRAGMENT_PAYLOAD - 1) / MAX_FRAGMENT_PAYLOAD);
            for (uint8_t i = 0; i < frag_count; ++i)
            {
                size_t off = static_cast<size_t>(i) * MAX_FRAGMENT_PAYLOAD;
                size_t chunk = (std::min)(MAX_FRAGMENT_PAYLOAD, payload.size() - off);
                cli.send_fragment(lib_msg_id, i, frag_count, payload.data() + off, chunk, 0, app_hdr.channel_id);
            }
            ++result.streams[fit->second].retransmissions;
            result.total_data_packets_sent += frag_count;
            ++result.total_retransmissions;
            ++sweep_frag;
        }

        // 2) Unconfirmed simple reliable — retransmit as fresh packets
        if (enable_reliable)
        {
            for (uint32_t mid = 0; mid < static_cast<uint32_t>(result.streams[S_REL].messages_sent); ++mid)
            {
                if (result.streams[S_REL].confirmed_ids.count(mid))
                    continue;

                soak_msg payload;
                payload.msg_id = mid;
                payload.total_expected = 0;
                payload.channel_id = channels::RELIABLE.id;

                uint64_t seq = 0;
                cli.send(&payload, SOAK_MSG_SIZE, channels::RELIABLE.id, 0, nullptr, &seq);
                seq_to_msg[seq] = {channels::RELIABLE.id, mid};

                ++result.streams[S_REL].retransmissions;
                ++result.streams[S_REL].total_packets;
                ++result.total_retransmissions;
                ++result.total_data_packets_sent;
                ++sweep_simple;
            }
        }

        // 3) Pending ordered-frag — retransmit with a fresh library message_id.
        //    A fresh ID creates a new echo cycle on the server, bypassing any
        //    stale reassembly state on the client side (e.g. clogged slots from
        //    previous echo attempts whose fragments were partially lost).
        if (ordered_frag_pending && !ordered_frag_pending_payload.empty())
        {
            uint32_t new_lib_msg_id = 0;
            int sent = cli.send(ordered_frag_pending_payload.data(), ordered_frag_pending_payload.size(),
                                channels::ORDERED.id, 0, &new_lib_msg_id);
            if (sent > 0 && new_lib_msg_id != 0)
            {
                frag_payloads[new_lib_msg_id] = ordered_frag_pending_payload;
                ordered_frag_lib_msg_id = new_lib_msg_id;
                ordered_frag_send_time = std::chrono::steady_clock::now();
                ++result.streams[F_ORD].retransmissions;
                result.total_data_packets_sent += 3;
                ++result.total_retransmissions;
                ++sweep_frag;
            }
        }

        // 4) Pending ordered-simple — retransmit
        if (ordered_simple_pending)
        {
            soak_msg payload;
            payload.msg_id = ordered_simple_pending_msg_id;
            payload.total_expected = 0;
            payload.channel_id = channels::ORDERED.id;

            cli.send(&payload, SOAK_MSG_SIZE, channels::ORDERED.id);

            ordered_simple_send_time = std::chrono::steady_clock::now();
            ++result.streams[S_ORD].retransmissions;
            ++result.streams[S_ORD].total_packets;
            ++result.total_retransmissions;
            ++result.total_data_packets_sent;
            ++sweep_simple;
        }

        ++drain_sweeps;
        {
            std::lock_guard<std::mutex> lk(g_cout_mutex);
            std::cout << "[client " << id << "] Drain sweep #" << drain_sweeps << ": frag=" << sweep_frag
                      << " simple=" << sweep_simple << std::endl;
        }
    };

    while (true)
    {
        cli.poll();
        cli.update(on_loss);

        // Continue retransmitting pending ordered messages during drain
        retry_ordered_timeouts();

        auto now = std::chrono::steady_clock::now();
        if (now - drain_start > MAX_DRAIN)
            break;

        int confirmed = 0;
        if (enable_reliable)
            confirmed += result.streams[S_REL].echoes_received + result.streams[F_REL].echoes_received;
        if (enable_ordered)
            confirmed += result.streams[S_ORD].echoes_received + result.streams[F_ORD].echoes_received;
        if (confirmed >= expected_reliable)
        {
            // All reliable echoes received — final flush
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            cli.poll();
            cli.update(on_loss);
            break;
        }

        // Track progress
        if (confirmed > last_drain_confirmed)
        {
            last_drain_confirmed = confirmed;
            last_drain_progress = now;
        }

        // Active sweep: when no progress for DRAIN_SWEEP_INTERVAL, retransmit
        // ALL unconfirmed reliable messages. This handles server-side reassembly
        // eviction and echo-only losses that the per-fragment loss handler misses.
        if (now >= next_sweep_time && confirmed < expected_reliable)
        {
            drain_sweep();
            next_sweep_time = now + DRAIN_SWEEP_INTERVAL;
            last_drain_progress = now; // reset progress tracker
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        ++result.drain_iterations;
    }

    auto t_end = std::chrono::steady_clock::now();
    result.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count();

    // Connection stats
    auto &conn = cli.connection();
    result.final_local_seq = conn.local_sequence();
    result.final_remote_seq = conn.remote_sequence();
    result.rtt_samples = conn.rtt_sample_count();
    result.srtt_ms = conn.srtt_ms();
    result.rttvar_ms = conn.rttvar_ms();
    result.rto_ms = conn.rto_ms();
    auto ci = cli.congestion();
    result.final_cwnd = ci.cwnd;
    result.final_ssthresh = ci.ssthresh;
    result.final_in_flight = ci.in_flight;

    cli.disconnect();

    {
        std::lock_guard<std::mutex> lk(g_cout_mutex);
        std::cout << "[client " << id << "] Finished: s_echo=" << result.total_simple_echoes
                  << " f_echo=" << result.total_frag_echoes << " loss=" << result.total_losses_detected
                  << " retx=" << result.total_retransmissions << " drain=" << result.drain_iterations;
#ifdef ENTANGLEMENT_SIMULATE_LOSS
        std::cout << " sim_drops=" << cli.simulated_drop_count();
#endif
        std::cout << std::endl;
    }

    return result;
}

// ============================================================================
// Print per-stream stats
// ============================================================================
static void print_stream(const stream_send_stats &s)
{
    const char *mode_str = "UNKNOWN";
    switch (s.mode)
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

    std::cout << "\n  --- " << s.name << " (id=" << static_cast<int>(s.channel_id) << ", " << mode_str << ") ---"
              << std::endl;
    std::cout << "    Messages sent:          " << s.messages_sent << std::endl;
    if (!s.fragmented)
        std::cout << "    Total packets (w/retx): " << s.total_packets << std::endl;
    std::cout << "    Echoes received:        " << s.echoes_received << " / " << s.messages_sent << " ("
              << fmt_pct(s.echoes_received, s.messages_sent) << ")" << std::endl;
    std::cout << "    Losses detected:        " << s.losses_detected << std::endl;
    std::cout << "    Retransmissions:        " << s.retransmissions << std::endl;

    if (s.mode == channel_mode::RELIABLE || s.mode == channel_mode::RELIABLE_ORDERED)
    {
        bool all_ok = (s.echoes_received == s.messages_sent);
        std::cout << "    All delivered:          " << (all_ok ? "YES" : "NO") << std::endl;

        auto missing = s.missing_ids();
        if (!missing.empty())
        {
            std::cout << "    Missing msg_ids:        " << missing.size();
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

    if (s.mode == channel_mode::RELIABLE_ORDERED)
    {
        std::cout << "    Ordering violations:    " << s.echo_order_violations << std::endl;
        std::cout << "    Order intact:           " << (s.echo_order_violations == 0 ? "YES" : "NO") << std::endl;
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
    double drop_rate = 0.0;
    bool enable_reliable = true;
    bool enable_ordered = true;
    bool enable_unreliable = true;

    // --- Parse args ---
    for (int i = 1; i < argc; ++i)
    {
        if ((std::strcmp(argv[i], "-s") == 0 || std::strcmp(argv[i], "--server") == 0) && i + 1 < argc)
        {
            server_ip = argv[++i];
        }
        else if ((std::strcmp(argv[i], "-p") == 0 || std::strcmp(argv[i], "--port") == 0) && i + 1 < argc)
        {
            server_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        }
        else if ((std::strcmp(argv[i], "-t") == 0 || std::strcmp(argv[i], "--time") == 0) && i + 1 < argc)
        {
            duration_minutes = std::atoi(argv[++i]);
            if (duration_minutes < 1)
                duration_minutes = 1;
        }
        else if ((std::strcmp(argv[i], "-c") == 0 || std::strcmp(argv[i], "--clients") == 0) && i + 1 < argc)
        {
            num_clients = std::atoi(argv[++i]);
            if (num_clients < 1)
                num_clients = 1;
        }
        else if ((std::strcmp(argv[i], "-d") == 0 || std::strcmp(argv[i], "--drop") == 0) && i + 1 < argc)
        {
            drop_rate = std::atof(argv[++i]) / 100.0; // percent → [0,1]
            if (drop_rate < 0.0)
                drop_rate = 0.0;
            if (drop_rate > 1.0)
                drop_rate = 1.0;
        }
        else if ((std::strcmp(argv[i], "-m") == 0 || std::strcmp(argv[i], "--modes") == 0) && i + 1 < argc)
        {
            enable_reliable = enable_ordered = enable_unreliable = false;
            std::string modes = argv[++i];
            for (char c : modes)
            {
                if (c == 'r' || c == 'R')
                    enable_reliable = true;
                else if (c == 'o' || c == 'O')
                    enable_ordered = true;
                else if (c == 'u' || c == 'U')
                    enable_unreliable = true;
            }
        }
        else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0)
        {
            std::cout << "Usage: EntanglementClient [-s ip] [-p port] [-t minutes] [-c clients] [-d drop%] [-m modes]"
                      << std::endl;
            std::cout << "  -s, --server   Server IP (default: 127.0.0.1)" << std::endl;
            std::cout << "  -p, --port     Server port (default: " << DEFAULT_PORT << ")" << std::endl;
            std::cout << "  -t, --time     Duration in minutes (default: 1)" << std::endl;
            std::cout << "  -c, --clients  Number of clients (default: 1)" << std::endl;
            std::cout << "  -d, --drop     Simulated drop rate in percent (default: 0)" << std::endl;
            std::cout << "  -m, --modes    Channel modes to test: r=reliable, o=ordered, u=unreliable" << std::endl;
            std::cout << "                 Comma-separated (default: r,o,u = all)" << std::endl;
            return 0;
        }
        else
        {
            std::cerr << "Unknown argument: " << argv[i] << std::endl;
            std::cerr << "Usage: EntanglementClient [-s ip] [-p port] [-t minutes] [-c clients] [-d drop%] [-m modes]"
                      << std::endl;
            return 1;
        }
    }

    int duration_seconds = duration_minutes * 60;

    std::cout << "=============================================" << std::endl;
    std::cout << " Entanglement Soak Test Client v4.0" << std::endl;
    std::cout << " Server:    " << server_ip << ":" << server_port << std::endl;
    std::cout << " Duration:  " << duration_minutes << " min (" << duration_seconds << "s)" << std::endl;
    std::cout << " Clients:   " << num_clients << std::endl;
    int num_modes = (int)enable_reliable + (int)enable_ordered + (int)enable_unreliable;
    std::cout << " Modes:     ";
    if (enable_reliable)
        std::cout << "reliable ";
    if (enable_ordered)
        std::cout << "ordered ";
    if (enable_unreliable)
        std::cout << "unreliable ";
    std::cout << "(" << num_modes << " modes, " << (num_modes * 2) << " streams)" << std::endl;
    std::cout << " Simple payload:  " << SOAK_MSG_SIZE << " bytes" << std::endl;
    std::cout << " Frag payload:    " << FRAG_PAYLOAD_SIZE << " bytes (~"
              << ((FRAG_PAYLOAD_SIZE + MAX_FRAGMENT_PAYLOAD - 1) / MAX_FRAGMENT_PAYLOAD) << " fragments)" << std::endl;
#ifdef ENTANGLEMENT_SIMULATE_LOSS
    std::cout << " Drop rate: " << (drop_rate * 100.0) << "%" << std::endl;
#endif
    std::cout << "=============================================" << std::endl;

    // --- Launch client threads ---
    std::vector<std::thread> threads;
    std::vector<client_result> results(num_clients);

    auto global_start = std::chrono::steady_clock::now();

    for (int i = 0; i < num_clients; ++i)
    {
        threads.emplace_back(
            [&results, i, server_ip, server_port, duration_seconds, drop_rate, enable_reliable, enable_ordered,
             enable_unreliable]()
            {
                results[i] = run_client(i, server_ip, server_port, duration_seconds, drop_rate, enable_reliable,
                                        enable_ordered, enable_unreliable);
            });
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
    int64_t agg_sent = 0, agg_retx = 0, agg_losses = 0;
    int64_t agg_simple_echoes = 0, agg_frag_echoes = 0;

    // Per-stream aggregates: [0..5] same order as result.streams
    int64_t agg_stream_sent[6] = {}, agg_stream_echo[6] = {}, agg_stream_violations[6] = {};
    const bool ch_enabled[3] = {enable_reliable, enable_ordered, enable_unreliable};

    for (auto &r : results)
    {
        if (!r.connected)
        {
            std::cerr << "\n  Client " << r.id << ": FAILED TO CONNECT" << std::endl;
            continue;
        }

        std::cout << "\n--- Client " << r.id << " ---" << std::endl;
        std::cout << "  Duration:   " << r.elapsed_ms << " ms (send=" << r.send_phase_ms << " ms)" << std::endl;
        std::cout << "  Packets:    " << r.total_data_packets_sent << " (retx=" << r.total_retransmissions << ")"
                  << std::endl;
        std::cout << "  Echoes:     simple=" << r.total_simple_echoes << " frag=" << r.total_frag_echoes << std::endl;
        std::cout << "  Losses:     " << r.total_losses_detected << std::endl;
        std::cout << "  Pacing:     " << r.pacing_waits << " waits" << std::endl;
        std::cout << "  Drain:      " << r.drain_iterations << " iterations" << std::endl;
        std::cout << "  Sequence:   local=" << (r.final_local_seq > 0 ? r.final_local_seq - 1 : 0)
                  << " remote=" << r.final_remote_seq << std::endl;
        std::cout << "  RTT:        " << std::fixed << std::setprecision(3) << r.srtt_ms << "ms var=" << r.rttvar_ms
                  << "ms rto=" << r.rto_ms << "ms" << std::endl;
        std::cout << "  Congestion: cwnd=" << r.final_cwnd << " ssthresh=" << r.final_ssthresh
                  << " inflight=" << r.final_in_flight << std::endl;

        if (r.elapsed_ms > 0)
        {
            double tp = 1000.0 * r.total_data_packets_sent / r.elapsed_ms;
            std::cout << "  Throughput: " << std::fixed << std::setprecision(1) << tp << " pkt/s" << std::endl;
        }

        for (int s = 0; s < 6; ++s)
        {
            if (!ch_enabled[s % 3])
                continue;
            print_stream(r.streams[s]);
            agg_stream_sent[s] += r.streams[s].messages_sent;
            agg_stream_echo[s] += r.streams[s].echoes_received;
            agg_stream_violations[s] += r.streams[s].echo_order_violations;
        }

        agg_sent += r.total_data_packets_sent;
        agg_retx += r.total_retransmissions;
        agg_losses += r.total_losses_detected;
        agg_simple_echoes += r.total_simple_echoes;
        agg_frag_echoes += r.total_frag_echoes;
    }

    // Shorthand indices
    constexpr int S_REL = 0, S_ORD = 1, S_UNR = 2;
    constexpr int F_REL = 3, F_ORD = 4, F_UNR = 5;

    std::cout << "\n=============================================" << std::endl;
    std::cout << " AGGREGATE" << std::endl;
    std::cout << "=============================================" << std::endl;
    std::cout << "  Packets sent:          " << agg_sent << " (retx=" << agg_retx << ")" << std::endl;
    std::cout << "  Echoes:                simple=" << agg_simple_echoes << " frag=" << agg_frag_echoes << std::endl;
    std::cout << "  Losses:                " << agg_losses << std::endl;
    std::cout << std::endl;
    std::cout << "  --- Simple Streams ---" << std::endl;
    if (enable_reliable)
        std::cout << "  Reliable:              " << agg_stream_echo[S_REL] << " / " << agg_stream_sent[S_REL] << " ("
                  << fmt_pct(agg_stream_echo[S_REL], agg_stream_sent[S_REL]) << ")" << std::endl;
    if (enable_ordered)
    {
        std::cout << "  Ordered:               " << agg_stream_echo[S_ORD] << " / " << agg_stream_sent[S_ORD] << " ("
                  << fmt_pct(agg_stream_echo[S_ORD], agg_stream_sent[S_ORD]) << ")" << std::endl;
        std::cout << "  Ord. violations:       " << agg_stream_violations[S_ORD] << std::endl;
    }
    if (enable_unreliable)
        std::cout << "  Unreliable:            " << agg_stream_echo[S_UNR] << " / " << agg_stream_sent[S_UNR] << " ("
                  << fmt_pct(agg_stream_echo[S_UNR], agg_stream_sent[S_UNR]) << ")" << std::endl;
    std::cout << std::endl;
    std::cout << "  --- Fragmented Streams ---" << std::endl;
    if (enable_reliable)
        std::cout << "  Reliable:              " << agg_stream_echo[F_REL] << " / " << agg_stream_sent[F_REL] << " ("
                  << fmt_pct(agg_stream_echo[F_REL], agg_stream_sent[F_REL]) << ")" << std::endl;
    if (enable_ordered)
    {
        std::cout << "  Ordered:               " << agg_stream_echo[F_ORD] << " / " << agg_stream_sent[F_ORD] << " ("
                  << fmt_pct(agg_stream_echo[F_ORD], agg_stream_sent[F_ORD]) << ")" << std::endl;
        std::cout << "  Ord. violations:       " << agg_stream_violations[F_ORD] << std::endl;
    }
    if (enable_unreliable)
        std::cout << "  Unreliable:            " << agg_stream_echo[F_UNR] << " / " << agg_stream_sent[F_UNR] << " ("
                  << fmt_pct(agg_stream_echo[F_UNR], agg_stream_sent[F_UNR]) << ")" << std::endl;

    if (global_elapsed.count() > 0)
    {
        double tp = 1000.0 * agg_sent / global_elapsed.count();
        std::cout << "\n  Throughput:            " << std::fixed << std::setprecision(1) << tp << " pkt/s" << std::endl;
    }

    // --- Verdicts ---
    std::cout << "\n=============================================\n" << std::endl;

    bool all_pass = true;

    // Simple verdicts
    if (enable_reliable || enable_ordered)
    {
        int64_t simple_rel_sent = 0, simple_rel_echo = 0;
        if (enable_reliable)
        {
            simple_rel_sent += agg_stream_sent[S_REL];
            simple_rel_echo += agg_stream_echo[S_REL];
        }
        if (enable_ordered)
        {
            simple_rel_sent += agg_stream_sent[S_ORD];
            simple_rel_echo += agg_stream_echo[S_ORD];
        }
        bool simple_rel_pass = (simple_rel_echo == simple_rel_sent);
        if (!simple_rel_pass)
            all_pass = false;

        if (simple_rel_pass)
            std::cout << "  SIMPLE RELIABLE DELIVERED:                [PASS]" << std::endl;
        else
            std::cout << "  SIMPLE RELIABLE DELIVERED:                [FAIL] (" << (simple_rel_sent - simple_rel_echo)
                      << " missing)" << std::endl;
    }

    if (enable_ordered)
    {
        bool simple_ord_pass = (agg_stream_violations[S_ORD] == 0);
        if (!simple_ord_pass)
            all_pass = false;

        if (simple_ord_pass)
            std::cout << "  SIMPLE ORDERED DELIVERY + ORDER:          [PASS]" << std::endl;
        else
            std::cout << "  SIMPLE ORDERED DELIVERY + ORDER:          [FAIL] (" << agg_stream_violations[S_ORD]
                      << " violations)" << std::endl;
    }

    if (enable_unreliable)
        std::cout << "  SIMPLE UNRELIABLE DELIVERY:               "
                  << fmt_pct(agg_stream_echo[S_UNR], agg_stream_sent[S_UNR]) << std::endl;

    // Frag verdicts
    if (enable_reliable || enable_ordered)
    {
        int64_t frag_rel_sent = 0, frag_rel_echo = 0;
        if (enable_reliable)
        {
            frag_rel_sent += agg_stream_sent[F_REL];
            frag_rel_echo += agg_stream_echo[F_REL];
        }
        if (enable_ordered)
        {
            frag_rel_sent += agg_stream_sent[F_ORD];
            frag_rel_echo += agg_stream_echo[F_ORD];
        }
        bool frag_rel_pass = (frag_rel_echo == frag_rel_sent);
        if (!frag_rel_pass)
            all_pass = false;

        if (frag_rel_pass)
            std::cout << "  FRAG RELIABLE DELIVERED:                 [PASS]" << std::endl;
        else
            std::cout << "  FRAG RELIABLE DELIVERED:                 [FAIL] (" << (frag_rel_sent - frag_rel_echo)
                      << " missing)" << std::endl;
    }

    if (enable_ordered)
    {
        bool frag_ord_pass = (agg_stream_violations[F_ORD] == 0);
        if (!frag_ord_pass)
            all_pass = false;

        if (frag_ord_pass)
            std::cout << "  FRAG ORDERED INTACT:                     [PASS]" << std::endl;
        else
            std::cout << "  FRAG ORDERED INTACT:                     [FAIL] (" << agg_stream_violations[F_ORD]
                      << " violations)" << std::endl;
    }

    if (enable_unreliable)
        std::cout << "  FRAG UNRELIABLE DELIVERY:                "
                  << fmt_pct(agg_stream_echo[F_UNR], agg_stream_sent[F_UNR]) << std::endl;

    std::cout << "\n=============================================" << std::endl;

    platform_shutdown();
    return all_pass ? 0 : 1;
}
