#pragma once

#include "l2a/protocol.hpp"
#include "l2a/sha256.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace l2a {

using Key256 = std::array<uint8_t, 32>;
using Nonce96 = std::array<uint8_t, 12>;
using Tag128 = std::array<uint8_t, 16>;

// RFC 7748 X25519 Diffie-Hellman
Key256 x25519_basepoint(const Key256& scalar) noexcept;
Key256 x25519_shared(const Key256& scalar, const Key256& peer_public) noexcept;

// RFC 8439 ChaCha20-Poly1305 AEAD
std::vector<uint8_t> chacha20_poly1305_encrypt(
    const Key256& key,
    const Nonce96& nonce,
    std::span<const uint8_t> aad,
    std::span<const uint8_t> plaintext);

std::optional<std::vector<uint8_t>> chacha20_poly1305_decrypt(
    const Key256& key,
    const Nonce96& nonce,
    std::span<const uint8_t> aad,
    std::span<const uint8_t> ciphertext_and_tag);

struct DeviceIdentity {
    std::string device_id;
    std::string device_name;
    Key256 private_key{};
    Key256 public_key{};
    Sha256Digest fingerprint{};  // SHA-256(public_key)

    static DeviceIdentity load_or_create(
        const std::filesystem::path& storage_dir,
        std::string_view default_device_name);
};

struct TrustedPeer {
    std::string device_id;
    std::string device_name;
    Key256 public_key{};
    Sha256Digest fingerprint{};
};

class TrustedPeerStore {
public:
    explicit TrustedPeerStore(std::filesystem::path store_file);

    void load();
    bool save() const;

    std::optional<TrustedPeer> find(std::string_view device_id) const;
    void upsert(const TrustedPeer& peer);
    std::vector<TrustedPeer> list_all() const;

private:
    std::filesystem::path store_file_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, TrustedPeer> peers_;
};

// Callback invoked when an unknown peer connects for the first time (TOFU).
// Return true to trust and pin the peer's public key fingerprint.
using TofuVerifyCallback = std::function<bool(
    const std::string& peer_device_id,
    const std::string& peer_device_name,
    const Sha256Digest& peer_fingerprint)>;

class TlsChannel {
public:
    TlsChannel(int socket_fd, bool is_client);
    ~TlsChannel();

    TlsChannel(const TlsChannel&) = delete;
    TlsChannel& operator=(const TlsChannel&) = delete;

    // Performs 3DH X25519 + HKDF-SHA256 + TOFU verification handshake.
    bool perform_handshake(
        const DeviceIdentity& local_id,
        TrustedPeerStore& peer_store,
        const TofuVerifyCallback& tofu_cb);

    // Thread-safe encrypted frame send
    bool send_frame(const Frame& frame);

    // Receives and authenticates the next encrypted frame
    std::optional<Frame> recv_frame();

    void close();
    int socket_fd() const noexcept { return sock_fd_.load(); }
    bool is_client() const noexcept { return is_client_; }
    const std::string& peer_device_id() const noexcept { return peer_device_id_; }
    const std::string& peer_device_name() const noexcept { return peer_device_name_; }
    const Sha256Digest& peer_fingerprint() const noexcept { return peer_fingerprint_; }

private:
    bool write_all(std::span<const uint8_t> data);
    bool read_exact(std::span<uint8_t> buf);
    Nonce96 make_nonce(const Nonce96& base_iv, uint64_t seq) const noexcept;

    std::atomic<int> sock_fd_{-1};
    bool is_client_{false};
    bool handshake_done_{false};

    Key256 send_key_{};
    Key256 recv_key_{};
    Nonce96 send_iv_{};
    Nonce96 recv_iv_{};
    uint64_t send_seq_{0};
    uint64_t recv_seq_{0};

    std::string peer_device_id_;
    std::string peer_device_name_;
    Sha256Digest peer_fingerprint_{};

    std::mutex send_mutex_;
    std::mutex recv_mutex_;
};

}  // namespace l2a
