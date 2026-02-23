#include "server.h"
#include <algorithm>
#include <cstring>
#include <iostream>

#ifdef ENTANGLEMENT_PLATFORM_WINDOWS
#include <WinSock2.h>
#else
#include <arpa/inet.h>
#endif

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

    bool server::start()
    {
        if (!m_socket.bind(m_port, m_bind_address))
        {
            return false;
        }

        if (!m_socket.set_non_blocking(true))
        {
            if (m_verbose)
            {
                std::cerr << "[server] Failed to set non-blocking mode" << std::endl;
            }
            m_socket.close();
            return false;
        }

        m_running = true;
        if (m_verbose)
        {
            std::cout << "[server] Listening on " << m_bind_address << ":" << m_port << std::endl;
        }
        return true;
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
        std::string sender_addr;
        uint16_t sender_port = 0;

        while (count < max_packets)
        {
            int result = m_socket.recv_packet(header, payload, MAX_PAYLOAD_SIZE, sender_addr, sender_port);
            if (result <= 0)
                break;

            endpoint_key key{};
            inet_pton(AF_INET, sender_addr.c_str(), &key.address);
            key.port = sender_port;

            // Control packets are handled internally
            if ((header.flags & FLAG_CONTROL) && header.payload_size >= 1)
            {
                handle_control(key, header, payload, header.payload_size, sender_addr, sender_port);
                ++count;
                continue;
            }

            // Data packets — only from connected peers
            udp_connection *conn = find(key);
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

            // Handle fragmented data packets — route to reassembler
            if ((header.flags & FLAG_FRAGMENT) && header.payload_size > FRAGMENT_HEADER_SIZE)
            {
                fragment_header fhdr;
                std::memcpy(&fhdr, payload, FRAGMENT_HEADER_SIZE);
                m_reassembler.process_fragment(key, header.channel_id, fhdr, payload + FRAGMENT_HEADER_SIZE,
                                               header.payload_size - FRAGMENT_HEADER_SIZE);
                ++count;
                continue;
            }

            if (is_new && m_on_packet_received)
            {
                m_on_packet_received(header, payload, header.payload_size, sender_addr, sender_port);
            }
            ++count;
        }

        return count;
    }

    int server::update()
    {
        if (!m_running)
            return 0;

        auto now = std::chrono::steady_clock::now();

        // Fixed buffer for timed-out keys (can't modify m_index while iterating)
        endpoint_key timed_out[MAX_TIMEOUTS_PER_UPDATE];
        int timeout_count = 0;
        char addr_buf[16];

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
                inet_ntop(AF_INET, &key.address, addr_buf, sizeof(addr_buf));
                send_control_to(&conn, CONTROL_HEARTBEAT, addr_buf, key.port);
            }
        }

        for (int i = 0; i < timeout_count; ++i)
        {
            inet_ntop(AF_INET, &timed_out[i].address, addr_buf, sizeof(addr_buf));
            if (m_verbose)
            {
                std::cout << "[server] Client timed out: " << addr_buf << ":" << timed_out[i].port << std::endl;
            }

            if (m_on_client_disconnected)
            {
                m_on_client_disconnected(timed_out[i], addr_buf, timed_out[i].port);
            }
            disconnect_client(timed_out[i]);
        }

        // Expire stale reassembly entries
        m_reassembler.cleanup_stale(now, m_reassembly_timeout_us);

        return timeout_count;
    }

    // --- Callbacks ---

    void server::set_on_packet_received(on_packet_received callback)
    {
        m_on_packet_received = std::move(callback);
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

    // --- Sending ---

    int server::send_to(packet_header &header, const void *payload, const std::string &address, uint16_t port)
    {
        endpoint_key key{};
        inet_pton(AF_INET, address.c_str(), &key.address);
        key.port = port;

        udp_connection *conn = find(key);
        if (conn && conn->state() == connection_state::CONNECTED)
        {
            bool reliable = m_channels.is_reliable(header.channel_id);
            conn->prepare_header(header, reliable);
        }

        return m_socket.send_packet(header, payload, address, port);
    }

    int server::send_payload_to(const void *data, size_t size, uint8_t channel_id, const std::string &address,
                                uint16_t port, uint8_t flags)
    {
        // Single-packet path
        if (size <= MAX_PAYLOAD_SIZE)
        {
            packet_header header{};
            header.flags = flags;
            header.channel_id = channel_id;
            header.payload_size = static_cast<uint16_t>(size);
            return send_to(header, data, address, port);
        }

        // Look up connection
        endpoint_key key{};
        inet_pton(AF_INET, address.c_str(), &key.address);
        key.port = port;

        udp_connection *conn = find(key);
        if (!conn || conn->state() != connection_state::CONNECTED)
            return -1;

        // --- Fragmented send ---
        const uint8_t *src = static_cast<const uint8_t *>(data);
        uint8_t fragment_count = static_cast<uint8_t>((size + MAX_FRAGMENT_PAYLOAD - 1) / MAX_FRAGMENT_PAYLOAD);
        if (fragment_count == 0)
            return -1;

        uint32_t message_id = conn->next_message_id();
        int total_sent = 0;

        for (uint8_t i = 0; i < fragment_count; ++i)
        {
            size_t offset = static_cast<size_t>(i) * MAX_FRAGMENT_PAYLOAD;
            size_t chunk = (std::min)(MAX_FRAGMENT_PAYLOAD, size - offset);

            int result = send_fragment_to(conn, message_id, i, fragment_count, src + offset, chunk, flags, channel_id,
                                          address, port);
            if (result <= 0)
                return result;
            total_sent += static_cast<int>(chunk);
        }

        conn->register_pending_message(message_id, fragment_count);
        return total_sent;
    }

    int server::send_fragment_to(udp_connection *conn, uint32_t message_id, uint8_t index, uint8_t count,
                                 const void *data, size_t size, uint8_t flags, uint8_t channel_id,
                                 const std::string &address, uint16_t port)
    {
        // Build fragment header on stack (4 bytes)
        fragment_header fhdr{message_id, index, count};

        packet_header header{};
        header.flags = flags | FLAG_FRAGMENT;
        header.channel_id = channel_id;
        header.payload_size = static_cast<uint16_t>(FRAGMENT_HEADER_SIZE + size);

        bool reliable = m_channels.is_reliable(channel_id);
        conn->prepare_header(header, reliable);

        // Tag fragment metadata for loss tracking
        size_t idx = header.sequence % SEQUENCE_BUFFER_SIZE;
        auto &entry = conn->send_buffer_entry(idx);
        entry.message_id = message_id;
        entry.fragment_index = index;

        // Scatter-gather: [packet_header] + [fragment_header] + [user data] — zero intermediate copy
        const void *segments[2] = {&fhdr, data};
        size_t seg_sizes[2] = {FRAGMENT_HEADER_SIZE, size};
        return m_socket.send_packet_gather(header, segments, seg_sizes, 2, address, port);
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
        return -1;
    }

    // --- Control packet handling ---

    void server::handle_control(const endpoint_key &key, const packet_header &header, const uint8_t *payload,
                                size_t payload_size, const std::string &address, uint16_t port)
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
                    send_control_to(conn, CONTROL_CONNECTION_ACCEPTED, address, port);
                    return;
                }

                // New connection
                conn = find_or_create(key);
                if (!conn)
                {
                    // Pool full
                    send_raw_control(CONTROL_CONNECTION_DENIED, address, port);
                    if (m_verbose)
                    {
                        std::cerr << "[server] Connection denied (pool full) to " << address << ":" << port
                                  << std::endl;
                    }
                    return;
                }

                conn->set_state(connection_state::CONNECTED);
                conn->process_incoming(header);
                send_control_to(conn, CONTROL_CONNECTION_ACCEPTED, address, port);

                if (m_verbose)
                {
                    std::cout << "[server] Client connected: " << address << ":" << port << " (slot " << m_index[key]
                              << ", total: " << m_index.size() << ")" << std::endl;
                }

                if (m_on_client_connected)
                {
                    m_on_client_connected(key, address, port);
                }
                break;
            }

            case CONTROL_DISCONNECT:
            {
                if (conn)
                {
                    conn->process_incoming(header);

                    if (m_verbose)
                    {
                        std::cout << "[server] Client disconnected: " << address << ":" << port << std::endl;
                    }

                    if (m_on_client_disconnected)
                    {
                        m_on_client_disconnected(key, address, port);
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
                send_control_payload_to(conn, ack_payload, sizeof(ack_payload), address, port);

                if (m_verbose)
                {
                    std::cout << "[server] Channel " << (accepted ? "accepted" : "rejected")
                              << ": id=" << static_cast<int>(ch_id) << " mode=" << static_cast<int>(payload[2])
                              << " from " << address << ":" << port << std::endl;
                }
                break;
            }
        }
    }

    void server::send_control_to(udp_connection *conn, uint8_t control_type, const std::string &address, uint16_t port)
    {
        send_control_payload_to(conn, &control_type, 1, address, port);
    }

    void server::send_control_payload_to(udp_connection *conn, const void *payload, size_t size,
                                         const std::string &address, uint16_t port)
    {
        packet_header header{};
        header.flags = FLAG_CONTROL;
        header.channel_id = channels::CONTROL.id;
        header.payload_size = static_cast<uint16_t>(size);
        // Control channel is always reliable
        conn->prepare_header(header, true);
        m_socket.send_packet(header, payload, address, port);
    }

    void server::send_raw_control(uint8_t control_type, const std::string &address, uint16_t port)
    {
        packet_header header{};
        header.magic = PROTOCOL_MAGIC;
        header.version = PROTOCOL_VERSION;
        header.flags = FLAG_CONTROL;
        header.sequence = 0;
        header.ack = 0;
        header.ack_bitmap = 0;
        header.payload_size = 1;
        m_socket.send_packet(header, &control_type, address, port);
    }

    void server::set_on_allocate_message(on_allocate_message cb)
    {
        m_reassembler.set_on_allocate(std::move(cb));
    }

    void server::set_on_message_complete(on_message_complete cb)
    {
        m_reassembler.set_on_complete(std::move(cb));
    }

    void server::set_on_message_expired(on_message_expired cb)
    {
        m_reassembler.set_on_expired(std::move(cb));
    }

} // namespace entanglement
