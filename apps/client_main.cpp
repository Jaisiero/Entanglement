#include <iostream>
#include <cstring>
#include "platform.h"
#include "packet_header.h"
#include "client.h"

int main() {
    using namespace entanglement;

    if (!platform_init()) {
        std::cerr << "Failed to initialize platform" << std::endl;
        return 1;
    }

    std::cout << "Entanglement Client v0.1.0" << std::endl;
    std::cout << "Header size: " << sizeof(packet_header) << " bytes" << std::endl;

    constexpr uint16_t PORT = 9876;

    client cli("127.0.0.1", PORT);
    cli.set_on_response([](const packet_header& hdr, const uint8_t* payload, size_t size) {
        std::string msg(reinterpret_cast<const char*>(payload), size);
        std::cout << "[client] Response seq=" << hdr.sequence
                  << " -> \"" << msg << "\"" << std::endl;
    });

    if (!cli.connect()) {
        platform_shutdown();
        return 1;
    }

    const char* message = "Hello Entanglement!";
    cli.send_payload(message, std::strlen(message), FLAG_RELIABLE);

    std::cout << "[client] Sent message, waiting for echo..." << std::endl;

    // Poll briefly for response
    for (int i = 0; i < 100; ++i) {
        if (cli.poll() > 0) break;
    }

    cli.disconnect();
    platform_shutdown();
    return 0;
}
