#pragma once

#include "constants.h"
#include <array>
#include <cstdint>
#include <cstring>

namespace entanglement
{

    // --- Channel delivery modes ---

    enum class channel_mode : uint8_t
    {
        UNRELIABLE,       // Fire and forget — no loss tracking
        RELIABLE,         // Detect-and-notify loss — app decides retransmission
        RELIABLE_ORDERED, // Reliable + ordered delivery within channel
    };

    // --- Channel configuration ---

    struct channel_config
    {
        uint8_t id = 0;
        channel_mode mode = channel_mode::UNRELIABLE;
        uint8_t priority = 0; // Higher = more important (0–255)
        char name[MAX_CHANNEL_NAME] = {};
    };

    // constexpr helper to build a channel_config with a string literal name
    constexpr channel_config make_channel_config(uint8_t id, channel_mode mode, uint8_t priority, const char *src)
    {
        channel_config cfg{};
        cfg.id = id;
        cfg.mode = mode;
        cfg.priority = priority;
        for (size_t i = 0; i < MAX_CHANNEL_NAME - 1 && src[i] != '\0'; ++i)
        {
            cfg.name[i] = src[i];
        }
        return cfg;
    }

    // --- Predefined base channels (one per delivery mode) ---

    namespace channels
    {
        constexpr channel_config CONTROL = make_channel_config(0, channel_mode::RELIABLE_ORDERED, 255, "control");
        constexpr channel_config UNRELIABLE = make_channel_config(1, channel_mode::UNRELIABLE, 64, "unreliable");
        constexpr channel_config RELIABLE = make_channel_config(2, channel_mode::RELIABLE, 128, "reliable");
        constexpr channel_config ORDERED = make_channel_config(3, channel_mode::RELIABLE_ORDERED, 128, "ordered");
    } // namespace channels

    // --- Channel manager ---
    //
    // Flat array of MAX_CHANNELS slots (matches uint8_t channel_id in the header).
    // O(1) lookup.  The application registers channels before connecting.

    class channel_manager
    {
    public:
        channel_manager() = default;

        // Register a channel. Returns error_code::ok or error_code::channel_in_use.
        error_code register_channel(const channel_config &config);

        // Open a new channel with the given mode and priority.
        // Picks the next available ID (starting from 'hint', default = 4).
        // Returns the assigned channel_id, or -1 if no slots are available.
        int open_channel(channel_mode mode, uint8_t priority = 128, const char *name = "", uint8_t hint = 4);

        // Remove a channel registration. Returns error_code::ok or error_code::channel_not_found.
        error_code unregister_channel(uint8_t id);

        // Get channel configuration. Returns nullptr if not registered.
        const channel_config *get_channel(uint8_t id) const;

        // Quick mode queries
        bool is_reliable(uint8_t id) const;
        bool is_ordered(uint8_t id) const;
        uint8_t priority(uint8_t id) const;

        // Is this channel ID registered?
        bool is_registered(uint8_t id) const { return m_registered[id]; }

        // Number of registered channels
        size_t channel_count() const { return m_count; }

        // Register the predefined gaming channel presets
        void register_defaults();

    private:
        std::array<channel_config, MAX_CHANNELS> m_channels{};
        std::array<bool, MAX_CHANNELS> m_registered{};
        size_t m_count = 0;
    };

} // namespace entanglement
