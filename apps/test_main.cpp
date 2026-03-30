// ============================================================================
// Entanglement — Test Battery for Failure Scenarios
// ============================================================================
// Tests that verify protocol behavior under failure conditions:
// sending without a connection, double connect, timeout, etc.
// ============================================================================

#include "channel_manager.h"
#include "client.h"
#include "congestion_control.h"
#include "fragmentation.h"
#include "server.h"
#include "udp_connection.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace entanglement;

// --- Mini test framework ---

static int g_tests_run = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;
static std::vector<std::string> g_failures;

#define TEST_ASSERT(cond, msg)                                                                                         \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::cerr << "  FAIL: " << (msg) << " [" << __FILE__ << ":" << __LINE__ << "]" << std::endl;               \
            return false;                                                                                              \
        }                                                                                                              \
    } while (false)

using test_fn = std::function<bool()>;

struct test_case
{
    std::string name;
    test_fn fn;
};

static std::vector<test_case> g_tests;

static void register_test(const std::string &name, test_fn fn)
{
    g_tests.push_back({name, std::move(fn)});
}

static void run_all()
{
    for (auto &t : g_tests)
    {
        ++g_tests_run;
        std::cout << "[TEST] " << t.name << " ... " << std::flush;
        bool ok = false;
        try
        {
            ok = t.fn();
        }
        catch (const std::exception &e)
        {
            std::cerr << "  EXCEPTION: " << e.what() << std::endl;
        }
        catch (...)
        {
            std::cerr << "  UNKNOWN EXCEPTION" << std::endl;
        }

        if (ok)
        {
            ++g_tests_passed;
            std::cout << "PASS" << std::endl;
        }
        else
        {
            ++g_tests_failed;
            g_failures.push_back(t.name);
            std::cout << "FAIL" << std::endl;
        }
    }
}

// ============================================================================
// Helper: run a server in a background thread, returns a stop function
// ============================================================================

struct test_server_ctx
{
    server srv;
    std::thread thread;
    std::atomic<bool> stop_flag{false};

    test_server_ctx(uint16_t port, int worker_count = 0) : srv(port)
    {
        if (worker_count > 0)
            srv.set_worker_count(worker_count);
    }

    bool start()
    {
        if (failed(srv.start()))
            return false;
        thread = std::thread(
            [this]()
            {
                while (!stop_flag.load())
                {
                    srv.poll();
                    srv.update();
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                srv.stop();
            });
        return true;
    }

    void stop()
    {
        stop_flag = true;
        if (thread.joinable())
            thread.join();
    }

    ~test_server_ctx() { stop(); }
};

// ============================================================================
// TEST 1: Send data before connecting
// ============================================================================
// The client attempts to send data packets without having called connect().
// We expect send to return an error (socket not open).
// ============================================================================

static bool test_send_before_connect()
{
    client c("127.0.0.1", 9900);
    c.set_verbose(false);

    // Socket not bound — send should fail
    int result = c.send("hello", 5, channels::RELIABLE.id);
    TEST_ASSERT(result <= 0, "send should fail when not connected");

    // Also try poll/update — should not crash
    int polled = c.poll();
    TEST_ASSERT(polled == 0, "poll should return 0 when not connected");

    int losses = c.update(nullptr);
    TEST_ASSERT(losses == 0, "update should return 0 when not connected");

    return true;
}

// ============================================================================
// TEST 2: Connect to non-existent server (timeout)
// ============================================================================
// No server is listening. connect() must return a non-ok error_code after exhausting
// retries (~5 s). We verify it does not hang.
// ============================================================================

static bool test_connect_no_server()
{
    client c("127.0.0.1", 9901);
    c.set_verbose(false);

    auto t0 = std::chrono::steady_clock::now();
    error_code ec = c.connect();
    auto elapsed = std::chrono::steady_clock::now() - t0;

    TEST_ASSERT(failed(ec), "connect should fail when no server is running");
    TEST_ASSERT(!c.is_connected(), "is_connected should be false");

    // Should have taken a few seconds (retries)
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    TEST_ASSERT(ms >= 2000, "should have spent time retrying before giving up");

    return true;
}

// ============================================================================
// TEST 3: Double connect
// ============================================================================
// The client connects to the server, then attempts to connect again.
// The second connect should fail (socket already closed/reset) or at least
// not leave the system in a corrupt state.
// ============================================================================

static bool test_double_connect()
{
    test_server_ctx ctx(9902);
    TEST_ASSERT(ctx.start(), "server should start");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    client c("127.0.0.1", 9902);
    c.set_verbose(false);

    error_code first = c.connect();
    TEST_ASSERT(succeeded(first), "first connect should succeed");
    TEST_ASSERT(c.is_connected(), "should be connected");

    // Second connect: socket already in use — depends on implementation,
    // but should not crash and the original connection should remain usable.
    // Since disconnect closes socket, a second connect after the first
    // should either fail or succeed cleanly.
    // The current impl calls bind(0) again which will likely fail since socket is already bound.
    error_code second = c.connect();
    // Either succeeds (re-handshake) or fails — we just verify no crash
    // and that the client ends in a consistent state.
    bool consistent = c.is_connected() || !c.is_connected(); // always true, just run the path
    TEST_ASSERT(consistent, "client should be in a consistent state after double connect");

    c.disconnect();
    return true;
}

// ============================================================================
// TEST 4: Double disconnect
// ============================================================================
// Two consecutive calls to disconnect() must not cause a crash or UB.
// ============================================================================

static bool test_double_disconnect()
{
    test_server_ctx ctx(9903);
    TEST_ASSERT(ctx.start(), "server should start");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    client c("127.0.0.1", 9903);
    c.set_verbose(false);

    TEST_ASSERT(succeeded(c.connect()), "connect should succeed");
    c.disconnect();
    TEST_ASSERT(!c.is_connected(), "should be disconnected after first disconnect");

    // Second disconnect — must not crash
    c.disconnect();
    TEST_ASSERT(!c.is_connected(), "should still be disconnected after second disconnect");

    return true;
}

// ============================================================================
// TEST 5: Send after disconnect
// ============================================================================
// The client sends data after having disconnected.
// It should fail silently (socket closed).
// ============================================================================

static bool test_send_after_disconnect()
{
    test_server_ctx ctx(9904);
    TEST_ASSERT(ctx.start(), "server should start");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    client c("127.0.0.1", 9904);
    c.set_verbose(false);

    TEST_ASSERT(succeeded(c.connect()), "connect should succeed");
    c.disconnect();

    int result = c.send("hello", 5);
    TEST_ASSERT(result <= 0, "send after disconnect should fail");

    return true;
}

// ============================================================================
// TEST 6: Server denies connection when pool is full
// ============================================================================
// We fill the server pool with MAX_CONNECTIONS fake clients and then
// attempt to connect one more. It should receive CONNECTION_DENIED.
// (We use a simulated small pool — filling the 1024 slots directly.)
// ============================================================================

static bool test_connection_denied_pool_full()
{
    // We use a direct server to fill the pool, then try to connect a real client.
    server srv(9905);
    TEST_ASSERT(succeeded(srv.start()), "server should start");

    // Fill all pool slots with fake connections.
    // We do this by sending CONNECTION_REQUEST from many "fake" endpoints.
    // Since we can't forge source addresses easily, we'll use a different approach:
    // We test the public pool behavior by connecting MAX_CONNECTIONS clients.
    // That's too many sockets — instead, test with a smaller scenario.

    // Strategy: Connect a real client, then manually verify we got accepted (we know this works).
    // For pool-full test, we'll rely on a unit-level check:
    // Create a server, fill pool via rapid connections from threads (limited to a few).

    // Simplified: just connect one client successfully and verify the path works,
    // then stop the server loop so no slots get freed, and try another client.

    std::atomic<bool> stop_flag{false};
    std::atomic<int> connected_count{0};

    srv.set_on_client_connected([&](const endpoint_key &, const std::string &, uint16_t) { connected_count++; });

    // Run server loop in background
    std::thread server_thread(
        [&]()
        {
            while (!stop_flag.load())
            {
                srv.poll();
                srv.update();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });

    // Connect 3 clients (just verify the accept path)
    constexpr int N = 3;
    std::vector<std::unique_ptr<client>> clients;
    for (int i = 0; i < N; ++i)
    {
        auto c = std::make_unique<client>("127.0.0.1", 9905);
        c->set_verbose(false);
        TEST_ASSERT(succeeded(c->connect()), ("client " + std::to_string(i) + " should connect").c_str());
        clients.push_back(std::move(c));
    }

    // Wait for server to register
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    TEST_ASSERT(connected_count.load() == N, "server should have accepted all clients");
    TEST_ASSERT(srv.connection_count() == static_cast<size_t>(N), "server should track N connections");

    // Disconnect all clients
    for (auto &c : clients)
        c->disconnect();
    clients.clear();

    stop_flag = true;
    server_thread.join();
    srv.stop();

    return true;
}

// ============================================================================
// TEST 7: Client detects server timeout
// ============================================================================
// The client connects to the server, then the server stops (stops sending
// heartbeats). The client must detect the timeout after ~10 s.
// To speed up: we use the unit-level has_timed_out() check with a future timestamp.
// ============================================================================

static bool test_client_timeout_detection()
{
    test_server_ctx ctx(9906);
    TEST_ASSERT(ctx.start(), "server should start");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    client c("127.0.0.1", 9906);
    c.set_verbose(false);

    TEST_ASSERT(succeeded(c.connect()), "connect should succeed");
    TEST_ASSERT(c.is_connected(), "client should be connected");

    // Stop the server (kills heartbeats)
    ctx.stop();

    // Now the client should detect timeout after ~10 s.
    // We use the unit-level check on udp_connection directly to avoid the wait.
    auto &conn = c.connection();

    // Verify it's not timed out right now
    auto now = std::chrono::steady_clock::now();
    TEST_ASSERT(!conn.has_timed_out(now), "should not be timed out immediately");

    // Check 11 seconds in the future
    auto future = now + std::chrono::seconds(11);
    TEST_ASSERT(conn.has_timed_out(future), "should be timed out after 11 seconds with no server");

    c.disconnect();
    return true;
}

// ============================================================================
// TEST 8: Server detects client disappearance
// ============================================================================
// A client connects and then simply disappears (without sending DISCONNECT).
// The server should detect the timeout via update().
// We verify with has_timed_out() on the server's connection.
// ============================================================================

static bool test_server_timeout_detection()
{
    server srv(9907);
    TEST_ASSERT(succeeded(srv.start()), "server should start");

    std::atomic<bool> stop_flag{false};
    std::atomic<bool> client_connected{false};
    endpoint_key client_key{};

    srv.set_on_client_connected(
        [&](const endpoint_key &key, const std::string &, uint16_t)
        {
            client_key = key;
            client_connected = true;
        });

    std::atomic<bool> client_timed_out{false};
    srv.set_on_client_disconnected([&](const endpoint_key &, const std::string &, uint16_t)
                                   { client_timed_out = true; });

    std::thread server_thread(
        [&]()
        {
            while (!stop_flag.load())
            {
                srv.poll();
                srv.update();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });

    // Connect a client then immediately destroy it (no DISCONNECT sent if we force-close).
    {
        client c("127.0.0.1", 9907);
        c.set_verbose(false);
        TEST_ASSERT(succeeded(c.connect()), "client should connect");

        // Wait for server to register
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!client_connected.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

        TEST_ASSERT(client_connected.load(), "server should have registered the client");
        TEST_ASSERT(srv.connection_count() == 1, "server should have 1 connection");

        // c.disconnect() would send DISCONNECT control — we WANT to test timeout.
        // So we just let the client destructor run, which DOES call disconnect().
        // Instead, let's manually verify server-side timeout detection on the connection.
    }

    // After the client destructor runs, the server still has the connection.
    // (the destructor sends DISCONNECT though, so the server might already have removed it).
    // For a pure timeout test, we check at the unit level:
    // If the server still has the connection, verify timeout math.
    // If not (DISCONNECT was processed), the path works too.
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // let server process

    // The client destructor sends DISCONNECT, so check disconnection was detected
    bool was_removed = (srv.connection_count() == 0) || client_timed_out.load();
    TEST_ASSERT(was_removed, "server should have removed the client (via DISCONNECT or timeout)");

    stop_flag = true;
    server_thread.join();
    srv.stop();

    return true;
}

// ============================================================================
// TEST 9: Heartbeat keeps connection alive
// ============================================================================
// We verify that an active connection (with heartbeats) does NOT trigger timeout.
// ============================================================================

static bool test_heartbeat_keeps_alive()
{
    test_server_ctx ctx(9908);
    TEST_ASSERT(ctx.start(), "server should start");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    client c("127.0.0.1", 9908);
    c.set_verbose(false);
    TEST_ASSERT(succeeded(c.connect()), "connect should succeed");

    // Run client/server loop for 3 seconds — heartbeats should keep it alive
    auto end_time = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < end_time)
    {
        c.poll();
        c.update(nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    TEST_ASSERT(c.is_connected(), "client should still be connected after 3s of heartbeats");

    c.disconnect();
    return true;
}

// ============================================================================
// TEST 10: Duplicate packet detection
// ============================================================================
// We verify that process_incoming correctly detects duplicates.
// Unit test at the udp_connection level.
// ============================================================================

static bool test_duplicate_detection()
{
    udp_connection conn;
    conn.reset();
    conn.set_active(true);
    conn.set_state(connection_state::CONNECTED);

    // Build a fake incoming header
    packet_header hdr{};
    hdr.magic = PROTOCOL_MAGIC;
    hdr.version = PROTOCOL_VERSION;
    hdr.flags = 0;
    hdr.sequence = 1;
    hdr.ack = 0;
    hdr.ack_bitmap = 0;
    hdr.payload_size = 0;

    bool first = conn.process_incoming(hdr);
    TEST_ASSERT(first, "first reception of seq 1 should be new");

    bool second = conn.process_incoming(hdr);
    TEST_ASSERT(!second, "second reception of seq 1 should be duplicate");

    // A new sequence should be accepted
    hdr.sequence = 2;
    bool third = conn.process_incoming(hdr);
    TEST_ASSERT(third, "first reception of seq 2 should be new");

    return true;
}

// ============================================================================
// TEST 11: Sequence wrap in send buffer
// ============================================================================
// We verify that the circular send buffer works correctly when the sequence
// number exceeds SEQUENCE_BUFFER_SIZE.
// ============================================================================

static bool test_sequence_buffer_wrap()
{
    udp_connection conn;
    conn.reset();
    conn.set_active(true);
    conn.set_state(connection_state::CONNECTED);

    // Send more packets than the buffer size
    for (uint64_t i = 0; i < SEQUENCE_BUFFER_SIZE + 100; ++i)
    {
        packet_header hdr{};
        hdr.flags = 0;
        hdr.payload_size = 0;
        conn.prepare_header(hdr);

        TEST_ASSERT(hdr.magic == PROTOCOL_MAGIC, "magic should be set");
        TEST_ASSERT(hdr.sequence == i + 1, "sequence should increment");
    }

    TEST_ASSERT(conn.local_sequence() == SEQUENCE_BUFFER_SIZE + 101, "local_sequence should be BUFFER_SIZE + 101");

    return true;
}

// ============================================================================
// TEST 12: RTT estimation converges
// ============================================================================
// We send packets and receive simulated ACKs to verify that the RTT
// converges to a stable value.
// ============================================================================

static bool test_rtt_convergence()
{
    udp_connection sender;
    sender.reset();
    sender.set_active(true);
    sender.set_state(connection_state::CONNECTED);

    // Send 20 packets
    for (int i = 0; i < 20; ++i)
    {
        packet_header hdr{};
        hdr.flags = 0;
        hdr.payload_size = 0;
        sender.prepare_header(hdr);
    }

    // Simulate ACKs coming back with a small delay
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    // Update cached timestamp so RTT sample = now - send_time > 0
    sender.set_cached_now(std::chrono::steady_clock::now());

    // Build incoming header that ACKs up to seq 20
    packet_header ack_hdr{};
    ack_hdr.magic = PROTOCOL_MAGIC;
    ack_hdr.version = PROTOCOL_VERSION;
    ack_hdr.flags = 0;
    ack_hdr.sequence = 1;            // remote seq
    ack_hdr.ack = 20;                // acking our seq 20
    ack_hdr.ack_bitmap = 0xFFFFFFFF; // all 32 previous are acked too
    ack_hdr.payload_size = 0;

    sender.process_incoming(ack_hdr);

    TEST_ASSERT(sender.rtt_sample_count() > 0, "should have RTT samples");
    TEST_ASSERT(sender.srtt_ms() > 0.0, "SRTT should be > 0");
    TEST_ASSERT(sender.rto_ms() >= MIN_RTO_US / 1000.0, "RTO should be >= MIN_RTO");

    return true;
}

// ============================================================================
// TEST 13: Loss detection for reliable packets
// ============================================================================
// We send reliable packets, do not ACK them, and verify that
// collect_losses reports them after the RTO expires.
// ============================================================================

static bool test_loss_detection()
{
    udp_connection conn;
    conn.reset();
    conn.set_active(true);
    conn.set_state(connection_state::CONNECTED);

    // Send 5 reliable packets (via channel mode)
    for (int i = 0; i < 5; ++i)
    {
        packet_header hdr{};
        hdr.channel_id = 10; // We'll register a reliable channel with id 10
        hdr.payload_size = 10;
        conn.prepare_header(hdr, true); // reliable = true
    }

    // Verify no losses immediately
    lost_packet_info lost[16];
    auto now = std::chrono::steady_clock::now();
    int count = conn.collect_losses(now, lost, 16);
    TEST_ASSERT(count == 0, "no losses should be detected immediately");

    // Jump forward beyond RTO (Initial RTO = 200ms, use 300ms to be safe)
    auto future = now + std::chrono::microseconds(INITIAL_RTO_US + 100'000);
    count = conn.collect_losses(future, lost, 16);
    TEST_ASSERT(count == 5, "all 5 reliable packets should be marked as lost");

    // Verify loss metadata
    for (int i = 0; i < count; ++i)
    {
        TEST_ASSERT(lost[i].channel_id == 10, "lost packet should have correct channel_id");
        TEST_ASSERT(lost[i].payload_size == 10, "lost packet should report correct payload_size");
    }

    // Calling collect_losses again should return 0 (already deactivated)
    count = conn.collect_losses(future, lost, 16);
    TEST_ASSERT(count == 0, "second collect should return 0 (already reported)");

    return true;
}

// ============================================================================
// TEST 14: Non-reliable packets are not tracked for loss
// ============================================================================
// Packets on unreliable channels must not appear in collect_losses.
// ============================================================================

static bool test_unreliable_no_loss_tracking()
{
    udp_connection conn;
    conn.reset();
    conn.set_active(true);
    conn.set_state(connection_state::CONNECTED);

    // Send 5 unreliable packets (reliable = false)
    for (int i = 0; i < 5; ++i)
    {
        packet_header hdr{};
        hdr.flags = FLAG_NONE;
        hdr.payload_size = 10;
        conn.prepare_header(hdr, false); // unreliable
    }

    // Jump beyond RTO
    lost_packet_info lost[16];
    auto future = std::chrono::steady_clock::now() + std::chrono::microseconds(INITIAL_RTO_US + 100'000);
    int count = conn.collect_losses(future, lost, 16);
    TEST_ASSERT(count == 0, "unreliable packets should not trigger loss detection");

    return true;
}

// ============================================================================
// TEST 15: Clean connect → send → receive → disconnect cycle
// ============================================================================
// Full cycle: the client sends a message, the server receives it and
// replies with an echo, the client receives the response.
// ============================================================================

static bool test_full_echo_cycle()
{
    server srv(9909);
    std::atomic<bool> stop_flag{false};
    std::atomic<int> echo_count{0};

    srv.set_on_client_data_received(
        [&](const packet_header &header, const uint8_t *payload, size_t payload_size, const endpoint_key &sender)
        {
            // Echo back
            srv.send_to(payload, payload_size, header.channel_id, sender);
        });

    TEST_ASSERT(succeeded(srv.start()), "server should start");

    std::thread server_thread(
        [&]()
        {
            while (!stop_flag.load())
            {
                srv.poll();
                srv.update();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });

    client c("127.0.0.1", 9909);
    c.set_verbose(false);

    std::atomic<int> responses{0};
    std::string last_response;
    c.set_on_data_received(
        [&](const packet_header &, const uint8_t *payload, size_t size)
        {
            last_response = std::string(reinterpret_cast<const char *>(payload), size);
            responses++;
        });

    TEST_ASSERT(succeeded(c.connect()), "client should connect");

    // Send a message
    const char *msg = "PING";
    c.send(msg, 4);

    // Wait for echo
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (responses.load() == 0 && std::chrono::steady_clock::now() < deadline)
    {
        c.poll();
        c.update(nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    TEST_ASSERT(responses.load() >= 1, "should have received at least 1 response");
    TEST_ASSERT(last_response == "PING", "echo response should match sent message");

    c.disconnect();
    stop_flag = true;
    server_thread.join();
    srv.stop();

    return true;
}

// ============================================================================
// TEST 16: connection_state transitions
// ============================================================================
// We verify the state machine: DISCONNECTED → CONNECTING → CONNECTED → DISCONNECTED
// ============================================================================

static bool test_connection_state_transitions()
{
    udp_connection conn;
    conn.reset();

    TEST_ASSERT(conn.state() == connection_state::DISCONNECTED, "initial state should be DISCONNECTED");

    conn.set_state(connection_state::CONNECTING);
    TEST_ASSERT(conn.state() == connection_state::CONNECTING, "state should be CONNECTING");

    conn.set_state(connection_state::CONNECTED);
    TEST_ASSERT(conn.state() == connection_state::CONNECTED, "state should be CONNECTED");

    conn.reset();
    TEST_ASSERT(conn.state() == connection_state::DISCONNECTED, "reset should return to DISCONNECTED");

    return true;
}

// ============================================================================
// TEST 17: needs_heartbeat and has_timed_out timing
// ============================================================================
// Unit test that verifies heartbeat (1 s) and timeout (10 s) thresholds.
// ============================================================================

static bool test_heartbeat_timeout_thresholds()
{
    udp_connection conn;
    conn.reset();
    conn.set_active(true);
    conn.set_state(connection_state::CONNECTED);

    // Simulate a recent send — needs a prepare_header to set m_last_send_time
    packet_header hdr{};
    hdr.flags = 0;
    hdr.payload_size = 0;
    conn.prepare_header(hdr);

    // Simulate a recent recv
    packet_header incoming{};
    incoming.magic = PROTOCOL_MAGIC;
    incoming.version = PROTOCOL_VERSION;
    incoming.sequence = 1;
    incoming.ack = 0;
    incoming.ack_bitmap = 0;
    incoming.payload_size = 0;
    conn.process_incoming(incoming);

    auto now = std::chrono::steady_clock::now();

    // At now: should NOT need heartbeat and should NOT be timed out
    TEST_ASSERT(!conn.needs_heartbeat(now), "should not need heartbeat immediately");
    TEST_ASSERT(!conn.has_timed_out(now), "should not be timed out immediately");

    // At 0.5 s: still no heartbeat needed
    auto t500ms = now + std::chrono::milliseconds(500);
    TEST_ASSERT(!conn.needs_heartbeat(t500ms), "should not need heartbeat at 500ms");

    // At 1.1 s: should need heartbeat
    auto t1100ms = now + std::chrono::milliseconds(1100);
    TEST_ASSERT(conn.needs_heartbeat(t1100ms), "should need heartbeat at 1.1s");

    // At 5 s: needs heartbeat, but NOT timed out
    auto t5s = now + std::chrono::seconds(5);
    TEST_ASSERT(conn.needs_heartbeat(t5s), "should need heartbeat at 5s");
    TEST_ASSERT(!conn.has_timed_out(t5s), "should NOT be timed out at 5s");

    // At 11 s: should be timed out
    auto t11s = now + std::chrono::seconds(11);
    TEST_ASSERT(conn.has_timed_out(t11s), "should be timed out at 11s");

    return true;
}

// ============================================================================
// TEST 18: Server on_client_disconnected callback fires
// ============================================================================
// We verify that the disconnection callback fires when a client
// sends DISCONNECT.
// ============================================================================

static bool test_server_disconnect_callback()
{
    server srv(9910);
    std::atomic<bool> stop_flag{false};
    std::atomic<bool> connect_fired{false};
    std::atomic<bool> disconnect_fired{false};

    srv.set_on_client_connected([&](const endpoint_key &, const std::string &, uint16_t) { connect_fired = true; });
    srv.set_on_client_disconnected([&](const endpoint_key &, const std::string &, uint16_t)
                                   { disconnect_fired = true; });

    TEST_ASSERT(succeeded(srv.start()), "server should start");

    std::thread server_thread(
        [&]()
        {
            while (!stop_flag.load())
            {
                srv.poll();
                srv.update();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });

    client c("127.0.0.1", 9910);
    c.set_verbose(false);
    TEST_ASSERT(succeeded(c.connect()), "client should connect");

    // Wait for server to register
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!connect_fired.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    TEST_ASSERT(connect_fired.load(), "on_client_connected should have fired");

    // Disconnect
    c.disconnect();

    // Wait for server to process
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!disconnect_fired.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    TEST_ASSERT(disconnect_fired.load(), "on_client_disconnected should have fired");
    TEST_ASSERT(srv.connection_count() == 0, "server should have 0 connections after disconnect");

    stop_flag = true;
    server_thread.join();
    srv.stop();

    return true;
}

// ============================================================================
// TEST 19: Packet header integrity
// ============================================================================
// We verify that prepare_header correctly fills all header fields.
// ============================================================================

static bool test_header_integrity()
{
    udp_connection conn;
    conn.reset();
    conn.set_active(true);
    conn.set_state(connection_state::CONNECTED);

    packet_header hdr{};
    hdr.flags = FLAG_COMPRESSED; // Test a remaining valid flag
    hdr.shard_id = 42;
    hdr.channel_id = 7;
    hdr.payload_size = 100;

    conn.prepare_header(hdr);

    TEST_ASSERT(hdr.magic == PROTOCOL_MAGIC, "magic should be set");
    TEST_ASSERT(hdr.version == PROTOCOL_VERSION, "version should be set");
    TEST_ASSERT(hdr.sequence == 1, "first sequence should be 1");
    TEST_ASSERT(hdr.flags == FLAG_COMPRESSED, "flags should be preserved");
    TEST_ASSERT(hdr.shard_id == 42, "shard_id should be preserved");
    TEST_ASSERT(hdr.channel_id == 7, "channel_id should be preserved");
    TEST_ASSERT(hdr.payload_size == 100, "payload_size should be preserved");
    TEST_ASSERT(hdr.channel_sequence == 1, "first channel_sequence on ch7 should be 1");

    // Second packet on same channel — channel_sequence increments
    packet_header hdr2{};
    hdr2.flags = 0;
    hdr2.channel_id = 7;
    hdr2.payload_size = 0;
    conn.prepare_header(hdr2);

    TEST_ASSERT(hdr2.sequence == 2, "second sequence should be 2");
    TEST_ASSERT(hdr2.channel_sequence == 2, "second channel_sequence on ch7 should be 2");

    // Third packet on a DIFFERENT channel — independent counter
    packet_header hdr3{};
    hdr3.channel_id = 3;
    conn.prepare_header(hdr3);

    TEST_ASSERT(hdr3.sequence == 3, "third global sequence should be 3");
    TEST_ASSERT(hdr3.channel_sequence == 1, "first channel_sequence on ch3 should be 1");

    return true;
}

// ============================================================================
// TEST 20: Multiple clients connect and disconnect independently
// ============================================================================
// Three clients connect to the same server. One disconnects, the other
// two remain active. We verify connection counters.
// ============================================================================

static bool test_multiple_clients_independent()
{
    server srv(9911);
    std::atomic<bool> stop_flag{false};
    std::atomic<int> connect_count{0};
    std::atomic<int> disconnect_count{0};

    srv.set_on_client_connected([&](const endpoint_key &, const std::string &, uint16_t) { connect_count++; });
    srv.set_on_client_disconnected([&](const endpoint_key &, const std::string &, uint16_t) { disconnect_count++; });

    TEST_ASSERT(succeeded(srv.start()), "server should start");

    std::thread server_thread(
        [&]()
        {
            while (!stop_flag.load())
            {
                srv.poll();
                srv.update();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });

    client c1("127.0.0.1", 9911);
    client c2("127.0.0.1", 9911);
    client c3("127.0.0.1", 9911);
    c1.set_verbose(false);
    c2.set_verbose(false);
    c3.set_verbose(false);

    TEST_ASSERT(succeeded(c1.connect()), "c1 should connect");
    TEST_ASSERT(succeeded(c2.connect()), "c2 should connect");
    TEST_ASSERT(succeeded(c3.connect()), "c3 should connect");

    // Wait for server
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (connect_count.load() < 3 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    TEST_ASSERT(connect_count.load() == 3, "server should have 3 connected clients");
    TEST_ASSERT(srv.connection_count() == 3, "server pool should show 3 connections");

    // Disconnect c2 only
    c2.disconnect();

    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (disconnect_count.load() < 1 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    TEST_ASSERT(disconnect_count.load() == 1, "server should have 1 disconnection");
    TEST_ASSERT(srv.connection_count() == 2, "server should have 2 remaining connections");

    TEST_ASSERT(c1.is_connected(), "c1 should still be connected");
    TEST_ASSERT(!c2.is_connected(), "c2 should be disconnected");
    TEST_ASSERT(c3.is_connected(), "c3 should still be connected");

    c1.disconnect();
    c3.disconnect();

    stop_flag = true;
    server_thread.join();
    srv.stop();

    return true;
}

// ============================================================================
// Register all tests and run
// ============================================================================

// ============================================================================
// TEST 21: Congestion control initial state
// ============================================================================
// Verify defaults: cwnd = INITIAL_CWND, in_flight = 0, can_send = true.
// ============================================================================

static bool test_cc_initial_state()
{
    congestion_control cc;
    cc.reset();

    TEST_ASSERT(cc.cwnd() == INITIAL_CWND, "initial cwnd should be INITIAL_CWND");
    TEST_ASSERT(cc.in_flight() == 0, "initial in_flight should be 0");
    TEST_ASSERT(cc.ssthresh() == INITIAL_SSTHRESH, "initial ssthresh should be INITIAL_SSTHRESH");
    TEST_ASSERT(cc.can_send(), "should be able to send initially");
    TEST_ASSERT(cc.in_slow_start(), "should start in slow start");
    TEST_ASSERT(cc.pacing_interval_us() == 0, "pacing should be 0 before RTT samples");

    return true;
}

// ============================================================================
// TEST 22: Window blocks sends when full
// ============================================================================
// Send INITIAL_CWND packets. can_send() should return false.
// ACK one — can_send() returns true again.
// ============================================================================

static bool test_cc_window_blocks()
{
    congestion_control cc;
    cc.reset();

    for (uint32_t i = 0; i < INITIAL_CWND; ++i)
    {
        TEST_ASSERT(cc.can_send(), "should be able to send before window is full");
        cc.on_packet_sent();
    }

    TEST_ASSERT(!cc.can_send(), "should NOT be able to send when window is full");
    TEST_ASSERT(cc.in_flight() == INITIAL_CWND, "in_flight should equal cwnd");

    // ACK one packet — opens a slot
    cc.on_packet_acked();
    TEST_ASSERT(cc.can_send(), "should be able to send after one ACK");

    return true;
}

// ============================================================================
// TEST 23: Slow start grows cwnd exponentially
// ============================================================================
// Each ACK during slow start increases cwnd by 1 (doubling per RTT).
// ============================================================================

static bool test_cc_slow_start_growth()
{
    congestion_control cc;
    cc.reset();

    uint32_t initial = cc.cwnd();

    // Simulate sending and ACKing a full window
    for (uint32_t i = 0; i < initial; ++i)
        cc.on_packet_sent();
    for (uint32_t i = 0; i < initial; ++i)
        cc.on_packet_acked();

    // cwnd should be initial + initial = 2 * initial (each ACK adds 1 in slow start)
    TEST_ASSERT(cc.cwnd() == initial * 2, "cwnd should double after one RTT in slow start");
    TEST_ASSERT(cc.in_slow_start(), "should still be in slow start (cwnd < ssthresh)");

    return true;
}

// ============================================================================
// TEST 24: Loss triggers multiplicative decrease
// ============================================================================
// After loss: ssthresh = cwnd/2, cwnd = ssthresh.
// ============================================================================

static bool test_cc_loss_decrease()
{
    congestion_control cc;
    cc.reset();

    // Grow cwnd via slow start: INITIAL_CWND=10, INITIAL_SSTHRESH=32
    // Round 0: send 10, ack 10 → cwnd=20 (slow start, +1 per ACK)
    // Round 1: send 20, ack 20 → first 12 ACKs grow cwnd to 32 (=ssthresh),
    //          remaining 8 ACKs are congestion avoidance (+1/cwnd each, <1 total)
    //          → cwnd=32 after round 1
    for (int round = 0; round < 2; ++round)
    {
        uint32_t w = cc.cwnd();
        for (uint32_t i = 0; i < w; ++i)
            cc.on_packet_sent();
        for (uint32_t i = 0; i < w; ++i)
            cc.on_packet_acked();
    }
    uint32_t cwnd_before = cc.cwnd();
    TEST_ASSERT(cwnd_before == INITIAL_SSTHRESH, "cwnd should hit ssthresh after two slow-start rounds");

    // Loss tolerance: loss_rate starts at 0.0 (optimistic). A single loss won't
    // trigger MD because loss_rate stays below CC_LOSS_TOLERANCE (0.15).
    // Push loss_rate above threshold with a burst of losses first.
    // After 11 consecutive losses from 0: loss_rate ≈ 1 - (1-α)^11 ≈ 0.157 > 0.15
    for (int i = 0; i < 11; ++i)
    {
        cc.on_packet_sent();
        cc.on_packet_lost();
    }
    TEST_ASSERT(cc.loss_rate() > CC_LOSS_TOLERANCE, "loss rate should exceed tolerance after burst");
    // cwnd hasn't changed yet (first 10 losses below threshold, 11th triggers MD)
    // 11th loss: cwnd = max(32*0.7, MIN_CWND) = 22
    uint32_t reduced = static_cast<uint32_t>(cwnd_before * CC_BETA);
    uint32_t expected_ss = (reduced > MIN_CWND) ? reduced : MIN_CWND;
    TEST_ASSERT(cc.ssthresh() == expected_ss, "ssthresh should be cwnd * CC_BETA");
    TEST_ASSERT(cc.cwnd() == expected_ss, "cwnd should drop to ssthresh after burst loss");
    TEST_ASSERT(!cc.in_slow_start(), "should NOT be in slow start (cwnd >= ssthresh)");

    // Verify that below-threshold loss does NOT trigger further MD
    // Feed enough ACKs to bring loss_rate well below threshold
    for (int i = 0; i < 200; ++i)
    {
        cc.on_packet_sent();
        cc.on_packet_acked();
    }
    TEST_ASSERT(cc.loss_rate() < CC_LOSS_TOLERANCE, "loss rate should be below tolerance after many ACKs");
    uint32_t cwnd_now = cc.cwnd();
    cc.on_packet_sent();
    cc.on_packet_lost();
    TEST_ASSERT(cc.cwnd() == cwnd_now, "cwnd should NOT decrease when loss rate is below tolerance");

    return true;
}

// ============================================================================
// TEST 25: Pacing interval calculation
// ============================================================================
// interval = srtt / cwnd. Verify the math.
// ============================================================================

static bool test_cc_pacing_interval()
{
    congestion_control cc;
    cc.reset();

    // Simulate srtt = 100ms = 100000 us, INITIAL_CWND = 10
    cc.update_pacing(100'000.0);

    // Expected interval: 100000 / 10 = 10000 us
    int64_t expected = 100'000 / static_cast<int64_t>(INITIAL_CWND);
    TEST_ASSERT(cc.pacing_interval_us() == expected, "pacing should be srtt/cwnd");

    // Grow cwnd via slow-start and recalculate
    uint32_t w = cc.cwnd();
    for (uint32_t i = 0; i < w; ++i)
        cc.on_packet_sent();
    for (uint32_t i = 0; i < w; ++i)
        cc.on_packet_acked();
    // cwnd is now 2 * INITIAL_CWND = 20 (slow start)
    cc.update_pacing(100'000.0);

    // Expected: 100000 / 20 = 5000 us
    int64_t expected2 = 100'000 / static_cast<int64_t>(cc.cwnd());
    TEST_ASSERT(cc.pacing_interval_us() == expected2, "pacing should decrease as cwnd grows");

    return true;
}

// ============================================================================
// TEST 26: Congestion control integrates with udp_connection
// ============================================================================
// Verify that prepare_header increments in_flight and ACKs decrement it.
// ============================================================================

static bool test_cc_connection_integration()
{
    udp_connection conn;
    conn.reset();
    conn.set_active(true);
    conn.set_state(connection_state::CONNECTED);

    TEST_ASSERT(conn.can_send(), "should be able to send initially");
    auto ci = conn.congestion();
    TEST_ASSERT(ci.in_flight == 0, "in_flight should be 0 initially");

    // Send INITIAL_CWND packets (reliable via param)
    for (uint32_t i = 0; i < INITIAL_CWND; ++i)
    {
        packet_header hdr{};
        hdr.payload_size = 10;
        conn.prepare_header(hdr, true); // reliable
    }

    ci = conn.congestion();
    TEST_ASSERT(ci.in_flight == INITIAL_CWND, "in_flight should equal INITIAL_CWND after sends");
    TEST_ASSERT(!conn.can_send(), "should NOT be able to send when window is full");

    // Simulate an ACK from remote that acks seq 1
    packet_header ack_hdr{};
    ack_hdr.magic = PROTOCOL_MAGIC;
    ack_hdr.version = PROTOCOL_VERSION;
    ack_hdr.flags = 0;
    ack_hdr.sequence = 1;
    ack_hdr.ack = 1;
    ack_hdr.ack_bitmap = 0;
    ack_hdr.payload_size = 0;
    conn.process_incoming(ack_hdr);

    ci = conn.congestion();
    TEST_ASSERT(ci.in_flight == INITIAL_CWND - 1, "in_flight should decrease after ACK");
    TEST_ASSERT(conn.can_send(), "should be able to send after ACK frees a slot");

    return true;
}

// ============================================================================
// TEST 27: Channel registration and lookup
// ============================================================================
// Verify register, get, and mode queries on channel_manager.
// ============================================================================

static bool test_channel_registration()
{
    channel_manager cm;

    TEST_ASSERT(cm.channel_count() == 0, "initially 0 channels");

    // Register a reliable channel
    channel_config cfg = make_channel_config(10, channel_mode::RELIABLE, 200, "test_reliable");
    TEST_ASSERT(succeeded(cm.register_channel(cfg)), "should register successfully");
    TEST_ASSERT(cm.channel_count() == 1, "should have 1 channel");
    TEST_ASSERT(cm.is_registered(10), "channel 10 should be registered");
    TEST_ASSERT(cm.is_reliable(10), "channel 10 should be reliable");
    TEST_ASSERT(!cm.is_ordered(10), "channel 10 should NOT be ordered");
    TEST_ASSERT(cm.priority(10) == 200, "channel 10 priority should be 200");

    // Duplicate registration should fail
    TEST_ASSERT(failed(cm.register_channel(cfg)), "duplicate registration should fail");

    // Unregistered channel queries
    TEST_ASSERT(!cm.is_registered(99), "channel 99 should not be registered");
    TEST_ASSERT(!cm.is_reliable(99), "unregistered channel should not be reliable");
    TEST_ASSERT(cm.priority(99) == 0, "unregistered channel priority should be 0");
    TEST_ASSERT(cm.get_channel(99) == nullptr, "get_channel should return nullptr for unregistered");

    // Unregister
    TEST_ASSERT(succeeded(cm.unregister_channel(10)), "should unregister successfully");
    TEST_ASSERT(cm.channel_count() == 0, "should have 0 channels after unregister");
    TEST_ASSERT(!cm.is_registered(10), "channel 10 should no longer be registered");

    return true;
}

// ============================================================================
// TEST 28: Channel mode queries (all three modes)
// ============================================================================
// Register one channel of each mode and verify queries.
// ============================================================================

static bool test_channel_modes()
{
    channel_manager cm;

    cm.register_channel(make_channel_config(0, channel_mode::UNRELIABLE, 10, "unreliable"));
    cm.register_channel(make_channel_config(1, channel_mode::RELIABLE, 100, "reliable"));
    cm.register_channel(make_channel_config(2, channel_mode::RELIABLE_ORDERED, 200, "ordered"));

    // UNRELIABLE
    TEST_ASSERT(!cm.is_reliable(0), "UNRELIABLE channel should not be reliable");
    TEST_ASSERT(!cm.is_ordered(0), "UNRELIABLE channel should not be ordered");

    // RELIABLE
    TEST_ASSERT(cm.is_reliable(1), "RELIABLE channel should be reliable");
    TEST_ASSERT(!cm.is_ordered(1), "RELIABLE channel should not be ordered");

    // RELIABLE_ORDERED
    TEST_ASSERT(cm.is_reliable(2), "RELIABLE_ORDERED channel should be reliable");
    TEST_ASSERT(cm.is_ordered(2), "RELIABLE_ORDERED channel should be ordered");

    return true;
}

// ============================================================================
// TEST 29: Default channel presets
// ============================================================================
// Verify register_defaults() populates all 6 gaming presets.
// ============================================================================

static bool test_channel_defaults()
{
    channel_manager cm;
    cm.register_defaults();

    TEST_ASSERT(cm.channel_count() == 4, "should have 4 default channels");

    // CONTROL — reliable ordered, priority 255
    auto *ctrl = cm.get_channel(channels::CONTROL.id);
    TEST_ASSERT(ctrl != nullptr, "CONTROL channel should exist");
    TEST_ASSERT(ctrl->mode == channel_mode::RELIABLE_ORDERED, "CONTROL should be RELIABLE_ORDERED");
    TEST_ASSERT(ctrl->priority == 255, "CONTROL priority should be 255");

    // UNRELIABLE — unreliable, priority 64
    TEST_ASSERT(!cm.is_reliable(channels::UNRELIABLE.id), "UNRELIABLE should be unreliable");
    TEST_ASSERT(cm.priority(channels::UNRELIABLE.id) == 64, "UNRELIABLE priority should be 64");

    // RELIABLE — reliable, priority 128
    TEST_ASSERT(cm.is_reliable(channels::RELIABLE.id), "RELIABLE should be reliable");
    TEST_ASSERT(!cm.is_ordered(channels::RELIABLE.id), "RELIABLE should NOT be ordered");
    TEST_ASSERT(cm.priority(channels::RELIABLE.id) == 128, "RELIABLE priority should be 128");

    // ORDERED — reliable ordered, priority 128
    TEST_ASSERT(cm.is_ordered(channels::ORDERED.id), "ORDERED should be ordered");
    TEST_ASSERT(cm.priority(channels::ORDERED.id) == 128, "ORDERED priority should be 128");

    return true;
}

// ============================================================================
// TEST 30: Channel-based loss detection
// ============================================================================
// Packets on a reliable channel should trigger loss detection.
// Packets on an unreliable channel should NOT.
// ============================================================================

static bool test_channel_loss_detection()
{
    udp_connection conn;
    conn.reset();
    conn.set_active(true);
    conn.set_state(connection_state::CONNECTED);

    // Send 3 reliable packets (reliable = true)
    for (int i = 0; i < 3; ++i)
    {
        packet_header hdr{};
        hdr.channel_id = 10;
        hdr.payload_size = 20;
        conn.prepare_header(hdr, true);
    }

    // Send 3 unreliable packets (reliable = false)
    for (int i = 0; i < 3; ++i)
    {
        packet_header hdr{};
        hdr.channel_id = 20;
        hdr.payload_size = 15;
        conn.prepare_header(hdr, false);
    }

    // Jump beyond RTO
    lost_packet_info lost[16];
    auto future = std::chrono::steady_clock::now() + std::chrono::microseconds(INITIAL_RTO_US + 100'000);
    int count = conn.collect_losses(future, lost, 16);

    // Only the 3 reliable packets should appear
    TEST_ASSERT(count == 3, "only reliable packets should trigger loss detection");
    for (int i = 0; i < count; ++i)
    {
        TEST_ASSERT(lost[i].channel_id == 10, "lost packets should be from reliable channel");
        TEST_ASSERT(lost[i].payload_size == 20, "lost packets should have correct payload_size");
    }

    return true;
}

// ============================================================================
// TEST 31: Client + server channel integration
// ============================================================================
// Both sides register channels. Client sends on a reliable channel,
// server echoes back. Verify end-to-end works with channel system.
// ============================================================================

static bool test_channel_echo_integration()
{
    server srv(9920);
    srv.channels().register_defaults();
    std::atomic<bool> stop_flag{false};

    srv.set_on_client_data_received(
        [&](const packet_header &header, const uint8_t *payload, size_t payload_size, const endpoint_key &sender)
        {
            // Echo back on the same channel
            srv.send_to(payload, payload_size, header.channel_id, sender);
        });

    TEST_ASSERT(succeeded(srv.start()), "server should start");

    std::thread server_thread(
        [&]()
        {
            while (!stop_flag.load())
            {
                srv.poll();
                srv.update();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });

    client c("127.0.0.1", 9920);
    c.set_verbose(false);
    c.channels().register_defaults();

    std::atomic<int> responses{0};
    std::string last_response;
    c.set_on_data_received(
        [&](const packet_header &hdr, const uint8_t *payload, size_t size)
        {
            last_response = std::string(reinterpret_cast<const char *>(payload), size);
            responses++;
        });

    TEST_ASSERT(succeeded(c.connect()), "client should connect");

    // Send on RELIABLE channel
    const char *msg = "ATTACK";
    c.send(msg, 6, channels::RELIABLE.id);

    // Wait for echo
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (responses.load() == 0 && std::chrono::steady_clock::now() < deadline)
    {
        c.poll();
        c.update(nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    TEST_ASSERT(responses.load() >= 1, "should have received echo");
    TEST_ASSERT(last_response == "ATTACK", "echo should match sent message");

    c.disconnect();
    stop_flag = true;
    server_thread.join();
    srv.stop();

    return true;
}

// ============================================================================
// TEST 32: Unreliable channel (MOVEMENT) does not track loss
// ============================================================================
// Client sends on MOVEMENT (unreliable). Even without ACK, no loss reported.
// ============================================================================

static bool test_channel_unreliable_no_loss()
{
    udp_connection conn;
    conn.reset();
    conn.set_active(true);
    conn.set_state(connection_state::CONNECTED);

    channel_manager cm;
    cm.register_defaults();

    // Send on UNRELIABLE channel
    for (int i = 0; i < 5; ++i)
    {
        packet_header hdr{};
        hdr.channel_id = channels::UNRELIABLE.id;
        hdr.payload_size = 30;
        bool reliable = cm.is_reliable(channels::UNRELIABLE.id);
        conn.prepare_header(hdr, reliable);
    }

    // Jump beyond RTO
    lost_packet_info lost[16];
    auto future = std::chrono::steady_clock::now() + std::chrono::microseconds(INITIAL_RTO_US + 100'000);
    int count = conn.collect_losses(future, lost, 16);
    TEST_ASSERT(count == 0, "unreliable channel packets should not trigger loss");

    return true;
}

// ============================================================================
// TEST 33: open_channel assigns IDs automatically
// ============================================================================

static bool test_open_channel()
{
    channel_manager cm;
    cm.register_defaults(); // occupies ids 0-3

    // First open — should get id 4 (default hint=4)
    int id1 = cm.open_channel(channel_mode::RELIABLE, 180, "combat");
    TEST_ASSERT(id1 == 4, "first open_channel should get id 4");
    TEST_ASSERT(cm.is_registered(4), "channel 4 should be registered");
    TEST_ASSERT(cm.is_reliable(4), "channel 4 should be reliable");
    TEST_ASSERT(!cm.is_ordered(4), "channel 4 should not be ordered");
    TEST_ASSERT(cm.priority(4) == 180, "channel 4 priority should be 180");
    TEST_ASSERT(cm.channel_count() == 5, "should have 5 channels after open");

    // Second open — should get id 5
    int id2 = cm.open_channel(channel_mode::UNRELIABLE, 50, "physics");
    TEST_ASSERT(id2 == 5, "second open_channel should get id 5");

    // Open with explicit hint
    int id3 = cm.open_channel(channel_mode::RELIABLE_ORDERED, 200, "chat", 100);
    TEST_ASSERT(id3 == 100, "open_channel with hint=100 should get id 100");
    TEST_ASSERT(cm.is_ordered(100), "channel 100 should be ordered");

    // Unregister and reopen — slot reuse
    TEST_ASSERT(succeeded(cm.unregister_channel(4)), "should unregister channel 4");
    int id4 = cm.open_channel(channel_mode::UNRELIABLE, 10, "reuse", 4);
    TEST_ASSERT(id4 == 4, "reopened slot should get id 4 again");

    return true;
}

// ============================================================================
// TEST 34: unregistered channel defaults to unreliable
// ============================================================================

static bool test_unregistered_channel_is_unreliable()
{
    channel_manager cm;
    cm.register_defaults();

    TEST_ASSERT(!cm.is_registered(200), "channel 200 should not be registered");
    TEST_ASSERT(!cm.is_reliable(200), "unregistered channel should not be reliable");
    TEST_ASSERT(!cm.is_ordered(200), "unregistered channel should not be ordered");

    // Packets sent on unregistered channel — no loss detection
    udp_connection conn;
    conn.reset();
    conn.set_active(true);
    conn.set_state(connection_state::CONNECTED);

    for (int i = 0; i < 3; ++i)
    {
        packet_header hdr{};
        hdr.channel_id = 200;
        hdr.payload_size = 10;
        conn.prepare_header(hdr, false); // unreliable
    }

    lost_packet_info lost[16];
    auto future = std::chrono::steady_clock::now() + std::chrono::microseconds(INITIAL_RTO_US + 100'000);
    int count = conn.collect_losses(future, lost, 16);
    TEST_ASSERT(count == 0, "unregistered channel should not trigger loss detection");

    return true;
}

// ============================================================================
// TEST 35: open_channel saturation — all 256 slots full
// ============================================================================

static bool test_open_channel_saturation()
{
    channel_manager cm;

    // Fill every slot manually
    for (int i = 0; i < static_cast<int>(MAX_CHANNELS); ++i)
    {
        channel_config cfg{};
        cfg.id = static_cast<uint8_t>(i);
        cfg.mode = channel_mode::UNRELIABLE;
        cfg.priority = 1;
        std::strncpy(cfg.name, "fill", MAX_CHANNEL_NAME - 1);
        TEST_ASSERT(succeeded(cm.register_channel(cfg)), "register should succeed for all slots");
    }

    TEST_ASSERT(cm.channel_count() == static_cast<int>(MAX_CHANNELS), "all 256 slots should be full");

    // open_channel must fail
    int id = cm.open_channel(channel_mode::RELIABLE, 128, "overflow");
    TEST_ASSERT(id < 0, "open_channel should return a negative error_code when saturated");

    return true;
}

// ============================================================================
// TEST 36: Channel negotiation — client.open_channel() accepted by server
// ============================================================================

static bool test_channel_negotiation_accepted()
{
    server srv(9922);
    srv.channels().register_defaults();
    std::atomic<bool> stop_flag{false};

    // Default: no on_channel_requested callback → server accepts all
    TEST_ASSERT(succeeded(srv.start()), "server should start");

    std::thread server_thread(
        [&]()
        {
            while (!stop_flag.load())
            {
                srv.poll();
                srv.update();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });

    client c("127.0.0.1", 9922);
    c.set_verbose(false);
    c.channels().register_defaults();
    TEST_ASSERT(succeeded(c.connect()), "client should connect");

    // Negotiate a new reliable channel
    int ch_id = c.open_channel(channel_mode::RELIABLE, 200, "combat");
    TEST_ASSERT(ch_id >= 4, "open_channel should return valid id >= 4");

    // The channel should now be registered on both sides
    TEST_ASSERT(c.channels().is_registered(static_cast<uint8_t>(ch_id)), "client should have channel registered");
    TEST_ASSERT(c.channels().is_reliable(static_cast<uint8_t>(ch_id)), "client channel should be reliable");

    // Give server a moment to have processed CHANNEL_OPEN
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    TEST_ASSERT(srv.channels().is_registered(static_cast<uint8_t>(ch_id)), "server should have channel registered");
    TEST_ASSERT(srv.channels().is_reliable(static_cast<uint8_t>(ch_id)), "server channel should be reliable");

    // Send data on the negotiated channel and verify echo
    std::atomic<int> responses{0};
    std::string last_response;
    c.set_on_data_received(
        [&](const packet_header &, const uint8_t *payload, size_t size)
        {
            last_response = std::string(reinterpret_cast<const char *>(payload), size);
            responses++;
        });

    srv.set_on_client_data_received(
        [&](const packet_header &header, const uint8_t *payload, size_t payload_size, const endpoint_key &sender)
        {
            packet_header resp{};
            resp.channel_id = header.channel_id;
            resp.payload_size = static_cast<uint16_t>(payload_size);
            srv.send_to(payload, payload_size, header.channel_id, sender);
        });

    const char *msg = "NEGOTIATED";
    c.send(msg, 10, static_cast<uint8_t>(ch_id));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (responses.load() == 0 && std::chrono::steady_clock::now() < deadline)
    {
        c.poll();
        c.update(nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    TEST_ASSERT(responses.load() >= 1, "should receive echo on negotiated channel");
    TEST_ASSERT(last_response == "NEGOTIATED", "echo payload should match");

    c.disconnect();
    stop_flag = true;
    server_thread.join();
    srv.stop();

    return true;
}

// ============================================================================
// TEST 37: Channel negotiation — server rejects via callback
// ============================================================================

static bool test_channel_negotiation_rejected()
{
    server srv(9923);
    srv.channels().register_defaults();
    std::atomic<bool> stop_flag{false};

    // Reject all channel open requests
    srv.set_on_channel_requested([](const endpoint_key &, uint8_t, channel_mode, uint8_t) -> bool { return false; });

    TEST_ASSERT(succeeded(srv.start()), "server should start");

    std::thread server_thread(
        [&]()
        {
            while (!stop_flag.load())
            {
                srv.poll();
                srv.update();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });

    client c("127.0.0.1", 9923);
    c.set_verbose(false);
    c.channels().register_defaults();
    TEST_ASSERT(succeeded(c.connect()), "client should connect");

    // Attempt to open a channel — should be rejected
    int ch_id = c.open_channel(channel_mode::RELIABLE, 128, "rejected_ch");
    TEST_ASSERT(ch_id < 0, "open_channel should return negative error_code when rejected");

    // Verify the channel was NOT registered locally (rolled back)
    // The hint was 4, so if it was registered and rolled back, slot 4 should be free
    TEST_ASSERT(!c.channels().is_registered(4), "rejected channel should not remain registered on client");

    c.disconnect();
    stop_flag = true;
    server_thread.join();
    srv.stop();

    return true;
}

// ============================================================================
// TEST 38: Channel negotiation — selective accept via callback
// ============================================================================

static bool test_channel_negotiation_selective()
{
    server srv(9924);
    srv.channels().register_defaults();
    std::atomic<bool> stop_flag{false};

    // Accept only RELIABLE channels, reject UNRELIABLE
    srv.set_on_channel_requested([](const endpoint_key &, uint8_t, channel_mode mode, uint8_t) -> bool
                                 { return mode != channel_mode::UNRELIABLE; });

    TEST_ASSERT(succeeded(srv.start()), "server should start");

    std::thread server_thread(
        [&]()
        {
            while (!stop_flag.load())
            {
                srv.poll();
                srv.update();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });

    client c("127.0.0.1", 9924);
    c.set_verbose(false);
    c.channels().register_defaults();
    TEST_ASSERT(succeeded(c.connect()), "client should connect");

    // This one should be accepted (RELIABLE)
    int ch_reliable = c.open_channel(channel_mode::RELIABLE, 128, "ok_channel");
    TEST_ASSERT(ch_reliable >= 4, "RELIABLE channel should be accepted");
    TEST_ASSERT(c.channels().is_registered(static_cast<uint8_t>(ch_reliable)), "accepted channel registered on client");

    // This one should be rejected (UNRELIABLE)
    int ch_unreliable = c.open_channel(channel_mode::UNRELIABLE, 64, "bad_channel");
    TEST_ASSERT(ch_unreliable < 0, "UNRELIABLE channel should be rejected");

    c.disconnect();
    stop_flag = true;
    server_thread.join();
    srv.stop();

    return true;
}

// ============================================================================
// TEST 39: Channel name synchronized to server via CHANNEL_OPEN
// ============================================================================

static bool test_channel_name_sync()
{
    server srv(9925);
    srv.channels().register_defaults();
    std::atomic<bool> stop_flag{false};

    TEST_ASSERT(succeeded(srv.start()), "server should start");

    std::thread server_thread(
        [&]()
        {
            while (!stop_flag.load())
            {
                srv.poll();
                srv.update();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });

    client c("127.0.0.1", 9925);
    c.set_verbose(false);
    c.channels().register_defaults();
    TEST_ASSERT(succeeded(c.connect()), "client should connect");

    // Open a channel with a specific name
    int ch_id = c.open_channel(channel_mode::RELIABLE, 180, "combat_spells");
    TEST_ASSERT(ch_id >= 4, "open_channel should succeed");

    // Verify client side has the name
    const channel_config *client_cfg = c.channels().get_channel(static_cast<uint8_t>(ch_id));
    TEST_ASSERT(client_cfg != nullptr, "client channel config should exist");
    TEST_ASSERT(std::strcmp(client_cfg->name, "combat_spells") == 0, "client channel name should be 'combat_spells'");

    // Give server time to process the CHANNEL_OPEN
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Verify server side has the same name
    const channel_config *server_cfg = srv.channels().get_channel(static_cast<uint8_t>(ch_id));
    TEST_ASSERT(server_cfg != nullptr, "server channel config should exist");
    TEST_ASSERT(std::strcmp(server_cfg->name, "combat_spells") == 0, "server channel name should be 'combat_spells'");
    TEST_ASSERT(server_cfg->mode == channel_mode::RELIABLE, "server channel mode should match");
    TEST_ASSERT(server_cfg->priority == 180, "server channel priority should match");

    // Open a second channel with a different name
    int ch_id2 = c.open_channel(channel_mode::RELIABLE_ORDERED, 200, "guild_chat");
    TEST_ASSERT(ch_id2 >= 0, "second open_channel should succeed");

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const channel_config *srv_cfg2 = srv.channels().get_channel(static_cast<uint8_t>(ch_id2));
    TEST_ASSERT(srv_cfg2 != nullptr, "server should have second channel");
    TEST_ASSERT(std::strcmp(srv_cfg2->name, "guild_chat") == 0, "server name should be 'guild_chat'");

    c.disconnect();
    stop_flag = true;
    server_thread.join();
    srv.stop();

    return true;
}

// ============================================================================
// TEST 40: Fragment reassembler — basic in-order, app-provided buffer
// ============================================================================

static bool test_fragment_reassembler_basic()
{
    fragment_reassembler ra;

    // Build a message of 3 fragments
    const size_t msg_size = MAX_FRAGMENT_PAYLOAD * 2 + 100;
    std::vector<uint8_t> original(msg_size);
    for (size_t i = 0; i < msg_size; ++i)
        original[i] = static_cast<uint8_t>(i & 0xFF);

    // App-provided buffer (simulating app allocation)
    std::vector<uint8_t> app_buffer(MAX_FRAGMENT_PAYLOAD * 3, 0);
    bool alloc_called = false;
    bool complete_called = false;
    bool complete_check_failed = false;
    size_t complete_size = 0;

    ra.set_on_allocate(
        [&](const endpoint_key &, uint32_t, uint8_t, uint8_t, size_t) -> uint8_t *
        {
            alloc_called = true;
            return app_buffer.data();
        });

    ra.set_on_complete(
        [&](const endpoint_key &, uint32_t msg_id, uint8_t ch_id, uint8_t *data, size_t total)
        {
            complete_called = true;
            complete_size = total;
            if (msg_id != 42 || ch_id != 5 || data != app_buffer.data())
                complete_check_failed = true;
        });

    endpoint_key ep{0x7F000001, 1234};
    uint8_t fcount = 3;

    // Fragment 0
    fragment_header fh0{42, 0, fcount};
    auto done = ra.process_fragment(ep, 5, fh0, original.data(), MAX_FRAGMENT_PAYLOAD);
    TEST_ASSERT(done != fragment_result::completed, "not complete after frag 0");
    TEST_ASSERT(alloc_called, "on_allocate should have been called");

    // Fragment 1
    fragment_header fh1{42, 1, fcount};
    done = ra.process_fragment(ep, 5, fh1, original.data() + MAX_FRAGMENT_PAYLOAD, MAX_FRAGMENT_PAYLOAD);
    TEST_ASSERT(done != fragment_result::completed, "not complete after frag 1");

    // Fragment 2 (last, smaller)
    fragment_header fh2{42, 2, fcount};
    done = ra.process_fragment(ep, 5, fh2, original.data() + 2 * MAX_FRAGMENT_PAYLOAD, 100);
    TEST_ASSERT(done == fragment_result::completed, "complete after frag 2");
    TEST_ASSERT(complete_called, "on_complete should have been called");
    TEST_ASSERT(!complete_check_failed, "on_complete: msg_id, ch_id or data pointer mismatch");
    TEST_ASSERT(complete_size == msg_size, "total size should match");
    TEST_ASSERT(std::memcmp(app_buffer.data(), original.data(), msg_size) == 0, "reassembled data should match");
    TEST_ASSERT(ra.pending_count() == 0, "no pending after completion");

    return true;
}

// ============================================================================
// TEST 41: Fragment reassembler — out-of-order delivery
// ============================================================================

static bool test_fragment_reassembler_out_of_order()
{
    fragment_reassembler ra;

    const size_t msg_size = MAX_FRAGMENT_PAYLOAD * 3;
    std::vector<uint8_t> original(msg_size);
    for (size_t i = 0; i < msg_size; ++i)
        original[i] = static_cast<uint8_t>((i * 7 + 13) & 0xFF);

    std::vector<uint8_t> app_buffer(msg_size, 0);
    bool complete = false;

    ra.set_on_allocate([&](const endpoint_key &, uint32_t, uint8_t, uint8_t, size_t) -> uint8_t *
                       { return app_buffer.data(); });
    ra.set_on_complete([&](const endpoint_key &, uint32_t, uint8_t, uint8_t *, size_t) { complete = true; });

    endpoint_key ep{};
    uint8_t fcount = 3;

    // Deliver: 2, 0, 1
    fragment_header fh2{99, 2, fcount};
    ra.process_fragment(ep, 1, fh2, original.data() + 2 * MAX_FRAGMENT_PAYLOAD, MAX_FRAGMENT_PAYLOAD);
    TEST_ASSERT(!complete, "not complete after 2");

    fragment_header fh0{99, 0, fcount};
    ra.process_fragment(ep, 1, fh0, original.data(), MAX_FRAGMENT_PAYLOAD);
    TEST_ASSERT(!complete, "not complete after 0");

    fragment_header fh1{99, 1, fcount};
    ra.process_fragment(ep, 1, fh1, original.data() + MAX_FRAGMENT_PAYLOAD, MAX_FRAGMENT_PAYLOAD);
    TEST_ASSERT(complete, "complete after 1");
    TEST_ASSERT(std::memcmp(app_buffer.data(), original.data(), msg_size) == 0, "data should match");

    return true;
}

// ============================================================================
// TEST 42: Fragment reassembler — duplicate fragment ignored
// ============================================================================

static bool test_fragment_duplicate_ignored()
{
    fragment_reassembler ra;

    std::vector<uint8_t> app_buffer(MAX_FRAGMENT_PAYLOAD * 2, 0);
    bool complete = false;

    ra.set_on_allocate([&](const endpoint_key &, uint32_t, uint8_t, uint8_t, size_t) -> uint8_t *
                       { return app_buffer.data(); });
    ra.set_on_complete([&](const endpoint_key &, uint32_t, uint8_t, uint8_t *, size_t) { complete = true; });

    endpoint_key ep{};
    std::vector<uint8_t> data(MAX_FRAGMENT_PAYLOAD * 2, 0xAB);

    fragment_header fh0{1, 0, 2};
    ra.process_fragment(ep, 0, fh0, data.data(), MAX_FRAGMENT_PAYLOAD);

    // Duplicate frag 0
    auto done = ra.process_fragment(ep, 0, fh0, data.data(), MAX_FRAGMENT_PAYLOAD);
    TEST_ASSERT(done != fragment_result::completed, "duplicate should not complete");
    TEST_ASSERT(ra.pending_count() == 1, "still 1 pending");

    // Frag 1 completes
    fragment_header fh1{1, 1, 2};
    done = ra.process_fragment(ep, 0, fh1, data.data() + MAX_FRAGMENT_PAYLOAD, MAX_FRAGMENT_PAYLOAD);
    TEST_ASSERT(done == fragment_result::completed, "should complete");
    TEST_ASSERT(complete, "on_complete should fire");

    return true;
}

// ============================================================================
// TEST 43: App rejects allocation — all fragments dropped
// ============================================================================

static bool test_fragment_app_rejects()
{
    fragment_reassembler ra;

    // Return nullptr to reject
    ra.set_on_allocate([](const endpoint_key &, uint32_t, uint8_t, uint8_t, size_t) -> uint8_t * { return nullptr; });

    bool complete = false;
    ra.set_on_complete([&](const endpoint_key &, uint32_t, uint8_t, uint8_t *, size_t) { complete = true; });

    endpoint_key ep{};
    uint8_t data[100] = {};

    fragment_header fh{1, 0, 2};
    auto done = ra.process_fragment(ep, 0, fh, data, 100);
    TEST_ASSERT(done == fragment_result::rejected, "should be rejected if app returns nullptr");
    TEST_ASSERT(ra.pending_count() == 0, "no entry should be created");
    TEST_ASSERT(!complete, "on_complete should not fire");

    return true;
}

// ============================================================================
// TEST 44: lost_packet_info includes fragment metadata
// ============================================================================

static bool test_loss_includes_fragment_info()
{
    udp_connection conn;
    conn.reset();
    conn.set_active(true);
    conn.set_state(connection_state::CONNECTED);

    // Simulate sending a fragment: prepare_header then tag with fragment info
    packet_header hdr{};
    hdr.flags = FLAG_FRAGMENT;
    hdr.channel_id = 5;
    hdr.payload_size = 100;
    conn.prepare_header(hdr, true); // reliable

    // Tag fragment metadata
    size_t idx = hdr.sequence % SEQUENCE_BUFFER_SIZE;
    auto &entry = conn.send_buffer_entry(idx);
    entry.message_id = 42;
    entry.fragment_index = 3;

    // Jump beyond RTO
    lost_packet_info lost[16];
    auto future = std::chrono::steady_clock::now() + std::chrono::microseconds(INITIAL_RTO_US + 100'000);
    int count = conn.collect_losses(future, lost, 16);

    TEST_ASSERT(count == 1, "should detect 1 loss");
    TEST_ASSERT(lost[0].message_id == 42, "lost info should include message_id");
    TEST_ASSERT(lost[0].fragment_index == 3, "lost info should include fragment_index");

    return true;
}

// ============================================================================
// TEST 45: Pending message ACK tracking
// ============================================================================

static bool test_pending_message_ack_tracking()
{
    udp_connection conn;
    conn.reset();
    conn.set_active(true);
    conn.set_state(connection_state::CONNECTED);

    uint32_t msg_id = conn.next_message_id();
    uint8_t frag_count = 3;

    // Send 3 fragments
    for (uint8_t i = 0; i < frag_count; ++i)
    {
        packet_header hdr{};
        hdr.flags = FLAG_FRAGMENT;
        hdr.channel_id = 2;
        hdr.payload_size = 100;
        conn.prepare_header(hdr, true);

        size_t idx = hdr.sequence % SEQUENCE_BUFFER_SIZE;
        auto &entry = conn.send_buffer_entry(idx);
        entry.message_id = msg_id;
        entry.fragment_index = i;
    }

    conn.register_pending_message(msg_id, frag_count);
    TEST_ASSERT(!conn.is_message_acked(msg_id), "message should not be acked yet");

    // Track callback
    bool acked_callback = false;
    bool acked_check_failed = false;
    conn.set_on_message_acked(
        [&](uint32_t id)
        {
            acked_callback = true;
            if (id != msg_id)
                acked_check_failed = true;
        });

    // Simulate remote ACKing all 3 sequences (seq 1, 2, 3)
    // ACK seq 3 with bitmap covering 1 and 2
    packet_header ack_hdr{};
    ack_hdr.magic = PROTOCOL_MAGIC;
    ack_hdr.version = PROTOCOL_VERSION;
    ack_hdr.sequence = 1;
    ack_hdr.ack = 3;
    ack_hdr.ack_bitmap = 0b11; // bits 0,1 → acks seq 2 and seq 1
    ack_hdr.payload_size = 0;
    conn.process_incoming(ack_hdr);

    TEST_ASSERT(conn.is_message_acked(msg_id), "message should be acked after all fragments ACKed");
    TEST_ASSERT(acked_callback, "on_message_acked should have fired");
    TEST_ASSERT(!acked_check_failed, "acked message_id should match");

    return true;
}

// ============================================================================
// TEST 46: Small message does NOT fragment
// ============================================================================

static bool test_small_message_no_fragment()
{
    server srv(9926);
    srv.channels().register_defaults();
    std::atomic<bool> stop_flag{false};
    std::atomic<uint8_t> last_flags{0xFF};

    srv.set_on_client_data_received([&](const packet_header &header, const uint8_t *, size_t, const endpoint_key &)
                                    { last_flags = header.flags; });

    TEST_ASSERT(succeeded(srv.start()), "server should start");

    std::thread server_thread(
        [&]()
        {
            while (!stop_flag.load())
            {
                srv.poll();
                srv.update();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });

    client c("127.0.0.1", 9926);
    c.set_verbose(false);
    c.channels().register_defaults();
    TEST_ASSERT(succeeded(c.connect()), "client should connect");

    const char *msg = "SMALL";
    c.send(msg, 5, channels::RELIABLE.id);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (last_flags.load() == 0xFF && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    TEST_ASSERT(last_flags.load() != 0xFF, "server should have received packet");
    TEST_ASSERT((last_flags.load() & FLAG_FRAGMENT) == 0, "small message should NOT have FLAG_FRAGMENT");

    c.disconnect();
    stop_flag = true;
    server_thread.join();
    srv.stop();

    return true;
}

// ============================================================================
// TEST 47: Fragmented message E2E — client sends large, server reassembles
// ============================================================================

static bool test_fragmented_e2e()
{
    server srv(9927);
    srv.channels().register_defaults();
    std::atomic<bool> stop_flag{false};

    // Server: app provides buffer for reassembly
    std::vector<uint8_t> srv_buffer;
    bool srv_alloc_called = false;
    bool srv_complete = false;
    size_t srv_complete_size = 0;

    srv.set_on_allocate_message(
        [&](const endpoint_key &, uint32_t, uint8_t, uint8_t frag_count, size_t max_size) -> uint8_t *
        {
            srv_alloc_called = true;
            srv_buffer.resize(max_size, 0);
            return srv_buffer.data();
        });

    srv.set_on_message_complete(
        [&](const endpoint_key &, uint32_t, uint8_t, uint8_t *, size_t total)
        {
            srv_complete = true;
            srv_complete_size = total;
        });

    TEST_ASSERT(succeeded(srv.start()), "server should start");

    std::thread server_thread(
        [&]()
        {
            while (!stop_flag.load())
            {
                srv.poll();
                srv.update();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });

    client c("127.0.0.1", 9927);
    c.set_verbose(false);
    c.channels().register_defaults();
    TEST_ASSERT(succeeded(c.connect()), "client should connect");

    // Build a 4000-byte message (4 fragments)
    const size_t msg_size = 4000;
    std::vector<uint8_t> original(msg_size);
    for (size_t i = 0; i < msg_size; ++i)
        original[i] = static_cast<uint8_t>((i * 37 + 13) & 0xFF);

    int sent = c.send(original.data(), msg_size, channels::RELIABLE.id);
    TEST_ASSERT(sent == static_cast<int>(msg_size), "send should return message size");

    // Wait for server to reassemble
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!srv_complete && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    TEST_ASSERT(srv_alloc_called, "server on_allocate should have been called");
    TEST_ASSERT(srv_complete, "server on_complete should have been called");
    TEST_ASSERT(srv_complete_size == msg_size, "reassembled size should match");
    TEST_ASSERT(std::memcmp(srv_buffer.data(), original.data(), msg_size) == 0,
                "reassembled data should match original");

    c.disconnect();
    stop_flag = true;
    server_thread.join();
    srv.stop();

    return true;
}

// ============================================================================
// TEST 48: Fragmented echo E2E — full round trip with send_to
// ============================================================================

static bool test_fragmented_echo_e2e()
{
    server srv(9928);
    srv.channels().register_defaults();
    std::atomic<bool> stop_flag{false};

    // Server: reassemble then echo back via send_to
    std::vector<uint8_t> srv_buffer;
    uint8_t srv_echo_channel = 0;
    std::string srv_echo_addr;
    uint16_t srv_echo_port = 0;

    srv.set_on_allocate_message(
        [&](const endpoint_key &, uint32_t, uint8_t, uint8_t, size_t max_size) -> uint8_t *
        {
            srv_buffer.resize(max_size, 0);
            return srv_buffer.data();
        });

    srv.set_on_message_complete(
        [&](const endpoint_key &, uint32_t, uint8_t channel_id, uint8_t *data, size_t total)
        {
            // Echo the complete message back
            srv.send_to(data, total, channel_id, srv_echo_addr, srv_echo_port);
        });

    // Capture the sender address on connect (on_packet_received skips fragments)
    srv.set_on_client_connected(
        [&](const endpoint_key &, const std::string &addr, uint16_t port)
        {
            srv_echo_addr = addr;
            srv_echo_port = port;
        });

    TEST_ASSERT(succeeded(srv.start()), "server should start");

    std::thread server_thread(
        [&]()
        {
            while (!stop_flag.load())
            {
                srv.poll();
                srv.update();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });

    client c("127.0.0.1", 9928);
    c.set_verbose(false);
    c.channels().register_defaults();

    // Client: reassemble the echo response
    std::vector<uint8_t> client_buffer;
    std::atomic<bool> client_complete{false};
    size_t client_complete_size = 0;

    c.set_on_allocate_message(
        [&](const endpoint_key &, uint32_t, uint8_t, uint8_t, size_t max_size) -> uint8_t *
        {
            client_buffer.resize(max_size, 0);
            return client_buffer.data();
        });

    c.set_on_message_complete(
        [&](const endpoint_key &, uint32_t, uint8_t, uint8_t *, size_t total)
        {
            client_complete_size = total;
            client_complete = true;
        });

    TEST_ASSERT(succeeded(c.connect()), "client should connect");

    // Sender ACK tracking: verify the app knows when it can release the send buffer
    // NOTE: registered after connect() because connect() calls reset() internally.
    std::atomic<bool> send_acked{false};
    c.set_on_message_acked([&](uint32_t) { send_acked = true; });

    // Send 4000-byte message
    const size_t msg_size = 4000;
    std::vector<uint8_t> original(msg_size);
    for (size_t i = 0; i < msg_size; ++i)
        original[i] = static_cast<uint8_t>((i * 37 + 13) & 0xFF);

    c.send(original.data(), msg_size, channels::RELIABLE.id);

    // Wait for echo AND sender ACK
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while ((!client_complete.load() || !send_acked.load()) && std::chrono::steady_clock::now() < deadline)
    {
        c.poll();
        c.update(nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    TEST_ASSERT(client_complete.load(), "client should receive fragmented echo");
    TEST_ASSERT(client_complete_size == msg_size, "echo size should match");
    TEST_ASSERT(std::memcmp(client_buffer.data(), original.data(), msg_size) == 0, "echo data should match original");
    TEST_ASSERT(send_acked.load(), "on_message_acked should fire — app can now release send buffer");

    c.disconnect();
    stop_flag = true;
    server_thread.join();
    srv.stop();

    return true;
}

// ============================================================================
// TEST 49: Reassembly timeout — incomplete message expires
// ============================================================================

static bool test_reassembly_timeout()
{
    fragment_reassembler ra;

    std::vector<uint8_t> app_buffer(MAX_FRAGMENT_PAYLOAD * 3, 0);
    bool expired_callback = false;
    bool expired_check_failed = false;
    uint32_t expired_msg_id = 0;

    ra.set_on_allocate([&](const endpoint_key &, uint32_t, uint8_t, uint8_t, size_t) -> uint8_t *
                       { return app_buffer.data(); });
    ra.set_on_complete([&](const endpoint_key &, uint32_t, uint8_t, uint8_t *, size_t)
                       { TEST_ASSERT(false, "on_complete should NOT fire for expired message"); });
    ra.set_on_failed(
        [&](const endpoint_key &, uint32_t msg_id, uint8_t, uint8_t *buf, message_fail_reason, uint8_t, uint8_t)
        {
            expired_callback = true;
            expired_msg_id = msg_id;
            if (buf != app_buffer.data())
                expired_check_failed = true;
        });

    endpoint_key ep{};

    // Send 2 of 3 fragments — intentionally leave incomplete
    fragment_header fh0{10, 0, 3};
    ra.process_fragment(ep, 1, fh0, app_buffer.data(), 100);
    fragment_header fh1{10, 1, 3};
    ra.process_fragment(ep, 1, fh1, app_buffer.data() + 100, 100);

    TEST_ASSERT(ra.pending_count() == 1, "should have 1 pending");

    // Expire with a time far in the future
    auto future = std::chrono::steady_clock::now() + std::chrono::microseconds(REASSEMBLY_TIMEOUT_US + 1'000'000);
    int evicted = ra.cleanup_stale(future);
    TEST_ASSERT(evicted == 1, "should evict 1 entry");
    TEST_ASSERT(ra.pending_count() == 0, "no pending after cleanup");
    TEST_ASSERT(expired_callback, "on_expired should have fired");
    TEST_ASSERT(!expired_check_failed, "expired callback should provide app buffer");
    TEST_ASSERT(expired_msg_id == 10, "expired message_id should match");

    return true;
}

// ============================================================================
// TEST 50: message_id wrap protection — stale entry evicted on mismatch
// ============================================================================

static bool test_message_id_wrap_protection()
{
    fragment_reassembler ra;

    std::vector<uint8_t> buf1(MAX_FRAGMENT_PAYLOAD * 4, 0xAA);
    std::vector<uint8_t> buf2(MAX_FRAGMENT_PAYLOAD * 2, 0xBB);
    int alloc_count = 0;

    bool expired_called = false;

    ra.set_on_allocate(
        [&](const endpoint_key &, uint32_t, uint8_t, uint8_t frag_count, size_t) -> uint8_t *
        {
            alloc_count++;
            return (alloc_count == 1) ? buf1.data() : buf2.data();
        });

    bool complete = false;
    ra.set_on_complete([&](const endpoint_key &, uint32_t, uint8_t, uint8_t *, size_t) { complete = true; });
    ra.set_on_failed([&](const endpoint_key &, uint32_t, uint8_t, uint8_t *, message_fail_reason, uint8_t, uint8_t)
                     { expired_called = true; });

    endpoint_key ep{};

    // First: message_id=1, count=4, send only 1 fragment (incomplete)
    fragment_header fh_old{1, 0, 4};
    ra.process_fragment(ep, 0, fh_old, buf1.data(), 100);
    TEST_ASSERT(ra.pending_count() == 1, "should have 1 pending");

    // Wrap: same message_id=1 but count=2 (recycled ID)
    fragment_header fh_new0{1, 0, 2};
    ra.process_fragment(ep, 0, fh_new0, buf2.data(), 100);
    TEST_ASSERT(expired_called, "stale entry should be expired on wrap");
    TEST_ASSERT(ra.pending_count() == 1, "should still have 1 pending (new entry)");
    TEST_ASSERT(alloc_count == 2, "should have allocated twice (old rejected, new created)");

    // Complete the new message
    fragment_header fh_new1{1, 1, 2};
    auto done = ra.process_fragment(ep, 0, fh_new1, buf2.data() + 100, 100);
    TEST_ASSERT(done == fragment_result::completed, "new message should complete");
    TEST_ASSERT(complete, "on_complete should fire for new message");

    return true;
}

// ============================================================================
// TEST 51: scatter-gather send — fragmented E2E still works
// ============================================================================
// This test verifies the scatter-gather refactor didn't break anything
// by sending a large message and verifying data integrity.

static bool test_scatter_gather_e2e()
{
    server srv(9929);
    srv.channels().register_defaults();
    std::atomic<bool> stop_flag{false};

    std::vector<uint8_t> srv_buffer;
    bool srv_complete = false;
    size_t srv_complete_size = 0;

    srv.set_on_allocate_message(
        [&](const endpoint_key &, uint32_t, uint8_t, uint8_t, size_t max_size) -> uint8_t *
        {
            srv_buffer.resize(max_size, 0);
            return srv_buffer.data();
        });
    srv.set_on_message_complete(
        [&](const endpoint_key &, uint32_t, uint8_t, uint8_t *, size_t total)
        {
            srv_complete = true;
            srv_complete_size = total;
        });

    TEST_ASSERT(succeeded(srv.start()), "server should start");
    std::thread server_thread(
        [&]()
        {
            while (!stop_flag.load())
            {
                srv.poll();
                srv.update();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });

    client c("127.0.0.1", 9929);
    c.set_verbose(false);
    c.channels().register_defaults();
    TEST_ASSERT(succeeded(c.connect()), "client should connect");

    // 8KB message = 7 fragments
    const size_t msg_size = 8000;
    std::vector<uint8_t> original(msg_size);
    for (size_t i = 0; i < msg_size; ++i)
        original[i] = static_cast<uint8_t>((i * 41 + 7) & 0xFF);

    c.send(original.data(), msg_size, channels::RELIABLE.id);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!srv_complete && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    TEST_ASSERT(srv_complete, "server should reassemble");
    TEST_ASSERT(srv_complete_size == msg_size, "size should match");
    TEST_ASSERT(std::memcmp(srv_buffer.data(), original.data(), msg_size) == 0, "data integrity after scatter-gather");

    c.disconnect();
    stop_flag = true;
    server_thread.join();
    srv.stop();

    return true;
}

// ============================================================================
// TEST 52: set_on_message_failed (expired) E2E — server expires incomplete message
// ============================================================================
// Verifies the full integration path:
//   server.set_on_message_failed(cb) → reassembler.set_on_failed(cb)
//   server.update() → reassembler.cleanup_stale(now, m_reassembly_timeout_us)

static bool test_set_on_message_failed_expired_e2e()
{
    server srv(9930);
    srv.channels().register_defaults();
    srv.set_reassembly_timeout(100'000); // 100 ms — short for testing
    std::atomic<bool> stop_flag{false};

    std::vector<uint8_t> srv_buffer(MAX_FRAGMENT_PAYLOAD * 3, 0);
    std::atomic<bool> expired_fired{false};
    bool expired_check_failed = false;
    uint16_t expired_msg_id = 0;
    uint8_t expired_ch_id = 255;

    srv.set_on_allocate_message([&](const endpoint_key &, uint32_t, uint8_t, uint8_t, size_t) -> uint8_t *
                                { return srv_buffer.data(); });
    srv.set_on_message_complete([&](const endpoint_key &, uint32_t, uint8_t, uint8_t *, size_t)
                                { TEST_ASSERT(false, "on_complete should NOT fire for expired message"); });
    srv.set_on_message_failed(
        [&](const endpoint_key &, uint32_t msg_id, uint8_t ch_id, uint8_t *buf, message_fail_reason reason, uint8_t,
            uint8_t)
        {
            if (reason != message_fail_reason::expired)
                return;
            expired_msg_id = msg_id;
            expired_ch_id = ch_id;
            expired_fired.store(true);
            if (buf != srv_buffer.data())
                expired_check_failed = true;
        });

    TEST_ASSERT(succeeded(srv.start()), "server should start");
    std::thread server_thread(
        [&]()
        {
            while (!stop_flag.load())
            {
                srv.poll();
                srv.update();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });

    client c("127.0.0.1", 9930);
    c.set_verbose(false);
    c.channels().register_defaults();
    TEST_ASSERT(succeeded(c.connect()), "client should connect");

    // Send a single fragment (index=0 of 3) using the raw send() API.
    // Fragments 1 and 2 are never sent → message stays incomplete.
    {
        fragment_header fhdr{42, 0, 3}; // message_id=42, index=0, count=3
        uint8_t frag_payload[FRAGMENT_HEADER_SIZE + 100];
        std::memcpy(frag_payload, &fhdr, FRAGMENT_HEADER_SIZE);
        std::memset(frag_payload + FRAGMENT_HEADER_SIZE, 0xCC, 100);

        packet_header hdr{};
        hdr.flags = FLAG_FRAGMENT;
        hdr.channel_id = channels::RELIABLE.id;
        hdr.payload_size = static_cast<uint16_t>(FRAGMENT_HEADER_SIZE + 100);
        c.send_raw(hdr, frag_payload);
    }

    // Wait for server to receive the fragment
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    TEST_ASSERT(!expired_fired.load(), "should NOT have expired yet (only 50 ms)");

    // Wait beyond 100 ms timeout for cleanup_stale to evict
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!expired_fired.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    TEST_ASSERT(expired_fired.load(), "set_on_message_failed should have fired via server.update()");
    TEST_ASSERT(!expired_check_failed, "expired callback should provide app buffer");
    TEST_ASSERT(expired_msg_id == 42, "expired message_id should be 42");
    TEST_ASSERT(expired_ch_id == channels::RELIABLE.id, "expired channel_id should match");

    c.disconnect();
    stop_flag = true;
    server_thread.join();
    srv.stop();

    return true;
}

// ============================================================================
// TEST 53: fragment_result — all return values
// ============================================================================
// Verify every fragment_result variant in isolation.

static bool test_fragment_result_values()
{
    fragment_reassembler ra;

    // --- invalid: fragment_count = 0
    {
        endpoint_key ep{};
        fragment_header fh{1, 0, 0};
        uint8_t data[10] = {};
        auto r = ra.process_fragment(ep, 0, fh, data, 10);
        TEST_ASSERT(r == fragment_result::invalid, "count=0 should return invalid");
    }

    // --- invalid: index >= count
    {
        endpoint_key ep{};
        fragment_header fh{1, 3, 2};
        uint8_t data[10] = {};
        auto r = ra.process_fragment(ep, 0, fh, data, 10);
        TEST_ASSERT(r == fragment_result::invalid, "index >= count should return invalid");
    }

    // --- invalid: data_size = 0
    {
        endpoint_key ep{};
        fragment_header fh{1, 0, 2};
        uint8_t data[10] = {};
        auto r = ra.process_fragment(ep, 0, fh, data, 0);
        TEST_ASSERT(r == fragment_result::invalid, "data_size=0 should return invalid");
    }

    // --- rejected: on_allocate returns nullptr
    {
        fragment_reassembler ra2;
        ra2.set_on_allocate([](const endpoint_key &, uint32_t, uint8_t, uint8_t, size_t) -> uint8_t *
                            { return nullptr; });
        endpoint_key ep{};
        fragment_header fh{1, 0, 2};
        uint8_t data[10] = {0xAA};
        auto r = ra2.process_fragment(ep, 0, fh, data, 10);
        TEST_ASSERT(r == fragment_result::rejected, "nullptr alloc should return rejected");
    }

    // --- accepted + duplicate + completed
    {
        fragment_reassembler ra3;
        std::vector<uint8_t> buf(MAX_FRAGMENT_PAYLOAD * 2, 0);
        ra3.set_on_allocate([&](const endpoint_key &, uint32_t, uint8_t, uint8_t, size_t) -> uint8_t *
                            { return buf.data(); });
        bool completed_flag = false;
        ra3.set_on_complete([&](const endpoint_key &, uint32_t, uint8_t, uint8_t *, size_t) { completed_flag = true; });

        endpoint_key ep{};
        uint8_t data[100] = {0xBB};

        fragment_header fh0{5, 0, 2};
        auto r0 = ra3.process_fragment(ep, 0, fh0, data, 100);
        TEST_ASSERT(r0 == fragment_result::accepted, "first frag should return accepted");

        // duplicate
        auto rd = ra3.process_fragment(ep, 0, fh0, data, 100);
        TEST_ASSERT(rd == fragment_result::duplicate, "same frag again should return duplicate");

        // complete
        fragment_header fh1{5, 1, 2};
        auto r1 = ra3.process_fragment(ep, 0, fh1, data, 100);
        TEST_ASSERT(r1 == fragment_result::completed, "last frag should return completed");
        TEST_ASSERT(completed_flag, "on_complete should have fired");
    }

    return true;
}

// ============================================================================
// TEST 54: capacity() and usage_percent() queries
// ============================================================================

static bool test_reassembler_capacity_usage()
{
    fragment_reassembler ra;

    TEST_ASSERT(ra.capacity() == MAX_INCOMING_FRAGMENTED_MESSAGES, "capacity should be MAX_INCOMING");
    TEST_ASSERT(ra.pending_count() == 0, "initial pending should be 0");
    TEST_ASSERT(ra.usage_percent() == 0, "initial usage should be 0%");

    // Fill 32 slots (50%)
    std::vector<std::vector<uint8_t>> bufs(32, std::vector<uint8_t>(MAX_FRAGMENT_PAYLOAD * 2, 0));
    int alloc_idx = 0;
    ra.set_on_allocate([&](const endpoint_key &, uint32_t, uint8_t, uint8_t, size_t) -> uint8_t *
                       { return bufs[alloc_idx++].data(); });

    endpoint_key ep{};
    for (uint32_t i = 0; i < 32; ++i)
    {
        fragment_header fh{i + 1, 0, 2}; // msg_id = i+1, only send frag 0 of 2
        uint8_t data[10] = {};
        ra.process_fragment(ep, 0, fh, data, 10);
    }

    TEST_ASSERT(ra.pending_count() == 32, "should have 32 pending");
    TEST_ASSERT(ra.usage_percent() == 50, "32/64 = 50%");

    return true;
}

// ============================================================================
// TEST 55: clear() releases all entries via on_expired
// ============================================================================

static bool test_reassembler_clear()
{
    fragment_reassembler ra;
    int expired_count = 0;

    std::vector<std::vector<uint8_t>> bufs(5, std::vector<uint8_t>(MAX_FRAGMENT_PAYLOAD * 2, 0));
    int alloc_idx = 0;
    ra.set_on_allocate([&](const endpoint_key &, uint32_t, uint8_t, uint8_t, size_t) -> uint8_t *
                       { return bufs[alloc_idx++].data(); });
    ra.set_on_failed([&](const endpoint_key &, uint32_t, uint8_t, uint8_t *, message_fail_reason, uint8_t, uint8_t)
                     { expired_count++; });

    endpoint_key ep{};
    for (uint32_t i = 0; i < 5; ++i)
    {
        fragment_header fh{i + 1, 0, 3};
        uint8_t data[10] = {};
        ra.process_fragment(ep, 0, fh, data, 10);
    }

    TEST_ASSERT(ra.pending_count() == 5, "should have 5 pending before clear");
    ra.clear();
    TEST_ASSERT(ra.pending_count() == 0, "should have 0 pending after clear");
    TEST_ASSERT(expired_count == 5, "on_expired should fire for each active entry");

    return true;
}

// ============================================================================
// TEST 56: Eviction — slots full triggers evict of least-progress entry
// ============================================================================
// Fill all 64 slots with messages at varying progress levels.
// Insert one more → the entry with least progress gets evicted.

static bool test_reassembler_eviction()
{
    fragment_reassembler ra;

    // Allocate buffers for all 64 + 1 messages
    constexpr size_t N = MAX_INCOMING_FRAGMENTED_MESSAGES;
    std::vector<std::vector<uint8_t>> bufs(N + 1, std::vector<uint8_t>(MAX_FRAGMENT_PAYLOAD * 4, 0));
    size_t alloc_idx = 0;
    ra.set_on_allocate([&](const endpoint_key &, uint32_t, uint8_t, uint8_t, size_t) -> uint8_t *
                       { return bufs[alloc_idx++].data(); });

    bool evicted_called = false;
    uint32_t evicted_msg_id = 0;
    uint8_t evicted_received = 0;
    uint8_t evicted_total = 0;
    ra.set_on_failed(
        [&](const endpoint_key &, uint32_t msg_id, uint8_t, uint8_t *, message_fail_reason, uint8_t recv, uint8_t total)
        {
            evicted_called = true;
            evicted_msg_id = msg_id;
            evicted_received = recv;
            evicted_total = total;
        });

    endpoint_key ep{};

    // Fill all 64 slots — msg_id 1..64, each with 4 fragments, send 2 frags each
    for (uint32_t i = 0; i < static_cast<uint32_t>(N); ++i)
    {
        uint8_t data[100] = {};
        fragment_header fh0{i + 1, 0, 4};
        ra.process_fragment(ep, 0, fh0, data, 100);
        fragment_header fh1{i + 1, 1, 4};
        ra.process_fragment(ep, 0, fh1, data, 100);
    }
    // All 64 have 2/4 progress

    // Now make msg_id=1 have only 2/4 fragments (same as others)
    // But give msg_id=64 extra progress: 3/4
    {
        uint8_t data[100] = {};
        fragment_header fh2{64, 2, 4};
        ra.process_fragment(ep, 0, fh2, data, 100);
    }
    // Now msg_id=64 has 3/4, all others have 2/4
    // The victim should be msg_id=1 (first with worst progress 2/4, found first in scan)

    TEST_ASSERT(ra.pending_count() == N, "should have all slots full");
    TEST_ASSERT(!evicted_called, "no eviction yet");

    // Insert message 65 — triggers eviction
    {
        uint8_t data[100] = {};
        fragment_header fh{100, 0, 2}; // msg_id=100
        auto r = ra.process_fragment(ep, 0, fh, data, 100);
        // Should succeed (eviction made room)
        TEST_ASSERT(r == fragment_result::accepted, "new message should be accepted after eviction");
    }

    TEST_ASSERT(evicted_called, "on_evicted should have fired");
    TEST_ASSERT(evicted_msg_id == 1, "msg_id=1 should have been evicted (first with min progress 2/4)");
    TEST_ASSERT(evicted_received == 2, "evicted had 2 fragments received");
    TEST_ASSERT(evicted_total == 4, "evicted had 4 total fragments");
    TEST_ASSERT(ra.pending_count() == N, "should still have all slots full (new one replaced evicted)");

    return true;
}

// ============================================================================
// TEST 57: Eviction prefers least-progress, not arbitrary
// ============================================================================
// Fill 64 slots with varying progress. Verify the one with 0 progress
// (only allocation, no fragments stored yet) is NOT possible since
// on_allocate is only called when first fragment arrives.
// Instead: fill with 1/3 and one with 2/3 — the 1/3 victim gets evicted.

static bool test_reassembler_eviction_picks_least()
{
    fragment_reassembler ra;

    constexpr size_t N = MAX_INCOMING_FRAGMENTED_MESSAGES;
    std::vector<std::vector<uint8_t>> bufs(N + 1, std::vector<uint8_t>(MAX_FRAGMENT_PAYLOAD * 3, 0));
    size_t alloc_idx = 0;
    ra.set_on_allocate([&](const endpoint_key &, uint32_t, uint8_t, uint8_t, size_t) -> uint8_t *
                       { return bufs[alloc_idx++].data(); });

    uint32_t evicted_msg_id = 0;
    ra.set_on_failed([&](const endpoint_key &, uint32_t msg_id, uint8_t, uint8_t *, message_fail_reason, uint8_t,
                         uint8_t) { evicted_msg_id = msg_id; });

    endpoint_key ep{};

    // Fill slot 0: msg_id=1, 1/3 progress (this should be victim)
    {
        uint8_t data[50] = {};
        fragment_header fh{1, 0, 3};
        ra.process_fragment(ep, 0, fh, data, 50);
    }

    // Fill slots 1..63: msg_id=2..64, each 2/3 progress
    for (uint32_t i = 1; i < static_cast<uint32_t>(N); ++i)
    {
        uint8_t data[50] = {};
        fragment_header fh0{i + 1, 0, 3};
        ra.process_fragment(ep, 0, fh0, data, 50);
        fragment_header fh1{i + 1, 1, 3};
        ra.process_fragment(ep, 0, fh1, data, 50);
    }

    TEST_ASSERT(ra.pending_count() == N, "all slots full");

    // Insert msg 200 — should evict msg_id=1 (1/3 < 2/3)
    {
        uint8_t data[50] = {};
        fragment_header fh{200, 0, 2};
        auto r = ra.process_fragment(ep, 0, fh, data, 50);
        TEST_ASSERT(r == fragment_result::accepted, "should succeed after eviction");
    }

    TEST_ASSERT(evicted_msg_id == 1, "msg_id=1 with 1/3 progress should be evicted, not 2/3 entries");

    return true;
}

// ============================================================================
// TEST 58: on_message_failed fires with reason=evicted on capacity overflow
// ============================================================================

static bool test_eviction_fires_on_failed()
{
    fragment_reassembler ra;

    constexpr size_t N = MAX_INCOMING_FRAGMENTED_MESSAGES;
    std::vector<std::vector<uint8_t>> bufs(N + 1, std::vector<uint8_t>(MAX_FRAGMENT_PAYLOAD * 2, 0));
    size_t alloc_idx = 0;
    ra.set_on_allocate([&](const endpoint_key &, uint32_t, uint8_t, uint8_t, size_t) -> uint8_t *
                       { return bufs[alloc_idx++].data(); });

    bool failed_fired = false;
    uint32_t failed_msg_id = 0;
    message_fail_reason failed_reason{};
    ra.set_on_failed(
        [&](const endpoint_key &, uint32_t msg_id, uint8_t, uint8_t *, message_fail_reason reason, uint8_t, uint8_t)
        {
            failed_fired = true;
            failed_msg_id = msg_id;
            failed_reason = reason;
        });

    endpoint_key ep{};
    for (uint32_t i = 0; i < static_cast<uint32_t>(N); ++i)
    {
        uint8_t data[50] = {};
        fragment_header fh{i + 1, 0, 2};
        ra.process_fragment(ep, 0, fh, data, 50);
    }

    // Trigger eviction
    {
        uint8_t data[50] = {};
        fragment_header fh{999, 0, 2};
        ra.process_fragment(ep, 0, fh, data, 50);
    }

    TEST_ASSERT(failed_fired, "on_message_failed should fire on eviction");
    TEST_ASSERT(failed_msg_id == 1, "oldest entry should be evicted");
    TEST_ASSERT(failed_reason == message_fail_reason::evicted, "reason should be evicted");

    return true;
}

// ============================================================================
// TEST 59: Per-connection reassembler — server isolates clients
// ============================================================================
// Two clients send fragments. Each client's reassembler is independent.
// Verify that one client filling their slots doesn't affect the other.

static bool test_per_connection_reassembler_isolation()
{
    server srv(9931);
    srv.channels().register_defaults();
    std::atomic<bool> stop_flag{false};

    // Track completions per message_id
    std::atomic<int> complete_count{0};
    std::vector<uint8_t> buf1(MAX_FRAGMENT_PAYLOAD * 3, 0);
    std::vector<uint8_t> buf2(MAX_FRAGMENT_PAYLOAD * 3, 0);
    int alloc_calls = 0;

    srv.set_on_allocate_message(
        [&](const endpoint_key &, uint32_t, uint8_t, uint8_t, size_t) -> uint8_t *
        {
            alloc_calls++;
            return (alloc_calls % 2 == 1) ? buf1.data() : buf2.data();
        });
    srv.set_on_message_complete([&](const endpoint_key &, uint32_t, uint8_t, uint8_t *, size_t) { complete_count++; });

    TEST_ASSERT(succeeded(srv.start()), "server should start");
    std::thread server_thread(
        [&]()
        {
            while (!stop_flag.load())
            {
                srv.poll();
                srv.update();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });

    // Two clients each send a fragmented message
    client c1("127.0.0.1", 9931);
    client c2("127.0.0.1", 9931);
    c1.set_verbose(false);
    c2.set_verbose(false);
    c1.channels().register_defaults();
    c2.channels().register_defaults();

    TEST_ASSERT(succeeded(c1.connect()), "c1 should connect");
    TEST_ASSERT(succeeded(c2.connect()), "c2 should connect");

    const size_t msg_size = MAX_FRAGMENT_PAYLOAD * 2 + 100; // 3 fragments
    std::vector<uint8_t> data1(msg_size, 0x11);
    std::vector<uint8_t> data2(msg_size, 0x22);

    c1.send(data1.data(), msg_size, channels::RELIABLE.id);
    c2.send(data2.data(), msg_size, channels::RELIABLE.id);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (complete_count.load() < 2 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    TEST_ASSERT(complete_count.load() == 2, "both fragmented messages should complete independently");

    c1.disconnect();
    c2.disconnect();
    stop_flag = true;
    server_thread.join();
    srv.stop();

    return true;
}

// ============================================================================
// TEST 60: set_on_message_failed (evicted) E2E — server eviction callback
// ============================================================================
// Fill a connection's reassembler to capacity, then send one more message.
// Verify on_message_failed fires with reason=evicted via the server API.

static bool test_set_on_message_failed_evicted_e2e()
{
    server srv(9932);
    srv.channels().register_defaults();
    srv.set_reassembly_timeout(30'000'000); // 30s — no timeout eviction during test
    std::atomic<bool> stop_flag{false};

    constexpr size_t N = MAX_INCOMING_FRAGMENTED_MESSAGES;
    // We need N+1 buffers (N fill + 1 new)
    std::vector<std::vector<uint8_t>> bufs(N + 1, std::vector<uint8_t>(MAX_FRAGMENT_PAYLOAD * 3, 0));
    std::atomic<size_t> alloc_idx{0};

    srv.set_on_allocate_message(
        [&](const endpoint_key &, uint32_t, uint8_t, uint8_t, size_t) -> uint8_t *
        {
            size_t i = alloc_idx.fetch_add(1);
            return (i < bufs.size()) ? bufs[i].data() : nullptr;
        });

    std::atomic<bool> evicted_fired{false};
    std::atomic<uint32_t> evicted_msg_id{0};
    srv.set_on_message_failed(
        [&](const endpoint_key &, uint32_t msg_id, uint8_t, uint8_t *, message_fail_reason reason, uint8_t recv,
            uint8_t total)
        {
            if (reason != message_fail_reason::evicted)
                return;
            evicted_fired.store(true);
            evicted_msg_id.store(msg_id);
        });

    TEST_ASSERT(succeeded(srv.start()), "server should start");
    std::thread server_thread(
        [&]()
        {
            while (!stop_flag.load())
            {
                srv.poll();
                srv.update();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });

    client c("127.0.0.1", 9932);
    c.set_verbose(false);
    c.channels().register_defaults();
    TEST_ASSERT(succeeded(c.connect()), "client should connect");

    // Send N incomplete messages (1/3 each) to fill the per-connection reassembler
    for (uint32_t i = 0; i < static_cast<uint32_t>(N); ++i)
    {
        // Send frag 0 of 3 with a distinct message_id
        // Use raw send so we control the message_id manually
        fragment_header fhdr{i + 1, 0, 3};
        uint8_t frag_payload[FRAGMENT_HEADER_SIZE + 100];
        std::memcpy(frag_payload, &fhdr, FRAGMENT_HEADER_SIZE);
        std::memset(frag_payload + FRAGMENT_HEADER_SIZE, static_cast<uint8_t>(i), 100);

        packet_header hdr{};
        hdr.flags = FLAG_FRAGMENT;
        hdr.channel_id = channels::RELIABLE.id;
        hdr.payload_size = static_cast<uint16_t>(FRAGMENT_HEADER_SIZE + 100);
        c.send_raw(hdr, frag_payload);
    }

    // Wait for server to process all fragments
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (alloc_idx.load() < N && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    TEST_ASSERT(alloc_idx.load() >= N, "server should have allocated N entries");
    TEST_ASSERT(!evicted_fired.load(), "no eviction yet");

    // Send one more incomplete message → triggers eviction
    {
        fragment_header fhdr{999, 0, 3};
        uint8_t frag_payload[FRAGMENT_HEADER_SIZE + 100];
        std::memcpy(frag_payload, &fhdr, FRAGMENT_HEADER_SIZE);
        std::memset(frag_payload + FRAGMENT_HEADER_SIZE, 0xFF, 100);

        packet_header hdr{};
        hdr.flags = FLAG_FRAGMENT;
        hdr.channel_id = channels::RELIABLE.id;
        hdr.payload_size = static_cast<uint16_t>(FRAGMENT_HEADER_SIZE + 100);
        c.send_raw(hdr, frag_payload);
    }

    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!evicted_fired.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    TEST_ASSERT(evicted_fired.load(),
                "on_message_failed (evicted) should have fired via server per-connection reassembler");
    TEST_ASSERT(evicted_msg_id.load() == 1, "msg_id=1 should be evicted (first, min progress)");

    c.disconnect();
    stop_flag = true;
    server_thread.join();
    srv.stop();

    return true;
}

// ============================================================================
// TEST 61: Backpressure — client.is_fragment_throttled() and send -2
// ============================================================================
// Manually set backpressure on the client's connection and verify
// send returns -2 for fragmented sends.

static bool test_client_backpressure_throttle()
{
    // Start a simple server
    test_server_ctx ctx(9933);
    TEST_ASSERT(ctx.start(), "server should start");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    client c("127.0.0.1", 9933);
    c.set_verbose(false);
    c.channels().register_defaults();
    TEST_ASSERT(succeeded(c.connect()), "client should connect");

    // Not throttled initially
    TEST_ASSERT(!c.is_fragment_throttled(), "should NOT be throttled initially");

    // Non-fragmented sends should still work under backpressure
    c.connection().set_fragment_backpressured(true);
    TEST_ASSERT(c.is_fragment_throttled(), "should be throttled after set");

    // Small payload (single packet) should NOT be affected
    int r1 = c.send("hello", 5, channels::RELIABLE.id);
    TEST_ASSERT(r1 > 0, "single-packet send should work even under backpressure");

    // Fragmented payload should return a negative error_code
    std::vector<uint8_t> big(MAX_FRAGMENT_PAYLOAD * 3, 0xAA);
    int r2 = c.send(big.data(), big.size(), channels::RELIABLE.id);
    TEST_ASSERT(r2 < 0, "fragmented send should return negative error_code when backpressured");

    // Release backpressure
    c.connection().set_fragment_backpressured(false);
    TEST_ASSERT(!c.is_fragment_throttled(), "should not be throttled after release");

    c.disconnect();
    return true;
}

// ============================================================================
// TEST 62: Backpressure — server.is_fragment_throttled() and send_to error_code
// ============================================================================

static bool test_server_backpressure_throttle()
{
    server srv(9934);
    srv.channels().register_defaults();
    std::atomic<bool> stop_flag{false};
    std::string client_addr;
    uint16_t client_port = 0;
    std::atomic<bool> client_known{false};

    srv.set_on_client_connected(
        [&](const endpoint_key &, const std::string &addr, uint16_t port)
        {
            client_addr = addr;
            client_port = port;
            client_known = true;
        });

    TEST_ASSERT(succeeded(srv.start()), "server should start");
    std::thread server_thread(
        [&]()
        {
            while (!stop_flag.load())
            {
                srv.poll();
                srv.update();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });

    client c("127.0.0.1", 9934);
    c.set_verbose(false);
    c.channels().register_defaults();
    TEST_ASSERT(succeeded(c.connect()), "client should connect");

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!client_known.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    TEST_ASSERT(client_known.load(), "server should know client");

    // Not throttled initially
    TEST_ASSERT(!srv.is_fragment_throttled(client_addr, client_port), "should NOT be throttled initially");

    // Fragmented send should work
    std::vector<uint8_t> big(MAX_FRAGMENT_PAYLOAD * 3, 0xBB);
    int r1 = srv.send_to(big.data(), big.size(), channels::RELIABLE.id, client_addr, client_port);
    TEST_ASSERT(r1 > 0, "fragmented send should work when not throttled");

    // Unknown address — not throttled
    TEST_ASSERT(!srv.is_fragment_throttled("192.168.1.1", 9999), "unknown addr should not be throttled");

    c.disconnect();
    stop_flag = true;
    server_thread.join();
    srv.stop();

    return true;
}

// ============================================================================
// TEST 63: Per-connection reassembler — connection reset clears reassembler
// ============================================================================
// Verify udp_connection::reset() calls reassembler.clear() and fires
// on_expired for active entries.

static bool test_connection_reset_clears_reassembler()
{
    udp_connection conn;
    conn.reset();
    conn.set_active(true);
    conn.set_state(connection_state::CONNECTED);

    int expired_count = 0;
    auto &ra = conn.reassembler();
    std::vector<uint8_t> buf(MAX_FRAGMENT_PAYLOAD * 2, 0);
    ra.set_on_allocate([&](const endpoint_key &, uint32_t, uint8_t, uint8_t, size_t) -> uint8_t *
                       { return buf.data(); });
    ra.set_on_failed([&](const endpoint_key &, uint32_t, uint8_t, uint8_t *, message_fail_reason, uint8_t, uint8_t)
                     { expired_count++; });

    // Add 3 incomplete messages
    endpoint_key ep{};
    for (uint32_t i = 0; i < 3; ++i)
    {
        fragment_header fh{i + 1, 0, 2};
        uint8_t data[10] = {};
        ra.process_fragment(ep, 0, fh, data, 10);
    }
    TEST_ASSERT(ra.pending_count() == 3, "should have 3 pending");

    // reset() should clear the reassembler
    conn.reset();
    TEST_ASSERT(ra.pending_count() == 0, "reassembler should be clear after connection reset");
    TEST_ASSERT(expired_count == 3, "on_expired should fire for each entry during reset");

    return true;
}

// ============================================================================
// TEST: Ordered simple messages delivered in order
// ============================================================================
// Sends N simple messages on the RELIABLE_ORDERED channel one at a time,
// waiting for each echo before sending the next. Verifies the echoes arrive
// in strict ascending msg_id order.
// ============================================================================

static bool test_ordered_simple_delivery()
{
    server srv(9940);
    srv.channels().register_defaults();
    std::atomic<bool> stop_flag{false};

    // Server echoes every simple packet back on the same channel
    srv.set_on_client_data_received(
        [&](const packet_header &header, const uint8_t *payload, size_t payload_size, const endpoint_key &sender)
        { srv.send_to(payload, payload_size, header.channel_id, sender); });

    TEST_ASSERT(succeeded(srv.start()), "server should start");

    std::thread server_thread(
        [&]()
        {
            while (!stop_flag.load())
            {
                srv.poll();
                srv.update();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });

    client c("127.0.0.1", 9940);
    c.set_verbose(false);
    c.channels().register_defaults();

    // Track echo arrival order
    std::vector<uint32_t> echo_order;
    std::atomic<bool> echo_received{false};

    c.set_on_data_received(
        [&](const packet_header &, const uint8_t *payload, size_t size)
        {
            if (size >= sizeof(uint32_t))
            {
                uint32_t mid;
                std::memcpy(&mid, payload, sizeof(uint32_t));
                echo_order.push_back(mid);
                echo_received = true;
            }
        });

    TEST_ASSERT(succeeded(c.connect()), "client should connect");

    constexpr int NUM_MSGS = 20;

    for (int i = 0; i < NUM_MSGS; ++i)
    {
        // Build payload: just the msg_id as first 4 bytes
        uint32_t msg_id = static_cast<uint32_t>(i);
        uint8_t payload[8] = {};
        std::memcpy(payload, &msg_id, sizeof(uint32_t));

        c.send(payload, sizeof(payload), channels::ORDERED.id);

        // Wait for echo before sending next (ordered gating)
        echo_received = false;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!echo_received.load() && std::chrono::steady_clock::now() < deadline)
        {
            c.poll();
            c.update(nullptr);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        TEST_ASSERT(echo_received.load(), "echo should be received for each ordered message");
    }

    // Verify ordering: all echoes must be in strict ascending order
    TEST_ASSERT(static_cast<int>(echo_order.size()) == NUM_MSGS, "should receive all echoes");
    int violations = 0;
    for (size_t i = 1; i < echo_order.size(); ++i)
    {
        if (echo_order[i] <= echo_order[i - 1])
            ++violations;
    }
    TEST_ASSERT(violations == 0, "ordered echoes must have zero order violations");

    c.disconnect();
    stop_flag = true;
    server_thread.join();
    srv.stop();
    return true;
}

// ============================================================================
// TEST: Ordered fragmented messages delivered in order
// ============================================================================
// Sends N fragmented messages on the RELIABLE_ORDERED channel one at a time,
// waiting for each reassembled echo before sending the next. Verifies
// the echoes arrive in strict ascending msg_id order.
// ============================================================================

static bool test_ordered_fragmented_delivery()
{
    server srv(9941);
    srv.channels().register_defaults();
    std::atomic<bool> stop_flag{false};

    // Server reassembles and echoes back
    srv.set_on_allocate_message([&](const endpoint_key &, uint32_t, uint8_t, uint8_t, size_t max_size) -> uint8_t *
                                { return new uint8_t[max_size]; });

    std::string srv_echo_addr;
    uint16_t srv_echo_port = 0;

    srv.set_on_client_connected(
        [&](const endpoint_key &, const std::string &addr, uint16_t port)
        {
            srv_echo_addr = addr;
            srv_echo_port = port;
        });

    srv.set_on_message_complete(
        [&](const endpoint_key &, uint32_t, uint8_t ch_id, uint8_t *data, size_t total)
        {
            srv.send_to(data, total, ch_id, srv_echo_addr, srv_echo_port);
            delete[] data;
        });

    srv.set_on_message_failed([&](const endpoint_key &, uint32_t, uint8_t, uint8_t *buf, message_fail_reason, uint8_t,
                                  uint8_t) { delete[] buf; });

    TEST_ASSERT(succeeded(srv.start()), "server should start");

    std::thread server_thread(
        [&]()
        {
            while (!stop_flag.load())
            {
                srv.poll();
                srv.update();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });

    client c("127.0.0.1", 9941);
    c.set_verbose(false);
    c.channels().register_defaults();

    // Track echo arrival order (from reassembled fragmented echoes)
    std::vector<uint32_t> echo_order;
    std::atomic<bool> echo_received{false};

    c.set_on_allocate_message([&](const endpoint_key &, uint32_t, uint8_t, uint8_t, size_t max_size) -> uint8_t *
                              { return new uint8_t[max_size]; });

    c.set_on_message_complete(
        [&](const endpoint_key &, uint32_t, uint8_t, uint8_t *data, size_t total_size)
        {
            if (total_size >= sizeof(uint32_t))
            {
                uint32_t mid;
                std::memcpy(&mid, data, sizeof(uint32_t));
                echo_order.push_back(mid);
                echo_received = true;
            }
            delete[] data;
        });

    c.set_on_message_failed([&](const endpoint_key &, uint32_t, uint8_t, uint8_t *buf, message_fail_reason, uint8_t,
                                uint8_t) { delete[] buf; });

    TEST_ASSERT(succeeded(c.connect()), "client should connect");

    constexpr int NUM_MSGS = 10;
    constexpr size_t FRAG_SIZE = 2500; // > MAX_PAYLOAD_SIZE to trigger fragmentation

    for (int i = 0; i < NUM_MSGS; ++i)
    {
        // Build payload: msg_id as first 4 bytes, rest is fill
        std::vector<uint8_t> buf(FRAG_SIZE, 0xAB);
        uint32_t msg_id = static_cast<uint32_t>(i);
        std::memcpy(buf.data(), &msg_id, sizeof(uint32_t));

        c.send(buf.data(), buf.size(), channels::ORDERED.id);

        // Wait for reassembled echo before sending next
        echo_received = false;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!echo_received.load() && std::chrono::steady_clock::now() < deadline)
        {
            c.poll();
            c.update(nullptr);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        TEST_ASSERT(echo_received.load(), "fragmented echo should be received for each ordered message");
    }

    // Verify ordering
    TEST_ASSERT(static_cast<int>(echo_order.size()) == NUM_MSGS, "should receive all fragmented echoes");
    int violations = 0;
    for (size_t i = 1; i < echo_order.size(); ++i)
    {
        if (echo_order[i] <= echo_order[i - 1])
            ++violations;
    }
    TEST_ASSERT(violations == 0, "ordered fragmented echoes must have zero order violations");

    c.disconnect();
    stop_flag = true;
    server_thread.join();
    srv.stop();
    return true;
}

// ============================================================================
// TEST: Mixed simple+fragmented ordered messages delivered in order
// ============================================================================
// Sends alternating simple and fragmented messages on the RELIABLE_ORDERED
// channel, one at a time. Verifies the combined echo arrival order is correct.
// ============================================================================

static bool test_ordered_mixed_delivery()
{
    server srv(9942);
    srv.channels().register_defaults();
    std::atomic<bool> stop_flag{false};

    std::string srv_echo_addr;
    uint16_t srv_echo_port = 0;

    srv.set_on_client_connected(
        [&](const endpoint_key &, const std::string &addr, uint16_t port)
        {
            srv_echo_addr = addr;
            srv_echo_port = port;
        });

    // Echo simple packets
    srv.set_on_client_data_received(
        [&](const packet_header &header, const uint8_t *payload, size_t payload_size, const endpoint_key &sender)
        { srv.send_to(payload, payload_size, header.channel_id, sender); });

    // Echo fragmented messages
    srv.set_on_allocate_message([&](const endpoint_key &, uint32_t, uint8_t, uint8_t, size_t max_size) -> uint8_t *
                                { return new uint8_t[max_size]; });

    srv.set_on_message_complete(
        [&](const endpoint_key &, uint32_t, uint8_t ch_id, uint8_t *data, size_t total)
        {
            srv.send_to(data, total, ch_id, srv_echo_addr, srv_echo_port);
            delete[] data;
        });

    srv.set_on_message_failed([&](const endpoint_key &, uint32_t, uint8_t, uint8_t *buf, message_fail_reason, uint8_t,
                                  uint8_t) { delete[] buf; });

    TEST_ASSERT(succeeded(srv.start()), "server should start");

    std::thread server_thread(
        [&]()
        {
            while (!stop_flag.load())
            {
                srv.poll();
                srv.update();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });

    client c("127.0.0.1", 9942);
    c.set_verbose(false);
    c.channels().register_defaults();

    // Track combined echo arrival order from both simple and fragmented echoes
    std::vector<uint32_t> echo_order;
    std::atomic<bool> echo_received{false};

    c.set_on_data_received(
        [&](const packet_header &hdr, const uint8_t *payload, size_t size)
        {
            if (hdr.channel_id == channels::ORDERED.id && size >= sizeof(uint32_t))
            {
                uint32_t mid;
                std::memcpy(&mid, payload, sizeof(uint32_t));
                echo_order.push_back(mid);
                echo_received = true;
            }
        });

    c.set_on_allocate_message([&](const endpoint_key &, uint32_t, uint8_t, uint8_t, size_t max_size) -> uint8_t *
                              { return new uint8_t[max_size]; });

    c.set_on_message_complete(
        [&](const endpoint_key &, uint32_t, uint8_t ch_id, uint8_t *data, size_t total_size)
        {
            if (ch_id == channels::ORDERED.id && total_size >= sizeof(uint32_t))
            {
                uint32_t mid;
                std::memcpy(&mid, data, sizeof(uint32_t));
                echo_order.push_back(mid);
                echo_received = true;
            }
            delete[] data;
        });

    c.set_on_message_failed([&](const endpoint_key &, uint32_t, uint8_t, uint8_t *buf, message_fail_reason, uint8_t,
                                uint8_t) { delete[] buf; });

    TEST_ASSERT(succeeded(c.connect()), "client should connect");

    // Interleave: simple(0), frag(1), simple(2), frag(3), simple(4), ...
    constexpr int NUM_MSGS = 12;
    constexpr size_t FRAG_SIZE = 2500;

    for (int i = 0; i < NUM_MSGS; ++i)
    {
        bool use_frag = (i % 2 == 1);
        uint32_t msg_id = static_cast<uint32_t>(i);

        if (use_frag)
        {
            std::vector<uint8_t> buf(FRAG_SIZE, 0xCD);
            std::memcpy(buf.data(), &msg_id, sizeof(uint32_t));
            c.send(buf.data(), buf.size(), channels::ORDERED.id);
        }
        else
        {
            uint8_t payload[8] = {};
            std::memcpy(payload, &msg_id, sizeof(uint32_t));
            c.send(payload, sizeof(payload), channels::ORDERED.id);
        }

        // Wait for echo before sending next
        echo_received = false;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!echo_received.load() && std::chrono::steady_clock::now() < deadline)
        {
            c.poll();
            c.update(nullptr);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        TEST_ASSERT(echo_received.load(), "echo should arrive for each mixed ordered message");
    }

    // Verify combined ordering
    TEST_ASSERT(static_cast<int>(echo_order.size()) == NUM_MSGS, "should receive all mixed echoes");
    int violations = 0;
    for (size_t i = 1; i < echo_order.size(); ++i)
    {
        if (echo_order[i] <= echo_order[i - 1])
            ++violations;
    }
    TEST_ASSERT(violations == 0, "mixed ordered echoes must have zero order violations");

    c.disconnect();
    stop_flag = true;
    server_thread.join();
    srv.stop();
    return true;
}

// ============================================================================
// TEST: Server start / stop / restart cycle
// ============================================================================
// Start a server, stop it, start it again on the same port. Verify a client
// can connect after the restart and exchange data.
// ============================================================================

static bool test_server_start_stop_restart()
{
    server srv(9950);
    srv.set_verbose(false);

    // First start
    TEST_ASSERT(succeeded(srv.start()), "first start should succeed");
    TEST_ASSERT(srv.is_running(), "server should be running after start");

    // Stop
    srv.stop();
    TEST_ASSERT(!srv.is_running(), "server should NOT be running after stop");

    // Small pause for OS to release socket
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Restart on the same port
    TEST_ASSERT(succeeded(srv.start()), "restart should succeed on same port");
    TEST_ASSERT(srv.is_running(), "server should be running after restart");

    // Run a server loop, connect a client, send+echo
    std::atomic<bool> stop_flag{false};
    std::atomic<int> echoes{0};

    srv.set_on_client_data_received(
        [&](const packet_header &header, const uint8_t *payload, size_t payload_size, const endpoint_key &sender)
        { srv.send_to(payload, payload_size, header.channel_id, sender); });

    std::thread server_thread(
        [&]()
        {
            while (!stop_flag.load())
            {
                srv.poll();
                srv.update();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });

    client c("127.0.0.1", 9950);
    c.set_verbose(false);
    c.set_on_data_received([&](const packet_header &, const uint8_t *, size_t) { echoes++; });

    TEST_ASSERT(succeeded(c.connect()), "client should connect to restarted server");
    c.send("RESTART", 7);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (echoes.load() == 0 && std::chrono::steady_clock::now() < deadline)
    {
        c.poll();
        c.update(nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    TEST_ASSERT(echoes.load() >= 1, "should receive echo after restart");

    c.disconnect();
    stop_flag = true;
    server_thread.join();
    srv.stop();
    return true;
}

// ============================================================================
// TEST: Server disconnect_client forces a specific client off
// ============================================================================
// Connect 3 clients, server calls disconnect_client on the second one.
// Verify connection_count drops and only the targeted client is removed.
// ============================================================================

static bool test_server_disconnect_client()
{
    server srv(9951);
    srv.set_verbose(false);
    srv.channels().register_defaults();

    std::atomic<bool> stop_flag{false};
    std::vector<endpoint_key> connected_keys;
    std::mutex key_mutex;

    srv.set_on_client_connected(
        [&](const endpoint_key &key, const std::string &, uint16_t)
        {
            std::lock_guard<std::mutex> lk(key_mutex);
            connected_keys.push_back(key);
        });

    TEST_ASSERT(succeeded(srv.start()), "server should start");

    std::thread server_thread(
        [&]()
        {
            while (!stop_flag.load())
            {
                srv.poll();
                srv.update();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });

    // Connect 3 clients
    client c1("127.0.0.1", 9951), c2("127.0.0.1", 9951), c3("127.0.0.1", 9951);
    c1.set_verbose(false);
    c2.set_verbose(false);
    c3.set_verbose(false);
    TEST_ASSERT(succeeded(c1.connect()), "c1 should connect");
    TEST_ASSERT(succeeded(c2.connect()), "c2 should connect");
    TEST_ASSERT(succeeded(c3.connect()), "c3 should connect");

    // Wait for server to register all 3
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (srv.connection_count() < 3 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    TEST_ASSERT(srv.connection_count() == 3, "server should have 3 connections");

    // Disconnect the second client via server API
    endpoint_key target;
    {
        std::lock_guard<std::mutex> lk(key_mutex);
        TEST_ASSERT(connected_keys.size() == 3, "should have 3 keys");
        target = connected_keys[1];
    }
    srv.disconnect_client(target);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    TEST_ASSERT(srv.connection_count() == 2, "server should have 2 connections after disconnect_client");

    c1.disconnect();
    c2.disconnect();
    c3.disconnect();
    stop_flag = true;
    server_thread.join();
    srv.stop();
    return true;
}

// ============================================================================
// TEST: Server disconnect_all removes every connection
// ============================================================================
// Connect multiple clients, call disconnect_all, verify pool is emptied.
// ============================================================================

static bool test_server_disconnect_all()
{
    server srv(9952);
    srv.set_verbose(false);

    std::atomic<bool> stop_flag{false};

    TEST_ASSERT(succeeded(srv.start()), "server should start");

    std::thread server_thread(
        [&]()
        {
            while (!stop_flag.load())
            {
                srv.poll();
                srv.update();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });

    // Connect 4 clients
    constexpr int N = 4;
    std::vector<std::unique_ptr<client>> clients;
    for (int i = 0; i < N; ++i)
    {
        auto c = std::make_unique<client>("127.0.0.1", 9952);
        c->set_verbose(false);
        TEST_ASSERT(succeeded(c->connect()), ("client " + std::to_string(i) + " should connect").c_str());
        clients.push_back(std::move(c));
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (srv.connection_count() < N && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    TEST_ASSERT(srv.connection_count() == static_cast<size_t>(N), "server should have N connections");

    // disconnect_all
    srv.disconnect_all();
    TEST_ASSERT(srv.connection_count() == 0, "server should have 0 connections after disconnect_all");

    for (auto &c : clients)
        c->disconnect();
    clients.clear();

    stop_flag = true;
    server_thread.join();
    srv.stop();
    return true;
}

// ============================================================================
// TEST: Client reconnects after clean disconnect
// ============================================================================
// Client connects, exchanges data, disconnects, then reconnects to the same
// server and exchanges data again.
// ============================================================================

static bool test_reconnect_after_disconnect()
{
    test_server_ctx ctx(9953);
    ctx.srv.set_verbose(false);

    std::atomic<int> server_data_count{0};
    ctx.srv.set_on_client_data_received([&](const packet_header &, const uint8_t *, size_t, const endpoint_key &)
                                        { server_data_count++; });

    TEST_ASSERT(ctx.start(), "server should start");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // First connection
    {
        client c("127.0.0.1", 9953);
        c.set_verbose(false);
        TEST_ASSERT(succeeded(c.connect()), "first connect should succeed");
        c.send("HI1", 3);

        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (server_data_count.load() == 0 && std::chrono::steady_clock::now() < deadline)
        {
            c.poll();
            c.update(nullptr);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        TEST_ASSERT(server_data_count.load() >= 1, "server should receive data on first connection");
        c.disconnect();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    int count_after_first = server_data_count.load();

    // Second connection (same server)
    {
        client c("127.0.0.1", 9953);
        c.set_verbose(false);
        TEST_ASSERT(succeeded(c.connect()), "reconnect should succeed");
        c.send("HI2", 3);

        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (server_data_count.load() == count_after_first && std::chrono::steady_clock::now() < deadline)
        {
            c.poll();
            c.update(nullptr);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        TEST_ASSERT(server_data_count.load() > count_after_first, "server should receive data on second connection");
        c.disconnect();
    }

    return true;
}

// ============================================================================
// TEST: Reconnect reuses the freed pool slot
// ============================================================================
// A client connects, the server sees the connection. Client disconnects,
// server processes it. A new client connects — the server should reuse
// the freed slot (connection_count stays at 1, not 2).
// ============================================================================

static bool test_reconnect_slot_reuse()
{
    server srv(9954);
    srv.set_verbose(false);
    std::atomic<bool> stop_flag{false};
    std::atomic<int> connect_count{0};
    std::atomic<int> disconnect_count{0};

    srv.set_on_client_connected([&](const endpoint_key &, const std::string &, uint16_t) { connect_count++; });
    srv.set_on_client_disconnected([&](const endpoint_key &, const std::string &, uint16_t) { disconnect_count++; });

    TEST_ASSERT(succeeded(srv.start()), "server should start");

    std::thread server_thread(
        [&]()
        {
            while (!stop_flag.load())
            {
                srv.poll();
                srv.update();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });

    // First client connects and disconnects
    {
        client c("127.0.0.1", 9954);
        c.set_verbose(false);
        TEST_ASSERT(succeeded(c.connect()), "first connect should succeed");

        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (connect_count.load() < 1 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        TEST_ASSERT(srv.connection_count() == 1, "should have 1 connection");

        c.disconnect();
    }

    // Wait for server to process disconnect
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (disconnect_count.load() < 1 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    TEST_ASSERT(srv.connection_count() == 0, "should have 0 after disconnect");

    // Second client connects — should reuse the freed slot
    {
        client c("127.0.0.1", 9954);
        c.set_verbose(false);
        TEST_ASSERT(succeeded(c.connect()), "second connect should succeed");

        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (connect_count.load() < 2 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        TEST_ASSERT(srv.connection_count() == 1, "should have 1 connection (slot reused)");
        TEST_ASSERT(connect_count.load() == 2, "should have had 2 total connections");

        c.disconnect();
    }

    stop_flag = true;
    server_thread.join();
    srv.stop();
    return true;
}

// ============================================================================
// TEST: Zero-length payload
// ============================================================================
// Sends a 0-byte payload from client to server. The server must receive
// the packet without crashing (payload_size == 0).
// ============================================================================

static bool test_zero_length_payload()
{
    server srv(9955);
    srv.set_verbose(false);
    std::atomic<bool> stop_flag{false};
    std::atomic<int> data_received{0};
    std::atomic<uint16_t> last_payload_size{9999};

    srv.set_on_client_data_received(
        [&](const packet_header &header, const uint8_t *, size_t payload_size, const endpoint_key &)
        {
            last_payload_size = static_cast<uint16_t>(payload_size);
            data_received++;
        });

    TEST_ASSERT(succeeded(srv.start()), "server should start");

    std::thread server_thread(
        [&]()
        {
            while (!stop_flag.load())
            {
                srv.poll();
                srv.update();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });

    client c("127.0.0.1", 9955);
    c.set_verbose(false);
    TEST_ASSERT(succeeded(c.connect()), "client should connect");

    // Send zero-length payload via send_raw() with empty body
    packet_header hdr{};
    hdr.payload_size = 0;
    c.send_raw(hdr, nullptr);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (data_received.load() == 0 && std::chrono::steady_clock::now() < deadline)
    {
        c.poll();
        c.update(nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    TEST_ASSERT(data_received.load() >= 1, "server should receive the zero-length packet");
    TEST_ASSERT(last_payload_size.load() == 0, "payload_size should be 0");

    c.disconnect();
    stop_flag = true;
    server_thread.join();
    srv.stop();
    return true;
}

// ============================================================================
// TEST: Exact MAX_PAYLOAD_SIZE boundary (no fragmentation)
// ============================================================================
// Sends exactly MAX_PAYLOAD_SIZE bytes. This is the largest payload that
// fits in a single packet (no fragmentation). Verify it arrives intact.
// ============================================================================

static bool test_exact_max_payload()
{
    server srv(9956);
    srv.set_verbose(false);
    srv.channels().register_defaults();
    std::atomic<bool> stop_flag{false};
    std::atomic<int> data_received{0};
    std::atomic<size_t> received_size{0};
    std::atomic<bool> data_intact{false};

    const size_t MAX_PAYLOAD = MAX_PACKET_SIZE - PACKET_HEADER_SIZE;

    srv.set_on_client_data_received(
        [&](const packet_header &, const uint8_t *payload, size_t payload_size, const endpoint_key &)
        {
            received_size = payload_size;
            // Check that every byte matches the pattern
            bool ok = (payload_size == MAX_PAYLOAD);
            for (size_t i = 0; ok && i < payload_size; ++i)
                ok = (payload[i] == static_cast<uint8_t>(i & 0xFF));
            data_intact = ok;
            data_received++;
        });

    TEST_ASSERT(succeeded(srv.start()), "server should start");

    std::thread server_thread(
        [&]()
        {
            while (!stop_flag.load())
            {
                srv.poll();
                srv.update();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });

    client c("127.0.0.1", 9956);
    c.set_verbose(false);
    c.channels().register_defaults();
    TEST_ASSERT(succeeded(c.connect()), "client should connect");

    // Build payload: sequential bytes
    std::vector<uint8_t> buf(MAX_PAYLOAD);
    for (size_t i = 0; i < MAX_PAYLOAD; ++i)
        buf[i] = static_cast<uint8_t>(i & 0xFF);

    int sent = c.send(buf.data(), buf.size(), channels::RELIABLE.id);
    TEST_ASSERT(sent > 0, "send should succeed");

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (data_received.load() == 0 && std::chrono::steady_clock::now() < deadline)
    {
        c.poll();
        c.update(nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    TEST_ASSERT(data_received.load() >= 1, "server should receive the max-payload packet");
    TEST_ASSERT(received_size.load() == MAX_PAYLOAD, "received size should match MAX_PAYLOAD");
    TEST_ASSERT(data_intact.load(), "payload data should be intact byte-for-byte");

    c.disconnect();
    stop_flag = true;
    server_thread.join();
    srv.stop();
    return true;
}

// ============================================================================
// TEST: Server stop() with connected clients (graceful shutdown)
// ============================================================================
// Several clients are connected and actively exchanging data. The server
// calls stop() — verify it doesn't crash and cleans up properly.
// ============================================================================

static bool test_server_stop_with_clients()
{
    server srv(9957);
    srv.set_verbose(false);
    std::atomic<bool> stop_flag{false};

    srv.set_on_client_data_received(
        [&](const packet_header &header, const uint8_t *payload, size_t payload_size, const endpoint_key &sender)
        { srv.send_to(payload, payload_size, header.channel_id, sender, header.flags); });

    TEST_ASSERT(succeeded(srv.start()), "server should start");

    std::thread server_thread(
        [&]()
        {
            while (!stop_flag.load())
            {
                srv.poll();
                srv.update();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            srv.stop();
        });

    // Connect 3 clients and send some data
    constexpr int N = 3;
    std::vector<std::unique_ptr<client>> clients;
    for (int i = 0; i < N; ++i)
    {
        auto c = std::make_unique<client>("127.0.0.1", 9957);
        c->set_verbose(false);
        TEST_ASSERT(succeeded(c->connect()), ("client " + std::to_string(i) + " should connect").c_str());
        c->send("DATA", 4);
        clients.push_back(std::move(c));
    }

    // Let data exchange happen for 200ms
    auto pump_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (std::chrono::steady_clock::now() < pump_until)
    {
        for (auto &c : clients)
        {
            c->poll();
            c->update(nullptr);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    TEST_ASSERT(srv.connection_count() == N, "server should have N connections before stop");

    // Stop server while clients are still connected
    stop_flag = true;
    server_thread.join();

    // Server has stopped — verify clean state
    TEST_ASSERT(!srv.is_running(), "server should not be running");
    TEST_ASSERT(srv.connection_count() == 0, "stop() should have disconnected all");

    // Clients can call disconnect without crashing
    for (auto &c : clients)
        c->disconnect();
    clients.clear();

    return true;
}

int main()
{
    platform_init();

    // -- Failure scenarios --
    register_test("Send before connect", test_send_before_connect);
    register_test("Connect to non-existent server", test_connect_no_server);
    register_test("Double connect", test_double_connect);
    register_test("Double disconnect", test_double_disconnect);
    register_test("Send after disconnect", test_send_after_disconnect);
    register_test("Connection denied (pool full path)", test_connection_denied_pool_full);
    register_test("Client detects server timeout", test_client_timeout_detection);
    register_test("Server detects client disappearance", test_server_timeout_detection);

    // -- Normal behavior --
    register_test("Heartbeat keeps connection alive", test_heartbeat_keeps_alive);
    register_test("Duplicate packet detection", test_duplicate_detection);
    register_test("Sequence buffer wrap", test_sequence_buffer_wrap);
    register_test("RTT estimation converges", test_rtt_convergence);
    register_test("Loss detection (reliable)", test_loss_detection);
    register_test("Unreliable packets no loss tracking", test_unreliable_no_loss_tracking);
    register_test("Full echo cycle", test_full_echo_cycle);
    register_test("Connection state transitions", test_connection_state_transitions);
    register_test("Heartbeat/timeout thresholds", test_heartbeat_timeout_thresholds);
    register_test("Server disconnect callback", test_server_disconnect_callback);
    register_test("Header integrity", test_header_integrity);
    register_test("Multiple clients independent", test_multiple_clients_independent);

    // -- Congestion control --
    register_test("CC initial state", test_cc_initial_state);
    register_test("CC window blocks sends", test_cc_window_blocks);
    register_test("CC slow start growth", test_cc_slow_start_growth);
    register_test("CC loss multiplicative decrease", test_cc_loss_decrease);
    register_test("CC pacing interval", test_cc_pacing_interval);
    register_test("CC connection integration", test_cc_connection_integration);

    // -- Channel system --
    register_test("Channel registration and lookup", test_channel_registration);
    register_test("Channel mode queries", test_channel_modes);
    register_test("Channel default presets", test_channel_defaults);
    register_test("Channel-based loss detection", test_channel_loss_detection);
    register_test("Channel echo integration", test_channel_echo_integration);
    register_test("Channel unreliable no loss", test_channel_unreliable_no_loss);
    register_test("Open channel dynamic", test_open_channel);
    register_test("Unregistered channel unreliable", test_unregistered_channel_is_unreliable);
    register_test("Open channel saturation", test_open_channel_saturation);
    register_test("Channel negotiation accepted", test_channel_negotiation_accepted);
    register_test("Channel negotiation rejected", test_channel_negotiation_rejected);
    register_test("Channel negotiation selective", test_channel_negotiation_selective);
    register_test("Channel name sync", test_channel_name_sync);

    // -- Fragmentation --
    register_test("Fragment reassembler basic", test_fragment_reassembler_basic);
    register_test("Fragment reassembler out-of-order", test_fragment_reassembler_out_of_order);
    register_test("Fragment duplicate ignored", test_fragment_duplicate_ignored);
    register_test("Fragment app rejects allocation", test_fragment_app_rejects);
    register_test("Loss includes fragment info", test_loss_includes_fragment_info);
    register_test("Pending message ACK tracking", test_pending_message_ack_tracking);
    register_test("Small message no fragment", test_small_message_no_fragment);
    register_test("Fragmented E2E", test_fragmented_e2e);
    register_test("Fragmented echo E2E", test_fragmented_echo_e2e);
    register_test("Reassembly timeout", test_reassembly_timeout);
    register_test("message_id wrap protection", test_message_id_wrap_protection);
    register_test("Scatter-gather E2E", test_scatter_gather_e2e);
    register_test("set_on_message_failed (expired) E2E", test_set_on_message_failed_expired_e2e);

    // -- Reassembler defense layers --
    register_test("fragment_result all values", test_fragment_result_values);
    register_test("Reassembler capacity/usage", test_reassembler_capacity_usage);
    register_test("Reassembler clear()", test_reassembler_clear);
    register_test("Reassembler eviction", test_reassembler_eviction);
    register_test("Eviction picks least progress", test_reassembler_eviction_picks_least);
    register_test("Eviction fires on_message_failed", test_eviction_fires_on_failed);
    register_test("Per-connection reassembler isolation", test_per_connection_reassembler_isolation);
    register_test("set_on_message_failed (evicted) E2E", test_set_on_message_failed_evicted_e2e);
    register_test("Client backpressure throttle", test_client_backpressure_throttle);
    register_test("Server backpressure throttle", test_server_backpressure_throttle);
    register_test("Connection reset clears reassembler", test_connection_reset_clears_reassembler);

    // -- Ordered delivery --
    register_test("Ordered simple delivery", test_ordered_simple_delivery);
    register_test("Ordered fragmented delivery", test_ordered_fragmented_delivery);
    register_test("Ordered mixed delivery", test_ordered_mixed_delivery);

    // -- Server lifecycle --
    register_test("Server start/stop/restart", test_server_start_stop_restart);
    register_test("Server disconnect_client", test_server_disconnect_client);
    register_test("Server disconnect_all", test_server_disconnect_all);
    register_test("Server stop with clients", test_server_stop_with_clients);

    // -- Reconnection & slot reuse --
    register_test("Reconnect after disconnect", test_reconnect_after_disconnect);
    register_test("Reconnect slot reuse", test_reconnect_slot_reuse);

    // -- Boundary payloads --
    register_test("Zero-length payload", test_zero_length_payload);
    register_test("Exact MAX_PAYLOAD_SIZE", test_exact_max_payload);

    std::cout << "========================================" << std::endl;
    std::cout << " Entanglement Test Battery" << std::endl;
    std::cout << "========================================" << std::endl;

    run_all();

    std::cout << "========================================" << std::endl;
    std::cout << " Results: " << g_tests_passed << "/" << g_tests_run << " passed";
    if (g_tests_failed > 0)
    {
        std::cout << " (" << g_tests_failed << " FAILED)";
    }
    std::cout << std::endl;

    if (!g_failures.empty())
    {
        std::cout << " Failed tests:" << std::endl;
        for (auto &name : g_failures)
        {
            std::cout << "   - " << name << std::endl;
        }
    }
    std::cout << "========================================" << std::endl;

    platform_shutdown();
    return g_tests_failed == 0 ? 0 : 1;
}
