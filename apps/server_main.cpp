#include "packet_header.h"
#include "platform.h"
#include "server.h"
#include <iostream>

int main()
{
    using namespace entanglement;

    if (!platform_init())
    {
        std::cerr << "Failed to initialize platform" << std::endl;
        return 1;
    }

    std::cout << "Entanglement Server v0.1.0" << std::endl;
    std::cout << "Header size: " << sizeof(packet_header) << " bytes" << std::endl;

    constexpr uint16_t PORT = DEFAULT_PORT;

    server srv(PORT);

    int packet_count = 0;
    srv.set_on_packet_received(
        [&](const packet_header &hdr, const uint8_t *payload, size_t size, const std::string &addr, uint16_t port)
        {
            ++packet_count;

            // Verbose for first 3, then every 100, then on notable sequences
            if (packet_count <= 3 || (packet_count % 100) == 0 || hdr.sequence == SEQUENCE_BUFFER_SIZE ||
                hdr.sequence == SEQUENCE_BUFFER_SIZE + 1)
            {
                std::string msg(reinterpret_cast<const char *>(payload), size);
                std::cout << "[server] #" << packet_count << " from " << addr << ":" << port << " seq=" << hdr.sequence
                          << " ack=" << hdr.ack << " -> \"" << msg << "\"" << std::endl;
            }

            // Echo back
            packet_header reply{};
            reply.flags = hdr.flags;
            reply.shard_id = hdr.shard_id;
            reply.channel_id = hdr.channel_id;
            reply.payload_size = static_cast<uint16_t>(size);
            srv.send_to(reply, payload, addr, port);
        });

    if (!srv.start())
    {
        platform_shutdown();
        return 1;
    }

    std::cout << "Press Enter to stop..." << std::endl;
    while (srv.is_running())
    {
        srv.poll();
    }

    std::cout << "[server] Total packets processed: " << packet_count << std::endl;
    platform_shutdown();
    return 0;
}
