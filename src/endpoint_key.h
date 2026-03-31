#pragma once

#include <cstdint>
#include <functional>
#include <string>

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
            // splitmix64 finalizer — excellent distribution for (IP, port) pairs.
            // Avoids the collision problem of identity-hash (MSVC) when many
            // clients share the same port but differ in IP address.
            uint64_t h = (static_cast<uint64_t>(k.address) << 16) ^ k.port;
            h ^= h >> 33;
            h *= 0xff51afd7ed558ccdULL;
            h ^= h >> 33;
            h *= 0xc4ceb9fe1a85ec53ULL;
            h ^= h >> 33;
            return static_cast<size_t>(h);
        }
    };

    // Build endpoint_key from dotted-decimal string + port (calls inet_pton once)
    endpoint_key endpoint_from_string(const std::string &ip, uint16_t port);

    // Convert endpoint_key address to dotted-decimal string (calls inet_ntop)
    std::string endpoint_address_string(const endpoint_key &key);

} // namespace entanglement
