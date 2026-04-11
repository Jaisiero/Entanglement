# Entanglement

High-performance C++ UDP networking library for real-time multiplayer games. Designed for MMO-scale server infrastructure with sub-millisecond latency targets.

## Overview

Entanglement provides reliable and unreliable UDP channels with connection management, fragmentation, congestion control, and platform abstraction. The library exposes a C API (`entanglement.h`) for interop with Rust and other languages.

## Features

- **Reliable & unreliable channels** — per-connection channel manager with ordered delivery
- **Message coalescing** — batch multiple messages into single UDP datagrams via `sendmmsg`
- **Fragmentation** — automatic splitting/reassembly for messages exceeding MTU
- **Congestion control** — adaptive send rate with RTT estimation
- **Server workers** — multi-threaded server with worker pool for parallel packet processing
- **AF_XDP support** — optional zero-copy networking on Linux via XDP sockets
- **Lock-free queues** — SPSC and MPSC queues for cross-thread message passing
- **Send pool** — buffer reuse to minimize allocations on the hot path
- **C API** — `ent_*` prefixed functions for FFI consumers (used by `entanglement-rs`)

## Architecture

```
include/
  entanglement.h          Public C API (ent_client_*, ent_server_*, ent_endpoint_*)

src/
  client.cpp/h            Client: connect, send, receive, disconnect
  server.cpp/h            Server: accept connections, manage endpoints
  server_worker.cpp/h     Worker threads for parallel packet processing
  udp_connection.cpp/h    Per-peer connection state, reliability, ordering
  udp_socket.cpp/h        Platform socket abstraction (BSD/Winsock/AF_XDP)
  channel_manager.cpp/h   Reliable/unreliable channel multiplexing
  congestion_control.cpp/h Adaptive send rate, RTT, bandwidth estimation
  fragmentation.cpp/h     Message splitting/reassembly above MTU
  entanglement_c.cpp      C API implementation (bridges C++ to C interface)
  packet_header.h         Wire format header layout
  endpoint_key.cpp/h      Connection identifier (IP:port tuple)
  platform.cpp/h          OS abstraction (timers, sockets, threads)
  constants.h             Protocol constants (MTU, timeouts, buffer sizes)
  spsc_queue.h            Lock-free single-producer single-consumer queue
  mpsc_queue.h            Lock-free multi-producer single-consumer queue
  send_pool.h             Pre-allocated buffer pool for zero-alloc sends

apps/
  server_main.cpp         Standalone test server
  client_main.cpp         Standalone test client
  net_bench.cpp           Network throughput/latency benchmark
  test_main.cpp           Unit/integration tests
  churn_test_main.cpp     Connection churn stress test
  stress_test_main.cpp    High-load stress test
```

## Source (~17,400 LOC)

| Component | LOC | Description |
|-----------|-----|-------------|
| `udp_connection` | 1,716 | Per-peer connection, reliability, ordering |
| `entanglement_c` | 918 | C API bridge |
| `server_worker` | 1,210 | Multi-threaded worker pool |
| `server` | 1,103 | Server accept/manage connections |
| `udp_socket` | 1,057 | Platform socket (BSD/Winsock/XDP) |
| `client` | 547 | Client connection management |
| `fragmentation` | 475 | Message split/reassembly |
| `congestion_control` | 296 | Adaptive send rate + RTT |
| `channel_manager` | 206 | Channel multiplexing |
| Apps/tests | 8,617 | Benchmarks, tests, examples |

## Build

```bash
# Linux (with AF_XDP support)
g++ -std=c++20 -O2 -o server apps/server_main.cpp src/*.cpp -lbpf -lxdp -lpthread

# CMake (recommended)
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### Requirements

- C++20 compiler (GCC 10+ / Clang 12+/ MSVC 2022+)
- Linux: `libbpf`, `libxdp` (for AF_XDP zero-copy path)
- Windows: Winsock2 (`ws2_32`)

## C API

The public API is in `include/entanglement.h`. Key functions:

```c
// Client
ent_client_t* ent_client_create(ent_client_config_t* config);
int ent_client_connect(ent_client_t* client, const char* address, uint16_t port);
int ent_client_send(ent_client_t* client, const void* data, uint32_t len, uint8_t channel);
int ent_client_receive(ent_client_t* client, void* buf, uint32_t buf_len, uint32_t* out_len);

// Server
ent_server_t* ent_server_create(ent_server_config_t* config);
int ent_server_send(ent_server_t* server, ent_endpoint_t endpoint, const void* data, uint32_t len, uint8_t channel);
```

## Branches

- `main` — stable release
- `message_coalescing` — active development: sendmmsg batching, MPSC queue, send pool
- `linux_port_reuse` — SO_REUSEPORT experiments
