#pragma once

#include "gtk_abi.hpp"
#include "l2a/clipboard_stream.hpp"
#include "l2a/engine.hpp"
#include "l2a/history_manager.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <sys/types.h>

namespace l2a::linux_app {

using LogMessageCallback = std::function<void(const std::string& msg)>;

struct ResolvedImageFile {
    bool is_image{false};
    std::filesystem::path path;
    std::string mime_type;
};

// Priority 2: Nautilus / File manager copied image resolution
ResolvedImageFile try_resolve_local_image_file(std::string_view text);

class WaylandClipboardBridge {
public:
    WaylandClipboardBridge(Engine& engine, LogMessageCallback log_cb);
    ~WaylandClipboardBridge();

    WaylandClipboardBridge(const WaylandClipboardBridge&) = delete;
    WaylandClipboardBridge& operator=(const WaylandClipboardBridge&) = delete;

    void start_gtk_monitoring();

    // Directional auto-share switches (Requirement R1)
    void set_pc_to_phone_auto_share(bool enabled) noexcept { pc_to_phone_auto_share_.store(enabled); }
    bool pc_to_phone_auto_share() const noexcept { return pc_to_phone_auto_share_.load(); }

    void set_phone_to_pc_auto_share(bool enabled) noexcept { phone_to_pc_auto_share_.store(enabled); }
    bool phone_to_pc_auto_share() const noexcept { return phone_to_pc_auto_share_.load(); }

    // Legacy compatibility toggle
    void set_sync_enabled(bool enabled) noexcept {
        set_pc_to_phone_auto_share(enabled);
        set_phone_to_pc_auto_share(enabled);
    }
    bool is_sync_enabled() const noexcept {
        return pc_to_phone_auto_share_.load() && phone_to_pc_auto_share_.load();
    }

    // Applies clipboard content received from Android into the Linux Wayland/GTK clipboard
    void apply_remote_clipboard(const ReceivedClipboardItem& item);

    // Manual push of current Linux system clipboard to phone
    bool push_current_clipboard(bool is_pull_response = false);

    // Manual pull of remote clipboard
    bool pull_remote_clipboard();

    // Non-destructive selective paste of Item #k with 3s lease
    void perform_selective_paste(
        const ClipboardHistoryItem& item_to_paste,
        const std::optional<ClipboardHistoryItem>& item_0);
    bool is_selective_pasting() const noexcept { return is_selective_pasting_.load(); }

    // MIME type validation
    static bool is_valid_mime_type(std::string_view mime);

    // Environment checks
    static bool is_wayland_session();
    static bool has_wl_clipboard_tools();
    static bool has_wl_paste_tools();
    static bool has_xdotool();

private:
    struct OutboundClipTask {
        enum class Kind { Text, ImageBytes, ImageFile };
        Kind kind{Kind::Text};
        std::string mime_type;
        std::string text;
        std::vector<uint8_t> bytes;
        std::filesystem::path file_path;
        std::array<uint8_t, 32> hash{};
        bool bypass_loopback{false};
        bool is_pull_response{false};
        std::string source_tag;
    };

    static void on_gtk_owner_change(GtkClipboard* clipboard, GdkEvent* event, gpointer user_data);
    static void on_gtk_text_received(GtkClipboard* clipboard, const gchar* text, gpointer user_data);

    static bool write_to_wl_copy(const std::string& mime_type, const uint8_t* data, size_t size);
    static bool write_file_to_wl_copy(const std::string& mime_type, const std::string& file_path);

    // Thread-safe GTK main-thread dispatch
    void dispatch_to_gtk_thread(std::function<void()> fn);
    bool run_on_gtk_thread_sync(std::function<void()> fn, int timeout_ms = 1500);

    // Single serial outbound worker queue
    void start_outbound_worker();
    void stop_outbound_worker();
    void enqueue_outbound_task(OutboundClipTask task);
    void outbound_worker_loop();

    // Anti-loopback & manual-pull helpers
    bool should_suppress_auto_push(const std::array<uint8_t, 32>& hash);
    bool consume_manual_pull_pending();

    // Background Wayland watcher lifecycle (wl-paste --watch)
    void start_wayland_watcher();
    void stop_wayland_watcher();
    void wayland_watcher_loop();

    // XWayland / X11 XFixes background capture lifecycle
    void start_x11_background_monitor();
    void stop_x11_background_monitor();
    void x11_monitor_loop();

    Engine& engine_;
    LogMessageCallback log_cb_;
    GtkClipboard* gtk_clip_{nullptr};
    std::thread::id gtk_main_thread_id_{};
    bool has_gtk_main_thread_{false};
    std::shared_ptr<std::atomic<bool>> alive_{std::make_shared<std::atomic<bool>>(true)};

    std::atomic<bool> pc_to_phone_auto_share_{true};
    std::atomic<bool> phone_to_pc_auto_share_{true};

    // Anti-loopback & Selective paste state
    std::atomic<bool> is_applying_remote_{false};
    std::atomic<bool> is_selective_pasting_{false};
    std::mutex hash_mutex_;
    std::array<uint8_t, 32> last_applied_hash_{};
    std::array<uint8_t, 32> last_applied_png_hash_{};
    std::array<uint8_t, 32> last_sent_hash_{};
    bool has_applied_hash_{false};
    bool has_applied_png_hash_{false};
    bool has_sent_hash_{false};
    std::chrono::steady_clock::time_point suppress_until_{};
    std::chrono::steady_clock::time_point manual_pull_until_{};

    // Serial outbound clipboard worker
    std::thread push_thread_;
    std::mutex push_mutex_;
    std::condition_variable push_cv_;
    std::deque<OutboundClipTask> push_queue_;
    std::atomic<bool> stop_push_{false};

    // Selective paste sequence and cancellation
    std::atomic<uint64_t> selective_seq_{0};
    std::atomic<bool> is_destroyed_{false};
    std::mutex selective_cv_mutex_;
    std::condition_variable selective_cv_;
    std::thread selective_restore_thread_;

    // Wayland watcher state
    std::thread watcher_thread_;
    std::atomic<bool> stop_watcher_{false};
    pid_t watcher_pid_{0};
    std::string fifo_path_;

    // X11 / XFixes monitor state
    std::thread x11_thread_;
    std::atomic<bool> stop_x11_{false};
};

}  // namespace l2a::linux_app
