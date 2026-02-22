#pragma once

#include "channel_manager.h"
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

    // Callback: data packet from a connected client
    using on_packet_received =
        std::function<void(const packet_header &header, const uint8_t *payload, size_t payload_size,
                           const std::string &sender_address, uint16_t sender_port)>;

    // Callback: a client completed the handshake
    using on_client_connected = std::function<void(const endpoint_key &key, const std::string &address, uint16_t port)>;

    // Callback: a client disconnected (explicit or timeout)
    using on_client_disconnected =
        std::function<void(const endpoint_key &key, const std::string &address, uint16_t port)>;

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

        // Process pending packets (call from your game loop)
        int poll(int max_packets = DEFAULT_MAX_POLL_PACKETS);

        // Check connection timeouts and send heartbeats. Call from your game loop after poll().
        // Returns the number of connections that timed out.
        int update();

        // Callbacks
        void set_on_packet_received(on_packet_received callback);
        void set_on_client_connected(on_client_connected callback);
        void set_on_client_disconnected(on_client_disconnected callback);

        // Send a data packet to a connected client
        int send_to(packet_header &header, const void *payload, const std::string &address, uint16_t port);

        // Disconnect a specific client
        void disconnect_client(const endpoint_key &key);

        // Disconnect all clients
        void disconnect_all();

        uint16_t port() const { return m_port; }
        size_t connection_count() const { return m_index.size(); }

        void set_verbose(bool verbose) { m_verbose = verbose; }
        bool verbose() const { return m_verbose; }

        // Channel configuration
        channel_manager &channels() { return m_channels; }
        const channel_manager &channels() const { return m_channels; }

    private:
        udp_socket m_socket;
        uint16_t m_port;
        std::string m_bind_address;
        std::atomic<bool> m_running{false};
        bool m_verbose = true;
        channel_manager m_channels;
        on_packet_received m_on_packet_received;
        on_client_connected m_on_client_connected;
        on_client_disconnected m_on_client_disconnected;

        // Connection pool + index
        std::unique_ptr<std::array<udp_connection, MAX_CONNECTIONS>> m_pool;
        std::unordered_map<endpoint_key, uint16_t, endpoint_key_hash> m_index;

        udp_connection *find_or_create(const endpoint_key &key);
        udp_connection *find(const endpoint_key &key);
        int allocate_slot();

        // Control packet handling
        void handle_control(const endpoint_key &key, const packet_header &header, uint8_t control_type,
                            const std::string &address, uint16_t port);

        // Send a control packet through an established connection
        void send_control_to(udp_connection *conn, uint8_t control_type, const std::string &address, uint16_t port);

        // Send a control packet without a connection (e.g. CONNECTION_DENIED)
        void send_raw_control(uint8_t control_type, const std::string &address, uint16_t port);
    };

} // namespace entanglement
