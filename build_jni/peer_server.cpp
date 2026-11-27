#include "l2a/engine.hpp"
#include <iostream>
#include <cstring>
#include <string>
#include <filesystem>

int main(int argc, char** argv) {
    uint16_t port = (argc > 1) ? static_cast<uint16_t>(std::stoi(argv[1])) : 53323;
    l2a::EngineConfig cfg{};
    cfg.storage_dir = "/tmp/l2a_peer_storage_" + std::to_string(port);
    cfg.receive_dir = "/tmp/l2a_peer_recv_" + std::to_string(port);
    cfg.device_name = "Linux-Host-Simulator";
    cfg.platform = "linux";
    cfg.listen_port = port;
    cfg.discovery_port = 0;

    std::filesystem::create_directories(cfg.storage_dir);
    std::filesystem::create_directories(cfg.receive_dir);

    l2a::Engine engine(cfg);
    if (!engine.start()) {
        std::cerr << "Failed to start engine on port " << port << ": errno=" << errno << " (" << strerror(errno) << ")" << std::endl;
        return 1;
    }
    std::cout << "PEER_LISTENING_ON_" << port << std::endl;
    std::cout.flush();

    std::string line;
    std::getline(std::cin, line);
    engine.stop();
    return 0;
}
