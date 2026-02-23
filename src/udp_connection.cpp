#include "udp_connection.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace entanglement
{

    void udp_connection::reset()
    {
        m_active = false;
        m_state = connection_state::DISCONNECTED;
        m_endpoint = {};
        m_local_sequence = 1;
        m_remote_sequence = 0;
        m_recv_bitmap = 0;
        m_send_buffer.fill({});

        // Reset timestamps
        auto now = std::chrono::steady_clock::now();
        m_last_recv_time = now;
        m_last_send_time = now;

        // Reset RTT state
        m_rtt_initialized = false;
        m_srtt = 0.0;
        m_rttvar = 0.0;
        m_rto = INITIAL_RTO_US;
        m_rtt_sample_count = 0;

        // Reset congestion control
        m_cc.reset();

        // Reset per-channel sequences
        std::memset(m_channel_sequences, 0, sizeof(m_channel_sequences));

        // Reset fragmentation state (preserve callback — it's app config, not session state)
        m_next_message_id = 1;
        for (auto &pm : m_pending_messages)
            pm.reset();
    }

    // --- Sending side ---

    uint64_t udp_connection::next_sequence()
    {
        return m_local_sequence++;
    }

    void udp_connection::prepare_header(packet_header &header, bool reliable)
    {
        header.magic = PROTOCOL_MAGIC;
        header.version = PROTOCOL_VERSION;

        uint64_t seq = next_sequence();
        header.sequence = seq;
        header.ack = m_remote_sequence;
        header.ack_bitmap = m_recv_bitmap;
        header.channel_sequence = ++m_channel_sequences[header.channel_id];

        // Register metadata in send buffer (no payload copy)
        size_t index = seq % SEQUENCE_BUFFER_SIZE;
        auto &entry = m_send_buffer[index];
        entry.sequence = seq;
        entry.acked = false;
        entry.active = true;
        entry.reliable = reliable;
        entry.flags = header.flags;
        entry.channel_id = header.channel_id;
        entry.shard_id = header.shard_id;
        entry.payload_size = header.payload_size;
        entry.message_id = 0;
        entry.fragment_index = 0;
        entry.send_time = std::chrono::steady_clock::now();
        m_last_send_time = entry.send_time;

        // Track in congestion window
        m_cc.on_packet_sent();
    }

    // --- Receiving side ---

    bool udp_connection::process_incoming(const packet_header &header)
    {
        // 1. Process piggybacked ACKs from the remote side
        //    The remote is telling us which of OUR packets it received
        if (header.ack > 0)
        {
            // Primary ACK: most recent packet remote received — valid RTT sample
            ack_packet(header.ack, /*take_rtt_sample=*/true);

            // Process bitmap: bit N = ack - (N+1)
            // Bitmap entries are older packets whose initial ACK may have been
            // dropped; measuring RTT from them would inflate the estimate.
            for (int i = 0; i < ACK_BITMAP_WIDTH; ++i)
            {
                if (header.ack_bitmap & (1u << i))
                {
                    uint64_t acked_seq = header.ack - static_cast<uint64_t>(i + 1);
                    if (acked_seq > 0)
                    {
                        ack_packet(acked_seq, /*take_rtt_sample=*/false);
                    }
                }
            }
        }

        // Update receive timestamp
        m_last_recv_time = std::chrono::steady_clock::now();

        // 2. Track this incoming sequence for our outgoing ACKs
        uint64_t seq = header.sequence;

        if (seq == 0)
        {
            return true; // ack-only packet, no sequence to track
        }

        // Duplicate check
        if (seq <= m_remote_sequence && is_sequence_received(seq))
        {
            return false;
        }

        if (seq > m_remote_sequence)
        {
            // New highest sequence: shift bitmap
            uint64_t shift = seq - m_remote_sequence;

            if (shift <= ACK_BITMAP_WIDTH)
            {
                m_recv_bitmap <<= shift;
                // The old m_remote_sequence becomes bit (shift-1)
                if (m_remote_sequence > 0)
                {
                    m_recv_bitmap |= (1u << (shift - 1));
                }
            }
            else
            {
                // Gap too large, reset bitmap but mark old top if within range
                m_recv_bitmap = 0;
            }

            m_remote_sequence = seq;
        }
        else
        {
            // Older packet: set its bit in the bitmap
            record_received(seq);
        }

        return true;
    }

    // --- Helpers ---

    void udp_connection::ack_packet(uint64_t sequence, bool take_rtt_sample)
    {
        size_t index = sequence % SEQUENCE_BUFFER_SIZE;
        auto &entry = m_send_buffer[index];

        if (entry.active && entry.sequence == sequence && !entry.acked)
        {
            entry.acked = true;

            // Track fragment ACK for pending messages
            if (entry.message_id != 0)
            {
                for (auto &pm : m_pending_messages)
                {
                    if (pm.active && pm.message_id == entry.message_id)
                    {
                        pm.acked_count++;
                        if (pm.is_complete())
                        {
                            pm.active = false;
                            if (m_on_message_acked)
                            {
                                m_on_message_acked(pm.message_id);
                            }
                        }
                        break;
                    }
                }
            }

            // Take RTT sample only from primary ACK (header.ack), not bitmap entries.
            // Bitmap ACKs represent older packets whose initial response may have been
            // dropped by simulated or real loss; their apparent delay is inflated and
            // would poison the smoothed RTT estimator.
            if (take_rtt_sample)
            {
                auto now = std::chrono::steady_clock::now();
                auto sample = std::chrono::duration_cast<std::chrono::microseconds>(now - entry.send_time).count();
                if (sample > 0)
                {
                    update_rtt(sample);
                }
            }

            // Notify congestion controller and update pacing
            m_cc.on_packet_acked();
            m_cc.update_pacing(m_srtt);
        }
    }

    bool udp_connection::is_sequence_received(uint64_t sequence) const
    {
        if (sequence == m_remote_sequence)
        {
            return true;
        }

        uint64_t diff = m_remote_sequence - sequence;
        if (diff > 0 && diff <= ACK_BITMAP_WIDTH)
        {
            return (m_recv_bitmap & (1u << (diff - 1))) != 0;
        }

        // Outside bitmap window — treat as received (too old to care)
        return true;
    }

    void udp_connection::record_received(uint64_t sequence)
    {
        uint64_t diff = m_remote_sequence - sequence;
        if (diff > 0 && diff <= ACK_BITMAP_WIDTH)
        {
            m_recv_bitmap |= (1u << (diff - 1));
        }
    }

    // --- Reliability ---

    int udp_connection::collect_losses(std::chrono::steady_clock::time_point now, lost_packet_info *out, int max_count)
    {
        int count = 0;

        // Scan the active window of the send buffer
        uint64_t oldest = (m_local_sequence > SEQUENCE_BUFFER_SIZE) ? m_local_sequence - SEQUENCE_BUFFER_SIZE : 1;

        for (uint64_t seq = oldest; seq < m_local_sequence; ++seq)
        {
            size_t idx = seq % SEQUENCE_BUFFER_SIZE;
            auto &entry = m_send_buffer[idx];

            if (!entry.active || entry.acked || entry.sequence != seq)
                continue;

            auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - entry.send_time);
            if (elapsed.count() < m_rto)
                continue;

            if (!entry.reliable)
            {
                // Unreliable packet timed out — reclaim its in_flight slot silently.
                // Without this, dropped unreliable packets leak in_flight, eventually
                // starving can_send() and collapsing throughput.
                entry.active = false;
                m_cc.on_packet_expired();
                continue;
            }

            // Reliable packet loss — report to application (up to max_count).
            // We keep scanning beyond max_count so unreliable expirations above
            // are still processed; reliable losses beyond the limit are deferred.
            if (count >= max_count)
                continue;

            // Report loss metadata to application
            out[count].sequence = seq;
            out[count].flags = entry.flags;
            out[count].channel_id = entry.channel_id;
            out[count].shard_id = entry.shard_id;
            out[count].payload_size = entry.payload_size;
            out[count].message_id = entry.message_id;
            out[count].fragment_index = entry.fragment_index;
            ++count;

            // Deactivate — we give up on this packet.
            // The application may resend as a new packet if it chooses.
            entry.active = false;

            // Track fragment loss in pending_message so zombie entries get cleaned up
            if (entry.message_id != 0)
            {
                for (auto &pm : m_pending_messages)
                {
                    if (pm.active && pm.message_id == entry.message_id)
                    {
                        pm.lost_count++;
                        if (pm.is_abandoned())
                        {
                            pm.reset(); // Free the slot — all fragments resolved
                        }
                        break;
                    }
                }
            }

            // Notify congestion controller
            m_cc.on_packet_lost();
            m_cc.update_pacing(m_srtt);
        }

        return count;
    }

    // --- Connection liveness ---

    bool udp_connection::has_timed_out(std::chrono::steady_clock::time_point now) const
    {
        if (m_state != connection_state::CONNECTED)
            return false;
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - m_last_recv_time).count();
        return elapsed > CONNECTION_TIMEOUT_US;
    }

    bool udp_connection::needs_heartbeat(std::chrono::steady_clock::time_point now) const
    {
        if (m_state != connection_state::CONNECTED)
            return false;
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - m_last_send_time).count();
        return elapsed > HEARTBEAT_INTERVAL_US;
    }

    // --- Fragmentation (sender side) ---

    bool udp_connection::register_pending_message(uint32_t message_id, uint8_t fragment_count)
    {
        for (auto &pm : m_pending_messages)
        {
            if (!pm.active)
            {
                pm.active = true;
                pm.message_id = message_id;
                pm.fragment_count = fragment_count;
                pm.acked_count = 0;
                return true;
            }
        }
        return false; // all slots full
    }

    bool udp_connection::is_message_acked(uint32_t message_id) const
    {
        for (const auto &pm : m_pending_messages)
        {
            if (pm.active && pm.message_id == message_id)
                return false; // still pending
        }
        // Not found means either already completed and cleaned up, or never registered
        return true;
    }

    // --- RTT estimation (RFC 6298 / Jacobson-Karels) ---

    void udp_connection::update_rtt(int64_t sample_us)
    {
        ++m_rtt_sample_count;
        double sample = static_cast<double>(sample_us);

        if (!m_rtt_initialized)
        {
            // First sample: SRTT = R, RTTVAR = R/2
            m_srtt = sample;
            m_rttvar = sample / 2.0;
            m_rtt_initialized = true;
        }
        else
        {
            // RTTVAR = (1 - beta) * RTTVAR + beta * |SRTT - R|  (beta = 1/4)
            // SRTT   = (1 - alpha) * SRTT  + alpha * R          (alpha = 1/8)
            double diff = std::abs(m_srtt - sample);
            m_rttvar = (1.0 - RTT_BETA) * m_rttvar + RTT_BETA * diff;
            m_srtt = (1.0 - RTT_ALPHA) * m_srtt + RTT_ALPHA * sample;
        }

        // RTO = SRTT + max(G, K * RTTVAR)
        m_rto = static_cast<int64_t>(
            m_srtt + std::max(static_cast<double>(CLOCK_GRANULARITY_US), RTT_VARIANCE_MULTIPLIER * m_rttvar));
        m_rto = std::clamp(m_rto, MIN_RTO_US, MAX_RTO_US);
    }

} // namespace entanglement
