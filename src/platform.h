#pragma once

// Platform detection and socket includes

#if defined(_WIN32) || defined(_WIN64)
#define ENTANGLEMENT_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <WinSock2.h>
#include <WS2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")

using socket_t = SOCKET;
using socklen_t_ = int;
constexpr socket_t INVALID_SOCK = INVALID_SOCKET;

inline int last_socket_error()
{
    return WSAGetLastError();
}
inline void close_socket(socket_t s)
{
    closesocket(s);
}

#elif defined(__linux__) || defined(__APPLE__)
#define ENTANGLEMENT_PLATFORM_POSIX
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using socket_t = int;
using socklen_t_ = socklen_t;
constexpr socket_t INVALID_SOCK = -1;

inline int last_socket_error()
{
    return errno;
}
inline void close_socket(socket_t s)
{
    close(s);
}

#else
#error "Unsupported platform"
#endif

#include <cstdint>
#include <cstring>

namespace entanglement
{

    // Initialize platform networking (call once at startup)
    bool platform_init();

    // Cleanup platform networking (call once at shutdown)
    void platform_shutdown();

} // namespace entanglement
