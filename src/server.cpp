#include "server.h"
#include <algorithm>
#include <cstring>
#include <iostream>

namespace entanglement
{

    server::server(uint16_t port, const std::string &bind_address)
        : m_port(port),
          m_bind_address(bind_address),
          m_pool(std::make_unique<std::array<udp_connection, MAX_CONNECTIONS>>())
    {
    }

    server::~server()
    {
        stop();
    }

    error_code server::start()
    {
        if (auto ec = m_socket.bind(m_port, m_bind_address); failed(ec))
        {
            return ec;
        }

        if (auto ec = m_socket.set_non_blocking(true); failed(ec))
        {
            if (m_verbose)
            {
                std::cerr << "[server] Failed to set non-blocking mode" << std::endl;
            }
            m_socket.close();
            return ec;
        }

        m_running = true;
        if (m_verbose)
        {
            std::cout << "[server] Listening on " << m_bind_address << ":" << m_port << std::endl;
        }
        return error_code::ok;
    }

    void server::stop()
    {
        if (!m_running)
            return;
        m_running = false;
        disconnect_all();
        m_socket.close();
        if (m_verbose)
        {
            std::cout << "[server] Stopped" << std::endl;
        }
    }

    // --- Packet processing ---

    int server::poll(int max_packets)
    {
        if (!m_running)
            return 0;

        int count = 0;
        packet_header header{};
        uint8_t payload[MAX_PAYLOAD_SIZE];
        endpoint_key sender{};

        while (count < max_packets)
        {
            int result = m_socket.recv_packet(header, payload, MAX_PAYLOAD_SIZE, sender);
            if (result <= 0)
                break;

            // Control packets are handled internally
            if ((header.flags & FLAG_CONTROL) && header.payload_size >= 1)
            {
                handle_control(sender, header, payload, header.payload_size);
                ++count;
                continue;
            }

            // Data packets — only from connected peers
            udp_connection *conn = find(sender);
            if (!conn || conn->state() != connection_state::CONNECTED)
            {
                ++count;
                continue;
            }

            bool is_new = conn->process_incoming(header);
            if (!is_new)
            {
                ++count;
                continue;
            }

            // Handle fragmented data packets — route to per-connection reassembler
            if ((header.flags & FLAG_FRAGMENT) && header.payload_size > FRAGMENT_HEADER_SIZE)
            {
                fragment_header fhdr;
                std::memcpy(&fhdr, payload, FRAGMENT_HEADER_SIZE);
                auto frag_result = conn->reassembler().process_fragment(sender, header.channel_id, fhdr,
                                                                        payload + FRAGMENT_HEADER_SIZE,
                                                                        header.payload_size - FRAGMENT_HEADER_SIZE,
                                                                        header.channel_sequence);

                // If reassembler is full/under pressure, send backpressure to this client
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

                ++count;
                continue;
            }

            if (is_new && m_on_client_data_received)
            {
                // RELIABLE_ORDERED channels: hold-back queue ensures in-order delivery
                if (m_channels.is_ordered(header.channel_id))
                {
                    if (conn->is_ordered_next(header.channel_id, header.channel_sequence))
                    {
                        // Expected packet — deliver immediately
                        std::string addr_str = endpoint_address_string(sender);
                        m_on_client_data_received(header, payload, header.payload_size, addr_str, sender.port);
                        conn->advance_ordered_seq(header.channel_id);

                        // Drain any consecutive buffered items (simple + fragmented)
                        drain_ordered_conn(*conn, header.channel_id);
                    }
                    else if (header.channel_sequence > conn->expected_ordered_seq(header.channel_id))
                    {
                        // Future packet — buffer for later delivery
                        if (!conn->buffer_ordered_packet(header, payload, header.payload_size))
                        {
                            // Buffer full — force-skip gap and retry
                            conn->skip_ordered_gap(header.channel_id);
                            drain_ordered_conn(*conn, header.channel_id);
                            conn->buffer_ordered_packet(header, payload, header.payload_size);
                        }
                    }
                    // else: channel_sequence < expected → stale duplicate, ignore
                }
                else
                {
                    std::string addr_str = endpoint_address_string(sender);
                    m_on_client_data_received(header, payload, header.payload_size, addr_str, sender.port);
                }
            }
            ++count;
        }

        return count;
    }

    int server::update()
    {
        return update(nullptr);
    }

    int server::update(on_server_packet_lost loss_callback)
    {
        if (!m_running)
            return 0;

        auto now = std::chrono::steady_clock::now();

        // Fixed buffer for timed-out keys (can't modify m_index while iterating)
        endpoint_key timed_out[MAX_TIMEOUTS_PER_UPDATE];
        int timeout_count = 0;

        for (auto &[key, idx] : m_index)
        {
            auto &conn = (*m_pool)[idx];

            if (conn.has_timed_out(now))
            {
                if (timeout_count < MAX_TIMEOUTS_PER_UPDATE)
                {
                    timed_out[timeout_count++] = key;
                }
                continue;
            }

            if (conn.needs_heartbeat(now))
            {
                send_control_to(&conn, CONTROL_HEARTBEAT, key);
            }

            // Collect losses for this connection
            if (loss_callback || m_on_packet_lost)
            {
                auto &cb = loss_callback ? loss_callback : m_on_packet_lost;
                lost_packet_info lost[MAX_LOSSES_PER_UPDATE];
                int loss_count = conn.collect_losses(now, lost, MAX_LOSSES_PER_UPDATE);
                if (loss_count > 0)
                {
                    std::string addr_str = endpoint_address_string(key);
                    for (int l = 0; l < loss_count; ++l)
                    {
                        cb(lost[l], addr_str, key.port);
                    }
                }
            }
            else
            {
                // Even without a callback, collect losses to expire unreliable packets
                lost_packet_info lost[MAX_LOSSES_PER_UPDATE];
                conn.collect_losses(now, lost, MAX_LOSSES_PER_UPDATE);
            }

            // Per-connection reassembler cleanup
            auto &ra = conn.reassembler();
            ra.cleanup_stale(now, ra.reassembly_timeout());

            // Send backpressure relief if usage dropped below low watermark
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
            {
                std::cout << "[server] Client timed out: " << addr_str << ":" << timed_out[i].port << std::endl;
            }

            if (m_on_client_disconnected)
            {
                m_on_client_disconnected(timed_out[i], addr_str, timed_out[i].port);
            }
            disconnect_client(timed_out[i]);
        }

        return timeout_count;
    }

    // --- Callbacks ---

    void server::set_on_client_data_received(on_client_data_received callback)
    {
        m_on_client_data_received = std::move(callback);
    }

    void server::set_on_client_connected(on_client_connected callback)
    {
        m_on_client_connected = std::move(callback);
    }

    void server::set_on_client_disconnected(on_client_disconnected callback)
    {
        m_on_client_disconnected = std::move(callback);
    }

    void server::set_on_channel_requested(on_channel_requested callback)
    {
        m_on_channel_requested = std::move(callback);
    }

    void server::set_on_packet_lost(on_server_packet_lost callback)
    {
        m_on_packet_lost = std::move(callback);
    }

    // --- Sending ---

    int server::send_to(packet_header &header, const void *payload, const std::string &address, uint16_t port)
    {
        endpoint_key key = endpoint_from_string(address, port);

        udp_connection *conn = find(key);
        if (conn && conn->state() == connection_state::CONNECTED)
        {
            bool reliable = m_channels.is_reliable(header.channel_id);
            conn->prepare_header(header, reliable);
        }

        return m_socket.send_packet(header, payload, key);
    }

    int server::send_payload_to(const void *data, size_t size, uint8_t channel_id, const std::string &address,
                                uint16_t port, uint8_t flags, uint32_t *out_message_id)
    {
        endpoint_key key = endpoint_from_string(address, port);

        udp_connection *conn = find(key);
        if (!conn || conn->state() != connection_state::CONNECTED)
            return static_cast<int>(error_code::not_connected);

        return conn->send_payload(m_socket, m_channels, data, size, flags, channel_id, key, out_message_id);
    }

    int server::send_fragment_to(uint32_t message_id, uint8_t index, uint8_t count, const void *data, size_t size,
                                 uint8_t flags, uint8_t channel_id, const std::string &address, uint16_t port,
                                 uint32_t channel_sequence)
    {
        endpoint_key key = endpoint_from_string(address, port);

        udp_connection *conn = find(key);
        if (!conn || conn->state() != connection_state::CONNECTED)
            return static_cast<int>(error_code::not_connected);

        return conn->send_fragment(m_socket, m_channels, message_id, index, count, data, size, flags, channel_id, key,
                                   channel_sequence);
    }

    // --- Connection management ---

    void server::disconnect_client(const endpoint_key &key)
    {
        auto it = m_index.find(key);
        if (it != m_index.end())
        {
            (*m_pool)[it->second].reset();
            m_index.erase(it);
        }
    }

    void server::disconnect_all()
    {
        for (auto &[key, idx] : m_index)
        {
            (*m_pool)[idx].reset();
        }
        m_index.clear();
    }

    udp_connection *server::find_or_create(const endpoint_key &key)
    {
        auto it = m_index.find(key);
        if (it != m_index.end())
        {
            return &(*m_pool)[it->second];
        }

        int slot = allocate_slot();
        if (slot < 0)
            return nullptr;

        auto &conn = (*m_pool)[slot];
        conn.reset();
        conn.set_active(true);
        conn.set_endpoint(key);

        // Set up per-connection reassembler with stored callback templates
        auto &ra = conn.reassembler();
        if (m_frag_alloc_cb)
            ra.set_on_allocate(m_frag_alloc_cb);
        if (m_app_on_message_complete)
            setup_ordered_complete_wrapper(conn);
        if (m_frag_failed_cb)
            ra.set_on_failed(m_frag_failed_cb);
        if (m_reassembly_timeout_us != REASSEMBLY_TIMEOUT_US)
            ra.set_reassembly_timeout(m_reassembly_timeout_us);

        m_index[key] = static_cast<uint16_t>(slot);
        return &conn;
    }

    udp_connection *server::find(const endpoint_key &key)
    {
        auto it = m_index.find(key);
        if (it != m_index.end())
        {
            return &(*m_pool)[it->second];
        }
        return nullptr;
    }

    int server::allocate_slot()
    {
        for (size_t i = 0; i < MAX_CONNECTIONS; ++i)
        {
            if (!(*m_pool)[i].is_active())
            {
                return static_cast<int>(i);
            }
        }
        return static_cast<int>(error_code::pool_full);
    }

    // --- Control packet handling ---

    void server::handle_control(const endpoint_key &key, const packet_header &header, const uint8_t *payload,
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
                    // Already known — they missed our ACCEPTED; resend
                    conn->process_incoming(header);
                    send_control_to(conn, CONTROL_CONNECTION_ACCEPTED, key);
                    return;
                }

                // New connection
                conn = find_or_create(key);
                if (!conn)
                {
                    // Pool full
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
                conn->process_incoming(header);
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
                    {
                        m_on_client_connected(key, address, key.port);
                    }
                }
                break;
            }

            case CONTROL_DISCONNECT:
            {
                if (conn)
                {
                    conn->process_incoming(header);

                    if (m_verbose || m_on_client_disconnected)
                    {
                        std::string address = endpoint_address_string(key);
                        if (m_verbose)
                        {
                            std::cout << "[server] Client disconnected: " << address << ":" << key.port << std::endl;
                        }
                        if (m_on_client_disconnected)
                        {
                            m_on_client_disconnected(key, address, key.port);
                        }
                    }
                    disconnect_client(key);
                }
                break;
            }

            case CONTROL_HEARTBEAT:
            {
                if (conn)
                {
                    conn->process_incoming(header);
                }
                break;
            }

            case CONTROL_BACKPRESSURE:
            {
                // Payload: [type(1)][available_slots(1)]
                // available=0 means throttle, >0 means resume
                if (conn && payload_size >= 2)
                {
                    conn->process_incoming(header);
                    uint8_t available = payload[1];
                    conn->set_fragment_backpressured(available == 0);
                }
                break;
            }

            case CONTROL_CHANNEL_OPEN:
            {
                if (!conn || conn->state() != connection_state::CONNECTED)
                    break;

                conn->process_incoming(header);

                // Payload: [type(1)][channel_id(1)][mode(1)][priority(1)][name(up to 32)] = 4+ bytes
                if (payload_size < 4)
                    break;

                uint8_t ch_id = payload[1];
                auto ch_mode = static_cast<channel_mode>(payload[2]);
                uint8_t ch_priority = payload[3];

                // Extract name from payload (bytes 4..35)
                char ch_name[MAX_CHANNEL_NAME] = {};
                if (payload_size > 4)
                {
                    size_t name_len = payload_size - 4;
                    if (name_len > MAX_CHANNEL_NAME - 1)
                        name_len = MAX_CHANNEL_NAME - 1;
                    std::memcpy(ch_name, &payload[4], name_len);
                }

                // Validate mode
                if (payload[2] > static_cast<uint8_t>(channel_mode::RELIABLE_ORDERED))
                    break;

                // Consult the application callback (accept all if no callback)
                bool accepted = true;
                if (m_on_channel_requested)
                {
                    accepted = m_on_channel_requested(key, ch_id, ch_mode, ch_priority);
                }

                if (accepted)
                {
                    // Register on server side (ignore if already registered — idempotent)
                    if (!m_channels.is_registered(ch_id))
                    {
                        channel_config cfg{};
                        cfg.id = ch_id;
                        cfg.mode = ch_mode;
                        cfg.priority = ch_priority;
                        std::memcpy(cfg.name, ch_name, MAX_CHANNEL_NAME);
                        m_channels.register_channel(cfg);
                    }
                }

                // Send CHANNEL_ACK: [type(1)][channel_id(1)][status(1)]
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

    void server::send_control_to(udp_connection *conn, uint8_t control_type, const endpoint_key &dest)
    {
        send_control_payload_to(conn, &control_type, 1, dest);
    }

    void server::send_control_payload_to(udp_connection *conn, const void *payload, size_t size,
                                         const endpoint_key &dest)
    {
        packet_header header{};
        header.flags = FLAG_CONTROL;
        header.channel_id = channels::CONTROL.id;
        header.payload_size = static_cast<uint16_t>(size);
        // Control channel is always reliable
        conn->prepare_header(header, true);
        m_socket.send_packet(header, payload, dest);
    }

    void server::send_raw_control(uint8_t control_type, const endpoint_key &dest)
    {
        packet_header header{};
        header.magic = PROTOCOL_MAGIC;
        header.version = PROTOCOL_VERSION;
        header.flags = FLAG_CONTROL;
        header.sequence = 0;
        header.ack = 0;
        header.ack_bitmap = 0;
        header.payload_size = 1;
        m_socket.send_packet(header, &control_type, dest);
    }

    void server::set_on_allocate_message(on_allocate_message cb)
    {
        m_frag_alloc_cb = cb;
        for (auto &[key, idx] : m_index)
            (*m_pool)[idx].reassembler().set_on_allocate(cb);
    }

    void server::set_on_message_complete(on_message_complete cb)
    {
        m_app_on_message_complete = cb;
        m_frag_complete_cb = cb; // store for template (used by setup_ordered_complete_wrapper)
        for (auto &[key, idx] : m_index)
            setup_ordered_complete_wrapper((*m_pool)[idx]);
    }

    void server::set_on_message_failed(on_message_failed cb)
    {
        m_frag_failed_cb = cb;
        for (auto &[key, idx] : m_index)
            (*m_pool)[idx].reassembler().set_on_failed(cb);
    }

    void server::set_reassembly_timeout(int64_t timeout_us)
    {
        m_reassembly_timeout_us = timeout_us;
        for (auto &[key, idx] : m_index)
            (*m_pool)[idx].reassembler().set_reassembly_timeout(timeout_us);
    }

    bool server::is_fragment_throttled(const std::string &address, uint16_t port) const
    {
        endpoint_key key = endpoint_from_string(address, port);

        auto it = m_index.find(key);
        if (it == m_index.end())
            return false;
        return (*m_pool)[it->second].is_fragment_backpressured();
    }

    void server::setup_ordered_complete_wrapper(udp_connection &conn)
    {
        udp_connection *conn_ptr = &conn;
        conn.reassembler().set_on_complete(
            [this, conn_ptr](const endpoint_key &sender, uint32_t msg_id, uint8_t ch_id, uint8_t *data,
                             size_t total_size)
            {
                uint32_t chan_seq = conn_ptr->reassembler().last_completed_channel_sequence();
                if (m_channels.is_ordered(ch_id) && chan_seq > 0)
                {
                    if (conn_ptr->is_ordered_next(ch_id, chan_seq))
                    {
                        if (m_app_on_message_complete)
                            m_app_on_message_complete(sender, msg_id, ch_id, data, total_size);
                        conn_ptr->advance_ordered_seq(ch_id);
                        drain_ordered_conn(*conn_ptr, ch_id);
                    }
                    else if (chan_seq > conn_ptr->expected_ordered_seq(ch_id))
                    {
                        if (!conn_ptr->buffer_ordered_message(sender, msg_id, ch_id, data, total_size, chan_seq))
                        {
                            conn_ptr->skip_ordered_gap(ch_id);
                            drain_ordered_conn(*conn_ptr, ch_id);
                            conn_ptr->buffer_ordered_message(sender, msg_id, ch_id, data, total_size, chan_seq);
                        }
                    }
                    else if (m_app_on_message_complete)
                    {
                        // Stale — deliver anyway to avoid leaking the buffer
                        m_app_on_message_complete(sender, msg_id, ch_id, data, total_size);
                    }
                }
                else
                {
                    if (m_app_on_message_complete)
                        m_app_on_message_complete(sender, msg_id, ch_id, data, total_size);
                }
            });
    }

    void server::drain_ordered_conn(udp_connection &conn, uint8_t channel_id)
    {
        std::string addr; // lazy-init
        uint16_t port = conn.endpoint().port;
        while (true)
        {
            if (auto *pkt = conn.peek_next_ordered(channel_id))
            {
                if (m_on_client_data_received)
                {
                    if (addr.empty())
                        addr = endpoint_address_string(conn.endpoint());
                    m_on_client_data_received(pkt->header, pkt->payload, pkt->payload_size, addr, port);
                }
                conn.advance_ordered_seq(channel_id);
                conn.release_ordered_packet(pkt);
                continue;
            }
            if (auto *msg = conn.peek_next_ordered_message(channel_id))
            {
                if (m_app_on_message_complete)
                    m_app_on_message_complete(msg->sender, msg->message_id, msg->channel_id, msg->data,
                                              msg->total_size);
                conn.advance_ordered_seq(channel_id);
                conn.release_ordered_message(msg);
                continue;
            }
            break;
        }
    }

} // namespace entanglement
