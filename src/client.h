#pragma once

#include "channel_manager.h"
#include "congestion_control.h"
#include "fragmentation.h"
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

        // Send raw payload with auto-filled header fields.
        // Messages <= MAX_PAYLOAD_SIZE are sent as a single packet.
        // Larger messages are automatically fragmented.
        // Returns bytes of user data sent, or -1 on error.
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

        // Channel configuration
        channel_manager &channels() { return m_channels; }
        const channel_manager &channels() const { return m_channels; }

        // Open a remote channel via negotiation with the server.
        // Registers locally, sends CONTROL_CHANNEL_OPEN and waits for ACK.
        // Returns the assigned channel_id (>= 0) or -1 on failure/rejection.
        // Must be called while connected.
        int open_channel(channel_mode mode, uint8_t priority = 128, const char *name = "", uint8_t hint = 4);

        // Fragmentation: receiver callbacks (app-provided buffer management)
        void set_on_allocate_message(on_allocate_message cb);
        void set_on_message_complete(on_message_complete cb);
        void set_on_message_expired(on_message_expired cb);

        // Fragmentation: sender callback (all fragments ACKed)
        void set_on_message_acked(on_message_acked cb);

        // Override the reassembly timeout (default: REASSEMBLY_TIMEOUT_US).
        void set_reassembly_timeout(int64_t timeout_us) { m_reassembly_timeout_us = timeout_us; }

        // Congestion control: application queries these to pace sends
        bool can_send() const { return m_connection.can_send(); }
        congestion_info congestion() const { return m_connection.congestion(); }

        void set_verbose(bool verbose) { m_verbose = verbose; }
        bool verbose() const { return m_verbose; }

        uint16_t local_port() const;

    private:
        udp_socket m_socket;
        udp_connection m_connection;
        channel_manager m_channels;
        std::string m_server_address;
        uint16_t m_server_port;
        std::atomic<bool> m_connected{false};
        bool m_verbose = true;
        on_response_received m_on_response;
        on_disconnected m_on_disconnected;

        // Pending channel-open negotiation state
        int m_pending_channel_id = -1;    // channel id awaiting ACK, or -1
        uint8_t m_channel_ack_status = 0; // last received ACK status

        // Fragment reassembly (receiver side)
        fragment_reassembler m_reassembler;
        int64_t m_reassembly_timeout_us = REASSEMBLY_TIMEOUT_US;

        // Send a control packet (FLAG_CONTROL + type byte)
        void send_control(uint8_t control_type);

        // Send a multi-byte control payload
        void send_control_payload(const void *payload, size_t size);

        // Dispatch incoming control packet
        void handle_control(const uint8_t *payload, size_t payload_size);

        // Send a single fragment (called from send_payload for fragmented paths)
        int send_fragment(uint32_t message_id, uint8_t index, uint8_t count, const void *data, size_t size,
                          uint8_t flags, uint8_t channel_id);
    };

} // namespace entanglement
