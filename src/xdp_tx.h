#pragma once

// AF_XDP TX-only kernel bypass for UDP sends.
// Linux-only; on other platforms these are stubs that return failure.
// Falls back to regular sendmsg when unavailable.

#ifdef __linux__

#include <cstdint>
#include <cstddef>

namespace entanglement
{

// Initialize AF_XDP TX subsystem.
// Loads a minimal XDP_PASS program, creates per-worker UMEM + XDP sockets.
// Returns 0 on success, negative errno on failure.
// On failure, caller should fall back to sendmsg (this is expected on
// non-Linux, in containers, or when CAP_BPF is missing).
int xdp_tx_init(const char *iface, int num_workers,
                uint32_t src_ip_net,   // source IP in network byte order
                uint16_t src_port_host // source port in host byte order
);

// Returns true if xdp_tx_init() succeeded and AF_XDP is active.
bool xdp_tx_available();

// Send a GSO-style buffer as individual AF_XDP frames.
// Mirrors udp_socket::send_gso(): splits buffer at segment_size boundaries,
// wraps each segment in Eth+IP+UDP headers, and enqueues to the TX ring.
// Does NOT flush — call xdp_tx_flush() after a batch of sends.
// Returns number of bytes enqueued, or negative error.
int xdp_tx_send_gso(int worker_idx,
                     uint32_t dst_ip_net,    // network byte order
                     uint16_t dst_port_host, // host byte order
                     const void *buffer, size_t total_size,
                     uint16_t segment_size);

// Flush pending TX frames for a worker: kick the TX ring and reclaim
// completed frames. Call once per tick after all sends for this worker.
void xdp_tx_flush(int worker_idx);

// Destroy all AF_XDP resources (sockets, UMEM, XDP program).
void xdp_tx_cleanup();

} // namespace entanglement

#else // !__linux__

namespace entanglement
{
inline int  xdp_tx_init(const char *, int, uint32_t, uint16_t) { return -1; }
inline bool xdp_tx_available() { return false; }
inline int  xdp_tx_send_gso(int, uint32_t, uint16_t, const void *, size_t, uint16_t) { return -1; }
inline void xdp_tx_flush(int) {}
inline void xdp_tx_cleanup() {}
} // namespace entanglement

#endif // __linux__
