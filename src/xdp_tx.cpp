#ifdef __linux__

#include "xdp_tx.h"

#include <cstring>
#include <cstdio>
#include <cerrno>
#include <array>
#include <vector>
#include <unordered_map>
#include <mutex>

// Linux / AF_XDP headers
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <linux/if_ether.h>
#include <linux/if_link.h>
#include <linux/bpf.h>
#include <linux/if_xdp.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <net/if_arp.h>
#include <csignal>
#include <cstdlib>

// libbpf / libxdp
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <xdp/xsk.h>

namespace entanglement
{

// ── Constants ──────────────────────────────────────────────────────────
static constexpr int    NUM_FRAMES   = 8192;
static constexpr int    FRAME_SIZE   = 2048;  // fits ETH+IP+UDP+34+1154 = 1230
static constexpr size_t UMEM_SIZE    = NUM_FRAMES * FRAME_SIZE;
static constexpr int    FQ_SIZE      = 256;   // fill queue (RX, minimal)
static constexpr int    ETH_HLEN_V   = 14;
static constexpr int    IP_HLEN_V    = 20;
static constexpr int    UDP_HLEN_V   = 8;
static constexpr int    L234_OVERHEAD = ETH_HLEN_V + IP_HLEN_V + UDP_HLEN_V; // 42

// ── Per-worker context ─────────────────────────────────────────────────
struct xdp_worker_ctx
{
    void                  *umem_area = nullptr;
    struct xsk_umem       *umem      = nullptr;
    struct xsk_socket     *xsk       = nullptr;
    struct xsk_ring_prod   fq{};
    struct xsk_ring_cons   cq{};
    struct xsk_ring_prod   tx{};
    struct xsk_ring_cons   rx{};    // unused but required by API

    // Frame allocator (stack-based, lock-free within a single worker)
    uint32_t frame_free[NUM_FRAMES];
    int      free_count = 0;

    uint32_t alloc_frame()
    {
        if (free_count <= 0) return UINT32_MAX;
        return frame_free[--free_count];
    }

    void release_frame(uint32_t addr)
    {
        frame_free[free_count++] = addr;
    }
};

// ── Global state ───────────────────────────────────────────────────────
static bool                         g_active     = false;
static int                          g_num_workers = 0;
static int                          g_ifindex    = 0;
static int                          g_xdp_prog_fd = -1;
static uint32_t                     g_xdp_flags  = 0;   // flags used for attach (DRV/SKB)
static std::vector<xdp_worker_ctx>  g_workers;
static uint8_t                      g_src_mac[6] = {};
static uint32_t                     g_src_ip     = 0;  // network order
static uint16_t                     g_src_port   = 0;  // host order

// ARP cache: dst_ip (network order) → MAC address
static std::unordered_map<uint32_t, std::array<uint8_t, 6>> g_arp_cache;
static std::mutex g_arp_mutex;

// ── Signal-safe XDP detach (best-effort on SIGTERM/SIGINT) ─────────────
// Only detaches the BPF program from the NIC — no heap ops, async-safe.
static struct sigaction g_prev_sigterm{};
static struct sigaction g_prev_sigint{};
static volatile sig_atomic_t g_signal_cleanup_done = 0;

static void xdp_signal_handler(int sig)
{
    // write() is async-signal-safe — use it for diagnostics
    static const char msg_enter[] = "[xdp_tx] Signal handler: detaching BPF\n";
    static const char msg_done[]  = "[xdp_tx] Signal handler: BPF detached OK\n";
    static const char msg_skip[]  = "[xdp_tx] Signal handler: already cleaned\n";

    if (!g_signal_cleanup_done && g_xdp_prog_fd >= 0 && g_ifindex > 0)
    {
        (void)write(STDERR_FILENO, msg_enter, sizeof(msg_enter) - 1);
        g_signal_cleanup_done = 1;
        // bpf_xdp_detach is a single syscall — safe in signal context
        // Must use same flags (DRV/SKB) that were used for attach
        LIBBPF_OPTS(bpf_xdp_attach_opts, opts);
        bpf_xdp_detach(g_ifindex, g_xdp_flags, &opts);
        close(g_xdp_prog_fd);
        g_xdp_prog_fd = -1;
        (void)write(STDERR_FILENO, msg_done, sizeof(msg_done) - 1);
    }
    else
    {
        (void)write(STDERR_FILENO, msg_skip, sizeof(msg_skip) - 1);
    }

    // Re-raise with original handler so the process terminates normally
    struct sigaction *prev = (sig == SIGTERM) ? &g_prev_sigterm : &g_prev_sigint;
    sigaction(sig, prev, nullptr);
    raise(sig);
}

static void install_signal_handlers()
{
    struct sigaction sa{};
    sa.sa_handler = xdp_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND; // one-shot

    sigaction(SIGTERM, &sa, &g_prev_sigterm);
    sigaction(SIGINT,  &sa, &g_prev_sigint);
}

// atexit callback — normal exit path
static void xdp_atexit_cleanup()
{
    if (g_active)
    {
        // Full cleanup only if signal handler didn't already detach
        xdp_tx_cleanup();
    }
}

// ── Helpers ────────────────────────────────────────────────────────────

static uint16_t ip_checksum(const void *data, int len)
{
    auto *p = static_cast<const uint16_t *>(data);
    uint32_t sum = 0;
    for (int i = 0; i < len / 2; i++)
        sum += p[i];
    if (len & 1)
        sum += static_cast<const uint8_t *>(data)[len - 1];
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return static_cast<uint16_t>(~sum);
}

// Read MAC address of our NIC
static bool read_own_mac(const char *iface, uint8_t mac[6])
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    struct ifreq ifr{};
    std::strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);
    bool ok = ioctl(fd, SIOCGIFHWADDR, &ifr) == 0;
    close(fd);
    if (ok) std::memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
    return ok;
}

// Resolve destination MAC via ARP ioctl (cached)
static bool resolve_mac(uint32_t dst_ip_net, uint8_t mac[6])
{
    {
        std::lock_guard<std::mutex> lock(g_arp_mutex);
        auto it = g_arp_cache.find(dst_ip_net);
        if (it != g_arp_cache.end())
        {
            std::memcpy(mac, it->second.data(), 6);
            return true;
        }
    }

    // ARP ioctl
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;

    struct arpreq arp{};
    auto *sa = reinterpret_cast<sockaddr_in *>(&arp.arp_pa);
    sa->sin_family = AF_INET;
    sa->sin_addr.s_addr = dst_ip_net;

    // Use the interface name from init
    char iface_name[IFNAMSIZ] = {};
    if_indextoname(g_ifindex, iface_name);
    std::strncpy(arp.arp_dev, iface_name, IFNAMSIZ - 1);

    bool ok = ioctl(fd, SIOCGARP, &arp) == 0 && (arp.arp_flags & ATF_COM);
    close(fd);

    if (ok)
    {
        std::memcpy(mac, arp.arp_ha.sa_data, 6);
        std::lock_guard<std::mutex> lock(g_arp_mutex);
        std::array<uint8_t, 6> arr;
        std::memcpy(arr.data(), mac, 6);
        g_arp_cache[dst_ip_net] = arr;
    }
    return ok;
}

// Reclaim completed TX frames back to the free list
static void reclaim_frames(xdp_worker_ctx &ctx)
{
    uint32_t idx = 0;
    unsigned int completed = xsk_ring_cons__peek(&ctx.cq, NUM_FRAMES, &idx);
    for (unsigned int i = 0; i < completed; i++)
    {
        uint64_t addr = *xsk_ring_cons__comp_addr(&ctx.cq, idx + i);
        ctx.release_frame(static_cast<uint32_t>(addr));
    }
    if (completed > 0)
        xsk_ring_cons__release(&ctx.cq, completed);
}

// ── XDP program loading ────────────────────────────────────────────────
// Embedded minimal XDP_PASS program (2 BPF instructions = 16 bytes).
// This passes ALL ingress packets to the regular kernel stack,
// while allowing AF_XDP sockets to be created for TX.

static int load_xdp_pass(const char *iface)
{
    g_ifindex = static_cast<int>(if_nametoindex(iface));
    if (g_ifindex == 0)
    {
        fprintf(stderr, "[xdp_tx] if_nametoindex(%s): %s\n", iface, strerror(errno));
        return -errno;
    }

    // BPF_MOV64_IMM(BPF_REG_0, 2)  →  r0 = XDP_PASS
    // BPF_EXIT_INSN()               →  return r0
    struct bpf_insn prog[] = {
        { .code = 0xb7, .dst_reg = 0, .src_reg = 0, .off = 0, .imm = 2 }, // r0 = 2
        { .code = 0x95, .dst_reg = 0, .src_reg = 0, .off = 0, .imm = 0 }, // exit
    };

    union bpf_attr attr{};
    attr.prog_type = BPF_PROG_TYPE_XDP;
    attr.insns = reinterpret_cast<uint64_t>(prog);
    attr.insn_cnt = 2;

    // License string in memory
    static const char license[] = "GPL";
    attr.license = reinterpret_cast<uint64_t>(license);

    // Log buffer for debugging
    static char log_buf[4096];
    attr.log_buf = reinterpret_cast<uint64_t>(log_buf);
    attr.log_size = sizeof(log_buf);
    attr.log_level = 1;

    g_xdp_prog_fd = static_cast<int>(syscall(__NR_bpf, BPF_PROG_LOAD, &attr, sizeof(attr)));
    if (g_xdp_prog_fd < 0)
    {
        fprintf(stderr, "[xdp_tx] BPF_PROG_LOAD failed: %s\n  log: %s\n",
                strerror(errno), log_buf);
        return -errno;
    }

    // Attach to interface — try native (DRV) mode first, then SKB
    LIBBPF_OPTS(bpf_xdp_attach_opts, opts);
    int ret = bpf_xdp_attach(g_ifindex, g_xdp_prog_fd, XDP_FLAGS_DRV_MODE, &opts);
    if (ret < 0)
    {
        fprintf(stderr, "[xdp_tx] DRV mode attach failed (%s), trying SKB\n", strerror(-ret));
        ret = bpf_xdp_attach(g_ifindex, g_xdp_prog_fd, XDP_FLAGS_SKB_MODE, &opts);
        if (ret < 0)
        {
            fprintf(stderr, "[xdp_tx] SKB mode attach also failed: %s\n", strerror(-ret));
            close(g_xdp_prog_fd);
            g_xdp_prog_fd = -1;
            return ret;
        }
        g_xdp_flags = XDP_FLAGS_SKB_MODE;
        printf("[xdp_tx] XDP_PASS attached (SKB/generic mode)\n");
    }
    else
    {
        g_xdp_flags = XDP_FLAGS_DRV_MODE;
        printf("[xdp_tx] XDP_PASS attached (native/DRV mode)\n");
    }

    return 0;
}

// ── Worker setup ───────────────────────────────────────────────────────

static int setup_worker(xdp_worker_ctx &ctx, int queue_id)
{
    // mmap UMEM
    ctx.umem_area = mmap(nullptr, UMEM_SIZE, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (ctx.umem_area == MAP_FAILED)
    {
        fprintf(stderr, "[xdp_tx] mmap UMEM failed: %s\n", strerror(errno));
        return -errno;
    }

    // Create UMEM
    struct xsk_umem_config umem_cfg = {
        .fill_size      = FQ_SIZE,
        .comp_size      = NUM_FRAMES,
        .frame_size     = FRAME_SIZE,
        .frame_headroom = 0,
        .flags          = 0,
    };

    int ret = xsk_umem__create(&ctx.umem, ctx.umem_area, UMEM_SIZE,
                                &ctx.fq, &ctx.cq, &umem_cfg);
    if (ret)
    {
        fprintf(stderr, "[xdp_tx] xsk_umem__create (q%d): %s\n", queue_id, strerror(-ret));
        munmap(ctx.umem_area, UMEM_SIZE);
        ctx.umem_area = nullptr;
        return ret;
    }

    // Create XDP socket — rx_size must be > 0 for API compatibility
    struct xsk_socket_config xsk_cfg{};
    xsk_cfg.rx_size      = FQ_SIZE;
    xsk_cfg.tx_size      = static_cast<__u32>(NUM_FRAMES);
    xsk_cfg.libbpf_flags = XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD;
    xsk_cfg.xdp_flags    = 0;  // program already attached
    xsk_cfg.bind_flags   = XDP_COPY | XDP_USE_NEED_WAKEUP;

    char iface_name[IFNAMSIZ] = {};
    if_indextoname(g_ifindex, iface_name);

    ret = xsk_socket__create(&ctx.xsk, iface_name, queue_id,
                              ctx.umem, &ctx.rx, &ctx.tx, &xsk_cfg);
    if (ret)
    {
        fprintf(stderr, "[xdp_tx] xsk_socket__create (q%d, COPY): %s\n",
                queue_id, strerror(-ret));
        // Try without XDP_COPY
        xsk_cfg.bind_flags = XDP_USE_NEED_WAKEUP;
        ret = xsk_socket__create(&ctx.xsk, iface_name, queue_id,
                                  ctx.umem, &ctx.rx, &ctx.tx, &xsk_cfg);
        if (ret)
        {
            fprintf(stderr, "[xdp_tx] xsk_socket__create (q%d, ZC): %s\n",
                    queue_id, strerror(-ret));
            xsk_umem__delete(ctx.umem);
            ctx.umem = nullptr;
            munmap(ctx.umem_area, UMEM_SIZE);
            ctx.umem_area = nullptr;
            return ret;
        }
    }

    // Initialize frame free list — first FQ_SIZE frames reserved for fill queue
    ctx.free_count = NUM_FRAMES - FQ_SIZE;
    for (int i = 0; i < ctx.free_count; i++)
        ctx.frame_free[i] = static_cast<uint32_t>((i + FQ_SIZE) * FRAME_SIZE);

    // Populate fill queue (kernel needs this even for TX-only)
    uint32_t fq_idx = 0;
    if (xsk_ring_prod__reserve(&ctx.fq, FQ_SIZE, &fq_idx) == FQ_SIZE)
    {
        for (int i = 0; i < FQ_SIZE; i++)
            *xsk_ring_prod__fill_addr(&ctx.fq, fq_idx + i) =
                static_cast<uint64_t>(i) * FRAME_SIZE;
        xsk_ring_prod__submit(&ctx.fq, FQ_SIZE);
    }

    return 0;
}

// ── Public API ─────────────────────────────────────────────────────────

int xdp_tx_init(const char *iface, int num_workers,
                uint32_t src_ip_net, uint16_t src_port_host)
{
    if (g_active) return 0; // already initialized

    printf("[xdp_tx] Initializing: iface=%s, workers=%d\n", iface, num_workers);

    // Read our MAC
    if (!read_own_mac(iface, g_src_mac))
    {
        fprintf(stderr, "[xdp_tx] Failed to read MAC for %s\n", iface);
        return -ENODEV;
    }
    printf("[xdp_tx] SRC MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
           g_src_mac[0], g_src_mac[1], g_src_mac[2],
           g_src_mac[3], g_src_mac[4], g_src_mac[5]);

    g_src_ip = src_ip_net;
    g_src_port = src_port_host;
    g_num_workers = num_workers;

    // Load and attach XDP_PASS program
    int ret = load_xdp_pass(iface);
    if (ret < 0) return ret;

    // Create per-worker contexts
    g_workers.resize(num_workers);
    for (int i = 0; i < num_workers; i++)
    {
        ret = setup_worker(g_workers[i], i);
        if (ret < 0)
        {
            fprintf(stderr, "[xdp_tx] Worker %d setup failed, cleaning up\n", i);
            xdp_tx_cleanup();
            return ret;
        }
        printf("[xdp_tx] Worker %d: %d frames available\n", i, g_workers[i].free_count);
    }

    g_active = true;

    // Register cleanup for SIGTERM/SIGINT and normal process exit
    install_signal_handlers();
    static bool atexit_registered = false;
    if (!atexit_registered) { std::atexit(xdp_atexit_cleanup); atexit_registered = true; }

    printf("[xdp_tx] Initialized successfully (%d workers)\n", num_workers);
    return 0;
}

bool xdp_tx_available()
{
    return g_active;
}

int xdp_tx_send_gso(int worker_idx,
                     uint32_t dst_ip_net, uint16_t dst_port_host,
                     const void *buffer, size_t total_size,
                     uint16_t segment_size)
{
    if (!g_active || worker_idx < 0 || worker_idx >= g_num_workers)
        return -EINVAL;
    if (!buffer || total_size == 0 || segment_size == 0)
        return -EINVAL;

    auto &ctx = g_workers[worker_idx];

    // Resolve destination MAC
    uint8_t dst_mac[6];
    if (!resolve_mac(dst_ip_net, dst_mac))
    {
        // Can't resolve — caller should fall back to sendmsg for this dest
        return -EHOSTUNREACH;
    }

    // Calculate number of segments (same logic as kernel GSO)
    int num_segments = static_cast<int>((total_size + segment_size - 1) / segment_size);
    auto *src = static_cast<const uint8_t *>(buffer);

    // Reserve TX ring slots in one batch
    uint32_t tx_idx = 0;
    unsigned int reserved = xsk_ring_prod__reserve(&ctx.tx, num_segments, &tx_idx);
    if (reserved == 0)
    {
        // TX ring full — try to flush and reclaim
        sendto(xsk_socket__fd(ctx.xsk), nullptr, 0, MSG_DONTWAIT, nullptr, 0);
        reclaim_frames(ctx);
        reserved = xsk_ring_prod__reserve(&ctx.tx, num_segments, &tx_idx);
        if (reserved == 0)
            return -ENOBUFS;
    }

    int total_enqueued = 0;

    for (unsigned int i = 0; i < reserved; i++)
    {
        // Determine segment bounds
        size_t seg_offset = static_cast<size_t>(i) * segment_size;
        size_t seg_len = (i < static_cast<unsigned>(num_segments - 1))
                             ? segment_size
                             : (total_size - seg_offset);

        // Allocate UMEM frame
        uint32_t addr = ctx.alloc_frame();
        if (addr == UINT32_MAX)
        {
            // Out of frames — submit what we have so far
            xsk_ring_prod__submit(&ctx.tx, i);
            return total_enqueued > 0 ? total_enqueued : -ENOMEM;
        }

        uint8_t *frame = static_cast<uint8_t *>(ctx.umem_area) + addr;
        int offset = 0;

        // ── Ethernet header (14 bytes) ──
        std::memcpy(frame + offset, dst_mac, 6);  offset += 6;
        std::memcpy(frame + offset, g_src_mac, 6); offset += 6;
        frame[offset++] = 0x08;
        frame[offset++] = 0x00; // EtherType = IPv4

        // ── IP header (20 bytes) ──
        uint16_t ip_total = static_cast<uint16_t>(IP_HLEN_V + UDP_HLEN_V + seg_len);
        auto *iph = reinterpret_cast<struct iphdr *>(frame + offset);
        std::memset(iph, 0, IP_HLEN_V);
        iph->ihl     = 5;
        iph->version = 4;
        iph->tot_len = htons(ip_total);
        iph->ttl     = 64;
        iph->protocol = IPPROTO_UDP;
        iph->saddr   = g_src_ip;
        iph->daddr   = dst_ip_net;
        iph->check   = ip_checksum(iph, IP_HLEN_V);
        offset += IP_HLEN_V;

        // ── UDP header (8 bytes) ──
        auto *udph = reinterpret_cast<struct udphdr *>(frame + offset);
        udph->source = htons(g_src_port);
        udph->dest   = htons(dst_port_host);
        udph->len    = htons(static_cast<uint16_t>(UDP_HLEN_V + seg_len));
        udph->check  = 0; // Valid for IPv4
        offset += UDP_HLEN_V;

        // ── Payload (Entanglement segment data) ──
        std::memcpy(frame + offset, src + seg_offset, seg_len);
        offset += static_cast<int>(seg_len);

        // Fill TX descriptor
        auto *desc = xsk_ring_prod__tx_desc(&ctx.tx, tx_idx + i);
        desc->addr = addr;
        desc->len  = static_cast<uint32_t>(offset);

        total_enqueued += static_cast<int>(seg_len);
    }

    xsk_ring_prod__submit(&ctx.tx, reserved);
    return total_enqueued;
}

void xdp_tx_flush(int worker_idx)
{
    if (!g_active || worker_idx < 0 || worker_idx >= g_num_workers)
        return;

    auto &ctx = g_workers[worker_idx];

    // Kick the TX ring
    if (xsk_ring_prod__needs_wakeup(&ctx.tx))
        sendto(xsk_socket__fd(ctx.xsk), nullptr, 0, MSG_DONTWAIT, nullptr, 0);

    // Reclaim completed frames
    reclaim_frames(ctx);
}

void xdp_tx_cleanup()
{
    for (auto &ctx : g_workers)
    {
        if (ctx.xsk) { xsk_socket__delete(ctx.xsk); ctx.xsk = nullptr; }
        if (ctx.umem) { xsk_umem__delete(ctx.umem); ctx.umem = nullptr; }
        if (ctx.umem_area) { munmap(ctx.umem_area, UMEM_SIZE); ctx.umem_area = nullptr; }
    }
    g_workers.clear();

    if (g_xdp_prog_fd >= 0 && g_ifindex > 0)
    {
        LIBBPF_OPTS(bpf_xdp_attach_opts, opts);
        bpf_xdp_detach(g_ifindex, g_xdp_flags, &opts);
        close(g_xdp_prog_fd);
        g_xdp_prog_fd = -1;
    }

    g_arp_cache.clear();
    g_active = false;
    g_num_workers = 0;
    g_signal_cleanup_done = 1; // prevent signal handler from double-detaching
    printf("[xdp_tx] Cleaned up\n");
}

} // namespace entanglement

#endif // __linux__
