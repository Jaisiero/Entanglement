#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <utility>

namespace entanglement
{

    // Lock-free Single-Producer Single-Consumer ring buffer.
    //
    // - Producer calls try_push() from exactly ONE thread.
    // - Consumer calls try_pop()  from exactly ONE thread (may differ from producer).
    // - No locks, no CAS loops, no allocations after construction.
    // - Cache-line padding between write and read positions to prevent false sharing.
    // - Capacity must be a power of 2 (enforced by static_assert).
    //
    // When full, try_push() returns false  — the caller must drop or retry.
    // When empty, try_pop()  returns false — the caller must wait or retry.

    template <typename T, size_t Capacity>
    class spsc_queue
    {
        static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0, "Capacity must be a positive power of 2");

    public:
        spsc_queue() : m_buffer(std::make_unique<T[]>(Capacity)) {}

        // Non-copyable, non-movable (contains atomics)
        spsc_queue(const spsc_queue &) = delete;
        spsc_queue &operator=(const spsc_queue &) = delete;

        // Producer: enqueue by move. Returns false if full.
        bool try_push(T &&item)
        {
            const size_t w = m_write.load(std::memory_order_relaxed);
            const size_t next = (w + 1) & (Capacity - 1);
            if (next == m_read.load(std::memory_order_acquire))
                return false; // full
            m_buffer[w] = std::move(item);
            m_write.store(next, std::memory_order_release);
            return true;
        }

        // Producer: enqueue by copy. Returns false if full.
        bool try_push(const T &item)
        {
            const size_t w = m_write.load(std::memory_order_relaxed);
            const size_t next = (w + 1) & (Capacity - 1);
            if (next == m_read.load(std::memory_order_acquire))
                return false;
            m_buffer[w] = item;
            m_write.store(next, std::memory_order_release);
            return true;
        }

        // Consumer: dequeue by move into 'item'. Returns false if empty.
        bool try_pop(T &item)
        {
            const size_t r = m_read.load(std::memory_order_relaxed);
            if (r == m_write.load(std::memory_order_acquire))
                return false; // empty
            item = std::move(m_buffer[r]);
            m_read.store((r + 1) & (Capacity - 1), std::memory_order_release);
            return true;
        }

        // Approximate size (racy but useful for diagnostics).
        size_t size_approx() const
        {
            const size_t w = m_write.load(std::memory_order_acquire);
            const size_t r = m_read.load(std::memory_order_acquire);
            return (w - r) & (Capacity - 1);
        }

        bool empty() const { return m_read.load(std::memory_order_acquire) == m_write.load(std::memory_order_acquire); }

        // --- Zero-copy direct-access API ---
        // Allows the producer to write directly into the ring buffer slot
        // and the consumer to read in-place, avoiding any copy of T.
        // Usage:
        //   Producer: T* slot = write_slot(); if (slot) { fill *slot; commit_write(); }
        //   Consumer: T* slot = read_slot();  if (slot) { use *slot;  commit_read();  }

        // Producer: get pointer to the next writable slot. Returns nullptr if full.
        T *write_slot()
        {
            const size_t w = m_write.load(std::memory_order_relaxed);
            const size_t next = (w + 1) & (Capacity - 1);
            if (next == m_read.load(std::memory_order_acquire))
                return nullptr; // full
            return &m_buffer[w];
        }

        // Producer: publish the slot filled by write_slot(). Must be called exactly once
        // after each successful write_slot() call.
        void commit_write()
        {
            const size_t w = m_write.load(std::memory_order_relaxed);
            m_write.store((w + 1) & (Capacity - 1), std::memory_order_release);
        }

        // Consumer: get pointer to the next readable slot. Returns nullptr if empty.
        T *read_slot()
        {
            const size_t r = m_read.load(std::memory_order_relaxed);
            if (r == m_write.load(std::memory_order_acquire))
                return nullptr; // empty
            return &m_buffer[r];
        }

        // Consumer: release the slot returned by read_slot(). Must be called exactly once
        // after each successful read_slot() call.
        void commit_read()
        {
            const size_t r = m_read.load(std::memory_order_relaxed);
            m_read.store((r + 1) & (Capacity - 1), std::memory_order_release);
        }

    private:
        // Cache-line padding to prevent false sharing between producer and consumer.
        alignas(64) std::atomic<size_t> m_write{0};
        alignas(64) std::atomic<size_t> m_read{0};

        // Heap-allocated ring buffer (avoids blowing the stack for large T).
        std::unique_ptr<T[]> m_buffer;
    };

} // namespace entanglement
