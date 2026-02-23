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
        uint16_t message_id;
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
        uint16_t message_id = 0;
        uint8_t fragment_count = 0;
        uint8_t acked_count = 0;

        void reset()
        {
            active = false;
            message_id = 0;
            fragment_count = 0;
            acked_count = 0;
        }

        bool is_complete() const { return acked_count == fragment_count; }
    };

    // -----------------------------------------------------------------------
    // RECEIVER SIDE — callback types for app-provided buffer management.
    //   The library never allocates.  The app decides where to store data.
    // -----------------------------------------------------------------------

    // Called when the first fragment of a new message arrives.
    // The app must return a pointer to a buffer of at least max_total_size bytes,
    // or nullptr to reject the message (all subsequent fragments are silently dropped).
    using on_allocate_message = std::function<uint8_t *(uint16_t message_id, uint8_t channel_id, uint8_t fragment_count,
                                                        size_t max_total_size)>;

    // Called when all fragments of a message have arrived.
    // 'data' is the same pointer the app returned from on_allocate_message.
    // 'total_size' is the exact number of bytes written (last fragment may be smaller).
    using on_message_complete =
        std::function<void(uint16_t message_id, uint8_t channel_id, uint8_t *data, size_t total_size)>;

    // Called when all fragments of a sent message have been ACKed.
    // The sender can now release its source buffer.
    using on_message_acked = std::function<void(uint16_t message_id)>;

    // Called when an incomplete fragmented message is expired (timeout).
    // The app should release the buffer it provided via on_allocate_message.
    using on_message_expired = std::function<void(uint16_t message_id, uint8_t channel_id, uint8_t *app_buffer)>;

    // -----------------------------------------------------------------------
    // RECEIVER SIDE — reassembly_entry tracks one incoming fragmented message.
    //   Fixed array, zero allocation.  The data lives in app-provided buffer.
    // -----------------------------------------------------------------------

    struct reassembly_entry
    {
        bool active = false;
        uint16_t message_id = 0;
        endpoint_key sender{};
        uint8_t channel_id = 0;
        uint8_t fragment_count = 0;
        uint8_t received_count = 0;
        size_t last_fragment_size = 0;                      // size of the last fragment (index == count-1)
        uint8_t *app_buffer = nullptr;                      // pointer returned by on_allocate_message
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
        void set_on_expired(on_message_expired cb) { m_on_expired = std::move(cb); }

        // Process one incoming fragment.  Calls on_allocate / on_complete as needed.
        // Returns true if a message was completed by this fragment.
        bool process_fragment(const endpoint_key &sender, uint8_t channel_id, const fragment_header &fhdr,
                              const uint8_t *frag_data, size_t frag_data_size);

        // Expire incomplete messages older than timeout_us microseconds.
        // Calls on_message_expired for each evicted entry.  Returns count evicted.
        int cleanup_stale(std::chrono::steady_clock::time_point now, int64_t timeout_us = REASSEMBLY_TIMEOUT_US);

        // Number of messages currently being reassembled
        size_t pending_count() const
        {
            size_t n = 0;
            for (const auto &e : m_entries)
                if (e.active)
                    ++n;
            return n;
        }

    private:
        reassembly_entry m_entries[MAX_INCOMING_FRAGMENTED_MESSAGES]{};
        on_allocate_message m_on_allocate;
        on_message_complete m_on_complete;
        on_message_expired m_on_expired;

        reassembly_entry *find_entry(const endpoint_key &sender, uint16_t message_id);
        reassembly_entry *allocate_entry();
    };

} // namespace entanglement
