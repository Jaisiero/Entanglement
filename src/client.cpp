#include "client.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <thread>

namespace entanglement
{

    client::client(const std::string &server_address, uint16_t server_port)
        : m_server_address(server_address), m_server_port(server_port)
    {
    }

    client::~client()
    {
        disconnect();
    }

    bool client::connect()
    {
        if (!m_socket.bind(0))
            return false;

        if (!m_socket.set_non_blocking(true))
        {
            if (m_verbose)
            {
                std::cerr << "[client] Failed to set non-blocking mode" << std::endl;
            }
            m_socket.close();
            return false;
        }

        m_connection.reset();
        m_connection.set_active(true);
        m_connection.set_state(connection_state::CONNECTING);

        // Handshake: send CONNECTION_REQUEST, wait for ACCEPTED with retries
        send_control(CONTROL_CONNECTION_REQUEST);

        auto retry_interval = std::chrono::milliseconds(HANDSHAKE_RETRY_INTERVAL_MS);
        auto last_attempt = std::chrono::steady_clock::now();
        int attempt = 0;

        while (attempt < HANDSHAKE_MAX_ATTEMPTS)
        {
            poll();

            if (m_connection.state() == connection_state::CONNECTED)
            {
                m_connected = true;
                if (m_verbose)
                {
                    std::cout << "[client] Connected to " << m_server_address << ":" << m_server_port << " (port "
                              << m_socket.local_port() << ")" << std::endl;
                }
                if (m_on_connected)
                {
                    m_on_connected();
                }
                return true;
            }

            if (m_connection.state() == connection_state::DISCONNECTED)
            {
                // Got CONNECTION_DENIED
                m_socket.close();
                m_connection.reset();
                return false;
            }

            auto now = std::chrono::steady_clock::now();
            if (now - last_attempt >= retry_interval)
            {
                ++attempt;
                if (m_verbose && attempt > 1)
                {
                    std::cout << "[client] Retrying connection (" << attempt << "/" << HANDSHAKE_MAX_ATTEMPTS << ")"
                              << std::endl;
                }
                send_control(CONTROL_CONNECTION_REQUEST);
                last_attempt = now;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        if (m_verbose)
        {
            std::cerr << "[client] Connection timed out" << std::endl;
        }
        m_connection.reset();
        m_socket.close();
        return false;
    }

    void client::disconnect()
    {
        if (m_connection.state() == connection_state::DISCONNECTED)
            return;

        // Send clean disconnect notification
        if (m_connection.state() == connection_state::CONNECTED || m_connection.state() == connection_state::CONNECTING)
        {
            send_control(CONTROL_DISCONNECT);
        }

        m_connected = false;
        m_connection.set_state(connection_state::DISCONNECTED);
        m_connection.reset();
        m_socket.close();
        if (m_verbose)
        {
            std::cout << "[client] Disconnected" << std::endl;
        }
    }

    int client::send(packet_header &header, const void *payload)
    {
        bool reliable = m_channels.is_reliable(header.channel_id);
        m_connection.prepare_header(header, reliable);
        return m_socket.send_packet(header, payload, m_server_address, m_server_port);
    }

    int client::send_payload(const void *data, size_t size, uint8_t flags, uint8_t channel_id, uint32_t *out_message_id)
    {
        // Single-packet path (no fragmentation overhead)
        if (size <= MAX_PAYLOAD_SIZE)
        {
            if (out_message_id)
                *out_message_id = 0;
            packet_header header{};
            header.flags = flags;
            header.shard_id = 0;
            header.channel_id = channel_id;
            header.reserved = 0;
            header.payload_size = static_cast<uint16_t>(size);
            return send(header, data);
        }

        // Fragmented send — check backpressure from server
        if (m_connection.is_fragment_backpressured())
        {
            return -2; // BACKPRESSURED: server's reassembler is full
        }

        // --- Fragmented send (zero-copy from user buffer) ---
        const uint8_t *src = static_cast<const uint8_t *>(data);
        uint8_t fragment_count = static_cast<uint8_t>((size + MAX_FRAGMENT_PAYLOAD - 1) / MAX_FRAGMENT_PAYLOAD);

        // Validate: uint8_t count means max 255 fragments
        if (fragment_count == 0)
            return -1;

        uint32_t message_id = m_connection.next_message_id();
        int total_sent = 0;

        for (uint8_t i = 0; i < fragment_count; ++i)
        {
            size_t offset = static_cast<size_t>(i) * MAX_FRAGMENT_PAYLOAD;
            size_t chunk = (std::min)(MAX_FRAGMENT_PAYLOAD, size - offset);

            int result = send_fragment(message_id, i, fragment_count, src + offset, chunk, flags, channel_id);
            if (result <= 0)
                return result;
            total_sent += static_cast<int>(chunk);
        }

        // Register for ACK tracking (sender side)
        m_connection.register_pending_message(message_id, fragment_count);

        if (out_message_id)
            *out_message_id = message_id;

        return total_sent;
    }

    int client::send_fragment(uint32_t message_id, uint8_t index, uint8_t count, const void *data, size_t size,
                              uint8_t flags, uint8_t channel_id)
    {
        // Build fragment header on stack (4 bytes)
        fragment_header fhdr{message_id, index, count};

        packet_header header{};
        header.flags = flags | FLAG_FRAGMENT;
        header.shard_id = 0;
        header.channel_id = channel_id;
        header.reserved = 0;
        header.payload_size = static_cast<uint16_t>(FRAGMENT_HEADER_SIZE + size);

        bool reliable = m_channels.is_reliable(channel_id);
        m_connection.prepare_header(header, reliable);

        // Tag the sent_packet_entry with fragment info for loss tracking
        size_t idx = header.sequence % SEQUENCE_BUFFER_SIZE;
        auto &entry = m_connection.send_buffer_entry(idx);
        entry.message_id = message_id;
        entry.fragment_index = index;
        entry.fragment_count = count;

        // Scatter-gather: [packet_header] + [fragment_header] + [user data] — zero intermediate copy
        const void *segments[2] = {&fhdr, data};
        size_t seg_sizes[2] = {FRAGMENT_HEADER_SIZE, size};
        return m_socket.send_packet_gather(header, segments, seg_sizes, 2, m_server_address, m_server_port);
    }

    int client::poll(int max_packets)
    {
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

            bool is_new = m_connection.process_incoming(header);
            if (!is_new)
            {
                ++count;
                continue;
            }

            // Handle control packets internally
            if ((header.flags & FLAG_CONTROL) && header.payload_size >= 1)
            {
                handle_control(payload, header.payload_size);
                ++count;
                continue;
            }

            // Handle fragmented data packets — route to per-connection reassembler
            if ((header.flags & FLAG_FRAGMENT) && header.payload_size > FRAGMENT_HEADER_SIZE)
            {
                fragment_header fhdr;
                std::memcpy(&fhdr, payload, FRAGMENT_HEADER_SIZE);
                endpoint_key server_ep{}; // zeroed — single server
                auto result = m_connection.reassembler().process_fragment(server_ep, header.channel_id, fhdr,
                                                                          payload + FRAGMENT_HEADER_SIZE,
                                                                          header.payload_size - FRAGMENT_HEADER_SIZE);

                // If reassembler is full/under pressure, send backpressure to server
                if (result == fragment_result::slots_full)
                {
                    if (!m_connection.backpressure_sent())
                    {
                        uint8_t bp[2] = {CONTROL_BACKPRESSURE, 0};
                        send_control_payload(bp, 2);
                        m_connection.set_backpressure_sent(true);
                    }
                }
                else if (m_connection.reassembler().usage_percent() >= BACKPRESSURE_HIGH_WATERMARK)
                {
                    if (!m_connection.backpressure_sent())
                    {
                        uint8_t bp[2] = {CONTROL_BACKPRESSURE, 0};
                        send_control_payload(bp, 2);
                        m_connection.set_backpressure_sent(true);
                    }
                }

                ++count;
                continue;
            }

            // Data packets — only when connected
            if (m_connection.state() == connection_state::CONNECTED && m_on_data_received)
            {
                m_on_data_received(header, payload, header.payload_size);
            }
            ++count;
        }

        return count;
    }

    int client::update(on_packet_lost loss_callback)
    {
        if (m_connection.state() != connection_state::CONNECTED)
            return 0;

        auto now = std::chrono::steady_clock::now();

        // Check connection timeout
        if (m_connection.has_timed_out(now))
        {
            if (m_verbose)
            {
                std::cout << "[client] Connection timed out" << std::endl;
            }
            m_connected = false;
            m_connection.set_state(connection_state::DISCONNECTED);
            if (m_on_disconnected)
            {
                m_on_disconnected();
            }
            return 0;
        }

        // Send heartbeat if idle
        if (m_connection.needs_heartbeat(now))
        {
            send_control(CONTROL_HEARTBEAT);
        }

        // Collect losses
        lost_packet_info lost[MAX_LOSSES_PER_UPDATE];
        int count = m_connection.collect_losses(now, lost, MAX_LOSSES_PER_UPDATE);

        auto &cb = loss_callback ? loss_callback : m_on_packet_lost;
        if (cb)
        {
            for (int i = 0; i < count; ++i)
            {
                cb(lost[i]);
            }
        }

        // Expire stale reassembly entries
        auto &ra = m_connection.reassembler();
        ra.cleanup_stale(now, ra.reassembly_timeout());

        // Send backpressure relief if usage dropped below low watermark
        if (m_connection.backpressure_sent() && ra.usage_percent() < BACKPRESSURE_LOW_WATERMARK)
        {
            uint8_t available = static_cast<uint8_t>(ra.capacity() - ra.pending_count());
            uint8_t bp[2] = {CONTROL_BACKPRESSURE, available};
            send_control_payload(bp, 2);
            m_connection.set_backpressure_sent(false);
        }

        return count;
    }

    void client::set_on_data_received(on_data_received callback)
    {
        m_on_data_received = std::move(callback);
    }

    void client::set_on_connected(on_connected callback)
    {
        m_on_connected = std::move(callback);
    }

    void client::set_on_disconnected(on_disconnected callback)
    {
        m_on_disconnected = std::move(callback);
    }

    void client::set_on_packet_lost(on_packet_lost callback)
    {
        m_on_packet_lost = std::move(callback);
    }

    void client::set_on_allocate_message(on_allocate_message cb)
    {
        m_connection.reassembler().set_on_allocate(std::move(cb));
    }

    void client::set_on_message_complete(on_message_complete cb)
    {
        m_connection.reassembler().set_on_complete(std::move(cb));
    }

    void client::set_on_message_failed(on_message_failed cb)
    {
        m_connection.reassembler().set_on_failed(std::move(cb));
    }

    void client::set_on_message_acked(on_message_acked cb)
    {
        m_connection.set_on_message_acked(std::move(cb));
    }

    void client::send_control(uint8_t control_type)
    {
        send_control_payload(&control_type, 1);
    }

    void client::send_control_payload(const void *payload, size_t size)
    {
        packet_header header{};
        header.flags = FLAG_CONTROL;
        header.channel_id = channels::CONTROL.id;
        header.payload_size = static_cast<uint16_t>(size);
        // Control channel is always reliable
        m_connection.prepare_header(header, true);
        m_socket.send_packet(header, payload, m_server_address, m_server_port);
    }

    void client::handle_control(const uint8_t *payload, size_t payload_size)
    {
        uint8_t control_type = payload[0];
        switch (control_type)
        {
            case CONTROL_CONNECTION_ACCEPTED:
                if (m_connection.state() == connection_state::CONNECTING)
                {
                    m_connection.set_state(connection_state::CONNECTED);
                    m_connected = true;
                }
                break;

            case CONTROL_CONNECTION_DENIED:
                if (m_verbose)
                {
                    std::cerr << "[client] Connection denied by server" << std::endl;
                }
                m_connection.set_state(connection_state::DISCONNECTED);
                m_connected = false;
                break;

            case CONTROL_DISCONNECT:
                if (m_verbose)
                {
                    std::cout << "[client] Server disconnected us" << std::endl;
                }
                m_connection.set_state(connection_state::DISCONNECTED);
                m_connected = false;
                if (m_on_disconnected)
                {
                    m_on_disconnected();
                }
                break;

            case CONTROL_HEARTBEAT:
                // process_incoming already updated timestamps
                break;

            case CONTROL_CHANNEL_ACK:
            {
                // Payload: [type(1)][channel_id(1)][status(1)]
                if (payload_size >= 3)
                {
                    uint8_t ch_id = payload[1];
                    uint8_t status = payload[2];
                    if (m_pending_channel_id == static_cast<int>(ch_id))
                    {
                        m_channel_ack_status = status;
                        m_pending_channel_id = -1; // ACK received — unblock open_channel
                    }
                }
                break;
            }

            case CONTROL_BACKPRESSURE:
            {
                // Payload: [type(1)][available_slots(1)]
                // available=0 means throttle, >0 means resume
                if (payload_size >= 2)
                {
                    uint8_t available = payload[1];
                    m_connection.set_fragment_backpressured(available == 0);
                }
                break;
            }
        }
    }

    uint16_t client::local_port() const
    {
        return m_socket.local_port();
    }

    // --- Channel negotiation ---

    int client::open_channel(channel_mode mode, uint8_t priority, const char *name, uint8_t hint)
    {
        if (m_connection.state() != connection_state::CONNECTED)
            return -1;

        // Register locally first (picks an available slot)
        int id = m_channels.open_channel(mode, priority, name, hint);
        if (id < 0)
            return -1;

        // Build CHANNEL_OPEN payload: [type(1)][channel_id(1)][mode(1)][priority(1)][name(up to 32)]
        uint8_t open_payload[4 + MAX_CHANNEL_NAME] = {};
        open_payload[0] = CONTROL_CHANNEL_OPEN;
        open_payload[1] = static_cast<uint8_t>(id);
        open_payload[2] = static_cast<uint8_t>(mode);
        open_payload[3] = priority;

        // Copy name into payload bytes [4..35]
        if (name)
        {
            std::strncpy(reinterpret_cast<char *>(&open_payload[4]), name, MAX_CHANNEL_NAME - 1);
        }
        size_t open_size = 4 + MAX_CHANNEL_NAME;

        m_pending_channel_id = id;
        m_channel_ack_status = CHANNEL_STATUS_REJECTED; // pessimistic default
        send_control_payload(open_payload, open_size);

        // Wait for ACK with retries (synchronous, like the handshake)
        auto retry_interval = std::chrono::milliseconds(CHANNEL_OPEN_RETRY_INTERVAL_MS);
        auto last_attempt = std::chrono::steady_clock::now();
        int attempt = 0;

        while (attempt < CHANNEL_OPEN_MAX_ATTEMPTS)
        {
            poll();

            if (m_pending_channel_id < 0)
            {
                // ACK received
                if (m_channel_ack_status == CHANNEL_STATUS_ACCEPTED)
                {
                    if (m_verbose)
                    {
                        std::cout << "[client] Channel " << id << " accepted by server" << std::endl;
                    }
                    return id;
                }
                else
                {
                    // Rejected — roll back local registration
                    if (m_verbose)
                    {
                        std::cerr << "[client] Channel " << id << " rejected by server" << std::endl;
                    }
                    m_channels.unregister_channel(static_cast<uint8_t>(id));
                    return -1;
                }
            }

            auto now = std::chrono::steady_clock::now();
            if (now - last_attempt >= retry_interval)
            {
                ++attempt;
                send_control_payload(open_payload, open_size);
                last_attempt = now;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        // Timed out — roll back
        if (m_verbose)
        {
            std::cerr << "[client] Channel " << id << " open timed out" << std::endl;
        }
        m_pending_channel_id = -1;
        m_channels.unregister_channel(static_cast<uint8_t>(id));
        return -1;
    }

} // namespace entanglement
