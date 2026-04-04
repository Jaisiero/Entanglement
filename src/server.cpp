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

        // Effective number of receive sockets (1 on Windows, m_socket_count on Linux)
        int recv_queue_count = 1;
#ifdef ENTANGLEMENT_PLATFORM_LINUX
        if (m_socket_count > 1 && m_use_async_io && m_worker_count > 0)
            recv_queue_count = m_socket_count;
#endif

        m_workers.clear();
        m_workers.reserve(static_cast<size_t>(actual));
        for (int i = 0; i < actual; ++i)
        {
            auto w = std::make_unique<server_worker>();
            w->init(pool_per_worker, &m_socket, &m_channels, &m_running, &m_send_pool, recv_queue_count);
            w->set_verbose(m_verbose);
            w->set_auto_retransmit(m_auto_retransmit);
            m_workers.push_back(std::move(w));
        }

        // --- Per-worker send sockets ---
        // Eliminates kernel socket-lock contention when workers send concurrently.
        m_worker_send_sockets.clear();
        if (actual > 1)
        {
#ifdef ENTANGLEMENT_PLATFORM_LINUX
            // On Linux, extra sockets bound to the same port (even SO_REUSEADDR only)
            // steal inbound packets. Instead, when SO_REUSEPORT multi-socket mode is
            // active, reuse the existing recv sockets for sending:
            //   worker 0 → m_socket (already the default from init())
            //   worker i → m_extra_recv_sockets[i-1]
            // This gives 1:1 contention (recv+send on same socket) instead of N:1.
            if (!m_extra_recv_sockets.empty())
            {
                int shared_sockets = 1 + static_cast<int>(m_extra_recv_sockets.size());
                int assignable = std::min(actual, shared_sockets);
                for (int i = 1; i < assignable; ++i)
                    m_workers[static_cast<size_t>(i)]->set_send_socket(
                        &m_extra_recv_sockets[static_cast<size_t>(i - 1)]);
                if (m_verbose)
                    std::cout << "[server] Per-worker send: reusing " << assignable << " recv sockets (SO_REUSEPORT)"
                              << std::endl;
            }
#else
            // On Windows, SO_REUSEADDR allows multiple UDP sockets bound to the
            // same port without inbound packet distribution (no SO_REUSEPORT).
            // Create dedicated per-worker send sockets.
            m_worker_send_sockets.resize(static_cast<size_t>(actual));
            bool all_ok = true;
            for (int i = 0; i < actual; ++i)
            {
                auto &ss = m_worker_send_sockets[static_cast<size_t>(i)];
                if (auto ec = ss.bind(m_port, m_bind_address, /*reuse_port=*/false); failed(ec))
                {
                    if (m_verbose)
                        std::cerr << "[server] Per-worker send socket " << i
                                  << " bind failed, falling back to shared socket" << std::endl;
                    all_ok = false;
                    break;
                }
                ss.set_non_blocking(true);
            }

            if (all_ok)
            {
                for (int i = 0; i < actual; ++i)
                    m_workers[static_cast<size_t>(i)]->set_send_socket(&m_worker_send_sockets[static_cast<size_t>(i)]);
                if (m_verbose)
                    std::cout << "[server] Per-worker send sockets enabled (" << actual << " sockets)" << std::endl;
            }
            else
            {
                for (auto &ss : m_worker_send_sockets)
                    ss.close();
                m_worker_send_sockets.clear();
            }
#endif
        }

        propagate_callbacks();
    }

    void server::propagate_callbacks()
    {
        for (auto &w : m_workers)
        {
            if (m_on_client_data_received)
                w->set_on_client_data_received(m_on_client_data_received);
            if (m_on_coalesced_data)
                w->set_on_coalesced_data(m_on_coalesced_data);
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
        // Determine effective socket count (SO_REUSEPORT only on Linux + async + threaded)
        int effective_sockets = 1;
#ifdef ENTANGLEMENT_PLATFORM_LINUX
        if (m_socket_count > 1 && m_use_async_io && m_worker_count > 0)
            effective_sockets = m_socket_count;
#endif
        const bool multi_socket = (effective_sockets > 1);
        const bool reuse_port = multi_socket;

        // Bind primary socket
        if (auto ec = m_socket.bind(m_port, m_bind_address, reuse_port); failed(ec))
            return ec;

        if (auto ec = m_socket.set_non_blocking(true); failed(ec))
        {
            if (m_verbose)
                std::cerr << "[server] Failed to set non-blocking mode" << std::endl;
            m_socket.close();
            return ec;
        }

        // Bind extra sockets for SO_REUSEPORT (Linux)
        m_extra_recv_sockets.clear();
        if (multi_socket)
        {
            m_extra_recv_sockets.resize(static_cast<size_t>(effective_sockets - 1));
            for (int i = 0; i < effective_sockets - 1; ++i)
            {
                if (auto ec = m_extra_recv_sockets[static_cast<size_t>(i)].bind(m_port, m_bind_address, true);
                    failed(ec))
                {
                    if (m_verbose)
                        std::cerr << "[server] Failed to bind extra socket " << (i + 1) << std::endl;
                    // Clean up already-bound extra sockets
                    for (int j = 0; j < i; ++j)
                        m_extra_recv_sockets[static_cast<size_t>(j)].close();
                    m_extra_recv_sockets.clear();
                    m_socket.close();
                    return ec;
                }
                m_extra_recv_sockets[static_cast<size_t>(i)].set_non_blocking(true);
            }
        }

        m_running = true;
        create_workers();

        m_threaded = (m_worker_count > 0);
        if (m_threaded)
        {
#if defined(ENTANGLEMENT_PLATFORM_WINDOWS) || defined(ENTANGLEMENT_PLATFORM_LINUX)
            // Initialise platform-optimized async I/O if requested
            if (m_use_async_io)
            {
                // Init async I/O on primary socket
#ifdef ENTANGLEMENT_PLATFORM_WINDOWS
                if (auto ec = m_socket.init_iocp(); failed(ec))
#elif defined(ENTANGLEMENT_PLATFORM_LINUX)
                if (auto ec = m_socket.init_epoll(); failed(ec))
#endif
                {
                    if (m_verbose)
                        std::cerr << "[server] Async I/O init failed, falling back to polling" << std::endl;
                    m_use_async_io = false;
                    // Ensure socket is non-blocking for fallback path
                    m_socket.set_non_blocking(true);
                    // Close extra sockets on fallback
                    for (auto &s : m_extra_recv_sockets)
                        s.close();
                    m_extra_recv_sockets.clear();
                }

#ifdef ENTANGLEMENT_PLATFORM_LINUX
                // Init epoll on extra sockets (multi-socket mode)
                if (m_use_async_io && !m_extra_recv_sockets.empty())
                {
                    for (size_t i = 0; i < m_extra_recv_sockets.size(); ++i)
                    {
                        if (auto ec = m_extra_recv_sockets[i].init_epoll(); failed(ec))
                        {
                            if (m_verbose)
                                std::cerr << "[server] Extra socket " << (i + 1) << " epoll init failed" << std::endl;
                            // Shut down all extra sockets
                            for (auto &s : m_extra_recv_sockets)
                            {
                                s.shutdown_epoll();
                                s.close();
                            }
                            m_extra_recv_sockets.clear();
                            break;
                        }
                    }
                }
#endif
            }

            // Launch primary receiver thread (async or polling)
            if (m_use_async_io)
                m_receiver_thread = std::thread([this]() { receiver_loop_async(0); });
            else
                m_receiver_thread = std::thread([this]() { receiver_loop(); });

#ifdef ENTANGLEMENT_PLATFORM_LINUX
            // Launch extra receiver threads (one per extra socket)
            m_extra_receiver_threads.clear();
            for (size_t i = 0; i < m_extra_recv_sockets.size(); ++i)
            {
                int receiver_id = static_cast<int>(i + 1);
                m_extra_receiver_threads.emplace_back([this, i, receiver_id]()
                                                      { receiver_loop_async_extra(static_cast<int>(i), receiver_id); });
            }
#endif

#else
            // No platform async I/O — use polling
            m_receiver_thread = std::thread([this]() { receiver_loop(); });
#endif

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
            {
                std::cout << " (" << m_workers.size() << " worker threads";
                if (m_use_async_io)
                {
#ifdef ENTANGLEMENT_PLATFORM_WINDOWS
                    std::cout << ", IOCP";
#elif defined(ENTANGLEMENT_PLATFORM_LINUX)
                    std::cout << ", epoll";
                    if (!m_extra_recv_sockets.empty())
                        std::cout << ", " << (m_extra_recv_sockets.size() + 1) << " recv sockets (SO_REUSEPORT)";
#endif
                }
                std::cout << ")";
            }
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
        for (auto &t : m_extra_receiver_threads)
        {
            if (t.joinable())
                t.join();
        }
        m_extra_receiver_threads.clear();
        for (auto &t : m_worker_threads)
        {
            if (t.joinable())
                t.join();
        }
        m_worker_threads.clear();

        // Disconnect all clients (still has open socket)
        disconnect_all();

        m_socket.close();
        for (auto &s : m_extra_recv_sockets)
            s.close();
        m_extra_recv_sockets.clear();
        for (auto &s : m_worker_send_sockets)
            s.close();
        m_worker_send_sockets.clear();
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
                if (!m_workers[w]->enqueue_packet(header, payload, header.payload_size, sender))
                {
                    m_recv_queue_drops.fetch_add(1, std::memory_order_relaxed);
                }
                ++batch;
            }

            if (batch == 0)
                std::this_thread::yield();
        }
    }

#if defined(ENTANGLEMENT_PLATFORM_WINDOWS) || defined(ENTANGLEMENT_PLATFORM_LINUX)
    void server::receiver_loop_async(int receiver_id)
    {
        recv_completion completions[ASYNC_MAX_COMPLETIONS];

        while (m_running.load(std::memory_order_relaxed))
        {
            // Batch dequeue — waits up to 1ms for completions, returns immediately if any ready
#ifdef ENTANGLEMENT_PLATFORM_WINDOWS
            int count = m_socket.recv_batch_iocp(completions, ASYNC_MAX_COMPLETIONS, 1);
#elif defined(ENTANGLEMENT_PLATFORM_LINUX)
            int count = m_socket.recv_batch_epoll(completions, ASYNC_MAX_COMPLETIONS, 1);
#endif

            for (int i = 0; i < count; ++i)
            {
                auto &c = completions[i];
                size_t w = worker_index(c.sender);
                if (!m_workers[w]->enqueue_packet(c.header, c.payload_ptr, c.payload_size, c.sender, receiver_id))
                {
                    m_recv_queue_drops.fetch_add(1, std::memory_order_relaxed);
                }
            }

#ifdef ENTANGLEMENT_PLATFORM_WINDOWS
            // Re-post IOCP buffers after all payloads have been copied to pool slots
            if (count > 0)
                m_socket.repost_iocp_batch();
#endif

            if (count == 0)
                std::this_thread::yield();
        }
    }
#endif

#ifdef ENTANGLEMENT_PLATFORM_LINUX
    void server::receiver_loop_async_extra(int extra_index, int receiver_id)
    {
        recv_completion completions[ASYNC_MAX_COMPLETIONS];
        auto &sock = m_extra_recv_sockets[static_cast<size_t>(extra_index)];

        while (m_running.load(std::memory_order_relaxed))
        {
            int count = sock.recv_batch_epoll(completions, ASYNC_MAX_COMPLETIONS, 1);

            for (int i = 0; i < count; ++i)
            {
                auto &c = completions[i];
                size_t w = worker_index(c.sender);
                if (!m_workers[w]->enqueue_packet(c.header, c.payload_ptr, c.payload_size, c.sender, receiver_id))
                {
                    m_recv_queue_drops.fetch_add(1, std::memory_order_relaxed);
                }
            }

            if (count == 0)
                std::this_thread::yield();
        }
    }
#endif

    void server::worker_loop(size_t worker_idx)
    {
        auto &worker = *m_workers[worker_idx];
        worker.set_thread_id(std::this_thread::get_id());

        while (m_running.load(std::memory_order_relaxed))
        {
            // Check pause request — spin until resumed
            if (m_paused.load(std::memory_order_acquire))
            {
                m_workers_paused.fetch_add(1, std::memory_order_release);
                while (m_paused.load(std::memory_order_acquire))
                    std::this_thread::yield();
                m_workers_paused.fetch_sub(1, std::memory_order_release);
                continue;
            }

            int processed = worker.poll_local();
            worker.update();

            if (processed == 0)
                std::this_thread::yield();
        }
    }

    // -----------------------------------------------------------------------
    // Worker direct-send API (bypass MPSC queue)
    // -----------------------------------------------------------------------

    void server::pause_workers()
    {
        if (!m_threaded || m_workers.empty())
            return;
        m_paused.store(true, std::memory_order_release);
        const int total = static_cast<int>(m_workers.size());
        while (m_workers_paused.load(std::memory_order_acquire) < total)
            std::this_thread::yield();
    }

    void server::resume_workers()
    {
        if (!m_threaded)
            return;
        m_paused.store(false, std::memory_order_release);
        while (m_workers_paused.load(std::memory_order_acquire) > 0)
            std::this_thread::yield();
    }

    int server::worker_send_to(size_t worker_idx, const void *data, size_t size,
                               uint8_t channel_id, const endpoint_key &key, uint8_t flags)
    {
        if (worker_idx >= m_workers.size())
            return static_cast<int>(error_code::not_connected);
        return m_workers[worker_idx]->send_to(data, size, channel_id, key, flags, nullptr);
    }

    int server::worker_send_to_multi(size_t worker_idx, const void *const *payloads, const uint16_t *sizes,
                                     uint32_t count, uint8_t channel_id, const endpoint_key &key, uint8_t flags)
    {
        if (worker_idx >= m_workers.size())
            return static_cast<int>(error_code::not_connected);
        return m_workers[worker_idx]->send_to_multi(payloads, sizes, count, channel_id, key, flags);
    }

    uint8_t *server::gso_buf(size_t worker_idx)
    {
        if (worker_idx >= m_workers.size())
            return nullptr;
        return m_workers[worker_idx]->gso_buf();
    }

    int server::gso_send(size_t worker_idx, uint32_t count, const uint16_t *payload_sizes,
                         uint16_t max_payload, uint8_t channel_id, const endpoint_key &key, uint8_t flags)
    {
        if (worker_idx >= m_workers.size())
            return static_cast<int>(error_code::not_connected);
        return m_workers[worker_idx]->gso_send(count, payload_sizes, max_payload, channel_id, key, flags);
    }

    void server::gso_batch_begin(size_t worker_idx)
    {
        if (worker_idx < m_workers.size())
            m_workers[worker_idx]->gso_batch_begin();
    }

    int server::gso_batch_flush(size_t worker_idx)
    {
        if (worker_idx >= m_workers.size())
            return 0;
        return m_workers[worker_idx]->gso_batch_flush();
    }

    void server::worker_begin_send_batch(size_t worker_idx)
    {
        if (worker_idx < m_workers.size())
            m_workers[worker_idx]->send_socket()->begin_send_batch();
    }

    void server::worker_flush_send_batch(size_t worker_idx)
    {
        if (worker_idx < m_workers.size())
            m_workers[worker_idx]->send_socket()->flush_send_batch();
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

    void server::set_on_coalesced_data(on_client_coalesced_data callback)
    {
        m_on_coalesced_data = callback;
        for (auto &w : m_workers)
            w->set_on_coalesced_data(callback);
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
        cmd.data_size = static_cast<uint16_t>(header.payload_size);
        cmd.pool_offset = m_send_pool.write(payload, cmd.data_size);
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
        cmd.data_size = static_cast<uint16_t>(size);
        cmd.pool_offset = m_send_pool.write(data, cmd.data_size);
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
        cmd.data_size = static_cast<uint16_t>(size);
        cmd.pool_offset = m_send_pool.write(data, cmd.data_size);
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

    void server::flush_coalesce(const endpoint_key &dest)
    {
        if (m_workers.empty())
            return;
        size_t w = worker_index(dest);
        m_workers[w]->flush_coalesce_for(dest);
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
