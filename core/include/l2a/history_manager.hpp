#pragma once

#include "l2a/protocol.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace l2a {

enum class ClipContentType : uint8_t {
    Text = 0,
    Image = 1
};

enum class ClipOrigin : uint8_t {
    Local = 0,
    Remote = 1
};

struct ClipboardHistoryItem {
    uint64_t id{0};
    ClipContentType type{ClipContentType::Text};
    ClipOrigin origin{ClipOrigin::Local};
    std::string mime_type{"text/plain"};
    std::string text_preview;
    std::string full_text;
    std::vector<uint8_t> thumbnail_bytes;
    std::filesystem::path image_path;
    Sha256Digest sha256{};
    int64_t timestamp_ms{0};
    bool is_pinned{false};
};

class ClipboardHistoryManager {
public:
    explicit ClipboardHistoryManager(
        std::filesystem::path history_file_path = {},
        size_t max_limit = 20);
    ~ClipboardHistoryManager() = default;

    ClipboardHistoryManager(const ClipboardHistoryManager&) = delete;
    ClipboardHistoryManager& operator=(const ClipboardHistoryManager&) = delete;

    // Add clipboard items (prepends to index 0, returns assigned item ID)
    uint64_t add_text_item(
        ClipOrigin origin,
        std::string_view text,
        const Sha256Digest& sha256);

    uint64_t add_image_item(
        ClipOrigin origin,
        std::string_view mime_type,
        std::span<const uint8_t> data,
        const std::filesystem::path& file_path,
        const Sha256Digest& sha256);

    uint64_t add_item(ClipboardHistoryItem item);

    // Dynamic capacity limit adjustment ([5, 200], default 20)
    void set_max_history_limit(size_t limit);
    size_t max_history_limit() const;
    size_t item_count() const;

    // Thread-safe query snapshots (index 0 is newest)
    std::vector<ClipboardHistoryItem> get_items() const;
    std::optional<ClipboardHistoryItem> get_item(uint64_t id) const;
    std::optional<ClipboardHistoryItem> get_latest_item() const;

    // Deletion & Pinning
    bool delete_item(uint64_t id);
    void clear_all();
    bool pin_item(uint64_t id, bool pin);

    // Non-destructive selective paste token tracking
    std::string mark_selective_paste(uint64_t id);
    bool is_selective_paste_active(uint64_t id) const;
    std::optional<uint64_t> active_selective_paste_id() const;
    void clear_selective_paste();

    // Disk persistence (~/.config/linux_to_android/history.json)
    bool save_to_disk();
    bool load_from_disk();
    std::string to_json() const;
    bool from_json(std::string_view json_str);

private:
    void trim_to_limit_locked();
    void auto_save_locked();
    std::string to_json_locked() const;
    bool from_json_locked(std::string_view json_str);

    mutable std::mutex mutex_;
    std::filesystem::path history_file_path_;
    size_t max_limit_{20};
    uint64_t next_id_{1};
    std::vector<ClipboardHistoryItem> items_;

    // Selective paste token tracking
    uint64_t selective_paste_item_id_{0};
    std::string selective_paste_token_;
    std::chrono::steady_clock::time_point selective_paste_time_{};
};

}  // namespace l2a
