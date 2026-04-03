#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace entanglement
{

    // Lock-free bounded Multi-Producer Single-Consumer ring buffer.
    //
    // Based on Dmitry Vyukov's bounded MPMC queue, simplified for single consumer.
    // - Multiple producers call try_push() concurrently (CAS on write index).
    // - Exactly ONE consumer calls try_pop() (no CAS needed on read side).
    // - No locks, no allocations after construction.
    // - Per-cell sequence numbers coordinate producers and detect full/empty.
    // - Capacity must be a power of 2 (enforced by static_assert).
    //
    // When full, try_push() returns false — the caller must drop or retry.
    // When empty, try_pop()  returns false — the caller must wait or retry.

    template <typename T, size_t Capacity>
    class mpsc_queue
    {
        static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0, "Capacity must be a positive power of 2");

        struct cell_t
        {
            std::atomic<size_t> sequence;
            T data;
        };

    public:
        mpsc_queue() : m_cells(std::make_unique<cell_t[]>(Capacity))
        {
            for (size_t i = 0; i < Capacity; ++i)
                m_cells[i].sequence.store(i, std::memory_order_relaxed);
        }

        // Non-copyable, non-movable
        mpsc_queue(const mpsc_queue &) = delete;
        mpsc_queue &operator=(const mpsc_queue &) = delete;

        // Producer (thread-safe): enqueue by move. Returns false if full.
        bool try_push(T &&item)
        {
            size_t pos = m_write.load(std::memory_order_relaxed);
            for (;;)
            {
                cell_t &cell = m_cells[pos & (Capacity - 1)];
                size_t seq = cell.sequence.load(std::memory_order_acquire);
                intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
                if (diff == 0)
                {
                    // Slot available — try to claim it
                    if (m_write.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                    {
                        cell.data = std::move(item);
                        cell.sequence.store(pos + 1, std::memory_order_release);
                        return true;
                    }
                    // CAS failed — another producer claimed it, retry with updated pos
                }
                else if (diff < 0)
                {
                    // Queue full
                    return false;
                }
                else
                {
                    // Slot was claimed by another producer but not yet published.
                    // Reload write position and retry.
                    pos = m_write.load(std::memory_order_relaxed);
                }
            }
        }

        // Producer (thread-safe): enqueue by copy. Returns false if full.
        bool try_push(const T &item)
        {
            size_t pos = m_write.load(std::memory_order_relaxed);
            for (;;)
            {
                cell_t &cell = m_cells[pos & (Capacity - 1)];
                size_t seq = cell.sequence.load(std::memory_order_acquire);
                intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
                if (diff == 0)
                {
                    if (m_write.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                    {
                        cell.data = item;
                        cell.sequence.store(pos + 1, std::memory_order_release);
                        return true;
                    }
                }
                else if (diff < 0)
                {
                    return false;
                }
                else
                {
                    pos = m_write.load(std::memory_order_relaxed);
                }
            }
        }

        // Consumer (single-threaded): dequeue by move into 'item'. Returns false if empty.
        bool try_pop(T &item)
        {
            cell_t &cell = m_cells[m_read & (Capacity - 1)];
            size_t seq = cell.sequence.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(m_read + 1);
            if (diff < 0)
                return false; // empty or not yet published
            item = std::move(cell.data);
            cell.sequence.store(m_read + Capacity, std::memory_order_release);
            ++m_read;
            return true;
        }

        // Approximate size (racy but useful for diagnostics).
        size_t size_approx() const
        {
            const size_t w = m_write.load(std::memory_order_relaxed);
            const size_t r = m_read; // only consumer reads m_read, no atomic needed for approx
            return w - r; // unsigned wrap is fine for approximate count
        }

        bool empty() const
        {
            const cell_t &cell = m_cells[m_read & (Capacity - 1)];
            size_t seq = cell.sequence.load(std::memory_order_acquire);
            return static_cast<intptr_t>(seq) - static_cast<intptr_t>(m_read + 1) < 0;
        }

    private:
        // Cache-line padding between write (contended by producers) and read (single consumer).
        alignas(64) std::atomic<size_t> m_write{0};
        alignas(64) size_t m_read{0}; // Only accessed by single consumer — no atomic needed.

        // Heap-allocated cell array (avoids blowing the stack for large T).
        std::unique_ptr<cell_t[]> m_cells;
    };

} // namespace entanglement
