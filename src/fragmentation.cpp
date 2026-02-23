#include "fragmentation.h"
#include <cstring>

namespace entanglement
{

    bool fragment_reassembler::process_fragment(const endpoint_key &sender, uint8_t channel_id,
                                                const fragment_header &fhdr, const uint8_t *frag_data,
                                                size_t frag_data_size)
    {
        if (fhdr.fragment_count == 0 || fhdr.fragment_index >= fhdr.fragment_count)
            return false;
        if (frag_data_size == 0 || frag_data_size > MAX_FRAGMENT_PAYLOAD)
            return false;

        // Find or create entry
        reassembly_entry *entry = find_entry(sender, fhdr.message_id);

        if (entry)
        {
            // message_id wrap protection: if the existing entry has a different
            // fragment_count, the ID has been recycled — evict the stale entry.
            if (entry->fragment_count != fhdr.fragment_count)
            {
                if (m_on_expired)
                    m_on_expired(entry->message_id, entry->channel_id, entry->app_buffer);
                entry->reset();
                entry = nullptr; // fall through to allocate new
            }
        }

        if (!entry)
        {
            entry = allocate_entry();
            if (!entry)
                return false; // all slots full

            // Ask the app for a buffer
            size_t max_size = static_cast<size_t>(fhdr.fragment_count) * MAX_FRAGMENT_PAYLOAD;
            uint8_t *buf = nullptr;
            if (m_on_allocate)
            {
                buf = m_on_allocate(fhdr.message_id, channel_id, fhdr.fragment_count, max_size);
            }

            if (!buf)
            {
                // App rejected — leave slot unused
                return false;
            }

            entry->active = true;
            entry->message_id = fhdr.message_id;
            entry->sender = sender;
            entry->channel_id = channel_id;
            entry->fragment_count = fhdr.fragment_count;
            entry->received_count = 0;
            entry->last_fragment_size = 0;
            entry->app_buffer = buf;
            entry->created_time = std::chrono::steady_clock::now();
            std::memset(entry->received_bitmap, 0, sizeof(entry->received_bitmap));
        }

        // Consistency check (after wrap protection, counts always match here)
        if (entry->fragment_count != fhdr.fragment_count)
            return false;

        // Duplicate fragment
        if (entry->has_fragment(fhdr.fragment_index))
            return false;

        // Write fragment data directly into app-provided buffer (single memcpy — the only one)
        size_t offset = static_cast<size_t>(fhdr.fragment_index) * MAX_FRAGMENT_PAYLOAD;
        std::memcpy(entry->app_buffer + offset, frag_data, frag_data_size);
        entry->mark_fragment(fhdr.fragment_index);
        entry->received_count++;

        // Track last fragment size for exact total computation
        if (fhdr.fragment_index == fhdr.fragment_count - 1)
        {
            entry->last_fragment_size = frag_data_size;
        }

        // Check completion
        if (!entry->is_complete())
            return false;

        // All fragments received — notify app
        if (m_on_complete)
        {
            m_on_complete(entry->message_id, entry->channel_id, entry->app_buffer, entry->total_size());
        }

        entry->reset();
        return true;
    }

    reassembly_entry *fragment_reassembler::find_entry(const endpoint_key &sender, uint32_t message_id)
    {
        for (auto &e : m_entries)
        {
            if (e.active && e.message_id == message_id && e.sender == sender)
                return &e;
        }
        return nullptr;
    }

    reassembly_entry *fragment_reassembler::allocate_entry()
    {
        for (auto &e : m_entries)
        {
            if (!e.active)
                return &e;
        }
        return nullptr; // all slots full — caller should drop
    }

    int fragment_reassembler::cleanup_stale(std::chrono::steady_clock::time_point now, int64_t timeout_us)
    {
        int evicted = 0;
        for (auto &e : m_entries)
        {
            if (!e.active)
                continue;
            auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - e.created_time).count();
            if (elapsed > timeout_us)
            {
                if (m_on_expired)
                    m_on_expired(e.message_id, e.channel_id, e.app_buffer);
                e.reset();
                ++evicted;
            }
        }
        return evicted;
    }

} // namespace entanglement
