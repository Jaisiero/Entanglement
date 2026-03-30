#include "server.h"
#include <algorithm>
#include <cstring>
#include <iostream>

namespace entanglement
{

    server::server(uint16_t port, const std::string &bind_address) : m_port(port), m_bind_address(bind_address) {}

    server::~server()
    {
        stop();
    }

    // -----------------------------------------------------------------------
    // Worker creation & callback propagation
    // -----------------------------------------------------------------------

    void server::create_workers()
    {
        int actual = (m_worker_count > 0) ? m_worker_count : 1;
        size_t pool_per_worker = MAX_CONNECTIONS / static_cast<size_t>(actual);
        if (pool_per_worker == 0)
            pool_per_worker = 1;

        m_workers.clear();
        m_workers.reserve(static_cast<size_t>(actual));
        for (int i = 0; i < actual; ++i)
        {
            auto w = std::make_unique<server_worker>();
            w->init(pool_per_worker, &m_socket, &m_channels, &m_running);
            w->set_verbose(m_verbose);
            w->set_auto_retransmit(m_auto_retransmit);
            m_workers.push_back(std::move(w));
        }
        propagate_callbacks();
    }

    void server::propagate_callbacks()
    {
        for (auto &w : m_workers)
        {
            if (m_on_client_data_received)
                w->set_on_client_data_received(m_on_client_data_received);
            if (m_on_client_connected)
                w->set_on_client_connected(m_on_client_connected);
            if (m_on_client_disconnected)
                w->set_on_client_disconnected(m_on_client_disconnected);
            if (m_on_channel_requested)
                w->set_on_channel_requested(m_on_channel_requested);
            if (m_on_packet_lost)
                w->set_on_packet_lost(m_on_packet_lost);
            if (m_frag_alloc_cb)
                w->set_on_allocate_message(m_frag_alloc_cb);
            if (m_app_on_message_complete)
                w->set_on_message_complete(m_app_on_message_complete);
            if (m_frag_failed_cb)
                w->set_on_message_failed(m_frag_failed_cb);
            if (m_reassembly_timeout_us != REASSEMBLY_TIMEOUT_US)
                w->set_reassembly_timeout(m_reassembly_timeout_us);
        }
    }

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    error_code server::start()
    {
        if (auto ec = m_socket.bind(m_port, m_bind_address); failed(ec))
            return ec;

        if (auto ec = m_socket.set_non_blocking(true); failed(ec))
        {
            if (m_verbose)
                std::cerr << "[server] Failed to set non-blocking mode" << std::endl;
            m_socket.close();
            return ec;
        }

        m_running = true;
        create_workers();

        m_threaded = (m_worker_count > 0);
        if (m_threaded)
        {
            // Launch receiver thread
            m_receiver_thread = std::thread([this]() { receiver_loop(); });

            // Launch worker threads
            m_worker_threads.reserve(m_workers.size());
            for (size_t i = 0; i < m_workers.size(); ++i)
            {
                m_worker_threads.emplace_back([this, i]() { worker_loop(i); });
            }
        }

        if (m_verbose)
        {
            std::cout << "[server] Listening on " << m_bind_address << ":" << m_port;
            if (m_threaded)
                std::cout << " (" << m_workers.size() << " worker threads)";
            std::cout << std::endl;
        }
        return error_code::ok;
    }

    void server::stop()
    {
        if (!m_running)
            return;
        m_running = false;

        // Join threads (multi-threaded mode)
        if (m_receiver_thread.joinable())
            m_receiver_thread.join();
        for (auto &t : m_worker_threads)
        {
            if (t.joinable())
                t.join();
        }
        m_worker_threads.clear();

        // Disconnect all clients (still has open socket)
        disconnect_all();

        m_socket.close();
        m_workers.clear();

        if (m_verbose)
            std::cout << "[server] Stopped" << std::endl;
    }

    // -----------------------------------------------------------------------
    // Thread loops (multi-threaded mode)
    // -----------------------------------------------------------------------

    void server::receiver_loop()
    {
        packet_header header{};
        uint8_t payload[MAX_PAYLOAD_SIZE];
        endpoint_key sender{};

        while (m_running.load(std::memory_order_relaxed))
        {
            // Drain up to a batch of packets before checking m_running / yielding.
            // Reduces per-packet overhead of atomic loads and yield syscalls.
            int batch = 0;
            while (batch < DEFAULT_MAX_POLL_PACKETS)
            {
                int result = m_socket.recv_packet(header, payload, MAX_PAYLOAD_SIZE, sender);
                if (result <= 0)
                    break;

                size_t w = worker_index(sender);
                queued_datagram dgram;
                dgram.header = header;
                dgram.sender = sender;
                std::memcpy(dgram.payload, payload, header.payload_size);

                if (!m_workers[w]->enqueue(std::move(dgram)))
                {
                    m_recv_queue_drops.fetch_add(1, std::memory_order_relaxed);
                }
                ++batch;
            }

            if (batch == 0)
                std::this_thread::yield();
        }
    }

    void server::worker_loop(size_t worker_idx)
    {
        auto &worker = *m_workers[worker_idx];
        worker.set_thread_id(std::this_thread::get_id());

        while (m_running.load(std::memory_order_relaxed))
        {
            int processed = worker.poll_local();
            worker.update();

            if (processed == 0)
                std::this_thread::yield();
        }
    }

    // -----------------------------------------------------------------------
    // Packet processing (single-threaded mode — called from game loop)
    // -----------------------------------------------------------------------

    int server::poll(int max_packets)
    {
        if (!m_running)
            return 0;

        // Multi-threaded: receiver + workers handle everything
        if (m_threaded)
            return 0;

        // Single-threaded: read socket and process in worker[0] directly
        int count = 0;
        packet_header header{};
        uint8_t payload[MAX_PAYLOAD_SIZE];
        endpoint_key sender{};

        while (count < max_packets)
        {
            int result = m_socket.recv_packet(header, payload, MAX_PAYLOAD_SIZE, sender);
            if (result <= 0)
                break;
            m_workers[0]->process_datagram(header, payload, sender);
            ++count;
        }
        return count;
    }

    int server::update(on_server_packet_lost loss_callback)
    {
        if (!m_running)
            return 0;
        if (m_threaded)
            return 0;
        return m_workers[0]->update(loss_callback);
    }

    // -----------------------------------------------------------------------
    // Callbacks — store locally and propagate to all workers
    // -----------------------------------------------------------------------

    void server::set_on_client_data_received(on_client_data_received callback)
    {
        m_on_client_data_received = callback;
        for (auto &w : m_workers)
            w->set_on_client_data_received(callback);
    }

    void server::set_on_client_connected(on_client_connected callback)
    {
        m_on_client_connected = std::move(callback);
        for (auto &w : m_workers)
            w->set_on_client_connected(m_on_client_connected);
    }

    void server::set_on_client_disconnected(on_client_disconnected callback)
    {
        m_on_client_disconnected = std::move(callback);
        for (auto &w : m_workers)
            w->set_on_client_disconnected(m_on_client_disconnected);
    }

    void server::set_on_channel_requested(on_channel_requested callback)
    {
        m_on_channel_requested = std::move(callback);
        for (auto &w : m_workers)
            w->set_on_channel_requested(m_on_channel_requested);
    }

    void server::set_on_packet_lost(on_server_packet_lost callback)
    {
        m_on_packet_lost = std::move(callback);
        for (auto &w : m_workers)
            w->set_on_packet_lost(m_on_packet_lost);
    }

    // -----------------------------------------------------------------------
    // Sending — route to correct worker
    // -----------------------------------------------------------------------

    int server::send_raw_to(packet_header &header, const void *payload, const endpoint_key &key)
    {
        if (m_workers.empty())
            return static_cast<int>(error_code::not_connected);

        if (!m_threaded)
            return m_workers[0]->send_raw_to(header, payload, key);

        size_t w = worker_index(key);
        // Same-thread optimisation: direct call if we're on the owning worker's thread
        if (std::this_thread::get_id() == m_workers[w]->thread_id())
            return m_workers[w]->send_raw_to(header, payload, key);

        // Cross-thread: enqueue
        send_command cmd;
        cmd.type = send_command::kind::RAW;
        cmd.dest = key;
        cmd.raw_header = header;
        cmd.data.assign(static_cast<const uint8_t *>(payload),
                        static_cast<const uint8_t *>(payload) + header.payload_size);
        m_workers[w]->enqueue_send(std::move(cmd));
        return header.payload_size;
    }

    int server::send_raw_to(packet_header &header, const void *payload, const std::string &address, uint16_t port)
    {
        return send_raw_to(header, payload, endpoint_from_string(address, port));
    }

    int server::send_to(const void *data, size_t size, uint8_t channel_id, const endpoint_key &key, uint8_t flags,
                        uint32_t *out_message_id)
    {
        if (m_workers.empty())
            return static_cast<int>(error_code::not_connected);

        if (!m_threaded)
            return m_workers[0]->send_to(data, size, channel_id, key, flags, out_message_id);

        size_t w = worker_index(key);
        if (std::this_thread::get_id() == m_workers[w]->thread_id())
            return m_workers[w]->send_to(data, size, channel_id, key, flags, out_message_id);

        // Cross-thread: enqueue (out_message_id cannot be set asynchronously)
        send_command cmd;
        cmd.type = send_command::kind::DATA;
        cmd.dest = key;
        cmd.channel_id = channel_id;
        cmd.flags = flags;
        cmd.data.assign(static_cast<const uint8_t *>(data), static_cast<const uint8_t *>(data) + size);
        m_workers[w]->enqueue_send(std::move(cmd));
        return static_cast<int>(size);
    }

    int server::send_to(const void *data, size_t size, uint8_t channel_id, const std::string &address, uint16_t port,
                        uint8_t flags, uint32_t *out_message_id)
    {
        return send_to(data, size, channel_id, endpoint_from_string(address, port), flags, out_message_id);
    }

    int server::send_fragment_to(uint32_t message_id, uint8_t fragment_index, uint8_t fragment_count, const void *data,
                                 size_t size, uint8_t flags, uint8_t channel_id, const endpoint_key &key,
                                 uint32_t channel_sequence)
    {
        if (m_workers.empty())
            return static_cast<int>(error_code::not_connected);

        if (!m_threaded)
            return m_workers[0]->send_fragment_to(message_id, fragment_index, fragment_count, data, size, flags,
                                                  channel_id, key, channel_sequence);

        size_t w = worker_index(key);
        if (std::this_thread::get_id() == m_workers[w]->thread_id())
            return m_workers[w]->send_fragment_to(message_id, fragment_index, fragment_count, data, size, flags,
                                                  channel_id, key, channel_sequence);

        send_command cmd;
        cmd.type = send_command::kind::FRAGMENT;
        cmd.dest = key;
        cmd.channel_id = channel_id;
        cmd.flags = flags;
        cmd.message_id = message_id;
        cmd.fragment_index = fragment_index;
        cmd.fragment_count = fragment_count;
        cmd.channel_sequence = channel_sequence;
        cmd.data.assign(static_cast<const uint8_t *>(data), static_cast<const uint8_t *>(data) + size);
        m_workers[w]->enqueue_send(std::move(cmd));
        return static_cast<int>(size);
    }

    int server::send_fragment_to(uint32_t message_id, uint8_t fragment_index, uint8_t fragment_count, const void *data,
                                 size_t size, uint8_t flags, uint8_t channel_id, const std::string &address,
                                 uint16_t port, uint32_t channel_sequence)
    {
        return send_fragment_to(message_id, fragment_index, fragment_count, data, size, flags, channel_id,
                                endpoint_from_string(address, port), channel_sequence);
    }

    // -----------------------------------------------------------------------
    // Connection management — route to correct worker
    // -----------------------------------------------------------------------

    void server::disconnect_client(const endpoint_key &key)
    {
        if (m_workers.empty())
            return;

        if (!m_threaded)
        {
            m_workers[0]->disconnect_client(key);
            return;
        }

        size_t w = worker_index(key);
        if (std::this_thread::get_id() == m_workers[w]->thread_id())
        {
            m_workers[w]->disconnect_client(key);
            return;
        }

        send_command cmd;
        cmd.type = send_command::kind::DISCONNECT;
        cmd.dest = key;
        m_workers[w]->enqueue_send(std::move(cmd));
    }

    void server::disconnect_all()
    {
        for (auto &w : m_workers)
            w->disconnect_all();
    }

    // -----------------------------------------------------------------------
    // Fragmentation callbacks — store and propagate
    // -----------------------------------------------------------------------

    void server::set_on_allocate_message(on_allocate_message cb)
    {
        m_frag_alloc_cb = cb;
        for (auto &w : m_workers)
            w->set_on_allocate_message(cb);
    }

    void server::set_on_message_complete(on_message_complete cb)
    {
        m_app_on_message_complete = cb;
        for (auto &w : m_workers)
            w->set_on_message_complete(cb);
    }

    void server::set_on_message_failed(on_message_failed cb)
    {
        m_frag_failed_cb = cb;
        for (auto &w : m_workers)
            w->set_on_message_failed(cb);
    }

    void server::set_reassembly_timeout(int64_t timeout_us)
    {
        m_reassembly_timeout_us = timeout_us;
        for (auto &w : m_workers)
            w->set_reassembly_timeout(timeout_us);
    }

    bool server::is_fragment_throttled(const endpoint_key &key) const
    {
        if (m_workers.empty())
            return false;
        if (!m_threaded)
            return m_workers[0]->is_fragment_throttled(key);
        size_t w = worker_index(key);
        return m_workers[w]->is_fragment_throttled(key);
    }

    bool server::is_fragment_throttled(const std::string &address, uint16_t port) const
    {
        return is_fragment_throttled(endpoint_from_string(address, port));
    }

} // namespace entanglement
