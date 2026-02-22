#pragma once

#include "udp_connection.h"
#include "udp_socket.h"
#include <atomic>
#include <functional>
#include <string>

namespace entanglement
{

    // Callback invoked when a response packet is received from the server
    using on_response_received =
        std::function<void(const packet_header &header, const uint8_t *payload, size_t payload_size)>;

    class client
    {
    public:
        client(const std::string &server_address, uint16_t server_port);
        ~client();

        // Non-copyable, non-movable
        client(const client &) = delete;
        client &operator=(const client &) = delete;

        // Initialize the client socket (binds to ephemeral port)
        bool connect();
        void disconnect();
        bool is_connected() const { return m_connected.load(); }

        // Send a packet to the server (header gets seq/ack filled automatically)
        int send(packet_header &header, const void *payload = nullptr);

        // Send raw payload with auto-filled header fields
        int send_payload(const void *data, size_t size, uint8_t flags = 0, uint8_t channel_id = 0);

        // Process incoming packets from the server (up to max_packets per call)
        int poll(int max_packets = DEFAULT_MAX_POLL_PACKETS);

        // Callback type: given a sequence number, return pointer+size of the
        // payload to retransmit.  Return {nullptr, 0} if unavailable.
        using payload_provider = std::function<std::pair<const uint8_t *, uint16_t>(uint64_t sequence)>;

        // Process retransmissions of reliable unACKed packets.
        // The application provides payloads via the callback. Returns retransmissions sent.
        int update(payload_provider provider);

        // Set the response callback
        void set_on_response(on_response_received callback);

        udp_connection &connection() { return m_connection; }

        // Suppress internal cout messages (useful for multi-threaded tests)
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
    };

} // namespace entanglement
