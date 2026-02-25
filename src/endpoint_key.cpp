#include "endpoint_key.h"
#include "platform.h"

namespace entanglement
{

    endpoint_key endpoint_from_string(const std::string &ip, uint16_t port)
    {
        endpoint_key key{};
        inet_pton(AF_INET, ip.c_str(), &key.address);
        key.port = port;
        return key;
    }

    std::string endpoint_address_string(const endpoint_key &key)
    {
        char buf[16]; // enough for "xxx.xxx.xxx.xxx\0"
        inet_ntop(AF_INET, &key.address, buf, sizeof(buf));
        return buf;
    }

} // namespace entanglement
