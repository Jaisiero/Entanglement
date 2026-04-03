#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <new>

namespace entanglement
{

    // Double-buffered lock-free linear allocator for cross-thread send data.
    //
    // Rayon (producer) threads write packet payloads here with a single
    // atomic fetch_add, then enqueue a tiny send_command carrying only the
    // encoded offset.  Worker (consumer) threads read the payload by offset.
    //
    // Two pools alternate each game tick so stale reads from the previous
    // tick never collide with new writes.
    class send_pool
    {
    public:
        static constexpr size_t POOL_CAPACITY = 16 * 1024 * 1024; // 16 MB per pool

        send_pool()
        {
            for (auto &p : m_pools)
            {
                p.data = new (std::nothrow) uint8_t[POOL_CAPACITY];
                p.head.store(0, std::memory_order_relaxed);
            }
        }

        ~send_pool()
        {
            for (auto &p : m_pools)
                delete[] p.data;
        }

        send_pool(const send_pool &) = delete;
        send_pool &operator=(const send_pool &) = delete;

        // Thread-safe: write payload into the active pool.
        // Returns an encoded offset (bit 31 = pool index, bits 0-30 = byte offset).
        // Returns UINT32_MAX on overflow (should never happen with correct sizing).
        uint32_t write(const void *src, uint16_t size)
        {
            const uint32_t idx = m_active.load(std::memory_order_relaxed);
            const uint32_t off = m_pools[idx].head.fetch_add(size, std::memory_order_relaxed);
            if (off + size > POOL_CAPACITY)
                return UINT32_MAX; // overflow guard
            std::memcpy(m_pools[idx].data + off, src, size);
            return (idx << 31) | off;
        }

        // Read payload by encoded offset (any thread, no synchronisation needed).
        const uint8_t *read(uint32_t encoded) const
        {
            const uint32_t idx = encoded >> 31;
            const uint32_t off = encoded & 0x7FFF'FFFFu;
            return m_pools[idx].data + off;
        }

        // Call once per tick from the game thread BEFORE any cross-thread sends.
        // Resets the next pool and makes it active.
        void advance()
        {
            const uint32_t next = 1u - m_active.load(std::memory_order_relaxed);
            m_pools[next].head.store(0, std::memory_order_relaxed);
            m_active.store(next, std::memory_order_release);
        }

    private:
        struct alignas(64) pool_buf
        {
            uint8_t *data = nullptr;
            std::atomic<uint32_t> head{0};
        };

        pool_buf m_pools[2];
        alignas(64) std::atomic<uint32_t> m_active{0};
    };

} // namespace entanglement
