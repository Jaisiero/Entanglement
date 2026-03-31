#include "server_worker.h"
#include <algorithm>
#include <cstring>
#include <iostream>

namespace entanglement
{

    server_worker::server_worker() = default;

    void server_worker::init(size_t pool_capacity, udp_socket *socket, channel_manager *channels,
                             const std::atomic<bool> *running_flag, int recv_queue_count)
    {
        m_socket = socket;
        m_channels = channels;
        m_running = running_flag;
        m_pool_capacity = pool_capacity;

        m_pool = std::make_unique<udp_connection[]>(pool_capacity);
        m_free_stack = std::make_unique<uint16_t[]>(pool_capacity);
        m_free_top = -1;
        m_free_initialized = false;

        // Create one SPSC recv queue per receiver thread (zero-copy via write_slot/read_slot)
        if (recv_queue_count < 1)
            recv_queue_count = 1;
        m_recv_queues.clear();
        m_recv_queues.reserve(static_cast<size_t>(recv_queue_count));
        for (int i = 0; i < recv_queue_count; ++i)
            m_recv_queues.push_back(std::make_unique<recv_queue_t>());

        m_send_queue = std::make_unique<spsc_queue<send_command, WORKER_SEND_QUEUE_SIZE>>();
    }

    // -----------------------------------------------------------------------
    // Queue interface
    // -----------------------------------------------------------------------

    bool server_worker::enqueue_packet(const packet_header &hdr, const uint8_t *payload, uint16_t payload_size,
                                       const endpoint_key &sender, int queue_id)
    {
        auto *slot = m_recv_queues[static_cast<size_t>(queue_id)]->write_slot();
        if (!slot)
            return false; // Queue full
        slot->header = hdr;
        slot->sender = sender;
        if (payload_size > 0)
            std::memcpy(slot->payload, payload, payload_size);
        m_recv_queues[static_cast<size_t>(queue_id)]->commit_write();
        return true;
    }
    bool server_worker::enqueue_send(send_command &&cmd)
    {
        return m_send_queue->try_push(std::move(cmd));
    }

    // -----------------------------------------------------------------------
    // Polling / processing
    // -----------------------------------------------------------------------

    int server_worker::poll_local(int max_packets)
    {
        // Cache timestamp for this batch — used by send paths to avoid
        // per-packet steady_clock::now() calls.
        m_cached_now = std::chrono::steady_clock::now();

        // 1. Process cross-thread send commands first
        flush_send_queue();

        // 2. Dequeue and process received datagrams (drain all recv queues round-robin)
        int count = 0;
        if (m_recv_queues.size() == 1)
        {
            // Fast path: single recv queue (common case) — zero-copy read
            auto &q = *m_recv_queues[0];
            while (count < max_packets)
            {
                auto *slot = q.read_slot();
                if (!slot)
                    break;
                process_datagram(slot->header, slot->payload, slot->sender);
                q.commit_read();
                ++count;
            }
        }
        else
        {
            // Multi-queue: drain all queues round-robin
            bool any = true;
            while (count < max_packets && any)
            {
                any = false;
                for (auto &qp : m_recv_queues)
                {
                    if (count >= max_packets)
                        break;
                    auto *slot = qp->read_slot();
                    if (slot)
                    {
                        process_datagram(slot->header, slot->payload, slot->sender);
                        qp->commit_read();
                        ++count;
                        any = true;
                    }
                }
            }
        }
        return count;
    }

    void server_worker::process_datagram(const packet_header &header, const uint8_t *payload,
                                         const endpoint_key &sender)
    {
        // Control packets
        if ((header.flags & FLAG_CONTROL) && header.payload_size >= 1)
        {
            handle_control(sender, header, payload, header.payload_size);
            return;
        }

        // Data packets — only from connected peers
        udp_connection *conn = find(sender);
        if (!conn || conn->state() != connection_state::CONNECTED)
            return;

        conn->set_cached_now(m_cached_now);
        bool is_new = conn->process_incoming(header, m_cached_now);
        if (!is_new)
            return;

        // Fragmented data
        if ((header.flags & FLAG_FRAGMENT) && header.payload_size > FRAGMENT_HEADER_SIZE)
        {
            fragment_header fhdr;
            std::memcpy(&fhdr, payload, FRAGMENT_HEADER_SIZE);
            auto frag_result = conn->reassembler().process_fragment(
                sender, header.channel_id, fhdr, payload + FRAGMENT_HEADER_SIZE,
                header.payload_size - FRAGMENT_HEADER_SIZE, header.channel_sequence);

            // Back-pressure if reassembler is under pressure
            if (frag_result == fragment_result::slots_full ||
                conn->reassembler().usage_percent() >= BACKPRESSURE_HIGH_WATERMARK)
            {
                if (!conn->backpressure_sent())
                {
                    uint8_t bp[2] = {CONTROL_BACKPRESSURE, 0};
                    send_control_payload_to(conn, bp, 2, sender);
                    conn->set_backpressure_sent(true);
                }
            }
            return;
        }

        // Normal (non-fragment) data delivery
        if (is_new && m_on_client_data_received)
        {
            if (!conn->deliver_ordered(header, payload, header.payload_size, *m_channels))
            {
                m_on_client_data_received(header, payload, header.payload_size, sender);
            }
        }
    }

    // -----------------------------------------------------------------------
    // Update — heartbeats, timeouts, losses, reassembly cleanup
    // -----------------------------------------------------------------------

    int server_worker::update(on_server_packet_lost loss_callback)
    {
        auto now = std::chrono::steady_clock::now();
        m_cached_now = now;
        uint32_t tick = m_tick_counter++;

        // Stagger period for expensive per-connection operations.
        // At 120 Hz server tick rate, period=4 means each connection runs
        // cleanup/stall/backpressure every ~33 ms — well within timeout margins.
        constexpr uint32_t STAGGER_PERIOD = 4;

        endpoint_key timed_out[MAX_TIMEOUTS_PER_UPDATE];
        int timeout_count = 0;

        for (auto &[key, idx] : m_index)
        {
            auto &conn = m_pool[idx];
            conn.set_cached_now(now);

            if (conn.has_timed_out(now))
            {
                if (timeout_count < MAX_TIMEOUTS_PER_UPDATE)
                    timed_out[timeout_count++] = key;
                continue;
            }

            if (conn.needs_heartbeat(now))
                send_control_to(&conn, CONTROL_HEARTBEAT, key);

            // Flush pending ACKs promptly so the remote's RTT isn't inflated
            if (conn.needs_ack_flush(now))
                conn.send_ack_flush(*m_socket, key);

            // Loss detection
            if (loss_callback || m_on_packet_lost)
            {
                auto &cb = loss_callback ? loss_callback : m_on_packet_lost;
                lost_packet_info lost[MAX_LOSSES_PER_UPDATE];
                int loss_count = conn.collect_losses(now, lost, MAX_LOSSES_PER_UPDATE);
                for (int l = 0; l < loss_count; ++l)
                {
                    if (conn.auto_retransmit_enabled() &&
                        conn.try_auto_retransmit(lost[l], *m_socket, *m_channels, key))
                        continue;
                    cb(lost[l], key);
                }
            }
            else
            {
                lost_packet_info lost[MAX_LOSSES_PER_UPDATE];
                int loss_count = conn.collect_losses(now, lost, MAX_LOSSES_PER_UPDATE);
                if (conn.auto_retransmit_enabled())
                {
                    for (int l = 0; l < loss_count; ++l)
                        conn.try_auto_retransmit(lost[l], *m_socket, *m_channels, key);
                }
            }

            // --- Staggered expensive operations (run every STAGGER_PERIOD ticks) ---
            if ((idx % STAGGER_PERIOD) != (tick % STAGGER_PERIOD))
                continue;

            // Reassembly cleanup
            auto &ra = conn.reassembler();
            ra.cleanup_stale(now, ra.reassembly_timeout());

            // Ordered-delivery stall check
            conn.check_ordered_stalls(*m_channels, now);

            // Back-pressure relief
            if (conn.backpressure_sent() && ra.usage_percent() < BACKPRESSURE_LOW_WATERMARK)
            {
                uint8_t available = static_cast<uint8_t>(ra.capacity() - ra.pending_count());
                uint8_t bp[2] = {CONTROL_BACKPRESSURE, available};
                send_control_payload_to(&conn, bp, 2, key);
                conn.set_backpressure_sent(false);
            }
        }

        for (int i = 0; i < timeout_count; ++i)
        {
            std::string addr_str = endpoint_address_string(timed_out[i]);
            if (m_verbose)
                std::cout << "[server] Client timed out: " << addr_str << ":" << timed_out[i].port << std::endl;

            if (m_on_client_disconnected)
                m_on_client_disconnected(timed_out[i], addr_str, timed_out[i].port);

            disconnect_client(timed_out[i]);
        }

        return timeout_count;
    }

    // -----------------------------------------------------------------------
    // Sending
    // -----------------------------------------------------------------------

    int server_worker::send_raw_to(packet_header &header, const void *payload, const endpoint_key &dest)
    {
        udp_connection *conn = find(dest);
        if (conn && conn->state() == connection_state::CONNECTED)
        {
            // Raw sends (server echoes) are always transport-unreliable:
            // if the echo is lost, the client retransmits the original message
            // and the server generates a fresh echo.  Marking them reliable
            // would cause the server's CC to fire on_packet_lost for every
            // dropped ACK, collapsing the congestion window under loss.
            conn->prepare_header(header, /*reliable=*/false, m_cached_now);
        }
        return m_socket->send_packet(header, payload, dest);
    }

    int server_worker::send_to(const void *data, size_t size, uint8_t channel_id, const endpoint_key &dest,
                               uint8_t flags, uint32_t *out_message_id)
    {
        udp_connection *conn = find(dest);
        if (!conn || conn->state() == connection_state::DISCONNECTED)
            return static_cast<int>(error_code::not_connected);
        return conn->send_payload(*m_socket, *m_channels, data, size, flags, channel_id, dest, out_message_id);
    }

    int server_worker::send_fragment_to(uint32_t message_id, uint8_t fragment_index, uint8_t fragment_count,
                                        const void *data, size_t size, uint8_t flags, uint8_t channel_id,
                                        const endpoint_key &dest, uint32_t channel_sequence)
    {
        udp_connection *conn = find(dest);
        if (!conn || conn->state() == connection_state::DISCONNECTED)
            return static_cast<int>(error_code::not_connected);
        return conn->send_fragment(*m_socket, *m_channels, message_id, fragment_index, fragment_count, data, size,
                                   flags, channel_id, dest, channel_sequence);
    }

    // -----------------------------------------------------------------------
    // Cross-thread send queue processing
    // -----------------------------------------------------------------------

    void server_worker::flush_send_queue()
    {
        send_command cmd;
        while (m_send_queue->try_pop(cmd))
        {
            switch (cmd.type)
            {
                case send_command::kind::DATA:
                    send_to(cmd.data.data(), cmd.data.size(), cmd.channel_id, cmd.dest, cmd.flags, nullptr);
                    break;

                case send_command::kind::RAW:
                    send_raw_to(cmd.raw_header, cmd.data.data(), cmd.dest);
                    break;

                case send_command::kind::FRAGMENT:
                    send_fragment_to(cmd.message_id, cmd.fragment_index, cmd.fragment_count, cmd.data.data(),
                                     cmd.data.size(), cmd.flags, cmd.channel_id, cmd.dest, cmd.channel_sequence);
                    break;

                case send_command::kind::DISCONNECT:
                    disconnect_client(cmd.dest);
                    break;
            }
        }
    }

    // -----------------------------------------------------------------------
    // Connection management
    // -----------------------------------------------------------------------

    udp_connection *server_worker::find(const endpoint_key &key)
    {
        auto it = m_index.find(key);
        if (it != m_index.end())
            return &m_pool[it->second];
        return nullptr;
    }

    udp_connection *server_worker::find_or_create(const endpoint_key &key)
    {
        auto it = m_index.find(key);
        if (it != m_index.end())
            return &m_pool[it->second];

        int slot = allocate_slot();
        if (slot < 0)
            return nullptr;

        auto &conn = m_pool[slot];
        conn.reset();
        conn.set_active(true);
        conn.set_endpoint(key);

        // Set up reassembler with stored callback templates
        auto &ra = conn.reassembler();
        if (m_frag_alloc_cb)
            ra.set_on_allocate(m_frag_alloc_cb);
        if (m_app_on_message_complete)
            conn.install_ordered_complete_wrapper(m_channels, m_app_on_message_complete);
        if (m_frag_failed_cb)
            ra.set_on_failed(m_frag_failed_cb);
        if (m_reassembly_timeout_us != REASSEMBLY_TIMEOUT_US)
            ra.set_reassembly_timeout(m_reassembly_timeout_us);

        // Wire ordered delivery callback
        if (m_on_client_data_received)
        {
            auto cb = m_on_client_data_received;
            conn.set_on_ordered_packet_deliver([cb, &conn](const packet_header &h, const uint8_t *p, uint16_t s)
                                               { cb(h, p, s, conn.endpoint()); });
        }

        if (m_auto_retransmit)
            conn.enable_auto_retransmit();

        m_index[key] = static_cast<uint16_t>(slot);
        return &conn;
    }

    void server_worker::disconnect_client(const endpoint_key &key)
    {
        auto it = m_index.find(key);
        if (it != m_index.end())
        {
            uint16_t slot = it->second;
            m_pool[slot].reset();
            m_index.erase(it);

            if (m_free_initialized)
                m_free_stack[++m_free_top] = slot;
        }
    }

    void server_worker::disconnect_all()
    {
        for (auto &[key, idx] : m_index)
            m_pool[idx].reset();
        m_index.clear();
    }

    int server_worker::allocate_slot()
    {
        if (!m_free_initialized)
            init_freelist();
        if (m_free_top < 0)
            return static_cast<int>(error_code::pool_full);
        return m_free_stack[m_free_top--];
    }

    void server_worker::init_freelist()
    {
        m_free_top = static_cast<int>(m_pool_capacity) - 1;
        for (size_t i = 0; i < m_pool_capacity; ++i)
            m_free_stack[i] = static_cast<uint16_t>(i);
        m_free_initialized = true;
    }

    bool server_worker::is_fragment_throttled(const endpoint_key &key) const
    {
        auto it = m_index.find(key);
        if (it == m_index.end())
            return false;
        return m_pool[it->second].is_fragment_backpressured();
    }

    // -----------------------------------------------------------------------
    // Callbacks
    // -----------------------------------------------------------------------

    void server_worker::set_on_client_data_received(on_client_data_received cb)
    {
        m_on_client_data_received = cb;
        for (auto &[key, idx] : m_index)
        {
            auto &conn = m_pool[idx];
            if (cb)
            {
                auto callback = cb;
                conn.set_on_ordered_packet_deliver(
                    [callback, &conn](const packet_header &h, const uint8_t *p, uint16_t s)
                    { callback(h, p, s, conn.endpoint()); });
            }
            else
            {
                conn.set_on_ordered_packet_deliver(nullptr);
            }
        }
    }

    void server_worker::set_on_allocate_message(on_allocate_message cb)
    {
        m_frag_alloc_cb = cb;
        for (auto &[key, idx] : m_index)
            m_pool[idx].reassembler().set_on_allocate(cb);
    }

    void server_worker::set_on_message_complete(on_message_complete cb)
    {
        m_app_on_message_complete = cb;
        for (auto &[key, idx] : m_index)
            m_pool[idx].install_ordered_complete_wrapper(m_channels, cb);
    }

    void server_worker::set_on_message_failed(on_message_failed cb)
    {
        m_frag_failed_cb = cb;
        for (auto &[key, idx] : m_index)
            m_pool[idx].reassembler().set_on_failed(cb);
    }

    void server_worker::set_reassembly_timeout(int64_t timeout_us)
    {
        m_reassembly_timeout_us = timeout_us;
        for (auto &[key, idx] : m_index)
            m_pool[idx].reassembler().set_reassembly_timeout(timeout_us);
    }

    // -----------------------------------------------------------------------
    // Control packet handling
    // -----------------------------------------------------------------------

    void server_worker::handle_control(const endpoint_key &key, const packet_header &header, const uint8_t *payload,
                                       size_t payload_size)
    {
        uint8_t control_type = payload[0];
        udp_connection *conn = find(key);

        switch (control_type)
        {
            case CONTROL_CONNECTION_REQUEST:
            {
                if (conn)
                {
                    conn->process_incoming(header, m_cached_now);
                    send_control_to(conn, CONTROL_CONNECTION_ACCEPTED, key);
                    return;
                }

                conn = find_or_create(key);
                if (!conn)
                {
                    send_raw_control(CONTROL_CONNECTION_DENIED, key);
                    if (m_verbose)
                    {
                        std::string address = endpoint_address_string(key);
                        std::cerr << "[server] Connection denied (pool full) to " << address << ":" << key.port
                                  << std::endl;
                    }
                    return;
                }

                conn->set_state(connection_state::CONNECTED);
                conn->set_cached_now(m_cached_now);
                conn->process_incoming(header, m_cached_now);
                send_control_to(conn, CONTROL_CONNECTION_ACCEPTED, key);

                if (m_verbose || m_on_client_connected)
                {
                    std::string address = endpoint_address_string(key);
                    if (m_verbose)
                    {
                        std::cout << "[server] Client connected: " << address << ":" << key.port << " (slot "
                                  << m_index[key] << ", total: " << m_index.size() << ")" << std::endl;
                    }
                    if (m_on_client_connected)
                        m_on_client_connected(key, address, key.port);
                }
                break;
            }

            case CONTROL_DISCONNECT:
            {
                if (conn)
                {
                    conn->process_incoming(header, m_cached_now);
                    if (m_verbose || m_on_client_disconnected)
                    {
                        std::string address = endpoint_address_string(key);
                        if (m_verbose)
                            std::cout << "[server] Client disconnected: " << address << ":" << key.port << std::endl;
                        if (m_on_client_disconnected)
                            m_on_client_disconnected(key, address, key.port);
                    }
                    disconnect_client(key);
                }
                break;
            }

            case CONTROL_HEARTBEAT:
            {
                if (conn)
                    conn->process_incoming(header, m_cached_now);
                break;
            }

            case CONTROL_BACKPRESSURE:
            {
                if (conn && payload_size >= 2)
                {
                    conn->process_incoming(header, m_cached_now);
                    uint8_t available = payload[1];
                    conn->set_fragment_backpressured(available == 0);
                }
                break;
            }

            case CONTROL_CHANNEL_OPEN:
            {
                if (!conn || conn->state() != connection_state::CONNECTED)
                    break;

                conn->process_incoming(header, m_cached_now);

                if (payload_size < 4)
                    break;

                uint8_t ch_id = payload[1];
                auto ch_mode = static_cast<channel_mode>(payload[2]);
                uint8_t ch_priority = payload[3];

                char ch_name[MAX_CHANNEL_NAME] = {};
                if (payload_size > 4)
                {
                    size_t name_len = payload_size - 4;
                    if (name_len > MAX_CHANNEL_NAME - 1)
                        name_len = MAX_CHANNEL_NAME - 1;
                    std::memcpy(ch_name, &payload[4], name_len);
                }

                if (payload[2] > static_cast<uint8_t>(channel_mode::RELIABLE_ORDERED))
                    break;

                bool accepted = true;
                if (m_on_channel_requested)
                    accepted = m_on_channel_requested(key, ch_id, ch_mode, ch_priority);

                if (accepted)
                {
                    if (!m_channels->is_registered(ch_id))
                    {
                        channel_config cfg{};
                        cfg.id = ch_id;
                        cfg.mode = ch_mode;
                        cfg.priority = ch_priority;
                        std::memcpy(cfg.name, ch_name, MAX_CHANNEL_NAME);
                        m_channels->register_channel(cfg);
                    }
                }

                uint8_t ack_payload[3] = {
                    CONTROL_CHANNEL_ACK,
                    ch_id,
                    accepted ? CHANNEL_STATUS_ACCEPTED : CHANNEL_STATUS_REJECTED,
                };
                send_control_payload_to(conn, ack_payload, sizeof(ack_payload), key);

                if (m_verbose)
                {
                    std::string address = endpoint_address_string(key);
                    std::cout << "[server] Channel " << (accepted ? "accepted" : "rejected")
                              << ": id=" << static_cast<int>(ch_id) << " mode=" << static_cast<int>(payload[2])
                              << " from " << address << ":" << key.port << std::endl;
                }
                break;
            }
        }
    }

    void server_worker::send_control_to(udp_connection *conn, uint8_t control_type, const endpoint_key &dest)
    {
        send_control_payload_to(conn, &control_type, 1, dest);
    }

    void server_worker::send_control_payload_to(udp_connection *conn, const void *payload, size_t size,
                                                const endpoint_key &dest)
    {
        packet_header header{};
        header.flags = FLAG_CONTROL;
        header.channel_id = channels::CONTROL.id;
        header.payload_size = static_cast<uint16_t>(size);
        conn->prepare_header(header, true, m_cached_now);
        m_socket->send_packet(header, payload, dest);
    }

    void server_worker::send_raw_control(uint8_t control_type, const endpoint_key &dest)
    {
        packet_header header{};
        header.magic = PROTOCOL_MAGIC;
        header.version = PROTOCOL_VERSION;
        header.flags = FLAG_CONTROL;
        header.sequence = 0;
        header.ack = 0;
        header.ack_bitmap = 0;
        header.payload_size = 1;
        m_socket->send_packet(header, &control_type, dest);
    }

} // namespace entanglement
