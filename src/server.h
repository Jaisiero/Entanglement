#pragma once

#include "udp_connection.h"
#include "udp_socket.h"
#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace entanglement
{

    // Callback invoked when a valid packet is received
    using on_packet_received =
        std::function<void(const packet_header &header, const uint8_t *payload, size_t payload_size,
                           const std::string &sender_address, uint16_t sender_port)>;

    class server
    {
    public:
        explicit server(uint16_t port, const std::string &bind_address = "0.0.0.0");
        ~server();

        // Non-copyable, non-movable
        server(const server &) = delete;
        server &operator=(const server &) = delete;

        bool start();
        void stop();
        bool is_running() const { return m_running.load(); }

        // Process pending packets (call from your game loop or a dedicated thread)
        // Returns the number of packets processed (up to max_packets per call)
        int poll(int max_packets = DEFAULT_MAX_POLL_PACKETS);

        // Set the packet received callback
        void set_on_packet_received(on_packet_received callback);

        // Send a response back to a client (header gets seq/ack filled automatically)
        int send_to(packet_header &header, const void *payload, const std::string &address, uint16_t port);

        // Disconnect a specific client
        void disconnect_client(const endpoint_key &key);

        // Disconnect all clients
        void disconnect_all();

        uint16_t port() const { return m_port; }
        size_t connection_count() const { return m_index.size(); }

    private:
        udp_socket m_socket;
        uint16_t m_port;
        std::string m_bind_address;
        std::atomic<bool> m_running{false};
        on_packet_received m_on_packet_received;

        // Connection pool (heap-allocated, too large for stack) + index lookup
        std::unique_ptr<std::array<udp_connection, MAX_CONNECTIONS>> m_pool;
        std::unordered_map<endpoint_key, uint16_t, endpoint_key_hash> m_index;

        // Find or create a connection for an endpoint. Returns nullptr if pool full.
        udp_connection *find_or_create(const endpoint_key &key);

        // Find existing connection. Returns nullptr if not found.
        udp_connection *find(const endpoint_key &key);

        // Allocate a free slot in the pool. Returns index or -1 if full.
        int allocate_slot();
    };

} // namespace entanglement
