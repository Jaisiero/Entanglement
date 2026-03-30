#pragma once

#include "constants.h"
#include "endpoint_key.h"
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>

namespace entanglement
{

    // -----------------------------------------------------------------------
    // Fragment header — prepended to payload when FLAG_FRAGMENT is set.
    //   [message_id : uint16_t][fragment_index : uint8_t][fragment_count : uint8_t]
    // Total: 4 bytes  (FRAGMENT_HEADER_SIZE)
    // -----------------------------------------------------------------------

#pragma pack(push, 1)
    struct fragment_header
    {
        uint32_t message_id;
        uint8_t fragment_index;
        uint8_t fragment_count;
    };
#pragma pack(pop)

    static_assert(sizeof(fragment_header) == FRAGMENT_HEADER_SIZE,
                  "fragment_header must be FRAGMENT_HEADER_SIZE bytes");

    // -----------------------------------------------------------------------
    // SENDER SIDE — pending_message tracks ACK progress for a fragmented send.
    //   Fixed array in udp_connection.  Zero allocation.
    // -----------------------------------------------------------------------

    struct pending_message
    {
        bool active = false;
        uint32_t message_id = 0;
        uint8_t fragment_count = 0;
        uint8_t acked_count = 0;
        uint8_t lost_count = 0; // fragments reported lost (to detect zombie entries)

        void reset()
        {
            active = false;
            message_id = 0;
            fragment_count = 0;
            acked_count = 0;
            lost_count = 0;
        }

        bool is_complete() const { return acked_count == fragment_count; }

        // A message is abandoned when all its fragments are either ACKed or lost
        bool is_abandoned() const { return (acked_count + lost_count) >= fragment_count; }
    };

    // -----------------------------------------------------------------------
    // RECEIVER SIDE — callback types for app-provided buffer management.
    //   The library never allocates.  The app decides where to store data.
    // -----------------------------------------------------------------------

    // Called when the first fragment of a new message arrives.
    // The app must return a pointer to a buffer of at least max_total_size bytes,
    // or nullptr to reject the message (all subsequent fragments are silently dropped).
    using on_allocate_message =
        std::function<uint8_t *(const endpoint_key &sender, uint32_t message_id, uint8_t channel_id,
                                uint8_t fragment_count, size_t max_total_size)>;

    // Called when all fragments of a message have arrived.
    // 'data' is the same pointer the app returned from on_allocate_message.
    // 'total_size' is the exact number of bytes written (last fragment may be smaller).
    using on_message_complete = std::function<void(const endpoint_key &sender, uint32_t message_id, uint8_t channel_id,
                                                   uint8_t *data, size_t total_size)>;

    // Called when all fragments of a sent message have been ACKed.
    // The sender can now release its source buffer.
    using on_message_acked = std::function<void(uint32_t message_id)>;

    // Reason an incomplete fragmented message was discarded.
    enum class message_fail_reason : uint8_t
    {
        expired, // Reassembly timeout elapsed before all fragments arrived
        evicted, // Slot was reclaimed to make room for a newer message
    };

    // Called when an incomplete fragmented message is discarded (timeout or eviction).
    // The app should release the buffer it provided via on_allocate_message.
    // received_count / fragment_count indicate how much data was collected.
    using on_message_failed =
        std::function<void(const endpoint_key &sender, uint32_t message_id, uint8_t channel_id, uint8_t *app_buffer,
                           message_fail_reason reason, uint8_t received_count, uint8_t fragment_count)>;

    // -----------------------------------------------------------------------
    // fragment_result — rich return from process_fragment.
    // -----------------------------------------------------------------------

    enum class fragment_result : uint8_t
    {
        accepted,   // Fragment stored, message not yet complete
        completed,  // This fragment completed the message
        duplicate,  // Fragment already received (no-op)
        slots_full, // No free reassembly slot (after eviction attempts)
        rejected,   // App rejected buffer allocation (on_allocate → nullptr)
        invalid,    // Bad fragment header or data
    };

    // -----------------------------------------------------------------------
    // RECEIVER SIDE — reassembly_entry tracks one incoming fragmented message.
    //   Fixed array, zero allocation.  The data lives in app-provided buffer.
    // -----------------------------------------------------------------------

    struct reassembly_entry
    {
        bool active = false;
        uint32_t message_id = 0;
        endpoint_key sender{};
        uint8_t channel_id = 0;
        uint8_t fragment_count = 0;
        uint8_t received_count = 0;
        size_t last_fragment_size = 0;                      // size of the last fragment (index == count-1)
        uint8_t *app_buffer = nullptr;                      // pointer returned by on_allocate_message
        uint32_t channel_sequence = 0;                      // ordering sequence (from fragment 0) for RELIABLE_ORDERED
        uint32_t received_bitmap[8]{};                      // 256 bits — covers up to 255 fragments
        std::chrono::steady_clock::time_point created_time; // for timeout-based eviction

        void reset()
        {
            active = false;
            message_id = 0;
            sender = {};
            channel_id = 0;
            fragment_count = 0;
            received_count = 0;
            last_fragment_size = 0;
            app_buffer = nullptr;
            channel_sequence = 0;
            std::memset(received_bitmap, 0, sizeof(received_bitmap));
            created_time = {};
        }

        bool has_fragment(uint8_t index) const { return (received_bitmap[index / 32] & (1u << (index % 32))) != 0; }

        void mark_fragment(uint8_t index) { received_bitmap[index / 32] |= (1u << (index % 32)); }

        bool is_complete() const { return received_count == fragment_count; }

        // Compute exact total size once all fragments are in
        size_t total_size() const
        {
            if (fragment_count == 0)
                return 0;
            return static_cast<size_t>(fragment_count - 1) * MAX_FRAGMENT_PAYLOAD + last_fragment_size;
        }
    };

    // -----------------------------------------------------------------------
    // fragment_reassembler — fixed-size pool, zero allocation.
    //   One per direction.  Called from poll() in client/server.
    // -----------------------------------------------------------------------

    class fragment_reassembler
    {
    public:
        void set_on_allocate(on_allocate_message cb) { m_on_allocate = std::move(cb); }
        void set_on_complete(on_message_complete cb) { m_on_complete = std::move(cb); }
        void set_on_failed(on_message_failed cb) { m_on_failed = std::move(cb); }

        // Process one incoming fragment.  Calls on_allocate / on_complete as needed.
        // Returns a fragment_result indicating what happened.
        // channel_sequence is captured for fragment 0 of RELIABLE_ORDERED messages.
        fragment_result process_fragment(const endpoint_key &sender, uint8_t channel_id, const fragment_header &fhdr,
                                         const uint8_t *frag_data, size_t frag_data_size,
                                         uint32_t channel_sequence = 0);

        // Channel sequence of the last message that completed reassembly.
        // Valid immediately after process_fragment returns fragment_result::completed.
        uint32_t last_completed_channel_sequence() const { return m_last_completed_channel_seq; }

        // Expire incomplete messages older than timeout_us microseconds.
        // Calls on_message_expired for each evicted entry.  Returns count evicted.
        int cleanup_stale(std::chrono::steady_clock::time_point now, int64_t timeout_us = REASSEMBLY_TIMEOUT_US);

        // Clear all active entries, calling on_expired for each (so app frees buffers).
        void clear();

        // Number of messages currently being reassembled
        size_t pending_count() const { return m_active_count; }

        // Capacity queries (for backpressure logic)
        static constexpr size_t capacity() { return MAX_INCOMING_FRAGMENTED_MESSAGES; }
        int usage_percent() const;

        // Reassembly timeout (used internally by eviction fallback)
        void set_reassembly_timeout(int64_t timeout_us) { m_reassembly_timeout_us = timeout_us; }
        int64_t reassembly_timeout() const { return m_reassembly_timeout_us; }

    private:
        reassembly_entry m_entries[MAX_INCOMING_FRAGMENTED_MESSAGES]{};
        size_t m_active_count = 0;
        on_allocate_message m_on_allocate;
        on_message_complete m_on_complete;
        on_message_failed m_on_failed;
        int64_t m_reassembly_timeout_us = REASSEMBLY_TIMEOUT_US;
        uint32_t m_last_completed_channel_seq = 0;

        // --- Recently-completed message tracking ---
        // Prevents spurious reassembly entries from retransmitted fragments
        // of already-completed messages (caused by ACK loss in the reverse
        // direction).  Without this, the reassembler slowly fills with
        // phantom entries that can evict legitimate in-progress messages.
        static constexpr size_t COMPLETED_TRACKING_SIZE = 512;
        uint32_t m_completed_ids[COMPLETED_TRACKING_SIZE]{};
        size_t m_completed_write_idx = 0;
        size_t m_completed_count = 0;

        void record_completed(uint32_t msg_id);
        bool was_recently_completed(uint32_t msg_id) const;

        reassembly_entry *find_entry(const endpoint_key &sender, uint32_t message_id);
        reassembly_entry *allocate_entry();
        reassembly_entry *try_evict_least_progress();
    };

} // namespace entanglement
