#pragma once

#include "l2a/clipboard_stream.hpp"
#include "l2a/discovery.hpp"
#include "l2a/file_transfer.hpp"
#include "l2a/history_manager.hpp"
#include "l2a/protocol.hpp"
#include "l2a/tls_channel.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace l2a {

struct PeerEndpoint {
    std::string ip;
    uint16_t port{0};
};

struct EngineConfig {
    std::filesystem::path storage_dir;
    std::filesystem::path receive_dir;
    std::string device_name{"L2A-Device"};
    std::string platform{"linux"};
    uint16_t listen_port{kDefaultTransferPort};
    uint16_t discovery_port{kDefaultDiscoveryPort};  // Set to 0 to disable UDP discovery
};

class ICoreListener {
public:
    virtual ~ICoreListener() = default;

    virtual void on_peer_discovered(const DiscoveredPeer& /*peer*/) {}

    // Called on first connection from an unknown device (TOFU).
    // Return true to trust and pin the device's public key fingerprint.
    virtual bool on_tofu_verify_request(
        const std::string& /*peer_id*/,
        const std::string& /*peer_name*/,
        const Sha256Digest& /*fingerprint*/) {
        return true;
    }

    virtual void on_connection_state_changed(
        bool /*connected*/,
        const std::string& /*peer_id*/,
        const std::string& /*peer_name*/) {}

    virtual void on_clipboard_received(const ReceivedClipboardItem& /*item*/) {}
    virtual void on_clipboard_lazy_offer(const ClipOfferPayload& /*offer*/) {}
    virtual void on_clipboard_pull_requested() {}
    virtual bool on_clipboard_pull_requested_sync() {
        on_clipboard_pull_requested();
        return false;
    }
    virtual void on_file_progress(const TransferProgress& /*progress*/) {}
    virtual void on_file_completed(
        uint64_t /*batch_id*/,
        uint32_t /*file_index*/,
        const std::filesystem::path& /*saved_path*/,
        bool /*success*/,
        const std::string& /*error_msg*/) {}
    virtual void on_file_transfer_completed(
        uint64_t batch_id,
        uint32_t file_index,
        const std::filesystem::path& saved_path,
        bool /*is_sending*/,
        bool success,
        const std::string& error_msg) {
        on_file_completed(batch_id, file_index, saved_path, success, error_msg);
    }
};

class Engine {
public:
    Engine(EngineConfig config, ICoreListener* listener = nullptr);
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    bool start();
    void stop();

    bool connect_to_peer(const std::string& ip_address, uint16_t port);
    bool connect_with_fd(int connected_fd, bool is_client);
    void disconnect_active_peer();
    bool is_connected() const noexcept { return connected_.load(); }
    uint16_t bound_tcp_port() const noexcept { return bound_port_.load(); }

    // Candidate Peers & On-demand Connection API
    void register_candidate_peer(const std::string& ip, uint16_t port);
    bool ensure_connected();
    std::vector<PeerEndpoint> candidate_peers() const;

    // Clipboard API
    bool push_clipboard_text(
        std::string_view utf8_text,
        bool bypass_loopback = false,
        bool is_pull_response = false);
    bool push_clipboard_bytes(
        std::string_view mime_type,
        std::span<const uint8_t> data,
        bool force_lazy_pull = false,
        bool bypass_loopback = false,
        bool is_pull_response = false);
    bool push_clipboard_file(
        std::string_view mime_type,
        const std::filesystem::path& file_path,
        bool lazy_pull = false,
        bool bypass_loopback = false,
        bool is_pull_response = false);
    bool request_clipboard_lazy_pull(uint64_t clip_id);
    bool pull_remote_clipboard();
    ClipboardHistoryManager& history_manager() noexcept;

    // File Transfer API
    uint64_t send_files(
        std::span<const std::filesystem::path> paths,
        uint64_t custom_batch_id = 0);

    const DeviceIdentity& identity() const noexcept { return identity_; }
    TrustedPeerStore& trusted_peers() noexcept { return peer_store_; }

private:
    struct SendWaitState {
        std::mutex mtx;
        std::condition_variable cv;
        bool done{false};
        bool result{false};
    };

    struct QueuedSendFrame {
        std::shared_ptr<TlsChannel> channel;
        Frame frame;
        std::shared_ptr<SendWaitState> wait;
    };

    static int frame_priority(FrameType type) noexcept;
    void accept_loop();
    void heartbeat_loop();
    void send_loop();
    void ensure_send_thread_started();
    void abort_pending_sends();
    void close_active_channel_quietly();
    bool attach_channel(std::shared_ptr<TlsChannel> channel);
    void channel_recv_loop(std::shared_ptr<TlsChannel> channel);
    bool send_frame_active(const Frame& frame);

    EngineConfig config_;
    ICoreListener* listener_{nullptr};

    DeviceIdentity identity_;
    TrustedPeerStore peer_store_;

    std::unique_ptr<ClipboardStreamManager> clipboard_mgr_;
    std::unique_ptr<ClipboardHistoryManager> history_mgr_;
    std::unique_ptr<FileTransferManager> file_mgr_;
    std::unique_ptr<DiscoveryManager> discovery_mgr_;

    std::atomic<int> listen_sock_{-1};
    std::atomic<uint16_t> bound_port_{0};
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};

    mutable std::mutex channel_mutex_;
    std::shared_ptr<TlsChannel> active_channel_;

    mutable std::mutex candidates_mutex_;
    std::vector<PeerEndpoint> candidate_endpoints_;
    std::mutex connect_attempt_mutex_;

    std::thread accept_thread_;
    std::thread recv_thread_;
    std::mutex recv_thread_mutex_;
    std::thread heartbeat_thread_;
    std::mutex heartbeat_mutex_;
    std::condition_variable heartbeat_cv_;

    std::thread send_thread_;
    std::mutex send_thread_mutex_;
    std::mutex send_queue_mutex_;
    std::condition_variable send_queue_cv_;
    std::condition_variable send_queue_space_cv_;
    std::array<std::deque<QueuedSendFrame>, 4> send_queues_;
    std::atomic<bool> send_running_{false};
};

}  // namespace l2a
