#pragma once

#include "udp_connection.h"
#include "udp_socket.h"
#include <atomic>
#include <functional>
#include <string>

namespace entanglement
{

    // Callback: data packet received from server
    using on_response_received =
        std::function<void(const packet_header &header, const uint8_t *payload, size_t payload_size)>;

    // Callback: reliable packet detected as lost
    using on_packet_lost = std::function<void(const lost_packet_info &info)>;

    // Callback: connection was lost (timeout or server kicked us)
    using on_disconnected = std::function<void()>;

    class client
    {
    public:
        client(const std::string &server_address, uint16_t server_port);
        ~client();

        // Non-copyable, non-movable
        client(const client &) = delete;
        client &operator=(const client &) = delete;

        // Perform handshake with the server (blocking, ~5 s timeout with retries).
        // Returns true if CONNECTION_ACCEPTED was received.
        bool connect();

        // Send DISCONNECT and close the socket.
        void disconnect();

        bool is_connected() const { return m_connected.load(); }

        // Send a data packet (header gets seq/ack filled automatically)
        int send(packet_header &header, const void *payload = nullptr);

        // Send raw payload with auto-filled header fields
        int send_payload(const void *data, size_t size, uint8_t flags = 0, uint8_t channel_id = 0);

        // Receive and dispatch incoming packets
        int poll(int max_packets = DEFAULT_MAX_POLL_PACKETS);

        // Check heartbeat/timeout and collect losses.
        // Sends heartbeat if idle, triggers on_disconnected if timed out.
        // Returns the number of losses detected.
        int update(on_packet_lost loss_callback = nullptr);

        // Callbacks
        void set_on_response(on_response_received callback);
        void set_on_disconnected(on_disconnected callback);

        udp_connection &connection() { return m_connection; }

        void set_verbose(bool verbose) { m_verbose = verbose; }
        bool verbose() const { return m_verbose; }

        uint16_t local_port() const;

    private:
        udp_socket m_socket;
        udp_connection m_connection;
        std::string m_server_address;
        uint16_t m_server_port;
        std::atomic<bool> m_connected{false};
        bool m_verbose = true;
        on_response_received m_on_response;
        on_disconnected m_on_disconnected;

        // Send a control packet (FLAG_CONTROL + type byte)
        void send_control(uint8_t control_type);

        // Dispatch incoming control packet
        void handle_control(uint8_t control_type);
    };

} // namespace entanglement
