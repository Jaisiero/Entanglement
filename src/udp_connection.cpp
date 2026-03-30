#include "udp_connection.h"
#include "channel_manager.h"
#include "udp_socket.h"
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
        m_cached_now = now;
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

        // Clear receiver-side reassembler (calls on_expired for each active entry so app frees buffers)
        m_reassembler.clear();

        // Reset backpressure flags
        m_fragment_backpressured = false;
        m_backpressure_sent = false;

        // Reset oldest un-acked tracking
        m_oldest_unacked_seq = 1;

        // Reset ordered delivery diagnostics
        m_ordered_gaps_skipped = 0;

        // Reset ordered stall timers
        for (size_t i = 0; i < MAX_CHANNELS; ++i)
            m_ordered_last_advance[i] = now;

        // Clear automatic retransmission buffer (keep allocation)
        if (m_retransmit)
            m_retransmit->clear();

        // Reset ordered delivery state — expected sequence starts at 1
        // (sender's first channel_sequence = ++0 = 1)
        for (size_t i = 0; i < MAX_CHANNELS; ++i)
        {
            m_recv_channel_seq[i] = 1;
        }
        for (auto &op : m_ordered_buffer)
            op.active = false;
        for (auto &om : m_ordered_msg_buffer)
            om.active = false;
    }

    // --- Sending side ---

    uint64_t udp_connection::next_sequence()
    {
        return m_local_sequence++;
    }

    void udp_connection::prepare_header(packet_header &header, bool reliable,
                                        std::chrono::steady_clock::time_point send_time)
    {
        header.magic = PROTOCOL_MAGIC;
        header.version = PROTOCOL_VERSION;

        uint64_t seq = next_sequence();
        header.sequence = seq;
        header.ack = m_remote_sequence;
        header.ack_bitmap = static_cast<uint32_t>(m_recv_bitmap); // truncate to 32-bit for wire

        // For fragment packets: keep whatever channel_sequence the caller set
        // (non-zero for fragment 0 of ordered messages, zero for all others).
        // For simple packets: preserve caller-set value (retransmissions on
        // RELIABLE_ORDERED), otherwise auto-assign the next monotonic value.
        if (!(header.flags & FLAG_FRAGMENT) && header.channel_sequence == 0)
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
        entry.channel_sequence = header.channel_sequence;
        entry.message_id = 0;
        entry.fragment_index = 0;
        entry.send_time =
            (send_time != std::chrono::steady_clock::time_point{}) ? send_time : std::chrono::steady_clock::now();
        m_last_send_time = entry.send_time;

        // Track in congestion window
        m_cc.on_packet_sent();
    }

    // --- Receiving side ---

    bool udp_connection::process_incoming(const packet_header &header, std::chrono::steady_clock::time_point recv_time)
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

        // Update receive timestamp — use cached time if available
        m_last_recv_time =
            (recv_time != std::chrono::steady_clock::time_point{}) ? recv_time : std::chrono::steady_clock::now();

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

            if (shift <= RECV_BITMAP_WIDTH)
            {
                m_recv_bitmap <<= shift;
                // The old m_remote_sequence becomes bit (shift-1)
                if (m_remote_sequence > 0)
                {
                    m_recv_bitmap |= (1ull << (shift - 1));
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
                auto now = (m_cached_now != std::chrono::steady_clock::time_point{}) ? m_cached_now
                                                                                     : std::chrono::steady_clock::now();
                auto sample = std::chrono::duration_cast<std::chrono::microseconds>(now - entry.send_time).count();
                if (sample > 0)
                {
                    update_rtt(sample);
                }
            }

            // Notify congestion controller and update pacing
            m_cc.on_packet_acked();
            m_cc.update_pacing(m_srtt);

            // Free retransmit entry for this sequence
            if (m_retransmit)
                m_retransmit->remove(sequence);

            // Advance oldest-unacked tracker
            if (sequence == m_oldest_unacked_seq)
            {
                // Scan forward to find the next un-acked entry
                uint64_t s = sequence + 1;
                while (s < m_local_sequence)
                {
                    size_t si = s % SEQUENCE_BUFFER_SIZE;
                    auto &se = m_send_buffer[si];
                    if (se.active && se.sequence == s && !se.acked)
                        break;
                    ++s;
                }
                m_oldest_unacked_seq = s;
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
        if (diff > 0 && diff <= RECV_BITMAP_WIDTH)
        {
            return (m_recv_bitmap & (1ull << (diff - 1))) != 0;
        }

        // Outside bitmap window — treat as received (too old to care)
        return true;
    }

    void udp_connection::record_received(uint64_t sequence)
    {
        uint64_t diff = m_remote_sequence - sequence;
        if (diff > 0 && diff <= RECV_BITMAP_WIDTH)
        {
            m_recv_bitmap |= (1ull << (diff - 1));
        }
    }

    // --- Reliability ---

    int udp_connection::collect_losses(std::chrono::steady_clock::time_point now, lost_packet_info *out, int max_count)
    {
        int count = 0;

        // Fast path: nothing in flight → nothing to scan
        if (m_oldest_unacked_seq >= m_local_sequence)
            return 0;

        // Scan from oldest un-acked (optimization: skip already-resolved entries)
        uint64_t oldest = (m_local_sequence > SEQUENCE_BUFFER_SIZE)
                              ? (std::max)(m_oldest_unacked_seq, m_local_sequence - SEQUENCE_BUFFER_SIZE)
                              : m_oldest_unacked_seq;

        // Track the new frontier: first sequence that is still active and un-acked.
        // This prevents the scan range from growing when timed-out entries are
        // deactivated but m_oldest_unacked_seq was never advanced past them.
        uint64_t new_oldest = m_local_sequence;

        for (uint64_t seq = oldest; seq < m_local_sequence; ++seq)
        {
            size_t idx = seq % SEQUENCE_BUFFER_SIZE;
            auto &entry = m_send_buffer[idx];

            if (!entry.active || entry.acked || entry.sequence != seq)
                continue;

            auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - entry.send_time);
            if (elapsed.count() < m_rto)
            {
                // Still in flight — this is the earliest un-resolved entry
                if (seq < new_oldest)
                    new_oldest = seq;
                continue;
            }

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
            {
                // Can't report this loss yet — it stays active for next scan
                if (seq < new_oldest)
                    new_oldest = seq;
                continue;
            }

            // Report loss metadata to application
            out[count].sequence = seq;
            out[count].flags = entry.flags;
            out[count].channel_id = entry.channel_id;
            out[count].shard_id = entry.shard_id;
            out[count].payload_size = entry.payload_size;
            out[count].channel_sequence = entry.channel_sequence;
            out[count].message_id = entry.message_id;
            out[count].fragment_index = entry.fragment_index;
            out[count].fragment_count = entry.fragment_count;
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

        // Advance oldest-unacked past all entries resolved in this scan
        m_oldest_unacked_seq = new_oldest;

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

    bool udp_connection::needs_ack_flush(std::chrono::steady_clock::time_point now) const
    {
        if (m_state != connection_state::CONNECTED)
            return false;
        // Only flush if we received data AFTER our last send (pending acks)
        if (m_last_recv_time <= m_last_send_time)
            return false;
        auto since_send = std::chrono::duration_cast<std::chrono::microseconds>(now - m_last_send_time).count();
        return since_send > ACK_FLUSH_INTERVAL_US;
    }

    void udp_connection::send_ack_flush(udp_socket &socket, const endpoint_key &dest)
    {
        packet_header header{};
        header.magic = PROTOCOL_MAGIC;
        header.version = PROTOCOL_VERSION;
        header.sequence = 0; // ack-only — no congestion tracking
        header.ack = m_remote_sequence;
        header.ack_bitmap = static_cast<uint32_t>(m_recv_bitmap);
        header.flags = 0;
        header.payload_size = 0;
        header.channel_id = 0;
        header.shard_id = 0;
        header.channel_sequence = 0;
        socket.send_packet(header, nullptr, dest);
        m_last_send_time = std::chrono::steady_clock::now();
    }

    // --- Fragmentation (sender side) ---

    error_code udp_connection::register_pending_message(uint32_t message_id, uint8_t fragment_count)
    {
        for (auto &pm : m_pending_messages)
        {
            if (!pm.active)
            {
                pm.active = true;
                pm.message_id = message_id;
                pm.fragment_count = fragment_count;
                pm.acked_count = 0;
                return error_code::ok;
            }
        }
        return error_code::pool_full;
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
            m_srtt + (std::max)(static_cast<double>(CLOCK_GRANULARITY_US), RTT_VARIANCE_MULTIPLIER * m_rttvar));
        m_rto = (std::clamp)(m_rto, MIN_RTO_US, MAX_RTO_US);
    }

    // --- Ordered delivery (receive-side hold-back for RELIABLE_ORDERED) ---

    bool udp_connection::buffer_ordered_packet(const packet_header &hdr, const uint8_t *data, uint16_t size)
    {
        for (auto &slot : m_ordered_buffer)
        {
            if (!slot.active)
            {
                slot.active = true;
                slot.channel_sequence = hdr.channel_sequence;
                slot.header = hdr;
                slot.payload_size = size;
                if (size > 0 && data)
                    std::memcpy(slot.payload, data, size);
                return true;
            }
        }
        return false; // buffer full
    }

    ordered_pending_packet *udp_connection::peek_next_ordered(uint8_t ch_id)
    {
        uint32_t expected = m_recv_channel_seq[ch_id];
        for (auto &slot : m_ordered_buffer)
        {
            if (slot.active && slot.header.channel_id == ch_id && slot.channel_sequence == expected)
                return &slot;
        }
        return nullptr;
    }

    int udp_connection::skip_ordered_gap(uint8_t ch_id)
    {
        // Find the lowest buffered channel_sequence across both simple and message buffers
        uint32_t lowest = 0;
        bool found = false;
        for (auto &slot : m_ordered_buffer)
        {
            if (slot.active && slot.header.channel_id == ch_id)
            {
                if (!found || sequence_less_than(slot.channel_sequence, lowest))
                {
                    lowest = slot.channel_sequence;
                    found = true;
                }
            }
        }
        for (auto &slot : m_ordered_msg_buffer)
        {
            if (slot.active && slot.channel_id == ch_id)
            {
                if (!found || sequence_less_than(slot.channel_sequence, lowest))
                {
                    lowest = slot.channel_sequence;
                    found = true;
                }
            }
        }
        if (!found)
            return 0; // nothing buffered

        // Advance expected to the lowest buffered sequence
        m_recv_channel_seq[ch_id] = lowest;
        ++m_ordered_gaps_skipped;
        return 1;
    }

    // --- Ordered delivery for fragmented messages ---

    bool udp_connection::buffer_ordered_message(const endpoint_key &sender, uint32_t message_id, uint8_t channel_id,
                                                uint8_t *data, size_t total_size, uint32_t channel_sequence)
    {
        for (auto &slot : m_ordered_msg_buffer)
        {
            if (!slot.active)
            {
                slot.active = true;
                slot.channel_sequence = channel_sequence;
                slot.channel_id = channel_id;
                slot.sender = sender;
                slot.message_id = message_id;
                slot.data = data;
                slot.total_size = total_size;
                return true;
            }
        }
        return false; // buffer full
    }

    ordered_pending_message *udp_connection::peek_next_ordered_message(uint8_t ch_id)
    {
        uint32_t expected = m_recv_channel_seq[ch_id];
        for (auto &slot : m_ordered_msg_buffer)
        {
            if (slot.active && slot.channel_id == ch_id && slot.channel_sequence == expected)
                return &slot;
        }
        return nullptr;
    }

    // --- Unified send (shared by client and server) ---

    int udp_connection::send_payload(udp_socket &socket, const channel_manager &channels, const void *data, size_t size,
                                     uint8_t flags, uint8_t channel_id, const endpoint_key &dest,
                                     uint32_t *out_message_id, uint64_t *out_sequence, uint32_t channel_sequence,
                                     uint32_t *out_channel_sequence)
    {
        // Single-packet path (no fragmentation overhead)
        if (size <= MAX_PAYLOAD_SIZE)
        {
            if (out_message_id)
                *out_message_id = 0;
            packet_header header{};
            header.flags = flags;
            header.channel_id = channel_id;
            header.channel_sequence = channel_sequence; // non-zero preserves value in prepare_header
            header.payload_size = static_cast<uint16_t>(size);
            bool reliable = channels.is_reliable(channel_id);
            prepare_header(header, reliable);

            if (out_sequence)
                *out_sequence = header.sequence;
            if (out_channel_sequence)
                *out_channel_sequence = header.channel_sequence;

            // Store for auto-retransmit (reliable channels only)
            if (reliable && m_retransmit)
                m_retransmit->store(header.sequence, data, static_cast<uint16_t>(size), flags, channel_id,
                                    header.channel_sequence, 0, 0, 0);

            return socket.send_packet(header, data, dest);
        }

        // Fragmented send — check backpressure from remote
        if (m_fragment_backpressured)
            return static_cast<int>(error_code::backpressured);

        const uint8_t *src = static_cast<const uint8_t *>(data);
        uint8_t fragment_count = static_cast<uint8_t>((size + MAX_FRAGMENT_PAYLOAD - 1) / MAX_FRAGMENT_PAYLOAD);
        if (fragment_count == 0)
            return static_cast<int>(error_code::invalid_argument);

        uint32_t message_id = next_message_id();
        int total_sent = 0;

        // For RELIABLE_ORDERED channels, allocate a single channel_sequence for the
        // whole message — carried on fragment 0 so the receiver can enforce ordering.
        // If the caller supplied a non-zero channel_sequence (retransmission), reuse it.
        uint32_t msg_channel_seq = 0;
        if (channels.is_ordered(channel_id))
        {
            msg_channel_seq = (channel_sequence != 0) ? channel_sequence : ++m_channel_sequences[channel_id];
        }

        if (out_channel_sequence)
            *out_channel_sequence = msg_channel_seq;

        for (uint8_t i = 0; i < fragment_count; ++i)
        {
            size_t offset = static_cast<size_t>(i) * MAX_FRAGMENT_PAYLOAD;
            size_t chunk = (std::min)(MAX_FRAGMENT_PAYLOAD, size - offset);

            uint32_t frag_chan_seq = (i == 0) ? msg_channel_seq : 0;
            int result = send_fragment(socket, channels, message_id, i, fragment_count, src + offset, chunk, flags,
                                       channel_id, dest, frag_chan_seq);
            if (result <= 0)
                return result;
            total_sent += static_cast<int>(chunk);
        }

        register_pending_message(message_id, fragment_count);

        if (out_message_id)
            *out_message_id = message_id;

        return total_sent;
    }

    int udp_connection::send_fragment(udp_socket &socket, const channel_manager &channels, uint32_t message_id,
                                      uint8_t index, uint8_t count, const void *data, size_t size, uint8_t flags,
                                      uint8_t channel_id, const endpoint_key &dest, uint32_t channel_sequence)
    {
        fragment_header fhdr{message_id, index, count};

        packet_header header{};
        header.flags = flags | FLAG_FRAGMENT;
        header.channel_id = channel_id;
        header.channel_sequence = channel_sequence; // non-zero only for fragment 0 of ordered messages
        header.payload_size = static_cast<uint16_t>(FRAGMENT_HEADER_SIZE + size);

        bool reliable = channels.is_reliable(channel_id);
        prepare_header(header, reliable);

        // Tag fragment metadata for loss tracking
        size_t idx = header.sequence % SEQUENCE_BUFFER_SIZE;
        auto &entry = m_send_buffer[idx];
        entry.message_id = message_id;
        entry.fragment_index = index;
        entry.fragment_count = count;

        // Scatter-gather: [packet_header] + [fragment_header] + [user data]
        const void *segments[2] = {&fhdr, data};
        size_t seg_sizes[2] = {FRAGMENT_HEADER_SIZE, size};

        // Store for auto-retransmit (reliable channels only)
        if (reliable && m_retransmit)
            m_retransmit->store(header.sequence, data, static_cast<uint16_t>(size), flags, channel_id, channel_sequence,
                                message_id, index, count);

        return socket.send_packet_gather(header, segments, seg_sizes, 2, dest);
    }

    // --- Ordered stall detection ---

    void udp_connection::check_ordered_stalls(const channel_manager &channels,
                                              std::chrono::steady_clock::time_point now)
    {
        // Fast path: skip entirely if no ordered channels exist
        size_t ord_count = channels.ordered_channel_count();
        if (ord_count == 0)
            return;
        const uint8_t *ord_ids = channels.ordered_channel_ids();

        for (size_t i = 0; i < ord_count; ++i)
        {
            uint8_t ch_id = ord_ids[i];

            // Check if there are any buffered entries for this channel
            bool has_buffered = false;
            for (auto &slot : m_ordered_buffer)
            {
                if (slot.active && slot.header.channel_id == ch_id)
                {
                    has_buffered = true;
                    break;
                }
            }
            if (!has_buffered)
            {
                for (auto &slot : m_ordered_msg_buffer)
                {
                    if (slot.active && slot.channel_id == ch_id)
                    {
                        has_buffered = true;
                        break;
                    }
                }
            }
            if (!has_buffered)
                continue; // no stall — nothing buffered

            auto stall_us =
                std::chrono::duration_cast<std::chrono::microseconds>(now - m_ordered_last_advance[ch_id]).count();
            if (stall_us >= m_ordered_stall_timeout_us)
            {
                skip_ordered_gap(ch_id);
                drain_ordered(ch_id);
            }
        }
    }

    // --- Shared ordered-delivery helpers ---

    void udp_connection::drain_ordered(uint8_t channel_id)
    {
        while (true)
        {
            if (auto *pkt = peek_next_ordered(channel_id))
            {
                if (m_on_ordered_pkt_deliver)
                    m_on_ordered_pkt_deliver(pkt->header, pkt->payload, pkt->payload_size);
                advance_ordered_seq(channel_id);
                release_ordered_packet(pkt);
                continue;
            }
            if (auto *msg = peek_next_ordered_message(channel_id))
            {
                if (m_on_ordered_msg_deliver)
                    m_on_ordered_msg_deliver(msg->sender, msg->message_id, msg->channel_id, msg->data, msg->total_size);
                advance_ordered_seq(channel_id);
                release_ordered_message(msg);
                continue;
            }
            break;
        }
    }

    bool udp_connection::deliver_ordered(const packet_header &header, const uint8_t *payload, uint16_t size,
                                         const channel_manager &channels)
    {
        if (!channels.is_ordered(header.channel_id))
            return false;

        if (is_ordered_next(header.channel_id, header.channel_sequence))
        {
            // Expected packet — deliver immediately
            if (m_on_ordered_pkt_deliver)
                m_on_ordered_pkt_deliver(header, payload, size);
            advance_ordered_seq(header.channel_id);
            drain_ordered(header.channel_id);
        }
        else if (sequence_greater_than(header.channel_sequence, expected_ordered_seq(header.channel_id)))
        {
            // Future packet — buffer for later delivery
            if (!buffer_ordered_packet(header, payload, size))
            {
                // Buffer full — force-skip gap and retry
                skip_ordered_gap(header.channel_id);
                drain_ordered(header.channel_id);
                buffer_ordered_packet(header, payload, size);
            }
        }
        else
        {
            // Stale packet (channel_sequence < expected) — deliver anyway.
            // This can happen when a gap-skip advanced the expected sequence
            // past a lost packet, and the retransmission arrives later with
            // the original (now-stale) channel_sequence.  Dropping it would
            // permanently lose the payload; delivering out-of-order is safe
            // because the gap-skip already broke strict ordering.
            if (m_on_ordered_pkt_deliver)
                m_on_ordered_pkt_deliver(header, payload, size);
        }
        return true;
    }

    void udp_connection::install_ordered_complete_wrapper(const channel_manager *channels, on_message_complete app_cb)
    {
        m_on_ordered_msg_deliver = app_cb; // store for drain_ordered
        m_reassembler.set_on_complete(
            [this, channels, app_cb](const endpoint_key &sender, uint32_t msg_id, uint8_t ch_id, uint8_t *data,
                                     size_t total_size)
            {
                uint32_t chan_seq = m_reassembler.last_completed_channel_sequence();
                if (channels->is_ordered(ch_id) && chan_seq > 0)
                {
                    if (is_ordered_next(ch_id, chan_seq))
                    {
                        // Expected message — deliver immediately
                        if (app_cb)
                            app_cb(sender, msg_id, ch_id, data, total_size);
                        advance_ordered_seq(ch_id);
                        drain_ordered(ch_id);
                    }
                    else if (sequence_greater_than(chan_seq, expected_ordered_seq(ch_id)))
                    {
                        // Future message — buffer
                        if (!buffer_ordered_message(sender, msg_id, ch_id, data, total_size, chan_seq))
                        {
                            // Buffer full — force skip and drain, then retry
                            skip_ordered_gap(ch_id);
                            drain_ordered(ch_id);
                            buffer_ordered_message(sender, msg_id, ch_id, data, total_size, chan_seq);
                        }
                    }
                    // Stale — deliver anyway to avoid leaking the buffer
                    else if (app_cb)
                    {
                        app_cb(sender, msg_id, ch_id, data, total_size);
                    }
                }
                else
                {
                    if (app_cb)
                        app_cb(sender, msg_id, ch_id, data, total_size);
                }
            });
    }

    // --- Automatic retransmission ---

    void udp_connection::enable_auto_retransmit()
    {
        if (!m_retransmit)
            m_retransmit = std::make_unique<retransmit_buffer>();
    }

    bool udp_connection::try_auto_retransmit(const lost_packet_info &loss, udp_socket &socket,
                                             const channel_manager &channels, const endpoint_key &dest)
    {
        if (!m_retransmit)
            return false;

        // Respect congestion window: don't pile up retransmissions beyond cwnd.
        // The loss already decremented in_flight; this retransmit would re-increment it.
        if (!m_cc.can_send())
            return false;

        auto *entry = m_retransmit->find(loss.sequence);
        if (!entry || entry->attempts >= MAX_RETRANSMIT_ATTEMPTS)
            return false;

        entry->attempts++;

        if (loss.message_id != 0)
        {
            // Fragment retransmit: resend via send_fragment (gets new sequence, preserves message_id)
            uint8_t base_flags = entry->flags & ~FLAG_FRAGMENT; // send_fragment adds FLAG_FRAGMENT
            int saved_attempts = entry->attempts;
            int result =
                send_fragment(socket, channels, loss.message_id, loss.fragment_index, loss.fragment_count, entry->data,
                              entry->data_size, base_flags, entry->channel_id, dest, entry->channel_sequence);
            if (result > 0)
            {
                // send_fragment stored a new retransmit entry with attempts=0.
                // Propagate the real attempt count to prevent infinite retransmissions.
                entry->active = false;
                for (auto &e : m_retransmit->entries)
                {
                    if (e.active && e.message_id == loss.message_id && e.fragment_index == loss.fragment_index &&
                        e.sequence != loss.sequence)
                    {
                        e.attempts = saved_attempts;
                        break;
                    }
                }
                return true;
            }
        }
        else
        {
            // Simple packet retransmit
            packet_header header{};
            header.flags = entry->flags;
            header.channel_id = entry->channel_id;
            header.channel_sequence = entry->channel_sequence; // preserve for ordered channels
            header.payload_size = entry->data_size;
            bool reliable = channels.is_reliable(entry->channel_id);
            prepare_header(header, reliable);

            // prepare_header + the retransmit store in send_payload won't fire here
            // because we're calling send_packet directly. Store manually.
            // (The new entry in the retransmit buffer is created by store below.)

            int result = socket.send_packet(header, entry->data, dest);
            if (result > 0)
            {
                // Move the retransmit entry to track the new sequence
                int attempts = entry->attempts;
                uint16_t data_size = entry->data_size;
                uint8_t flags_copy = entry->flags;
                uint8_t ch_id = entry->channel_id;
                uint32_t ch_seq = entry->channel_sequence;

                // Deactivate old entry
                entry->active = false;

                // Store under new sequence (preserving attempt count)
                auto *new_entry = m_retransmit->store(header.sequence, nullptr, 0, flags_copy, ch_id, ch_seq, 0, 0, 0);
                if (new_entry)
                {
                    // Copy payload from old entry (already in buffer, just update the new slot)
                    std::memcpy(new_entry->data, entry->data, data_size);
                    new_entry->data_size = data_size;
                    new_entry->attempts = attempts;
                }
                return true;
            }
        }

        return false;
    }

} // namespace entanglement
