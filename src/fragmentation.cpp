#include "fragmentation.h"
#include <cstring>

namespace entanglement
{

    fragment_result fragment_reassembler::process_fragment(const endpoint_key &sender, uint8_t channel_id,
                                                           const fragment_header &fhdr, const uint8_t *frag_data,
                                                           size_t frag_data_size, uint32_t channel_sequence)
    {
        if (fhdr.fragment_count == 0 || fhdr.fragment_index >= fhdr.fragment_count)
            return fragment_result::invalid;
        if (frag_data_size == 0 || frag_data_size > MAX_FRAGMENT_PAYLOAD)
            return fragment_result::invalid;

        // Find or create entry
        reassembly_entry *entry = find_entry(sender, fhdr.message_id);

        if (entry)
        {
            // message_id wrap protection: if the existing entry has a different
            // fragment_count, the ID has been recycled — evict the stale entry.
            if (entry->fragment_count != fhdr.fragment_count)
            {
                if (m_on_failed)
                    m_on_failed(entry->sender, entry->message_id, entry->channel_id, entry->app_buffer,
                                message_fail_reason::expired, entry->received_count, entry->fragment_count);
                entry->reset();
                --m_active_count;
                entry = nullptr; // fall through to allocate new
            }
        }

        if (!entry)
        {
            // Reject fragments for messages that already completed reassembly.
            // These are spurious retransmissions caused by ACK loss in the
            // reverse direction: the sender retransmits a fragment it thinks
            // was lost, but the receiver already assembled and delivered the
            // full message.  Without this check, these ghost fragments
            // allocate reassembly slots that never complete, eventually
            // evicting legitimate in-progress messages.
            if (was_recently_completed(fhdr.message_id))
                return fragment_result::duplicate;

            entry = allocate_entry();

            // Layer 3a: try aggressive cleanup (half timeout) to free stale slots
            if (!entry)
            {
                cleanup_stale(std::chrono::steady_clock::now(), m_reassembly_timeout_us / 2);
                entry = allocate_entry();
            }

            // Layer 3b: evict entry with least progress
            if (!entry)
            {
                entry = try_evict_least_progress();
            }

            if (!entry)
                return fragment_result::slots_full; // all slots truly full

            // Ask the app for a buffer
            size_t max_size = static_cast<size_t>(fhdr.fragment_count) * MAX_FRAGMENT_PAYLOAD;
            uint8_t *buf = nullptr;
            if (m_on_allocate)
            {
                buf = m_on_allocate(sender, fhdr.message_id, channel_id, fhdr.fragment_count, max_size);
            }

            if (!buf)
            {
                // App rejected — leave slot unused
                return fragment_result::rejected;
            }

            entry->active = true;
            ++m_active_count;
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

        // Capture channel_sequence from whichever fragment carries it (fragment 0 for ordered channels)
        if (channel_sequence > 0)
            entry->channel_sequence = channel_sequence;

        // Consistency check (after wrap protection, counts always match here)
        if (entry->fragment_count != fhdr.fragment_count)
            return fragment_result::invalid;

        // Duplicate fragment
        if (entry->has_fragment(fhdr.fragment_index))
            return fragment_result::duplicate;

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
            return fragment_result::accepted;

        // All fragments received — notify app
        if (m_on_complete)
        {
            m_last_completed_channel_seq = entry->channel_sequence;
            m_on_complete(entry->sender, entry->message_id, entry->channel_id, entry->app_buffer, entry->total_size());
        }

        record_completed(entry->message_id);
        entry->reset();
        --m_active_count;
        return fragment_result::completed;
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
        if (m_active_count == 0)
            return 0;

        int evicted = 0;
        for (auto &e : m_entries)
        {
            if (!e.active)
                continue;
            auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - e.created_time).count();
            if (elapsed > timeout_us)
            {
                if (m_on_failed)
                    m_on_failed(e.sender, e.message_id, e.channel_id, e.app_buffer, message_fail_reason::expired,
                                e.received_count, e.fragment_count);
                e.reset();
                --m_active_count;
                ++evicted;
            }
        }
        return evicted;
    }

    void fragment_reassembler::clear()
    {
        for (auto &e : m_entries)
        {
            if (e.active)
            {
                if (m_on_failed)
                    m_on_failed(e.sender, e.message_id, e.channel_id, e.app_buffer, message_fail_reason::expired,
                                e.received_count, e.fragment_count);
                e.reset();
            }
        }
        m_active_count = 0;
    }

    int fragment_reassembler::usage_percent() const
    {
        if (MAX_INCOMING_FRAGMENTED_MESSAGES == 0)
            return 0;
        return static_cast<int>((m_active_count * 100) / MAX_INCOMING_FRAGMENTED_MESSAGES);
    }

    reassembly_entry *fragment_reassembler::try_evict_least_progress()
    {
        reassembly_entry *victim = nullptr;
        float worst_progress = 2.0f; // > 1.0 means no victim found yet

        for (auto &e : m_entries)
        {
            if (!e.active)
                continue;
            float progress = (e.fragment_count > 0)
                                 ? static_cast<float>(e.received_count) / static_cast<float>(e.fragment_count)
                                 : 0.0f;
            if (progress < worst_progress)
            {
                worst_progress = progress;
                victim = &e;
            }
        }

        if (victim)
        {
            // Notify via on_failed
            if (m_on_failed)
                m_on_failed(victim->sender, victim->message_id, victim->channel_id, victim->app_buffer,
                            message_fail_reason::evicted, victim->received_count, victim->fragment_count);
            victim->reset();
            --m_active_count;
        }

        return victim;
    }

    void fragment_reassembler::record_completed(uint32_t msg_id)
    {
        m_completed_ids[m_completed_write_idx] = msg_id;
        m_completed_write_idx = (m_completed_write_idx + 1) % COMPLETED_TRACKING_SIZE;
        if (m_completed_count < COMPLETED_TRACKING_SIZE)
            ++m_completed_count;
    }

    bool fragment_reassembler::was_recently_completed(uint32_t msg_id) const
    {
        size_t n = (m_completed_count < COMPLETED_TRACKING_SIZE) ? m_completed_count : COMPLETED_TRACKING_SIZE;
        for (size_t i = 0; i < n; ++i)
        {
            if (m_completed_ids[i] == msg_id)
                return true;
        }
        return false;
    }

} // namespace entanglement
