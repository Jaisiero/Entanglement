#include "client.h"
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

        constexpr int MAX_ATTEMPTS = 10;
        constexpr auto RETRY_INTERVAL = std::chrono::milliseconds(500);
        auto last_attempt = std::chrono::steady_clock::now();
        int attempt = 0;

        while (attempt < MAX_ATTEMPTS)
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
            if (now - last_attempt >= RETRY_INTERVAL)
            {
                ++attempt;
                if (m_verbose && attempt > 1)
                {
                    std::cout << "[client] Retrying connection (" << attempt << "/" << MAX_ATTEMPTS << ")" << std::endl;
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
        m_connection.prepare_header(header);
        return m_socket.send_packet(header, payload, m_server_address, m_server_port);
    }

    int client::send_payload(const void *data, size_t size, uint8_t flags, uint8_t channel_id)
    {
        packet_header header{};
        header.flags = flags;
        header.shard_id = 0;
        header.channel_id = channel_id;
        header.reserved = 0;
        header.payload_size = static_cast<uint16_t>(size);

        return send(header, data);
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
                handle_control(payload[0]);
                ++count;
                continue;
            }

            // Data packets — only when connected
            if (m_connection.state() == connection_state::CONNECTED && m_on_response)
            {
                m_on_response(header, payload, header.payload_size);
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

        if (loss_callback)
        {
            for (int i = 0; i < count; ++i)
            {
                loss_callback(lost[i]);
            }
        }

        return count;
    }

    void client::set_on_response(on_response_received callback)
    {
        m_on_response = std::move(callback);
    }

    void client::set_on_disconnected(on_disconnected callback)
    {
        m_on_disconnected = std::move(callback);
    }

    void client::send_control(uint8_t control_type)
    {
        packet_header header{};
        header.flags = FLAG_CONTROL;
        header.payload_size = 1;
        send(header, &control_type);
    }

    void client::handle_control(uint8_t control_type)
    {
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
        }
    }

    uint16_t client::local_port() const
    {
        return m_socket.local_port();
    }

} // namespace entanglement
