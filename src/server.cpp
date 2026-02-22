#include "server.h"
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
            std::cerr << "[server] Failed to set non-blocking mode" << std::endl;
            m_socket.close();
            return false;
        }

        m_running = true;
        std::cout << "[server] Listening on " << m_bind_address << ":" << m_port << std::endl;
        return true;
    }

    void server::stop()
    {
        if (!m_running)
            return;
        m_running = false;
        disconnect_all();
        m_socket.close();
        std::cout << "[server] Stopped" << std::endl;
    }

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

            // Build endpoint key from the sender
            endpoint_key key{};
            inet_pton(AF_INET, sender_addr.c_str(), &key.address);
            key.port = sender_port;

            // Find or create connection for this peer
            udp_connection *conn = find_or_create(key);
            if (!conn)
            {
                std::cerr << "[server] Connection pool full, dropping packet from " << sender_addr << ":" << sender_port
                          << std::endl;
                ++count;
                continue;
            }

            // Process seq/ack through the connection
            bool is_new = conn->process_incoming(header);

            if (is_new && m_on_packet_received)
            {
                m_on_packet_received(header, payload, header.payload_size, sender_addr, sender_port);
            }
            ++count;
        }

        return count;
    }

    void server::set_on_packet_received(on_packet_received callback)
    {
        m_on_packet_received = std::move(callback);
    }

    int server::send_to(packet_header &header, const void *payload, const std::string &address, uint16_t port)
    {
        // Find the connection to fill seq/ack
        endpoint_key key{};
        inet_pton(AF_INET, address.c_str(), &key.address);
        key.port = port;

        udp_connection *conn = find(key);
        if (conn)
        {
            conn->prepare_header(header);
        }

        return m_socket.send_packet(header, payload, address, port);
    }

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
        // Check existing
        auto it = m_index.find(key);
        if (it != m_index.end())
        {
            return &(*m_pool)[it->second];
        }

        // Allocate new slot
        int slot = allocate_slot();
        if (slot < 0)
            return nullptr;

        auto &conn = (*m_pool)[slot];
        conn.reset();
        conn.set_active(true);
        conn.set_endpoint(key);
        m_index[key] = static_cast<uint16_t>(slot);

        std::cout << "[server] New connection (slot " << slot << "), total: " << m_index.size() << std::endl;
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

} // namespace entanglement
