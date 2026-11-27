#pragma once

#include "l2a/sha256.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace l2a {

constexpr uint16_t kDefaultDiscoveryPort = 53320;
constexpr uint16_t kDefaultTransferPort = 53321;

struct DiscoveredPeer {
    std::string device_id;
    std::string device_name;
    std::string platform;
    std::string ip_address;
    uint16_t tcp_port{kDefaultTransferPort};
    Sha256Digest pubkey_fingerprint{};
};

using PeerDiscoveredCallback = std::function<void(const DiscoveredPeer& peer)>;

class DiscoveryManager {
public:
    DiscoveryManager(
        std::string device_id,
        std::string device_name,
        std::string platform,
        uint16_t tcp_port,
        Sha256Digest pubkey_fingerprint,
        uint16_t discovery_port = kDefaultDiscoveryPort);

    ~DiscoveryManager();

    DiscoveryManager(const DiscoveryManager&) = delete;
    DiscoveryManager& operator=(const DiscoveryManager&) = delete;

    bool start(PeerDiscoveredCallback on_peer_discovered);
    void stop();

    // Broadcasts a beacon immediately (e.g., on startup or NFC tap)
    void broadcast_now(const std::string& target_ip = "255.255.255.255");

private:
    void recv_loop();
    void beacon_loop();

    std::string device_id_;
    std::string device_name_;
    std::string platform_;
    uint16_t tcp_port_;
    Sha256Digest pubkey_fingerprint_;
    uint16_t discovery_port_;

    int udp_sock_{-1};
    std::atomic<bool> running_{false};
    PeerDiscoveredCallback callback_;
    std::thread recv_thread_;
    std::thread beacon_thread_;
};

}  // namespace l2a
