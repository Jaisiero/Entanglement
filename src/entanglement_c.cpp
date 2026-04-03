/*
 * entanglement_c.cpp — C API implementation for Entanglement.
 *
 * Translates between the C-compatible types in entanglement.h and the
 * internal C++ classes.  Each opaque handle wraps a C++ object plus
 * stored user_data pointers for callbacks.
 */

#include "../include/entanglement.h"
#include "client.h"
#include "server.h"

#include <cstring>
#include <new>
#include <string>

using namespace entanglement;

/* -----------------------------------------------------------------------
 * Internal conversion helpers
 * ----------------------------------------------------------------------- */

static endpoint_key to_cpp(ent_endpoint ep)
{
    return {ep.address, ep.port};
}

static ent_endpoint to_c(const endpoint_key &ep)
{
    return {ep.address, ep.port};
}

static ent_packet_header to_c_header(const packet_header &h)
{
    ent_packet_header ch;
    static_assert(sizeof(ent_packet_header) == sizeof(packet_header), "header size mismatch");
    std::memcpy(&ch, &h, sizeof(ch));
    return ch;
}

static packet_header to_cpp_header(const ent_packet_header &ch)
{
    packet_header h;
    std::memcpy(&h, &ch, sizeof(h));
    return h;
}

static ent_lost_packet_info to_c_loss(const lost_packet_info &l)
{
    ent_lost_packet_info cl;
    cl.sequence = l.sequence;
    cl.flags = l.flags;
    cl.channel_id = l.channel_id;
    cl.shard_id = l.shard_id;
    cl.payload_size = l.payload_size;
    cl.channel_sequence = l.channel_sequence;
    cl.message_id = l.message_id;
    cl.fragment_index = l.fragment_index;
    cl.fragment_count = l.fragment_count;
    return cl;
}

static channel_mode to_cpp_mode(ent_channel_mode m)
{
    switch (m)
    {
        case ENT_CHANNEL_RELIABLE:
            return channel_mode::RELIABLE;
        case ENT_CHANNEL_RELIABLE_ORDERED:
            return channel_mode::RELIABLE_ORDERED;
        default:
            return channel_mode::UNRELIABLE;
    }
}

static channel_config to_cpp_config(const ent_channel_config &cc)
{
    channel_config cfg;
    cfg.id = cc.id;
    cfg.mode = to_cpp_mode(cc.mode);
    cfg.priority = cc.priority;
    cfg.coalesce = cc.coalesce != 0;
    cfg.coalesce_max_bytes = cc.coalesce_max_bytes;
    std::memset(cfg.name, 0, sizeof(cfg.name));
    std::strncpy(cfg.name, cc.name, MAX_CHANNEL_NAME - 1);
    return cfg;
}

/* -----------------------------------------------------------------------
 * Client wrapper
 * ----------------------------------------------------------------------- */

struct ent_client_t
{
    client cpp;
    ent_client_t(const std::string &addr, uint16_t port) : cpp(addr, port) {}
};

/* -----------------------------------------------------------------------
 * Server wrapper
 * ----------------------------------------------------------------------- */

struct ent_server_t
{
    server cpp;
    ent_server_t(uint16_t port, const std::string &bind) : cpp(port, bind) {}
};

/* -----------------------------------------------------------------------
 * Utility
 * ----------------------------------------------------------------------- */

ent_endpoint ent_endpoint_from_string(const char *ip, uint16_t port)
{
    auto ep = endpoint_from_string(ip ? ip : "0.0.0.0", port);
    return to_c(ep);
}

const char *ent_endpoint_to_string(ent_endpoint ep, char *buf, size_t buf_size)
{
    auto s = endpoint_address_string(to_cpp(ep));
    if (buf && buf_size > 0)
    {
        std::strncpy(buf, s.c_str(), buf_size - 1);
        buf[buf_size - 1] = '\0';
    }
    return buf;
}

/* -----------------------------------------------------------------------
 * Client: lifecycle
 * ----------------------------------------------------------------------- */

ent_client_t *ent_client_create(const char *server_address, uint16_t server_port)
{
    if (!server_address)
        return nullptr;
    return new (std::nothrow) ent_client_t(server_address, server_port);
}

void ent_client_destroy(ent_client_t *c)
{
    delete c;
}

int ent_client_connect(ent_client_t *c)
{
    if (!c)
        return ENT_ERROR_INVALID_ARGUMENT;
    return static_cast<int>(c->cpp.connect());
}

void ent_client_disconnect(ent_client_t *c)
{
    if (c)
        c->cpp.disconnect();
}

int ent_client_is_connected(const ent_client_t *c)
{
    return c ? c->cpp.is_connected() : 0;
}

/* -----------------------------------------------------------------------
 * Client: send
 * ----------------------------------------------------------------------- */

int ent_client_send(ent_client_t *c, const void *data, size_t size, uint8_t channel_id, uint8_t flags,
                    uint32_t *out_message_id, uint64_t *out_sequence, uint32_t channel_sequence,
                    uint32_t *out_channel_sequence)
{
    if (!c)
        return ENT_ERROR_INVALID_ARGUMENT;
    return c->cpp.send(data, size, channel_id, flags, out_message_id, out_sequence, channel_sequence,
                       out_channel_sequence);
}

int ent_client_send_raw(ent_client_t *c, ent_packet_header *header, const void *payload)
{
    if (!c || !header)
        return ENT_ERROR_INVALID_ARGUMENT;
    packet_header h = to_cpp_header(*header);
    int result = c->cpp.send_raw(h, payload);
    *header = to_c_header(h); /* write back filled fields */
    return result;
}

int ent_client_send_fragment(ent_client_t *c, uint32_t message_id, uint8_t fragment_index, uint8_t fragment_count,
                             const void *data, size_t size, uint8_t flags, uint8_t channel_id)
{
    if (!c)
        return ENT_ERROR_INVALID_ARGUMENT;
    return c->cpp.send_fragment(message_id, fragment_index, fragment_count, data, size, flags, channel_id);
}

/* -----------------------------------------------------------------------
 * Client: poll / update
 * ----------------------------------------------------------------------- */

int ent_client_poll(ent_client_t *c, int max_packets)
{
    if (!c)
        return 0;
    return c->cpp.poll(max_packets > 0 ? max_packets : DEFAULT_MAX_POLL_PACKETS);
}

int ent_client_update(ent_client_t *c)
{
    if (!c)
        return 0;
    return c->cpp.update();
}

/* -----------------------------------------------------------------------
 * Client: channels
 * ----------------------------------------------------------------------- */

int ent_client_open_channel(ent_client_t *c, ent_channel_mode mode, uint8_t priority, const char *name)
{
    if (!c)
        return ENT_ERROR_INVALID_ARGUMENT;
    return c->cpp.open_channel(to_cpp_mode(mode), priority, name ? name : "");
}

int ent_client_register_channel(ent_client_t *c, const ent_channel_config *config)
{
    if (!c || !config)
        return ENT_ERROR_INVALID_ARGUMENT;
    return static_cast<int>(c->cpp.channels().register_channel(to_cpp_config(*config)));
}

void ent_client_register_default_channels(ent_client_t *c)
{
    if (c)
        c->cpp.channels().register_defaults();
}

/* -----------------------------------------------------------------------
 * Client: callbacks
 * ----------------------------------------------------------------------- */

void ent_client_set_on_data_received(ent_client_t *c, ent_on_data_received_fn fn, void *user_data)
{
    if (!c)
        return;
    if (fn)
    {
        c->cpp.set_on_data_received(
            [fn, user_data](const packet_header &hdr, const uint8_t *payload, size_t size)
            {
                ent_packet_header ch = to_c_header(hdr);
                fn(&ch, payload, size, user_data);
            });
    }
    else
    {
        c->cpp.set_on_data_received(nullptr);
    }
}

void ent_client_set_on_connected(ent_client_t *c, ent_on_connected_fn fn, void *user_data)
{
    if (!c)
        return;
    if (fn)
        c->cpp.set_on_connected([fn, user_data]() { fn(user_data); });
    else
        c->cpp.set_on_connected(nullptr);
}

void ent_client_set_on_disconnected(ent_client_t *c, ent_on_disconnected_fn fn, void *user_data)
{
    if (!c)
        return;
    if (fn)
        c->cpp.set_on_disconnected([fn, user_data]() { fn(user_data); });
    else
        c->cpp.set_on_disconnected(nullptr);
}

void ent_client_set_on_packet_lost(ent_client_t *c, ent_on_packet_lost_fn fn, void *user_data)
{
    if (!c)
        return;
    if (fn)
    {
        c->cpp.set_on_packet_lost(
            [fn, user_data](const lost_packet_info &info)
            {
                ent_lost_packet_info ci = to_c_loss(info);
                fn(&ci, user_data);
            });
    }
    else
    {
        c->cpp.set_on_packet_lost(nullptr);
    }
}

void ent_client_set_on_allocate_message(ent_client_t *c, ent_on_allocate_message_fn fn, void *user_data)
{
    if (!c)
        return;
    if (fn)
    {
        c->cpp.set_on_allocate_message([fn, user_data](const endpoint_key &sender, uint32_t msg_id, uint8_t ch_id,
                                                       uint8_t frag_count, size_t max_size) -> uint8_t *
                                       { return fn(to_c(sender), msg_id, ch_id, frag_count, max_size, user_data); });
    }
    else
    {
        c->cpp.set_on_allocate_message(nullptr);
    }
}

void ent_client_set_on_message_complete(ent_client_t *c, ent_on_message_complete_fn fn, void *user_data)
{
    if (!c)
        return;
    if (fn)
    {
        c->cpp.set_on_message_complete([fn, user_data](const endpoint_key &sender, uint32_t msg_id, uint8_t ch_id,
                                                       uint8_t *data, size_t total_size)
                                       { fn(to_c(sender), msg_id, ch_id, data, total_size, user_data); });
    }
    else
    {
        c->cpp.set_on_message_complete(nullptr);
    }
}

void ent_client_set_on_message_failed(ent_client_t *c, ent_on_message_failed_fn fn, void *user_data)
{
    if (!c)
        return;
    if (fn)
    {
        c->cpp.set_on_message_failed(
            [fn, user_data](const endpoint_key &sender, uint32_t msg_id, uint8_t ch_id, uint8_t *buf,
                            message_fail_reason reason, uint8_t recv_count, uint8_t frag_count)
            {
                fn(to_c(sender), msg_id, ch_id, buf,
                   reason == message_fail_reason::expired ? ENT_MSG_FAIL_EXPIRED : ENT_MSG_FAIL_EVICTED, recv_count,
                   frag_count, user_data);
            });
    }
    else
    {
        c->cpp.set_on_message_failed(nullptr);
    }
}

void ent_client_set_on_message_acked(ent_client_t *c, ent_on_message_acked_fn fn, void *user_data)
{
    if (!c)
        return;
    if (fn)
    {
        c->cpp.set_on_message_acked([fn, user_data](uint32_t msg_id) { fn(msg_id, user_data); });
    }
    else
    {
        c->cpp.set_on_message_acked(nullptr);
    }
}

/* -----------------------------------------------------------------------
 * Client: configuration
 * ----------------------------------------------------------------------- */

void ent_client_enable_auto_retransmit(ent_client_t *c)
{
    if (c)
        c->cpp.enable_auto_retransmit();
}

int ent_client_auto_retransmit_enabled(const ent_client_t *c)
{
    return c ? c->cpp.auto_retransmit_enabled() : 0;
}

int ent_client_can_send(const ent_client_t *c)
{
    return c ? c->cpp.can_send() : 0;
}

ent_congestion_info ent_client_congestion(const ent_client_t *c)
{
    ent_congestion_info ci{};
    if (c)
    {
        auto info = c->cpp.congestion();
        ci.cwnd = info.cwnd;
        ci.in_flight = info.in_flight;
        ci.ssthresh = info.ssthresh;
        ci.pacing_interval_us = info.pacing_interval_us;
        ci.in_slow_start = info.in_slow_start ? 1 : 0;
    }
    return ci;
}

int ent_client_is_fragment_throttled(const ent_client_t *c)
{
    return c ? c->cpp.is_fragment_throttled() : 0;
}

void ent_client_set_reassembly_timeout(ent_client_t *c, int64_t timeout_us)
{
    if (c)
        c->cpp.set_reassembly_timeout(timeout_us);
}

void ent_client_flush_coalesce(ent_client_t *c)
{
    if (c)
        c->cpp.flush_coalesce();
}

void ent_client_set_verbose(ent_client_t *c, int verbose)
{
    if (c)
        c->cpp.set_verbose(verbose != 0);
}

uint16_t ent_client_local_port(const ent_client_t *c)
{
    return c ? c->cpp.local_port() : 0;
}

/* -----------------------------------------------------------------------
 * Server: lifecycle
 * ----------------------------------------------------------------------- */

ent_server_t *ent_server_create(uint16_t port, const char *bind_address)
{
    return new (std::nothrow) ent_server_t(port, bind_address ? bind_address : "0.0.0.0");
}

void ent_server_destroy(ent_server_t *s)
{
    delete s;
}

int ent_server_start(ent_server_t *s)
{
    if (!s)
        return ENT_ERROR_INVALID_ARGUMENT;
    return static_cast<int>(s->cpp.start());
}

void ent_server_stop(ent_server_t *s)
{
    if (s)
        s->cpp.stop();
}

int ent_server_is_running(const ent_server_t *s)
{
    return s ? s->cpp.is_running() : 0;
}

/* -----------------------------------------------------------------------
 * Server: poll / update
 * ----------------------------------------------------------------------- */

int ent_server_poll(ent_server_t *s, int max_packets)
{
    if (!s)
        return 0;
    return s->cpp.poll(max_packets > 0 ? max_packets : DEFAULT_MAX_POLL_PACKETS);
}

int ent_server_update(ent_server_t *s)
{
    if (!s)
        return 0;
    return s->cpp.update();
}

/* -----------------------------------------------------------------------
 * Server: send
 * ----------------------------------------------------------------------- */

int ent_server_send_to(ent_server_t *s, const void *data, size_t size, uint8_t channel_id, ent_endpoint dest,
                       uint8_t flags, uint32_t *out_message_id)
{
    if (!s)
        return ENT_ERROR_INVALID_ARGUMENT;
    return s->cpp.send_to(data, size, channel_id, to_cpp(dest), flags, out_message_id);
}

int ent_server_send_raw_to(ent_server_t *s, ent_packet_header *header, const void *payload, ent_endpoint dest)
{
    if (!s || !header)
        return ENT_ERROR_INVALID_ARGUMENT;
    packet_header h = to_cpp_header(*header);
    int result = s->cpp.send_raw_to(h, payload, to_cpp(dest));
    *header = to_c_header(h);
    return result;
}

int ent_server_send_fragment_to(ent_server_t *s, uint32_t message_id, uint8_t fragment_index, uint8_t fragment_count,
                                const void *data, size_t size, uint8_t flags, uint8_t channel_id, ent_endpoint dest,
                                uint32_t channel_sequence)
{
    if (!s)
        return ENT_ERROR_INVALID_ARGUMENT;
    return s->cpp.send_fragment_to(message_id, fragment_index, fragment_count, data, size, flags, channel_id,
                                   to_cpp(dest), channel_sequence);
}

/* -----------------------------------------------------------------------
 * Server: client management
 * ----------------------------------------------------------------------- */

void ent_server_disconnect_client(ent_server_t *s, ent_endpoint key)
{
    if (s)
        s->cpp.disconnect_client(to_cpp(key));
}

void ent_server_disconnect_all(ent_server_t *s)
{
    if (s)
        s->cpp.disconnect_all();
}

size_t ent_server_connection_count(const ent_server_t *s)
{
    return s ? s->cpp.connection_count() : 0;
}

uint64_t ent_server_recv_queue_drops(const ent_server_t *s)
{
    return s ? s->cpp.recv_queue_drops() : 0;
}

/* -----------------------------------------------------------------------
 * Server: channels
 * ----------------------------------------------------------------------- */

int ent_server_register_channel(ent_server_t *s, const ent_channel_config *config)
{
    if (!s || !config)
        return ENT_ERROR_INVALID_ARGUMENT;
    return static_cast<int>(s->cpp.channels().register_channel(to_cpp_config(*config)));
}

void ent_server_register_default_channels(ent_server_t *s)
{
    if (s)
        s->cpp.channels().register_defaults();
}

/* -----------------------------------------------------------------------
 * Server: configuration
 * ----------------------------------------------------------------------- */

void ent_server_set_worker_count(ent_server_t *s, int count)
{
    if (s)
        s->cpp.set_worker_count(count);
}

void ent_server_set_use_async_io(ent_server_t *s, int enabled)
{
    if (s)
        s->cpp.set_use_async_io(enabled != 0);
}

void ent_server_set_socket_count(ent_server_t *s, int count)
{
    if (s)
        s->cpp.set_socket_count(count);
}

void ent_server_enable_auto_retransmit(ent_server_t *s)
{
    if (s)
        s->cpp.enable_auto_retransmit();
}

int ent_server_auto_retransmit_enabled(const ent_server_t *s)
{
    return s ? s->cpp.auto_retransmit_enabled() : 0;
}

void ent_server_advance_send_pool(ent_server_t *s)
{
    if (s)
        s->cpp.advance_send_pool();
}

void ent_server_flush_coalesce(ent_server_t *s, ent_endpoint dest)
{
    if (s)
    {
        endpoint_key key{};
        key.address = dest.address;
        key.port = dest.port;
        s->cpp.flush_coalesce(key);
    }
}

void ent_server_set_verbose(ent_server_t *s, int verbose)
{
    if (s)
        s->cpp.set_verbose(verbose != 0);
}

uint16_t ent_server_port(const ent_server_t *s)
{
    return s ? s->cpp.port() : 0;
}

/* -----------------------------------------------------------------------
 * Server: callbacks
 * ----------------------------------------------------------------------- */

void ent_server_set_on_client_data(ent_server_t *s, ent_on_client_data_fn fn, void *user_data)
{
    if (!s)
        return;
    if (fn)
    {
        s->cpp.set_on_client_data_received(
            [fn, user_data](const packet_header &hdr, const uint8_t *payload, size_t size, const endpoint_key &sender)
            {
                ent_packet_header ch = to_c_header(hdr);
                fn(&ch, payload, size, to_c(sender), user_data);
            });
    }
    else
    {
        s->cpp.set_on_client_data_received(nullptr);
    }
}

void ent_server_set_on_coalesced_data(ent_server_t *s, ent_on_coalesced_data_fn fn, void *user_data)
{
    if (!s)
        return;
    if (fn)
    {
        s->cpp.set_on_coalesced_data(
            [fn, user_data](const packet_header &hdr, const uint8_t *raw_payload, size_t size, int msg_count,
                            const endpoint_key &sender)
            {
                ent_packet_header ch = to_c_header(hdr);
                fn(&ch, raw_payload, size, msg_count, to_c(sender), user_data);
            });
    }
    else
    {
        s->cpp.set_on_coalesced_data(nullptr);
    }
}

void ent_server_set_on_client_connected(ent_server_t *s, ent_on_client_connected_fn fn, void *user_data)
{
    if (!s)
        return;
    if (fn)
    {
        s->cpp.set_on_client_connected([fn, user_data](const endpoint_key &key, const std::string &addr, uint16_t port)
                                       { fn(to_c(key), addr.c_str(), port, user_data); });
    }
    else
    {
        s->cpp.set_on_client_connected(nullptr);
    }
}

void ent_server_set_on_client_disconnected(ent_server_t *s, ent_on_client_disconnected_fn fn, void *user_data)
{
    if (!s)
        return;
    if (fn)
    {
        s->cpp.set_on_client_disconnected(
            [fn, user_data](const endpoint_key &key, const std::string &addr, uint16_t port)
            { fn(to_c(key), addr.c_str(), port, user_data); });
    }
    else
    {
        s->cpp.set_on_client_disconnected(nullptr);
    }
}

void ent_server_set_on_channel_requested(ent_server_t *s, ent_on_channel_requested_fn fn, void *user_data)
{
    if (!s)
        return;
    if (fn)
    {
        s->cpp.set_on_channel_requested(
            [fn, user_data](const endpoint_key &key, uint8_t ch_id, channel_mode mode, uint8_t priority) -> bool
            {
                ent_channel_mode cm;
                switch (mode)
                {
                    case channel_mode::RELIABLE:
                        cm = ENT_CHANNEL_RELIABLE;
                        break;
                    case channel_mode::RELIABLE_ORDERED:
                        cm = ENT_CHANNEL_RELIABLE_ORDERED;
                        break;
                    default:
                        cm = ENT_CHANNEL_UNRELIABLE;
                        break;
                }
                return fn(to_c(key), ch_id, cm, priority, user_data) != 0;
            });
    }
    else
    {
        s->cpp.set_on_channel_requested(nullptr);
    }
}

void ent_server_set_on_packet_lost(ent_server_t *s, ent_on_server_packet_lost_fn fn, void *user_data)
{
    if (!s)
        return;
    if (fn)
    {
        s->cpp.set_on_packet_lost(
            [fn, user_data](const lost_packet_info &info, const endpoint_key &client)
            {
                ent_lost_packet_info ci = to_c_loss(info);
                fn(&ci, to_c(client), user_data);
            });
    }
    else
    {
        s->cpp.set_on_packet_lost(nullptr);
    }
}

void ent_server_set_on_allocate_message(ent_server_t *s, ent_on_allocate_message_fn fn, void *user_data)
{
    if (!s)
        return;
    if (fn)
    {
        s->cpp.set_on_allocate_message([fn, user_data](const endpoint_key &sender, uint32_t msg_id, uint8_t ch_id,
                                                       uint8_t frag_count, size_t max_size) -> uint8_t *
                                       { return fn(to_c(sender), msg_id, ch_id, frag_count, max_size, user_data); });
    }
    else
    {
        s->cpp.set_on_allocate_message(nullptr);
    }
}

void ent_server_set_on_message_complete(ent_server_t *s, ent_on_message_complete_fn fn, void *user_data)
{
    if (!s)
        return;
    if (fn)
    {
        s->cpp.set_on_message_complete([fn, user_data](const endpoint_key &sender, uint32_t msg_id, uint8_t ch_id,
                                                       uint8_t *data, size_t total_size)
                                       { fn(to_c(sender), msg_id, ch_id, data, total_size, user_data); });
    }
    else
    {
        s->cpp.set_on_message_complete(nullptr);
    }
}

void ent_server_set_on_message_failed(ent_server_t *s, ent_on_message_failed_fn fn, void *user_data)
{
    if (!s)
        return;
    if (fn)
    {
        s->cpp.set_on_message_failed(
            [fn, user_data](const endpoint_key &sender, uint32_t msg_id, uint8_t ch_id, uint8_t *buf,
                            message_fail_reason reason, uint8_t recv_count, uint8_t frag_count)
            {
                fn(to_c(sender), msg_id, ch_id, buf,
                   reason == message_fail_reason::expired ? ENT_MSG_FAIL_EXPIRED : ENT_MSG_FAIL_EVICTED, recv_count,
                   frag_count, user_data);
            });
    }
    else
    {
        s->cpp.set_on_message_failed(nullptr);
    }
}

void ent_server_set_reassembly_timeout(ent_server_t *s, int64_t timeout_us)
{
    if (s)
        s->cpp.set_reassembly_timeout(timeout_us);
}

int ent_server_is_fragment_throttled(const ent_server_t *s, ent_endpoint dest)
{
    return s ? s->cpp.is_fragment_throttled(to_cpp(dest)) : 0;
}
