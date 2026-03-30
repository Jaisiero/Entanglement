#include "client.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <thread>

namespace entanglement
{

    client::client(const std::string &server_address, uint16_t server_port)
        : m_server_endpoint(endpoint_from_string(server_address, server_port))
    {
    }

    client::~client()
    {
        disconnect();
    }

    error_code client::connect()
    {
        if (auto ec = m_socket.bind(0); failed(ec))
            return ec;

        if (auto ec = m_socket.set_non_blocking(true); failed(ec))
        {
            if (m_verbose)
            {
                std::cerr << "[client] Failed to set non-blocking mode" << std::endl;
            }
            m_socket.close();
            return ec;
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
                    std::string addr = endpoint_address_string(m_server_endpoint);
                    std::cout << "[client] Connected to " << addr << ":" << m_server_endpoint.port << " (port "
                              << m_socket.local_port() << ")" << std::endl;
                }
                if (m_on_connected)
                {
                    m_on_connected();
                }
                return error_code::ok;
            }

            if (m_connection.state() == connection_state::DISCONNECTED)
            {
                // Got CONNECTION_DENIED
                m_socket.close();
                m_connection.reset();
                return error_code::connection_denied;
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
        return error_code::connection_timeout;
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

    int client::send_raw(packet_header &header, const void *payload)
    {
        bool reliable = m_channels.is_reliable(header.channel_id);
        m_connection.prepare_header(header, reliable);
        return m_socket.send_packet(header, payload, m_server_endpoint);
    }

    int client::send(const void *data, size_t size, uint8_t channel_id, uint8_t flags, uint32_t *out_message_id,
                     uint64_t *out_sequence, uint32_t channel_sequence, uint32_t *out_channel_sequence)
    {
        return m_connection.send_payload(m_socket, m_channels, data, size, flags, channel_id, m_server_endpoint,
                                         out_message_id, out_sequence, channel_sequence, out_channel_sequence);
    }

    int client::send_fragment(uint32_t message_id, uint8_t fragment_index, uint8_t fragment_count, const void *data,
                              size_t size, uint8_t flags, uint8_t channel_id)
    {
        return m_connection.send_fragment(m_socket, m_channels, message_id, fragment_index, fragment_count, data, size,
                                          flags, channel_id, m_server_endpoint);
    }

    int client::poll(int max_packets)
    {
        int count = 0;
        packet_header header{};
        uint8_t payload[MAX_PAYLOAD_SIZE];
        endpoint_key sender{};

        while (count < max_packets)
        {
            int result = m_socket.recv_packet(header, payload, MAX_PAYLOAD_SIZE, sender);
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
                auto frag_result = m_connection.reassembler().process_fragment(
                    sender, header.channel_id, fhdr, payload + FRAGMENT_HEADER_SIZE,
                    header.payload_size - FRAGMENT_HEADER_SIZE, header.channel_sequence);

                // If reassembler is full/under pressure, send backpressure to server
                if (frag_result == fragment_result::slots_full ||
                    m_connection.reassembler().usage_percent() >= BACKPRESSURE_HIGH_WATERMARK)
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
                // RELIABLE_ORDERED channels: deliver_ordered handles hold-back + drain
                if (!m_connection.deliver_ordered(header, payload, header.payload_size, m_channels))
                {
                    m_on_data_received(header, payload, header.payload_size);
                }
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

        // Flush pending ACKs so the server's RTT isn't inflated
        if (m_connection.needs_ack_flush(now))
        {
            m_connection.send_ack_flush(m_socket, m_server_endpoint);
        }

        // Collect losses
        lost_packet_info lost[MAX_LOSSES_PER_UPDATE];
        int count = m_connection.collect_losses(now, lost, MAX_LOSSES_PER_UPDATE);

        auto &cb = loss_callback ? loss_callback : m_on_packet_lost;
        for (int i = 0; i < count; ++i)
        {
            // Try auto-retransmit first; only report to app if it can't be handled
            if (m_connection.auto_retransmit_enabled() &&
                m_connection.try_auto_retransmit(lost[i], m_socket, m_channels, m_server_endpoint))
                continue;
            if (cb)
                cb(lost[i]);
        }

        // Expire stale reassembly entries
        auto &ra = m_connection.reassembler();
        ra.cleanup_stale(now, ra.reassembly_timeout());

        // Check for ordered-delivery stalls (auto-skip gaps that block delivery)
        m_connection.check_ordered_stalls(m_channels);

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
        m_on_data_received = callback;
        // Wire the same callback into the ordered-delivery drain path
        m_connection.set_on_ordered_packet_deliver(
            [callback](const packet_header &h, const uint8_t *p, uint16_t s)
            {
                if (callback)
                    callback(h, p, s);
            });
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
        m_connection.install_ordered_complete_wrapper(&m_channels, cb);
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
        m_socket.send_packet(header, payload, m_server_endpoint);
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
            return static_cast<int>(error_code::not_connected);

        // Register locally first (picks an available slot)
        int id = m_channels.open_channel(mode, priority, name, hint);
        if (id < 0)
            return id; // propagate error_code

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
                    return static_cast<int>(error_code::channel_rejected);
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
        return static_cast<int>(error_code::channel_timeout);
    }

} // namespace entanglement
