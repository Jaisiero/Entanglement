#include "udp_connection.h"
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
    }

    // --- Sending side ---

    uint64_t udp_connection::next_sequence()
    {
        uint64_t seq = m_local_sequence++;
        size_t index = seq % SEQUENCE_BUFFER_SIZE;

        auto &entry = m_send_buffer[index];
        entry.sequence = seq;
        entry.acked = false;
        entry.active = true;

        return seq;
    }

    void udp_connection::prepare_header(packet_header &header)
    {
        header.magic = PROTOCOL_MAGIC;
        header.version = PROTOCOL_VERSION;
        header.sequence = next_sequence();
        header.ack = m_remote_sequence;
        header.ack_bitmap = m_recv_bitmap;
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

        if (entry.active && entry.sequence == sequence)
        {
            entry.acked = true;
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

} // namespace entanglement
