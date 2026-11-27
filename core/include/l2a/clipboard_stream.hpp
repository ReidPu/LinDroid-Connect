#pragma once

#include "l2a/protocol.hpp"
#include "l2a/sha256.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace l2a {

using SendFrameFunc = std::function<bool(const Frame& frame)>;

struct TempFileGuard {
    std::filesystem::path path;
    bool auto_delete{true};

    explicit TempFileGuard(std::filesystem::path p) : path(std::move(p)) {}
    ~TempFileGuard() {
        if (auto_delete && !path.empty()) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    }
    void release() noexcept { auto_delete = false; }
};

struct ReceivedClipboardItem {
    uint64_t clip_id{0};
    std::string mime_type;
    uint64_t total_size{0};
    Sha256Digest sha256{};
    // For inline items <= max_in_memory_size, data is populated directly.
    // For larger streamed items (e.g. large images), temp_file_path holds the streamed file on disk.
    std::vector<uint8_t> data;
    std::filesystem::path temp_file_path;
    std::shared_ptr<TempFileGuard> file_guard;
    bool is_pull_response{false};
};

using ClipboardReceivedCallback = std::function<void(const ReceivedClipboardItem& item)>;
using ClipboardLazyOfferCallback = std::function<void(const ClipOfferPayload& offer)>;
using ClipboardPullRequestedCallback = std::function<bool()>;

class ClipboardStreamManager {
public:
    explicit ClipboardStreamManager(
        SendFrameFunc send_frame_fn,
        std::filesystem::path temp_dir = std::filesystem::temp_directory_path());

    void set_callbacks(
        ClipboardReceivedCallback on_received,
        ClipboardLazyOfferCallback on_lazy_offer,
        ClipboardPullRequestedCallback on_pull_requested = nullptr);

    // Pushes in-memory clipboard content (e.g. text/plain, text/html, small/medium image).
    // Returns false if skipped due to anti-loopback (matches last received SHA-256) or send error.
    bool push_clipboard_bytes(
        std::string_view mime_type,
        std::span<const uint8_t> content,
        bool force_lazy_pull = false,
        bool bypass_loopback = false,
        bool is_pull_response = false);

    // Streams clipboard content directly from a file on disk using a single 64KB buffer.
    bool push_clipboard_file(
        std::string_view mime_type,
        const std::filesystem::path& file_path,
        bool lazy_pull = false,
        bool bypass_loopback = false,
        bool is_pull_response = false);

    // Requests the remote peer to stream a previously lazy-offered clip_id.
    bool request_lazy_pull(uint64_t clip_id);

    // Handles incoming CLIP_OFFER, CLIP_DATA_CHUNK, CLIP_PULL_REQ frames.
    void handle_frame(const Frame& frame);

    // Clears incomplete incoming clipboard streams when the transport disconnects.
    void on_disconnected();

    // Checks if a digest matches the most recently received clipboard item (anti-loopback).
    bool is_loopback_digest(const Sha256Digest& digest) const;

private:
    bool stream_bytes_now(uint64_t clip_id, uint32_t stream_id, std::span<const uint8_t> content);
    bool stream_file_now(uint64_t clip_id, uint32_t stream_id, const std::filesystem::path& path);

    struct LazySource {
        uint64_t clip_id{0};
        std::string mime_type;
        std::vector<uint8_t> inline_bytes;
        std::filesystem::path file_path;
    };

    struct IncomingStream {
        ClipOfferPayload offer{};
        Sha256Hasher hasher{};
        uint64_t bytes_received{0};
        std::vector<uint8_t> memory_buf;
        std::filesystem::path temp_file;
        std::unique_ptr<std::ofstream> temp_out;
        std::shared_ptr<TempFileGuard> temp_guard;
        bool use_temp_file{false};
        bool is_pull_response{false};
    };

    SendFrameFunc send_frame_fn_;
    std::filesystem::path temp_dir_;

    ClipboardReceivedCallback on_received_;
    ClipboardLazyOfferCallback on_lazy_offer_;
    ClipboardPullRequestedCallback on_pull_requested_;

    struct LatestItem {
        uint64_t clip_id{0};
        std::string mime_type;
        std::vector<uint8_t> inline_bytes;
        std::filesystem::path file_path;
        Sha256Digest sha256{};
        uint64_t total_size{0};
    };

    mutable std::mutex mutex_;
    LatestItem latest_local_item_;
    std::optional<Sha256Digest> last_received_hash_;
    std::atomic<uint64_t> next_clip_id_{1};
    std::atomic<uint32_t> next_stream_id_{100};

    std::unordered_map<uint64_t, LazySource> lazy_sources_;
    std::unordered_map<uint64_t, IncomingStream> incoming_streams_;
};

}  // namespace l2a
