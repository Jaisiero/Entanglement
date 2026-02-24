#include "channel_manager.h"

namespace entanglement
{

    error_code channel_manager::register_channel(const channel_config &config)
    {
        if (m_registered[config.id])
            return error_code::channel_in_use;

        m_channels[config.id] = config;
        m_registered[config.id] = true;
        ++m_count;
        return error_code::ok;
    }

    int channel_manager::open_channel(channel_mode mode, uint8_t priority, const char *name, uint8_t hint)
    {
        // Scan from hint upward, wrapping around, to find a free slot
        for (size_t i = 0; i < MAX_CHANNELS; ++i)
        {
            uint8_t id = static_cast<uint8_t>((static_cast<size_t>(hint) + i) % MAX_CHANNELS);
            if (!m_registered[id])
            {
                channel_config cfg{};
                cfg.id = id;
                cfg.mode = mode;
                cfg.priority = priority;
                if (name)
                {
                    std::strncpy(cfg.name, name, MAX_CHANNEL_NAME - 1);
                    cfg.name[MAX_CHANNEL_NAME - 1] = '\0';
                }
                register_channel(cfg);
                return static_cast<int>(id);
            }
        }
        return static_cast<int>(error_code::channel_slots_full); // All 256 slots occupied
    }

    error_code channel_manager::unregister_channel(uint8_t id)
    {
        if (!m_registered[id])
            return error_code::channel_not_found;

        m_channels[id] = {};
        m_registered[id] = false;
        --m_count;
        return error_code::ok;
    }

    const channel_config *channel_manager::get_channel(uint8_t id) const
    {
        if (!m_registered[id])
            return nullptr;
        return &m_channels[id];
    }

    bool channel_manager::is_reliable(uint8_t id) const
    {
        if (!m_registered[id])
            return false;
        return m_channels[id].mode == channel_mode::RELIABLE || m_channels[id].mode == channel_mode::RELIABLE_ORDERED;
    }

    bool channel_manager::is_ordered(uint8_t id) const
    {
        if (!m_registered[id])
            return false;
        return m_channels[id].mode == channel_mode::RELIABLE_ORDERED;
    }

    uint8_t channel_manager::priority(uint8_t id) const
    {
        if (!m_registered[id])
            return 0;
        return m_channels[id].priority;
    }

    void channel_manager::register_defaults()
    {
        register_channel(channels::CONTROL);
        register_channel(channels::UNRELIABLE);
        register_channel(channels::RELIABLE);
        register_channel(channels::ORDERED);
    }

} // namespace entanglement
