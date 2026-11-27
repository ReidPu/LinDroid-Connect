#include "l2a/discovery.hpp"
#include "l2a/protocol.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace l2a {

DiscoveryManager::DiscoveryManager(
    std::string device_id,
    std::string device_name,
    std::string platform,
    uint16_t tcp_port,
    Sha256Digest pubkey_fingerprint,
    uint16_t discovery_port)
    : device_id_(std::move(device_id)),
      device_name_(std::move(device_name)),
      platform_(std::move(platform)),
      tcp_port_(tcp_port),
      pubkey_fingerprint_(pubkey_fingerprint),
      discovery_port_(discovery_port) {}

DiscoveryManager::~DiscoveryManager() {
    stop();
}

bool DiscoveryManager::start(PeerDiscoveredCallback on_peer_discovered) {
    if (running_.load()) {
        return true;
    }
    callback_ = std::move(on_peer_discovered);

    udp_sock_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_sock_ < 0) {
        return false;
    }

    int yes = 1;
    ::setsockopt(udp_sock_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#ifdef SO_REUSEPORT
    ::setsockopt(udp_sock_, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
#endif
    ::setsockopt(udp_sock_, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));

    struct timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 250000;  // 250ms recv timeout for clean shutdown
    ::setsockopt(udp_sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(discovery_port_);

    if (::bind(udp_sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(udp_sock_);
        udp_sock_ = -1;
        return false;
    }

    running_.store(true);
    recv_thread_ = std::thread(&DiscoveryManager::recv_loop, this);
    beacon_thread_ = std::thread(&DiscoveryManager::beacon_loop, this);
    return true;
}

void DiscoveryManager::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (udp_sock_ >= 0) {
        ::shutdown(udp_sock_, SHUT_RDWR);
        ::close(udp_sock_);
        udp_sock_ = -1;
    }
    if (recv_thread_.joinable()) {
        recv_thread_.join();
    }
    if (beacon_thread_.joinable()) {
        beacon_thread_.join();
    }
}

void DiscoveryManager::broadcast_now(const std::string& target_ip) {
    if (udp_sock_ < 0) {
        return;
    }

    HelloHandshakePayload hello{};
    hello.device_id = device_id_;
    hello.device_name = device_name_;
    hello.platform = platform_;
    hello.protocol_version = kProtocolVersion;
    hello.pubkey_fingerprint = pubkey_fingerprint_;

    Frame frame{};
    frame.header.type = FrameType::kHelloHandshake;
    // Store tcp_port in stream_id of the UDP discovery frame
    frame.header.stream_id = tcp_port_;
    frame.payload = hello.encode();
    auto raw = frame.serialize();

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(discovery_port_);
    if (::inet_pton(AF_INET, target_ip.c_str(), &dest.sin_addr) == 1) {
        ::sendto(udp_sock_, raw.data(), raw.size(), 0,
                 reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
    }
}

void DiscoveryManager::beacon_loop() {
    while (running_.load()) {
        broadcast_now("255.255.255.255");
        broadcast_now("127.0.0.1");
        for (int i = 0; i < 30 && running_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

void DiscoveryManager::recv_loop() {
    std::array<uint8_t, 2048> buf{};
    while (running_.load()) {
        sockaddr_in src{};
        socklen_t src_len = sizeof(src);
        ssize_t n = ::recvfrom(udp_sock_, buf.data(), buf.size(), 0,
                               reinterpret_cast<sockaddr*>(&src), &src_len);
        if (n <= 0) {
            continue;
        }
        auto frame = Frame::deserialize(std::span<const uint8_t>(buf.data(), static_cast<size_t>(n)));
        if (!frame || frame->header.type != FrameType::kHelloHandshake) {
            continue;
        }
        auto hello = HelloHandshakePayload::decode(frame->payload);
        if (!hello || hello->device_id == device_id_) {
            continue;
        }

        char ip_str[INET_ADDRSTRLEN]{};
        ::inet_ntop(AF_INET, &src.sin_addr, ip_str, sizeof(ip_str));

        DiscoveredPeer peer{};
        peer.device_id = hello->device_id;
        peer.device_name = hello->device_name;
        peer.platform = hello->platform;
        peer.ip_address = ip_str;
        peer.tcp_port = static_cast<uint16_t>(frame->header.stream_id & 0xFFFFU);
        peer.pubkey_fingerprint = hello->pubkey_fingerprint;

        if (callback_) {
            callback_(peer);
        }
    }
}

}  // namespace l2a
