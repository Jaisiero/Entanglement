/*
 * entanglement.h — C API wrapper for the Entanglement UDP protocol library.
 *
 * Designed for FFI consumption (Rust, C#, Python, etc.).
 * All C++ types are hidden behind opaque pointers.
 * Callbacks use function pointers with a void* user_data context.
 */

#ifndef ENTANGLEMENT_C_API_H
#define ENTANGLEMENT_C_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /* -----------------------------------------------------------------------
     * Platform export macros
     * ----------------------------------------------------------------------- */

#if defined(_WIN32) || defined(_WIN64)
#ifdef ENTANGLEMENT_BUILDING_DLL
#define ENT_API __declspec(dllexport)
#else
#define ENT_API
#endif
#else
#ifdef ENTANGLEMENT_BUILDING_DLL
#define ENT_API __attribute__((visibility("default")))
#else
#define ENT_API
#endif
#endif

    /* -----------------------------------------------------------------------
     * Opaque handles
     * ----------------------------------------------------------------------- */

    typedef struct ent_client_t ent_client_t;
    typedef struct ent_server_t ent_server_t;

    /* -----------------------------------------------------------------------
     * Enums
     * ----------------------------------------------------------------------- */

    typedef enum ent_channel_mode
    {
        ENT_CHANNEL_UNRELIABLE = 0,
        ENT_CHANNEL_RELIABLE = 1,
        ENT_CHANNEL_RELIABLE_ORDERED = 2,
    } ent_channel_mode;

    typedef enum ent_error
    {
        ENT_OK = 0,
        ENT_ERROR_SOCKET = -1,
        ENT_ERROR_NOT_CONNECTED = -2,
        ENT_ERROR_CONNECTION_DENIED = -3,
        ENT_ERROR_CONNECTION_TIMEOUT = -4,
        ENT_ERROR_BACKPRESSURED = -5,
        ENT_ERROR_PAYLOAD_TOO_LARGE = -6,
        ENT_ERROR_INVALID_ARGUMENT = -7,
        ENT_ERROR_CHANNEL_IN_USE = -8,
        ENT_ERROR_CHANNEL_NOT_FOUND = -9,
        ENT_ERROR_CHANNEL_SLOTS_FULL = -10,
        ENT_ERROR_CHANNEL_REJECTED = -11,
        ENT_ERROR_CHANNEL_TIMEOUT = -12,
        ENT_ERROR_POOL_FULL = -13,
        ENT_ERROR_DUPLICATE_PACKET = -14,
        ENT_ERROR_ALREADY_STARTED = -15,
    } ent_error;

    typedef enum ent_message_fail_reason
    {
        ENT_MSG_FAIL_EXPIRED = 0,
        ENT_MSG_FAIL_EVICTED = 1,
    } ent_message_fail_reason;

    /* -----------------------------------------------------------------------
     * Value types (C-compatible mirrors of C++ structs)
     * ----------------------------------------------------------------------- */

    typedef struct ent_endpoint
    {
        uint32_t address; /* IPv4 in network byte order */
        uint16_t port;    /* in network byte order */
    } ent_endpoint;

#pragma pack(push, 1)
    typedef struct ent_packet_header
    {
        uint16_t magic;
        uint8_t version;
        uint8_t flags;
        uint16_t shard_id;
        uint8_t channel_id;
        uint8_t reserved;
        uint64_t sequence;
        uint64_t ack;
        uint32_t ack_bitmap;
        uint32_t channel_sequence;
        uint16_t payload_size;
    } ent_packet_header;
#pragma pack(pop)

    typedef struct ent_congestion_info
    {
        uint32_t cwnd;
        uint32_t in_flight;
        uint32_t ssthresh;
        int64_t pacing_interval_us;
        int in_slow_start; /* bool */
    } ent_congestion_info;

    typedef struct ent_lost_packet_info
    {
        uint64_t sequence;
        uint8_t flags;
        uint8_t channel_id;
        uint16_t shard_id;
        uint16_t payload_size;
        uint32_t channel_sequence;
        uint32_t message_id;
        uint8_t fragment_index;
        uint8_t fragment_count;
    } ent_lost_packet_info;

    typedef struct ent_channel_config
    {
        uint8_t id;
        ent_channel_mode mode;
        uint8_t priority;
        char name[32];
        int coalesce;              /* bool: enable message coalescing */
        uint16_t coalesce_max_bytes; /* max coalesced payload (0 = default 1160) */
    } ent_channel_config;

    /* -----------------------------------------------------------------------
     * Callback typedefs (all include void* user_data as last parameter)
     * ----------------------------------------------------------------------- */

    /* Client: data received from server */
    typedef void (*ent_on_data_received_fn)(const ent_packet_header *header, const uint8_t *payload,
                                            size_t payload_size, void *user_data);

    /* Client: connection events */
    typedef void (*ent_on_connected_fn)(void *user_data);
    typedef void (*ent_on_disconnected_fn)(void *user_data);

    /* Client: reliable packet lost */
    typedef void (*ent_on_packet_lost_fn)(const ent_lost_packet_info *info, void *user_data);

    /* Server: data received from client */
    typedef void (*ent_on_client_data_fn)(const ent_packet_header *header, const uint8_t *payload, size_t payload_size,
                                          ent_endpoint sender, void *user_data);

    /* Server: client connected/disconnected */
    typedef void (*ent_on_client_connected_fn)(ent_endpoint key, const char *address, uint16_t port, void *user_data);
    typedef void (*ent_on_client_disconnected_fn)(ent_endpoint key, const char *address, uint16_t port,
                                                  void *user_data);

    /* Server: channel open request (return non-zero to accept) */
    typedef int (*ent_on_channel_requested_fn)(ent_endpoint key, uint8_t channel_id, ent_channel_mode mode,
                                               uint8_t priority, void *user_data);

    /* Server: reliable packet lost for a specific client */
    typedef void (*ent_on_server_packet_lost_fn)(const ent_lost_packet_info *info, ent_endpoint client,
                                                 void *user_data);

    /* Fragmentation: allocate buffer for incoming message (return NULL to reject) */
    typedef uint8_t *(*ent_on_allocate_message_fn)(ent_endpoint sender, uint32_t message_id, uint8_t channel_id,
                                                   uint8_t fragment_count, size_t max_total_size, void *user_data);

    /* Fragmentation: all fragments of a message received */
    typedef void (*ent_on_message_complete_fn)(ent_endpoint sender, uint32_t message_id, uint8_t channel_id,
                                               uint8_t *data, size_t total_size, void *user_data);

    /* Fragmentation: incomplete message discarded */
    typedef void (*ent_on_message_failed_fn)(ent_endpoint sender, uint32_t message_id, uint8_t channel_id,
                                             uint8_t *app_buffer, ent_message_fail_reason reason,
                                             uint8_t received_count, uint8_t fragment_count, void *user_data);

    /* Fragmentation: all fragments of a sent message ACKed */
    typedef void (*ent_on_message_acked_fn)(uint32_t message_id, void *user_data);

    /* -----------------------------------------------------------------------
     * Utility
     * ----------------------------------------------------------------------- */

    /* Build endpoint from dotted-decimal string + port */
    ENT_API ent_endpoint ent_endpoint_from_string(const char *ip, uint16_t port);

    /* Convert endpoint address to dotted-decimal string.
     * Writes to buf (at least 16 bytes). Returns buf. */
    ENT_API const char *ent_endpoint_to_string(ent_endpoint ep, char *buf, size_t buf_size);

    /* -----------------------------------------------------------------------
     * Client API
     * ----------------------------------------------------------------------- */

    ENT_API ent_client_t *ent_client_create(const char *server_address, uint16_t server_port);
    ENT_API void ent_client_destroy(ent_client_t *c);

    ENT_API int ent_client_connect(ent_client_t *c);
    ENT_API void ent_client_disconnect(ent_client_t *c);
    ENT_API int ent_client_is_connected(const ent_client_t *c);

    /* Send data on a channel. Returns bytes sent or negative ent_error. */
    ENT_API int ent_client_send(ent_client_t *c, const void *data, size_t size, uint8_t channel_id, uint8_t flags,
                                uint32_t *out_message_id, uint64_t *out_sequence, uint32_t channel_sequence,
                                uint32_t *out_channel_sequence);

    /* Send pre-built header (low-level). Returns bytes sent or negative ent_error.
     * NOTE: `header` is an in/out parameter — the library fills internal fields
     * (sequence, ack_sequence, ack_bits, etc.) before sending.  The caller's
     * header is updated on return to reflect the values actually transmitted. */
    ENT_API int ent_client_send_raw(ent_client_t *c, ent_packet_header *header, const void *payload);

    /* Retransmit a single fragment (manual loss recovery). */
    ENT_API int ent_client_send_fragment(ent_client_t *c, uint32_t message_id, uint8_t fragment_index,
                                         uint8_t fragment_count, const void *data, size_t size, uint8_t flags,
                                         uint8_t channel_id);

    ENT_API int ent_client_poll(ent_client_t *c, int max_packets);
    ENT_API int ent_client_update(ent_client_t *c);

    /* Open a new channel via negotiation. Returns channel_id or negative ent_error. */
    ENT_API int ent_client_open_channel(ent_client_t *c, ent_channel_mode mode, uint8_t priority, const char *name);

    /* Register a channel locally (no negotiation). */
    ENT_API int ent_client_register_channel(ent_client_t *c, const ent_channel_config *config);

    /* Register default channel presets (unreliable, reliable, ordered). */
    ENT_API void ent_client_register_default_channels(ent_client_t *c);

    /* Callbacks */
    ENT_API void ent_client_set_on_data_received(ent_client_t *c, ent_on_data_received_fn fn, void *user_data);
    ENT_API void ent_client_set_on_connected(ent_client_t *c, ent_on_connected_fn fn, void *user_data);
    ENT_API void ent_client_set_on_disconnected(ent_client_t *c, ent_on_disconnected_fn fn, void *user_data);
    ENT_API void ent_client_set_on_packet_lost(ent_client_t *c, ent_on_packet_lost_fn fn, void *user_data);

    /* Fragmentation callbacks */
    ENT_API void ent_client_set_on_allocate_message(ent_client_t *c, ent_on_allocate_message_fn fn, void *user_data);
    ENT_API void ent_client_set_on_message_complete(ent_client_t *c, ent_on_message_complete_fn fn, void *user_data);
    ENT_API void ent_client_set_on_message_failed(ent_client_t *c, ent_on_message_failed_fn fn, void *user_data);
    ENT_API void ent_client_set_on_message_acked(ent_client_t *c, ent_on_message_acked_fn fn, void *user_data);

    /* Auto-retransmit */
    ENT_API void ent_client_enable_auto_retransmit(ent_client_t *c);
    ENT_API int ent_client_auto_retransmit_enabled(const ent_client_t *c);

    /* Congestion control */
    ENT_API int ent_client_can_send(const ent_client_t *c);
    ENT_API ent_congestion_info ent_client_congestion(const ent_client_t *c);

    /* Fragment flow control */
    ENT_API int ent_client_is_fragment_throttled(const ent_client_t *c);

    /* Reassembly timeout override (microseconds) */
    ENT_API void ent_client_set_reassembly_timeout(ent_client_t *c, int64_t timeout_us);

    /* Flush pending coalesced messages for all channels. */
    ENT_API void ent_client_flush_coalesce(ent_client_t *c);

    ENT_API void ent_client_set_verbose(ent_client_t *c, int verbose);
    ENT_API uint16_t ent_client_local_port(const ent_client_t *c);

    /* -----------------------------------------------------------------------
     * Server API
     * ----------------------------------------------------------------------- */

    ENT_API ent_server_t *ent_server_create(uint16_t port, const char *bind_address);
    ENT_API void ent_server_destroy(ent_server_t *s);

    ENT_API int ent_server_start(ent_server_t *s);
    ENT_API void ent_server_stop(ent_server_t *s);
    ENT_API int ent_server_is_running(const ent_server_t *s);

    ENT_API int ent_server_poll(ent_server_t *s, int max_packets);
    ENT_API int ent_server_update(ent_server_t *s);

    /* Send data to a client. Returns bytes sent or negative ent_error. */
    ENT_API int ent_server_send_to(ent_server_t *s, const void *data, size_t size, uint8_t channel_id,
                                   ent_endpoint dest, uint8_t flags, uint32_t *out_message_id);

    /* Send pre-built header (low-level).
     * NOTE: `header` is an in/out parameter — the library fills internal fields
     * (sequence, ack_sequence, ack_bits, etc.) before sending.  The caller's
     * header is updated on return to reflect the values actually transmitted. */
    ENT_API int ent_server_send_raw_to(ent_server_t *s, ent_packet_header *header, const void *payload,
                                       ent_endpoint dest);

    /* Retransmit a single fragment to a specific client. */
    ENT_API int ent_server_send_fragment_to(ent_server_t *s, uint32_t message_id, uint8_t fragment_index,
                                            uint8_t fragment_count, const void *data, size_t size, uint8_t flags,
                                            uint8_t channel_id, ent_endpoint dest, uint32_t channel_sequence);

    /* Client management */
    ENT_API void ent_server_disconnect_client(ent_server_t *s, ent_endpoint key);
    ENT_API void ent_server_disconnect_all(ent_server_t *s);
    ENT_API size_t ent_server_connection_count(const ent_server_t *s);
    ENT_API uint64_t ent_server_recv_queue_drops(const ent_server_t *s);

    /* Register a channel locally. */
    ENT_API int ent_server_register_channel(ent_server_t *s, const ent_channel_config *config);

    /* Register default channel presets. */
    ENT_API void ent_server_register_default_channels(ent_server_t *s);

    /* Threading / I/O configuration (call BEFORE ent_server_start) */
    ENT_API void ent_server_set_worker_count(ent_server_t *s, int count);
    ENT_API void ent_server_set_use_async_io(ent_server_t *s, int enabled);
    ENT_API void ent_server_set_socket_count(ent_server_t *s, int count);

    /* Auto-retransmit */
    ENT_API void ent_server_enable_auto_retransmit(ent_server_t *s);
    ENT_API int ent_server_auto_retransmit_enabled(const ent_server_t *s);

    /* Callbacks */
    ENT_API void ent_server_set_on_client_data(ent_server_t *s, ent_on_client_data_fn fn, void *user_data);
    ENT_API void ent_server_set_on_client_connected(ent_server_t *s, ent_on_client_connected_fn fn, void *user_data);
    ENT_API void ent_server_set_on_client_disconnected(ent_server_t *s, ent_on_client_disconnected_fn fn,
                                                       void *user_data);
    ENT_API void ent_server_set_on_channel_requested(ent_server_t *s, ent_on_channel_requested_fn fn, void *user_data);
    ENT_API void ent_server_set_on_packet_lost(ent_server_t *s, ent_on_server_packet_lost_fn fn, void *user_data);

    /* Fragmentation callbacks */
    ENT_API void ent_server_set_on_allocate_message(ent_server_t *s, ent_on_allocate_message_fn fn, void *user_data);
    ENT_API void ent_server_set_on_message_complete(ent_server_t *s, ent_on_message_complete_fn fn, void *user_data);
    ENT_API void ent_server_set_on_message_failed(ent_server_t *s, ent_on_message_failed_fn fn, void *user_data);

    /* Reassembly timeout override */
    ENT_API void ent_server_set_reassembly_timeout(ent_server_t *s, int64_t timeout_us);

    /* Fragment flow control */
    ENT_API int ent_server_is_fragment_throttled(const ent_server_t *s, ent_endpoint dest);

    /* Flush pending coalesced messages for a specific client. */
    ENT_API void ent_server_flush_coalesce(ent_server_t *s, ent_endpoint dest);

    ENT_API void ent_server_set_verbose(ent_server_t *s, int verbose);
    ENT_API uint16_t ent_server_port(const ent_server_t *s);

    /* -----------------------------------------------------------------------
     * Constants (exposed for FFI consumers)
     * ----------------------------------------------------------------------- */

#define ENT_MAX_PACKET_SIZE 1200
#define ENT_DEFAULT_PORT 9876
#define ENT_MAX_CONNECTIONS 4096
#define ENT_MAX_CHANNELS 256
#define ENT_MAX_CHANNEL_NAME 32
#define ENT_PACKET_HEADER_SIZE 34
#define ENT_FRAGMENT_HEADER_SIZE 6
#define ENT_MAX_FRAGMENT_PAYLOAD (ENT_MAX_PACKET_SIZE - ENT_PACKET_HEADER_SIZE - ENT_FRAGMENT_HEADER_SIZE)

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ENTANGLEMENT_C_API_H */
