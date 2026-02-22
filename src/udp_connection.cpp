#include "udp_connection.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace entanglement
{

    void udp_connection::reset()
    {
        m_active = false;
        m_endpoint = {};
        m_local_sequence = 1;
        m_remote_sequence = 0;
        m_recv_bitmap = 0;
        m_send_buffer.fill({});

        // Reset RTT state
        m_rtt_initialized = false;
        m_srtt = 0.0;
        m_rttvar = 0.0;
        m_rto = INITIAL_RTO_US;
        m_rtt_sample_count = 0;
    }

    // --- Sending side ---

    uint64_t udp_connection::next_sequence()
    {
        return m_local_sequence++;
    }

    void udp_connection::prepare_header(packet_header &header)
    {
        header.magic = PROTOCOL_MAGIC;
        header.version = PROTOCOL_VERSION;

        uint64_t seq = next_sequence();
        header.sequence = seq;
        header.ack = m_remote_sequence;
        header.ack_bitmap = m_recv_bitmap;

        // Register metadata in send buffer (no payload copy)
        size_t index = seq % SEQUENCE_BUFFER_SIZE;
        auto &entry = m_send_buffer[index];
        entry.sequence = seq;
        entry.acked = false;
        entry.active = true;
        entry.flags = header.flags;
        entry.retransmit_count = 0;
        entry.send_time = std::chrono::steady_clock::now();
        entry.header_copy = header;
    }

    // --- Receiving side ---

    bool udp_connection::process_incoming(const packet_header &header)
    {
        // 1. Process piggybacked ACKs from the remote side
        //    The remote is telling us which of OUR packets it received
        if (header.ack > 0)
        {
            ack_packet(header.ack);

            // Process bitmap: bit N = ack - (N+1)
            for (int i = 0; i < 32; ++i)
            {
                if (header.ack_bitmap & (1u << i))
                {
                    uint64_t acked_seq = header.ack - static_cast<uint64_t>(i + 1);
                    if (acked_seq > 0)
                    {
                        ack_packet(acked_seq);
                    }
                }
            }
        }

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

            if (shift <= 32)
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

    void udp_connection::ack_packet(uint64_t sequence)
    {
        size_t index = sequence % SEQUENCE_BUFFER_SIZE;
        auto &entry = m_send_buffer[index];

        if (entry.active && entry.sequence == sequence && !entry.acked)
        {
            entry.acked = true;

            // Compute RTT only from non-retransmitted packets (Karn's algorithm)
            if (entry.retransmit_count == 0)
            {
                auto now = std::chrono::steady_clock::now();
                auto sample = std::chrono::duration_cast<std::chrono::microseconds>(now - entry.send_time).count();
                if (sample > 0)
                {
                    update_rtt(sample);
                }
            }
        }
    }

    bool udp_connection::is_sequence_received(uint64_t sequence) const
    {
        if (sequence == m_remote_sequence)
        {
            return true;
        }

        uint64_t diff = m_remote_sequence - sequence;
        if (diff > 0 && diff <= 32)
        {
            return (m_recv_bitmap & (1u << (diff - 1))) != 0;
        }

        // Outside bitmap window — treat as received (too old to care)
        return true;
    }

    void udp_connection::record_received(uint64_t sequence)
    {
        uint64_t diff = m_remote_sequence - sequence;
        if (diff > 0 && diff <= 32)
        {
            m_recv_bitmap |= (1u << (diff - 1));
        }
    }

    // --- Reliability ---

    int udp_connection::collect_lost(std::chrono::steady_clock::time_point now, lost_packet_info *out, int max_count)
    {
        int count = 0;

        // Scan the active window of the send buffer
        uint64_t oldest = (m_local_sequence > SEQUENCE_BUFFER_SIZE) ? m_local_sequence - SEQUENCE_BUFFER_SIZE : 1;

        for (uint64_t seq = oldest; seq < m_local_sequence && count < max_count; ++seq)
        {
            size_t idx = seq % SEQUENCE_BUFFER_SIZE;
            auto &entry = m_send_buffer[idx];

            if (!entry.active || entry.acked || entry.sequence != seq)
                continue;
            if (!(entry.flags & FLAG_RELIABLE))
                continue;
            if (entry.retransmit_count >= MAX_RETRANSMISSIONS)
                continue;

            auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - entry.send_time);
            if (elapsed.count() < m_rto)
                continue;

            // This packet needs retransmission
            ++entry.retransmit_count;
            entry.send_time = now; // reset timer for next potential retransmit

            // Report to application with original header + refreshed ack state
            out[count].sequence = seq;
            out[count].header = entry.header_copy;
            out[count].header.ack = m_remote_sequence;
            out[count].header.ack_bitmap = m_recv_bitmap;
            out[count].retransmit_count = entry.retransmit_count;
            ++count;
        }

        return count;
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
            constexpr double alpha = 0.125;
            constexpr double beta = 0.25;

            double diff = std::abs(m_srtt - sample);
            m_rttvar = (1.0 - beta) * m_rttvar + beta * diff;
            m_srtt = (1.0 - alpha) * m_srtt + alpha * sample;
        }

        // RTO = SRTT + max(G, 4 * RTTVAR), G = clock granularity (1 ms = 1000 us)
        m_rto = static_cast<int64_t>(m_srtt + std::max(1000.0, 4.0 * m_rttvar));
        m_rto = std::clamp(m_rto, MIN_RTO_US, MAX_RTO_US);
    }

} // namespace entanglement
