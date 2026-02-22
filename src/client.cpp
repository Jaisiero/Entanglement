#include "client.h"
#include <cstring>
#include <iostream>

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
        // Bind to any available local port
        if (!m_socket.bind(0))
        {
            return false;
        }

        if (!m_socket.set_non_blocking(true))
        {
            std::cerr << "[client] Failed to set non-blocking mode" << std::endl;
            m_socket.close();
            return false;
        }

        m_connection.reset();
        m_connection.set_active(true);
        m_connected = true;
        if (m_verbose)
        {
            std::cout << "[client] Ready on local port " << m_socket.local_port() << " -> " << m_server_address << ":"
                      << m_server_port << std::endl;
        }
        return true;
    }

    void client::disconnect()
    {
        if (!m_connected)
            return;
        m_connected = false;
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
        if (!m_connected)
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

            // Process seq/ack through the connection
            bool is_new = m_connection.process_incoming(header);

            if (is_new && m_on_response)
            {
                m_on_response(header, payload, header.payload_size);
            }
            ++count;
        }

        return count;
    }

    void client::set_on_response(on_response_received callback)
    {
        m_on_response = std::move(callback);
    }

    uint16_t client::local_port() const
    {
        return m_socket.local_port();
    }

} // namespace entanglement
