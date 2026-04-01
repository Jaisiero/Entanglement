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

    // Callback: data packet received from server (non-fragmented)
    using on_data_received =
        std::function<void(const packet_header &header, const uint8_t *payload, size_t payload_size)>;

    // Callback: reliable packet detected as lost
    using on_packet_lost = std::function<void(const lost_packet_info &info)>;

    // Callback: connection established successfully
    using on_connected = std::function<void()>;

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
        // Returns error_code::ok on success.
        error_code connect();

        // Send DISCONNECT and close the socket.
        void disconnect();

        bool is_connected() const { return m_connected.load(); }

        // Send a data packet with a pre-built header (low-level).
        // The header gets seq/ack filled automatically.
        // Use send() instead for normal application data.
        int send_raw(packet_header &header, const void *payload = nullptr);

        // Unified send: auto-handles simple and fragmented paths.
        // Messages <= MAX_PAYLOAD_SIZE are sent as a single packet;
        // larger messages are automatically fragmented.
        // out_message_id:       if non-null, receives the library message_id (non-zero for fragmented sends).
        // out_sequence:         if non-null, receives the packet sequence (only for single-packet sends).
        // channel_sequence:     if non-zero, reuses this channel_sequence (for ordered retransmissions).
        // out_channel_sequence: if non-null, receives the channel_sequence that was assigned/used.
        // Returns bytes of user data sent, or a negative error_code on failure.
        int send(const void *data, size_t size, uint8_t channel_id = 0, uint8_t flags = 0,
                 uint32_t *out_message_id = nullptr, uint64_t *out_sequence = nullptr, uint32_t channel_sequence = 0,
                 uint32_t *out_channel_sequence = nullptr);

        // Retransmit a single fragment of a previously started fragmented message.
        // For manual loss recovery only — prefer enable_auto_retransmit() instead.
        int send_fragment(uint32_t message_id, uint8_t fragment_index, uint8_t fragment_count, const void *data,
                          size_t size, uint8_t flags, uint8_t channel_id);

        // Receive and dispatch incoming packets
        int poll(int max_packets = DEFAULT_MAX_POLL_PACKETS);

        // Check heartbeat/timeout and collect losses.
        // Sends heartbeat if idle, triggers on_disconnected if timed out.
        // Returns the number of losses detected.
        // If a loss_callback is provided it overrides the stored one for this call.
        int update(on_packet_lost loss_callback = nullptr);

        // Callbacks
        void set_on_data_received(on_data_received callback);
        void set_on_connected(on_connected callback);
        void set_on_disconnected(on_disconnected callback);
        void set_on_packet_lost(on_packet_lost callback);

        udp_connection &connection() { return m_connection; }

        // Channel configuration
        channel_manager &channels() { return m_channels; }
        const channel_manager &channels() const { return m_channels; }

        // Open a remote channel via negotiation with the server.
        // Registers locally, sends CONTROL_CHANNEL_OPEN and waits for ACK.
        // Returns the assigned channel_id (>= 0) or a negative error_code on failure.
        // Must be called while connected.
        int open_channel(channel_mode mode, uint8_t priority = 128, const char *name = "", uint8_t hint = 4);

        // Fragmentation: receiver callbacks (app-provided buffer management)
        void set_on_allocate_message(on_allocate_message cb);
        void set_on_message_complete(on_message_complete cb);
        void set_on_message_failed(on_message_failed cb);

        // Fragmentation: sender callback (all fragments ACKed)
        void set_on_message_acked(on_message_acked cb);

        // Override the reassembly timeout (default: REASSEMBLY_TIMEOUT_US).
        void set_reassembly_timeout(int64_t timeout_us)
        {
            m_connection.reassembler().set_reassembly_timeout(timeout_us);
        }

        // Fragment flow control: true if the server asked us to stop sending fragments
        bool is_fragment_throttled() const { return m_connection.is_fragment_backpressured(); }

        // Enable automatic retransmission for reliable channels.
        // Lost reliable packets are re-sent from an internal buffer automatically.
        void enable_auto_retransmit() { m_connection.enable_auto_retransmit(); }
        bool auto_retransmit_enabled() const { return m_connection.auto_retransmit_enabled(); }

        // Flush all pending coalesced message buffers immediately.
        void flush_coalesce() { m_connection.flush_all_coalesce(m_socket, m_channels, m_server_endpoint); }

        // Congestion control: application queries these to pace sends
        bool can_send() const { return m_connection.can_send(); }
        congestion_info congestion() const { return m_connection.congestion(); }

        void set_verbose(bool verbose) { m_verbose = verbose; }
        bool verbose() const { return m_verbose; }

#ifdef ENTANGLEMENT_SIMULATE_LOSS
        void set_simulated_drop_rate(double rate) { m_socket.set_drop_rate(rate); }
        double simulated_drop_rate() const { return m_socket.drop_rate(); }
        uint64_t simulated_drop_count() const { return m_socket.drop_count(); }
#endif

        uint16_t local_port() const;

    private:
        udp_socket m_socket;
        udp_connection m_connection;
        channel_manager m_channels;
        endpoint_key m_server_endpoint; // precomputed from address + port
        std::atomic<bool> m_connected{false};
        bool m_verbose = true;
        on_data_received m_on_data_received;
        on_connected m_on_connected;
        on_disconnected m_on_disconnected;
        on_packet_lost m_on_packet_lost;

        // Pending channel-open negotiation state
        int m_pending_channel_id = -1;    // channel id awaiting ACK, or -1
        uint8_t m_channel_ack_status = 0; // last received ACK status

        // Send a control packet (FLAG_CONTROL + type byte)
        void send_control(uint8_t control_type);

        // Send a multi-byte control payload
        void send_control_payload(const void *payload, size_t size);

        // Dispatch incoming control packet
        void handle_control(const uint8_t *payload, size_t payload_size);
    };

} // namespace entanglement
