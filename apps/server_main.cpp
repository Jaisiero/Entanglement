#include <iostream>
#include "platform.h"
#include "packet_header.h"
#include "server.h"

int main() {
    using namespace entanglement;

    if (!platform_init()) {
        std::cerr << "Failed to initialize platform" << std::endl;
        return 1;
    }

    std::cout << "Entanglement Server v0.1.0" << std::endl;
    std::cout << "Header size: " << sizeof(packet_header) << " bytes" << std::endl;

    constexpr uint16_t PORT = DEFAULT_PORT;

    server srv(PORT);
    srv.set_on_packet_received([&](const packet_header& hdr, const uint8_t* payload,
                                    size_t size, const std::string& addr, uint16_t port) {
        std::string msg(reinterpret_cast<const char*>(payload), size);
        std::cout << "[server] From " << addr << ":" << port
                  << " seq=" << hdr.sequence << " -> \"" << msg << "\"" << std::endl;

        // Echo back
        srv.send_to(hdr, payload, addr, port);
    });

    if (!srv.start()) {
        platform_shutdown();
        return 1;
    }

    std::cout << "Press Enter to stop..." << std::endl;
    while (srv.is_running()) {
        srv.poll();
    }

    platform_shutdown();
    return 0;
}
