#include "l2a/engine.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace l2a {

namespace {
constexpr size_t kMaxP3QueueSize = 16;
}  // namespace

Engine::Engine(EngineConfig config, ICoreListener* listener)
    : config_(std::move(config)),
      listener_(listener),
      identity_(DeviceIdentity::load_or_create(config_.storage_dir, config_.device_name)),
      peer_store_(config_.storage_dir / "trusted_peers.tsv") {
    auto send_fn = [this](const Frame& f) { return send_frame_active(f); };

    history_mgr_ = std::make_unique<ClipboardHistoryManager>(
        config_.storage_dir.empty() ? std::filesystem::path{} : (config_.storage_dir / "history.json"), 20);

    clipboard_mgr_ = std::make_unique<ClipboardStreamManager>(
        send_fn, config_.storage_dir / "tmp_clip");
    clipboard_mgr_->set_callbacks(
        [this](const ReceivedClipboardItem& item) {
            if (history_mgr_) {
                if (item.mime_type.rfind("text/", 0) == 0) {
                    std::string text;
                    if (!item.data.empty()) {
                        text.assign(reinterpret_cast<const char*>(item.data.data()), item.data.size());
                    } else if (!item.temp_file_path.empty()) {
                        std::ifstream in(item.temp_file_path, std::ios::binary);
                        if (in.is_open()) {
                            text.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                        }
                    }
                    if (!text.empty()) {
                        history_mgr_->add_text_item(ClipOrigin::Remote, text, item.sha256);
                    }
                } else if (item.mime_type.rfind("image/", 0) == 0) {
                    history_mgr_->add_image_item(
                        ClipOrigin::Remote, item.mime_type, item.data, item.temp_file_path, item.sha256);
                }
            }
            if (listener_) listener_->on_clipboard_received(item);
        },
        [this](const ClipOfferPayload& offer) {
            if (listener_) listener_->on_clipboard_lazy_offer(offer);
        },
        [this]() -> bool {
            return listener_ ? listener_->on_clipboard_pull_requested_sync() : false;
        });

    file_mgr_ = std::make_unique<FileTransferManager>(send_fn, config_.receive_dir);
    file_mgr_->set_callbacks(
        [this](const TransferProgress& prog) {
            if (listener_) listener_->on_file_progress(prog);
        },
        [this](uint64_t bid, uint32_t fidx, const std::filesystem::path& path,
               bool is_sending, bool ok, const std::string& err) {
            if (listener_) listener_->on_file_transfer_completed(bid, fidx, path, is_sending, ok, err);
        });
}

Engine::~Engine() {
    stop();
}

int Engine::frame_priority(FrameType type) noexcept {
    switch (type) {
        case FrameType::kPing:
        case FrameType::kPong:
        case FrameType::kHelloHandshake:
        case FrameType::kPairRequest:
        case FrameType::kPairResponse:
            return 0;
        case FrameType::kClipOffer:
        case FrameType::kClipDataChunk:
        case FrameType::kClipPullReq:
            return 1;
        case FrameType::kFileBatchMeta:
        case FrameType::kFileResumeOffset:
        case FrameType::kFileVerifiedAck:
            return 2;
        case FrameType::kFileDataChunk:
            return 3;
        default:
            return 2;
    }
}

void Engine::ensure_send_thread_started() {
    std::lock_guard<std::mutex> lock(send_thread_mutex_);
    if (!send_running_.load() && !send_thread_.joinable()) {
        send_running_.store(true);
        send_thread_ = std::thread(&Engine::send_loop, this);
    }
}

void Engine::abort_pending_sends() {
    std::array<std::deque<QueuedSendFrame>, 4> leftover;
    {
        std::lock_guard<std::mutex> lock(send_queue_mutex_);
        for (size_t i = 0; i < send_queues_.size(); ++i) {
            leftover[i].swap(send_queues_[i]);
        }
    }
    send_queue_space_cv_.notify_all();
    for (auto& q : leftover) {
        for (auto& item : q) {
            if (item.wait) {
                {
                    std::lock_guard<std::mutex> wlock(item.wait->mtx);
                    item.wait->result = false;
                    item.wait->done = true;
                }
                item.wait->cv.notify_all();
            }
        }
    }
}

void Engine::send_loop() {
    while (true) {
        QueuedSendFrame item{};
        bool has_item = false;
        {
            std::unique_lock<std::mutex> lock(send_queue_mutex_);
            send_queue_cv_.wait(lock, [this] {
                if (!send_running_.load()) return true;
                for (const auto& q : send_queues_) {
                    if (!q.empty()) return true;
                }
                return false;
            });
            if (!send_running_.load()) {
                break;
            }
            for (size_t p = 0; p < send_queues_.size(); ++p) {
                if (!send_queues_[p].empty()) {
                    item = std::move(send_queues_[p].front());
                    send_queues_[p].pop_front();
                    if (p == 3) {
                        send_queue_space_cv_.notify_one();
                    }
                    has_item = true;
                    break;
                }
            }
        }
        if (!has_item) continue;

        bool ok = false;
        if (item.channel && item.channel->socket_fd() >= 0) {
            ok = item.channel->send_frame(item.frame);
        }
        if (item.wait) {
            {
                std::lock_guard<std::mutex> wlock(item.wait->mtx);
                item.wait->result = ok;
                item.wait->done = true;
            }
            item.wait->cv.notify_all();
        }
    }
    abort_pending_sends();
}

bool Engine::start() {
    if (running_.load()) {
        return true;
    }

    int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return false;
    }

    int yes = 1;
    ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(config_.listen_port);

    if (::bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(sock);
        return false;
    }

    sockaddr_in bound{};
    socklen_t bound_len = sizeof(bound);
    if (::getsockname(sock, reinterpret_cast<sockaddr*>(&bound), &bound_len) == 0) {
        bound_port_.store(ntohs(bound.sin_port));
    }

    if (::listen(sock, 8) < 0) {
        ::close(sock);
        return false;
    }

    listen_sock_.store(sock);

    running_.store(true);
    ensure_send_thread_started();
    accept_thread_ = std::thread(&Engine::accept_loop, this);
    if (!heartbeat_thread_.joinable()) {
        heartbeat_thread_ = std::thread(&Engine::heartbeat_loop, this);
    }

    // Platform-aware default ADB tunnel candidate:
    // - Linux connects via `adb forward tcp:53322 tcp:53321`
    // - Android connects via `adb reverse tcp:53323 tcp:<pc_port>`
    if (config_.platform == "android") {
        register_candidate_peer("127.0.0.1", 53323);
    } else {
        register_candidate_peer("127.0.0.1", 53322);
    }

    if (config_.discovery_port != 0) {
        discovery_mgr_ = std::make_unique<DiscoveryManager>(
            identity_.device_id,
            identity_.device_name,
            config_.platform,
            bound_port_.load(),
            identity_.fingerprint,
            config_.discovery_port);
        discovery_mgr_->start([this](const DiscoveredPeer& peer) {
            register_candidate_peer(peer.ip_address, peer.tcp_port);
            if (listener_) listener_->on_peer_discovered(peer);
        });
    }

    return true;
}

void Engine::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    close_active_channel_quietly();

    // Stop send thread and abort any pending callers BEFORE stopping file_mgr_
    {
        std::lock_guard<std::mutex> lock(send_thread_mutex_);
        send_running_.store(false);
        send_queue_cv_.notify_all();
        send_queue_space_cv_.notify_all();
        if (send_thread_.joinable()) {
            if (std::this_thread::get_id() != send_thread_.get_id()) {
                send_thread_.join();
            } else {
                send_thread_.detach();
            }
        }
    }
    abort_pending_sends();

    if (discovery_mgr_) {
        discovery_mgr_->stop();
        discovery_mgr_.reset();
    }

    if (file_mgr_) {
        file_mgr_->stop();
    }

    heartbeat_cv_.notify_all();
    if (heartbeat_thread_.joinable()) {
        if (std::this_thread::get_id() != heartbeat_thread_.get_id()) {
            heartbeat_thread_.join();
        } else {
            heartbeat_thread_.detach();
        }
    }

    int lsock = listen_sock_.load();
    if (lsock >= 0) {
        ::shutdown(lsock, SHUT_RDWR);
    }

    if (accept_thread_.joinable()) {
        if (std::this_thread::get_id() != accept_thread_.get_id()) {
            accept_thread_.join();
        } else {
            accept_thread_.detach();
        }
    }

    lsock = listen_sock_.exchange(-1);
    if (lsock >= 0) {
        ::close(lsock);
    }

    {
        std::lock_guard<std::mutex> lock(recv_thread_mutex_);
        if (recv_thread_.joinable()) {
            if (std::this_thread::get_id() != recv_thread_.get_id()) {
                recv_thread_.join();
            } else {
                recv_thread_.detach();
            }
        }
    }
}

void Engine::heartbeat_loop() {
    while (running_.load()) {
        {
            std::unique_lock<std::mutex> lock(heartbeat_mutex_);
            heartbeat_cv_.wait_for(lock, std::chrono::seconds(4), [this] {
                return !running_.load();
            });
        }
        if (!running_.load()) break;

        if (connected_.load()) {
            Frame ping{};
            ping.header.type = FrameType::kPing;
            ping.header.flags = FrameFlags::kFin;
            send_frame_active(ping);
        }
    }
}

void Engine::accept_loop() {
    while (running_.load()) {
        int lsock = listen_sock_.load();
        if (lsock < 0) {
            break;
        }
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = ::accept(lsock, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client_fd < 0) {
            continue;
        }
        int nodelay = 1;
        ::setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        int keepalive = 1;
        ::setsockopt(client_fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
        struct timeval tv{};
        tv.tv_sec = 20;
        tv.tv_usec = 0;
        ::setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        auto channel = std::make_shared<TlsChannel>(client_fd, /*is_client=*/false);
        bool ok = channel->perform_handshake(
            identity_,
            peer_store_,
            [this](const std::string& pid, const std::string& pname, const Sha256Digest& fp) {
                return listener_ ? listener_->on_tofu_verify_request(pid, pname, fp) : true;
            });

        if (ok) {
            attach_channel(std::move(channel));
        } else {
            channel->close();
        }
    }
}

bool Engine::connect_to_peer(const std::string& ip_address, uint16_t port) {
    register_candidate_peer(ip_address, port);

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    int nodelay = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    int keepalive = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
    struct timeval tv{};
    tv.tv_sec = 20;
    tv.tv_usec = 0;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    if (::inet_pton(AF_INET, ip_address.c_str(), &dest.sin_addr) != 1) {
        ::close(fd);
        return false;
    }

    // Non-blocking connect with 1000ms timeout so candidate endpoints are checked quickly
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    int res = ::connect(fd, reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
    if (res < 0) {
        if (errno == EINPROGRESS) {
            struct pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLOUT;
            int poll_res = ::poll(&pfd, 1, 1000);
            if (poll_res <= 0) {
                ::close(fd);
                return false;
            }
            int so_error = 0;
            socklen_t len = sizeof(so_error);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0 || so_error != 0) {
                ::close(fd);
                return false;
            }
        } else {
            ::close(fd);
            return false;
        }
    }

    // Restore blocking mode
    if (flags >= 0) {
        ::fcntl(fd, F_SETFL, flags);
    }

    return connect_with_fd(fd, /*is_client=*/true);
}

bool Engine::connect_with_fd(int connected_fd, bool is_client) {
    running_.store(true);
    ensure_send_thread_started();
    if (!heartbeat_thread_.joinable()) {
        heartbeat_thread_ = std::thread(&Engine::heartbeat_loop, this);
    }
    int keepalive = 1;
    ::setsockopt(connected_fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
    struct timeval tv{};
    tv.tv_sec = 20;
    tv.tv_usec = 0;
    ::setsockopt(connected_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(connected_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    auto channel = std::make_shared<TlsChannel>(connected_fd, is_client);
    bool ok = channel->perform_handshake(
        identity_,
        peer_store_,
        [this](const std::string& pid, const std::string& pname, const Sha256Digest& fp) {
            return listener_ ? listener_->on_tofu_verify_request(pid, pname, fp) : true;
        });

    if (!ok) {
        channel->close();
        return false;
    }

    return attach_channel(std::move(channel));
}

bool Engine::attach_channel(std::shared_ptr<TlsChannel> channel) {
    // Symmetric arbitration for simultaneous dual-connect from the same peer
    {
        std::lock_guard<std::mutex> ch_lock(channel_mutex_);
        if (active_channel_ &&
            active_channel_->socket_fd() >= 0 &&
            active_channel_->peer_device_id() == channel->peer_device_id() &&
            active_channel_->peer_device_id() != identity_.device_id &&
            active_channel_->is_client() != channel->is_client()) {
            bool prefer_client = (identity_.device_id < channel->peer_device_id());
            if (active_channel_->is_client() == prefer_client) {
                channel->close();
                return true;
            }
        }
    }

    close_active_channel_quietly();

    std::unique_lock<std::mutex> lock(recv_thread_mutex_);
    if (recv_thread_.joinable()) {
        if (std::this_thread::get_id() == recv_thread_.get_id()) {
            recv_thread_.detach();
        } else {
            recv_thread_.join();
        }
    }

    if (!running_.load()) {
        lock.unlock();
        channel->close();
        return false;
    }

    ensure_send_thread_started();

    std::string pid = channel->peer_device_id();
    std::string pname = channel->peer_device_name();

    {
        std::lock_guard<std::mutex> ch_lock(channel_mutex_);
        active_channel_ = channel;
        connected_.store(true);
    }

    recv_thread_ = std::thread(&Engine::channel_recv_loop, this, channel);
    lock.unlock();

    if (listener_) {
        listener_->on_connection_state_changed(true, pid, pname);
    }
    if (file_mgr_) {
        file_mgr_->on_connection_restored();
    }
    return true;
}

void Engine::close_active_channel_quietly() {
    std::shared_ptr<TlsChannel> ch;
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        ch = std::move(active_channel_);
        connected_.store(false);
    }
    if (ch) {
        ch->close();
        send_queue_space_cv_.notify_all();
    }
}

void Engine::disconnect_active_peer() {
    std::shared_ptr<TlsChannel> ch;
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        ch = active_channel_;
        connected_.store(false);
    }
    if (ch) {
        ch->close();
        send_queue_space_cv_.notify_all();
    }
}

void Engine::channel_recv_loop(std::shared_ptr<TlsChannel> channel) {
    std::string pid = channel->peer_device_id();
    std::string pname = channel->peer_device_name();

    while (running_.load()) {
        auto frame_opt = channel->recv_frame();
        if (!frame_opt) {
            break;
        }
        const Frame& frame = *frame_opt;
        switch (frame.header.type) {
            case FrameType::kPing: {
                Frame pong{};
                pong.header.type = FrameType::kPong;
                pong.header.stream_id = 0;
                pong.header.flags = FrameFlags::kFin;
                channel->send_frame(pong);
                break;
            }
            case FrameType::kPong:
                break;
            case FrameType::kClipOffer:
            case FrameType::kClipDataChunk:
            case FrameType::kClipPullReq:
                clipboard_mgr_->handle_frame(frame);
                break;

            case FrameType::kFileBatchMeta:
            case FrameType::kFileResumeOffset:
            case FrameType::kFileDataChunk:
            case FrameType::kFileVerifiedAck:
                file_mgr_->handle_frame(frame);
                break;

            default:
                break;
        }
    }

    bool was_active = false;
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        if (active_channel_ == channel) {
            active_channel_.reset();
            connected_.store(false);
            was_active = true;
        }
    }
    send_queue_space_cv_.notify_all();

    if (was_active) {
        if (clipboard_mgr_) {
            clipboard_mgr_->on_disconnected();
        }
        if (file_mgr_) {
            file_mgr_->on_connection_lost();
        }
        if (listener_) {
            listener_->on_connection_state_changed(false, pid, pname);
        }
    }
}

void Engine::register_candidate_peer(const std::string& ip, uint16_t port) {
    if (ip.empty() || port == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(candidates_mutex_);
    for (auto it = candidate_endpoints_.begin(); it != candidate_endpoints_.end(); ++it) {
        if (it->ip == ip && it->port == port) {
            PeerEndpoint ep = *it;
            candidate_endpoints_.erase(it);
            candidate_endpoints_.insert(candidate_endpoints_.begin(), std::move(ep));
            return;
        }
    }
    candidate_endpoints_.insert(candidate_endpoints_.begin(), PeerEndpoint{ip, port});
}

bool Engine::ensure_connected() {
    if (connected_.load()) {
        return true;
    }
    std::unique_lock<std::mutex> lock(connect_attempt_mutex_);
    if (connected_.load()) {
        return true;
    }
    std::vector<PeerEndpoint> candidates;
    {
        std::lock_guard<std::mutex> c_lock(candidates_mutex_);
        candidates = candidate_endpoints_;
    }
    for (const auto& ep : candidates) {
        if (connect_to_peer(ep.ip, ep.port)) {
            return true;
        }
    }
    return false;
}

std::vector<PeerEndpoint> Engine::candidate_peers() const {
    std::lock_guard<std::mutex> lock(candidates_mutex_);
    return candidate_endpoints_;
}

bool Engine::send_frame_active(const Frame& frame) {
    const bool allow_auto_connect =
        (frame.header.type == FrameType::kClipOffer ||
         frame.header.type == FrameType::kClipPullReq ||
         frame.header.type == FrameType::kFileBatchMeta);

    std::shared_ptr<TlsChannel> ch;
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        ch = active_channel_;
    }
    if (!ch && allow_auto_connect) {
        if (ensure_connected()) {
            std::lock_guard<std::mutex> lock(channel_mutex_);
            ch = active_channel_;
        }
    }
    if (!ch || ch->socket_fd() < 0) {
        return false;
    }

    if (!send_running_.load()) {
        return false;
    }

    const int prio = frame_priority(frame.header.type);
    auto wait_state = std::make_shared<SendWaitState>();

    {
        std::unique_lock<std::mutex> lock(send_queue_mutex_);
        if (prio == 3) {
            send_queue_space_cv_.wait(lock, [this, &ch] {
                return !send_running_.load() ||
                       !running_.load() ||
                       ch->socket_fd() < 0 ||
                       send_queues_[3].size() < kMaxP3QueueSize;
            });
        }
        if (!send_running_.load() || !running_.load() || ch->socket_fd() < 0) {
            return false;
        }
        send_queues_[static_cast<size_t>(prio)].push_back(QueuedSendFrame{ch, frame, wait_state});
    }
    send_queue_cv_.notify_one();

    std::unique_lock<std::mutex> wlock(wait_state->mtx);
    wait_state->cv.wait(wlock, [&wait_state] { return wait_state->done; });
    return wait_state->result;
}

bool Engine::push_clipboard_text(
    std::string_view utf8_text,
    bool bypass_loopback,
    bool is_pull_response) {
    bool ok = clipboard_mgr_->push_clipboard_bytes(
        "text/plain",
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(utf8_text.data()),
                                 utf8_text.size()),
        false,
        bypass_loopback,
        is_pull_response);
    if (ok && history_mgr_) {
        history_mgr_->add_text_item(ClipOrigin::Local, utf8_text, sha256_string(utf8_text));
    }
    return ok;
}

bool Engine::push_clipboard_bytes(
    std::string_view mime_type,
    std::span<const uint8_t> data,
    bool force_lazy_pull,
    bool bypass_loopback,
    bool is_pull_response) {
    bool ok = clipboard_mgr_->push_clipboard_bytes(
        mime_type, data, force_lazy_pull, bypass_loopback, is_pull_response);
    if (ok && history_mgr_) {
        if (mime_type.rfind("text/", 0) == 0) {
            history_mgr_->add_text_item(
                ClipOrigin::Local,
                std::string_view(reinterpret_cast<const char*>(data.data()), data.size()),
                sha256_bytes(data));
        } else if (mime_type.rfind("image/", 0) == 0) {
            history_mgr_->add_image_item(ClipOrigin::Local, mime_type, data, {}, sha256_bytes(data));
        }
    }
    return ok;
}

bool Engine::push_clipboard_file(
    std::string_view mime_type,
    const std::filesystem::path& file_path,
    bool lazy_pull,
    bool bypass_loopback,
    bool is_pull_response) {
    bool ok = clipboard_mgr_->push_clipboard_file(
        mime_type, file_path, lazy_pull, bypass_loopback, is_pull_response);
    if (ok && history_mgr_) {
        auto digest_opt = sha256_file_range(file_path, 0, 0);
        if (digest_opt) {
            history_mgr_->add_image_item(ClipOrigin::Local, mime_type, {}, file_path, *digest_opt);
        }
    }
    return ok;
}

bool Engine::request_clipboard_lazy_pull(uint64_t clip_id) {
    return clipboard_mgr_->request_lazy_pull(clip_id);
}

bool Engine::pull_remote_clipboard() {
    if (!ensure_connected()) {
        return false;
    }
    return request_clipboard_lazy_pull(0);
}

ClipboardHistoryManager& Engine::history_manager() noexcept {
    return *history_mgr_;
}

uint64_t Engine::send_files(
    std::span<const std::filesystem::path> paths,
    uint64_t custom_batch_id) {
    return file_mgr_->send_paths(paths, custom_batch_id);
}

}  // namespace l2a
