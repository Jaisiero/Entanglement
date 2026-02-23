#pragma once

#include "channel_manager.h"
#include "fragmentation.h"
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

    // Callback: a client requests opening a dynamic channel.
    // Return true to accept, false to reject.  If no callback is set the server accepts all.
    using on_channel_requested =
        std::function<bool(const endpoint_key &key, uint8_t channel_id, channel_mode mode, uint8_t priority)>;

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
        void set_on_channel_requested(on_channel_requested callback);

        // Send a data packet to a connected client
        int send_to(packet_header &header, const void *payload, const std::string &address, uint16_t port);

        // Send payload to a connected client, auto-fragmenting if needed.
        // Returns bytes of user data sent, or -1 on error.
        int send_payload_to(const void *data, size_t size, uint8_t channel_id, const std::string &address,
                            uint16_t port, uint8_t flags = 0);

        // Disconnect a specific client
        void disconnect_client(const endpoint_key &key);

        // Disconnect all clients
        void disconnect_all();

        uint16_t port() const { return m_port; }
        size_t connection_count() const { return m_index.size(); }

        void set_verbose(bool verbose) { m_verbose = verbose; }
        bool verbose() const { return m_verbose; }

        // Fragmentation: receiver callbacks
        void set_on_allocate_message(on_allocate_message cb);
        void set_on_message_complete(on_message_complete cb);
        void set_on_message_expired(on_message_expired cb);

        // Override the reassembly timeout (default: REASSEMBLY_TIMEOUT_US).
        void set_reassembly_timeout(int64_t timeout_us) { m_reassembly_timeout_us = timeout_us; }

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
        fragment_reassembler m_reassembler;
        int64_t m_reassembly_timeout_us = REASSEMBLY_TIMEOUT_US;
        on_packet_received m_on_packet_received;
        on_client_connected m_on_client_connected;
        on_client_disconnected m_on_client_disconnected;
        on_channel_requested m_on_channel_requested;

        // Connection pool + index
        std::unique_ptr<std::array<udp_connection, MAX_CONNECTIONS>> m_pool;
        std::unordered_map<endpoint_key, uint16_t, endpoint_key_hash> m_index;

        udp_connection *find_or_create(const endpoint_key &key);
        udp_connection *find(const endpoint_key &key);
        int allocate_slot();

        // Control packet handling
        void handle_control(const endpoint_key &key, const packet_header &header, const uint8_t *payload,
                            size_t payload_size, const std::string &address, uint16_t port);

        // Send a control packet through an established connection
        void send_control_to(udp_connection *conn, uint8_t control_type, const std::string &address, uint16_t port);

        // Send a multi-byte control payload through an established connection
        void send_control_payload_to(udp_connection *conn, const void *payload, size_t size, const std::string &address,
                                     uint16_t port);

        // Send a control packet without a connection (e.g. CONNECTION_DENIED)
        void send_raw_control(uint8_t control_type, const std::string &address, uint16_t port);

        // Send a single fragment to a client (called from send_payload_to)
        int send_fragment_to(udp_connection *conn, uint32_t message_id, uint8_t index, uint8_t count, const void *data,
                             size_t size, uint8_t flags, uint8_t channel_id, const std::string &address, uint16_t port);
    };

} // namespace entanglement
