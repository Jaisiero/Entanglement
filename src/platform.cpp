#include "platform.h"

namespace entanglement
{

    bool platform_init()
    {
#ifdef ENTANGLEMENT_PLATFORM_WINDOWS
        WSADATA wsa;
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
        return true;
#endif
    }

    void platform_shutdown()
    {
#ifdef ENTANGLEMENT_PLATFORM_WINDOWS
        WSACleanup();
#endif
    }

} // namespace entanglement
