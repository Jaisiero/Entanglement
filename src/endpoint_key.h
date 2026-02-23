#pragma once

#include <cstdint>
#include <functional>

namespace entanglement
{

    // --- Endpoint identifier (IP + port) ---

    struct endpoint_key
    {
        uint32_t address = 0; // IPv4 in network byte order
        uint16_t port = 0;

        bool operator==(const endpoint_key &other) const { return address == other.address && port == other.port; }
    };

    struct endpoint_key_hash
    {
        size_t operator()(const endpoint_key &k) const
        {
            return std::hash<uint64_t>{}((static_cast<uint64_t>(k.address) << 16) | k.port);
        }
    };

} // namespace entanglement
