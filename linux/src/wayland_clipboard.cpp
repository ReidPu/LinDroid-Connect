#include "wayland_clipboard.hpp"
#include "l2a/sha256.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <fstream>
#include <poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>

namespace l2a::linux_app {

namespace {

constexpr size_t kMaxClipboardPayloadBytes = 50ULL * 1024ULL * 1024ULL;

bool check_cmd_in_path(const char* tool) {
    const char* path_env = std::getenv("PATH");
    if (!path_env) {
        return false;
    }
    std::string path_str = path_env;
    size_t start = 0;
    while (start < path_str.size()) {
        size_t end = path_str.find(':', start);
        if (end == std::string::npos) {
            end = path_str.size();
        }
        std::string dir = path_str.substr(start, end - start);
        start = end + 1;
        if (dir.empty()) {
            dir = ".";
        }
        std::string full_path = dir + "/" + tool;
        if (::access(full_path.c_str(), X_OK) == 0) {
            return true;
        }
    }
    return false;
}

// Minimal dynamic loader for libXfixes
using PfnXFixesQueryExtension = Bool (*)(Display*, int*, int*);
using PfnXFixesSelectSelectionInput = void (*)(Display*, Window, Atom, unsigned long);

constexpr unsigned long kXFixesSetSelectionOwnerNotifyMask = 1L << 0;
constexpr int kXFixesSelectionNotify = 0;

struct XFixesSelectionNotifyEventABI {
    int type;
    unsigned long serial;
    Bool send_event;
    Display* display;
    Window window;
    int subtype;
    Window owner;
    Atom selection;
    Time timestamp;
    Time selection_timestamp;
};

std::filesystem::path get_clip_cache_dir() {
    const char* home = std::getenv("HOME");
    std::filesystem::path cache_dir = home ? (std::filesystem::path(home) / ".config" / "linux_to_android" / "clip_cache")
                                           : (std::filesystem::current_path() / ".config" / "linux_to_android" / "clip_cache");
    std::error_code ec;
    std::filesystem::create_directories(cache_dir, ec);
    if (ec) {
        cache_dir = std::filesystem::temp_directory_path() / "l2a_clip_cache";
        ec.clear();
        std::filesystem::create_directories(cache_dir, ec);
    }
    return cache_dir;
}

std::string extension_for_mime(std::string_view mime_type) {
    if (mime_type.find("jpeg") != std::string_view::npos || mime_type.find("jpg") != std::string_view::npos) {
        return ".jpg";
    }
    if (mime_type.find("webp") != std::string_view::npos) {
        return ".webp";
    }
    if (mime_type.find("gif") != std::string_view::npos) {
        return ".gif";
    }
    if (mime_type.find("bmp") != std::string_view::npos) {
        return ".bmp";
    }
    return ".png";
}

bool run_wl_paste_cmd(
    const std::vector<std::string>& args,
    std::vector<uint8_t>& out,
    size_t max_bytes = kMaxClipboardPayloadBytes,
    int timeout_ms = 1500) {
    out.clear();
    if (args.empty()) return false;

    int pfd[2];
    if (::pipe(pfd) != 0) return false;

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pfd[0]);
        ::close(pfd[1]);
        return false;
    }

    if (pid == 0) {
        ::close(pfd[0]);
        ::dup2(pfd[1], STDOUT_FILENO);
        ::close(pfd[1]);

        int devnull = ::open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            ::dup2(devnull, STDIN_FILENO);
            ::dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) ::close(devnull);
        }

        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& a : args) {
            argv.push_back(const_cast<char*>(a.c_str()));
        }
        argv.push_back(nullptr);
        ::execvp(argv[0], argv.data());
        ::_exit(127);
    }

    ::close(pfd[1]);
    int flags = ::fcntl(pfd[0], F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(pfd[0], F_SETFL, flags | O_NONBLOCK);
    }

    auto start = std::chrono::steady_clock::now();
    bool timed_out = false;
    bool overflowed = false;
    uint8_t buf[16384];

    while (true) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        int remaining = timeout_ms - static_cast<int>(elapsed);
        if (remaining <= 0) {
            timed_out = true;
            break;
        }

        struct pollfd pfd_poll{};
        pfd_poll.fd = pfd[0];
        pfd_poll.events = POLLIN | POLLHUP | POLLERR;
        int rc = ::poll(&pfd_poll, 1, remaining);
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (rc == 0) {
            timed_out = true;
            break;
        }

        if (pfd_poll.revents & POLLIN) {
            ssize_t n = ::read(pfd[0], buf, sizeof(buf));
            if (n > 0) {
                if (out.size() + static_cast<size_t>(n) > max_bytes) {
                    overflowed = true;
                    break;
                }
                out.insert(out.end(), buf, buf + n);
            } else if (n == 0) {
                break;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                break;
            }
        }

        if (pfd_poll.revents & (POLLHUP | POLLERR)) {
            while (true) {
                ssize_t n = ::read(pfd[0], buf, sizeof(buf));
                if (n > 0) {
                    if (out.size() + static_cast<size_t>(n) > max_bytes) {
                        overflowed = true;
                        break;
                    }
                    out.insert(out.end(), buf, buf + n);
                } else {
                    break;
                }
            }
            break;
        }
    }

    ::close(pfd[0]);
    if (timed_out || overflowed) {
        ::kill(pid, SIGKILL);
        int st = 0;
        ::waitpid(pid, &st, 0);
        out.clear();
        return false;
    }

    int status = 0;
    ::waitpid(pid, &status, 0);
    return WIFEXITED(status) && (WEXITSTATUS(status) == 0);
}

std::vector<std::string> read_wl_paste_types() {
    std::vector<uint8_t> raw;
    if (!run_wl_paste_cmd({"wl-paste", "--list-types"}, raw, 64 * 1024, 1500)) {
        return {};
    }
    std::string str(reinterpret_cast<const char*>(raw.data()), raw.size());
    std::vector<std::string> types;
    size_t start = 0;
    while (start < str.size()) {
        size_t end = str.find_first_of("\r\n", start);
        if (end == std::string::npos) end = str.size();
        std::string line = str.substr(start, end - start);
        start = end + 1;
        if (!line.empty()) {
            types.push_back(std::move(line));
        }
    }
    return types;
}

struct CapturedWaylandClipboard {
    enum class Kind { Empty, ImageBytes, ImageFile, Text };
    Kind kind{Kind::Empty};
    std::string mime_type;
    std::vector<uint8_t> bytes;
    std::filesystem::path file_path;
    std::string text;
};

CapturedWaylandClipboard capture_via_wl_paste() {
    CapturedWaylandClipboard cap{};
    auto types = read_wl_paste_types();
    if (types.empty()) {
        return cap;
    }

    auto has_type = [&](std::string_view target) {
        return std::find(types.begin(), types.end(), target) != types.end();
    };

    // Priority 1: Raw image MIME types
    static constexpr const char* kImageMimes[] = {
        "image/png", "image/jpeg", "image/webp", "image/gif", "image/bmp"
    };
    for (const char* mime : kImageMimes) {
        if (has_type(mime)) {
            std::vector<uint8_t> raw;
            if (run_wl_paste_cmd({"wl-paste", "--no-newline", "--type", mime}, raw) && !raw.empty()) {
                cap.kind = CapturedWaylandClipboard::Kind::ImageBytes;
                cap.mime_type = mime;
                cap.bytes = std::move(raw);
                return cap;
            }
        }
    }

    // Priority 2: URI lists / file manager copies
    static constexpr const char* kUriMimes[] = {
        "x-special/gnome-copied-files", "text/uri-list"
    };
    for (const char* mime : kUriMimes) {
        if (has_type(mime)) {
            std::vector<uint8_t> raw;
            if (run_wl_paste_cmd({"wl-paste", "--no-newline", "--type", mime}, raw, 1024 * 1024) && !raw.empty()) {
                std::string text(reinterpret_cast<const char*>(raw.data()), raw.size());
                auto resolved = try_resolve_local_image_file(text);
                if (resolved.is_image) {
                    cap.kind = CapturedWaylandClipboard::Kind::ImageFile;
                    cap.mime_type = resolved.mime_type;
                    cap.file_path = resolved.path;
                    return cap;
                }
            }
        }
    }

    // Priority 3: Plain text (or local image path inside plain text)
    static constexpr const char* kTextMimes[] = {
        "text/plain;charset=utf-8", "text/plain", "UTF8_STRING"
    };
    for (const char* mime : kTextMimes) {
        if (has_type(mime)) {
            std::vector<uint8_t> raw;
            if (run_wl_paste_cmd({"wl-paste", "--no-newline", "--type", mime}, raw, 8 * 1024 * 1024) && !raw.empty()) {
                std::string text(reinterpret_cast<const char*>(raw.data()), raw.size());
                auto resolved = try_resolve_local_image_file(text);
                if (resolved.is_image) {
                    cap.kind = CapturedWaylandClipboard::Kind::ImageFile;
                    cap.mime_type = resolved.mime_type;
                    cap.file_path = resolved.path;
                    return cap;
                }
                cap.kind = CapturedWaylandClipboard::Kind::Text;
                cap.mime_type = "text/plain";
                cap.text = std::move(text);
                return cap;
            }
        }
    }

    return cap;
}

struct IdleTaskCtx {
    std::shared_ptr<std::atomic<bool>> alive;
    std::function<void()> fn;
};

struct SyncIdleState {
    std::shared_ptr<std::atomic<bool>> alive;
    std::function<void()> fn;
    std::mutex mtx;
    std::condition_variable cv;
    bool done{false};
};

gboolean idle_task_trampoline(gpointer data) {
    auto* ctx = static_cast<IdleTaskCtx*>(data);
    if (ctx->alive && ctx->alive->load() && ctx->fn) {
        ctx->fn();
    }
    delete ctx;
    return FALSE_VAL;
}

gboolean sync_idle_trampoline(gpointer data) {
    auto* holder = static_cast<std::shared_ptr<SyncIdleState>*>(data);
    auto state = *holder;
    delete holder;
    if (state->alive && state->alive->load() && state->fn) {
        state->fn();
    }
    {
        std::lock_guard<std::mutex> lock(state->mtx);
        state->done = true;
    }
    state->cv.notify_all();
    return FALSE_VAL;
}

}  // namespace

ResolvedImageFile try_resolve_local_image_file(std::string_view text) {
    std::string candidate;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find_first_of("\r\n", start);
        if (end == std::string::npos) end = text.size();
        std::string_view line = text.substr(start, end - start);
        start = end + 1;

        if (line == "copy" || line == "cut" || line.starts_with("#")) continue;
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
        if (line.empty()) continue;

        candidate = std::string(line);
        break;
    }

    if (candidate.empty()) return {};

    std::filesystem::path file_path;
    if (candidate.starts_with("file://")) {
        std::string raw = candidate.substr(7);
        if (raw.starts_with("localhost/")) raw = raw.substr(9);
        std::string decoded;
        decoded.reserve(raw.size());
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == '%' && i + 2 < raw.size()) {
                auto hex_val = [](char c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                    return -1;
                };
                int h1 = hex_val(raw[i + 1]);
                int h2 = hex_val(raw[i + 2]);
                if (h1 >= 0 && h2 >= 0) {
                    decoded.push_back(static_cast<char>((h1 << 4) | h2));
                    i += 2;
                    continue;
                }
            }
            decoded.push_back(raw[i]);
        }
        file_path = decoded;
    } else if (candidate.starts_with("/")) {
        file_path = candidate;
    } else {
        return {};
    }

    std::error_code ec;
    if (!std::filesystem::exists(file_path, ec) || !std::filesystem::is_regular_file(file_path, ec)) {
        return {};
    }

    std::string ext = file_path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    std::string mime;
    if (ext == ".png") mime = "image/png";
    else if (ext == ".jpg" || ext == ".jpeg") mime = "image/jpeg";
    else if (ext == ".webp") mime = "image/webp";
    else if (ext == ".gif") mime = "image/gif";
    else if (ext == ".bmp") mime = "image/bmp";
    else return {};

    uintmax_t sz = std::filesystem::file_size(file_path, ec);
    if (ec || sz == 0 || sz > kMaxClipboardPayloadBytes) {
        return {};  // Enforce 50MB safety limit
    }

    return {true, file_path, mime};
}

WaylandClipboardBridge::WaylandClipboardBridge(Engine& engine, LogMessageCallback log_cb)
    : engine_(engine), log_cb_(std::move(log_cb)) {
    start_outbound_worker();
}

WaylandClipboardBridge::~WaylandClipboardBridge() {
    alive_->store(false);
    {
        std::lock_guard<std::mutex> lock(selective_cv_mutex_);
        is_destroyed_.store(true);
        selective_cv_.notify_all();
    }
    if (selective_restore_thread_.joinable()) {
        selective_restore_thread_.join();
    }
    stop_x11_background_monitor();
    stop_wayland_watcher();
    stop_outbound_worker();
}

void WaylandClipboardBridge::dispatch_to_gtk_thread(std::function<void()> fn) {
    if (!has_gtk_main_thread_ || std::this_thread::get_id() == gtk_main_thread_id_) {
        fn();
        return;
    }
    auto* ctx = new IdleTaskCtx{alive_, std::move(fn)};
    g_idle_add(idle_task_trampoline, ctx);
}

bool WaylandClipboardBridge::run_on_gtk_thread_sync(std::function<void()> fn, int timeout_ms) {
    if (!has_gtk_main_thread_ || std::this_thread::get_id() == gtk_main_thread_id_) {
        fn();
        return true;
    }
    auto state = std::make_shared<SyncIdleState>();
    state->alive = alive_;
    state->fn = std::move(fn);
    auto* holder = new std::shared_ptr<SyncIdleState>(state);
    g_idle_add(sync_idle_trampoline, holder);

    std::unique_lock<std::mutex> lock(state->mtx);
    return state->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&state] {
        return state->done;
    });
}

void WaylandClipboardBridge::start_outbound_worker() {
    if (push_thread_.joinable()) return;
    stop_push_.store(false);
    push_thread_ = std::thread(&WaylandClipboardBridge::outbound_worker_loop, this);
}

void WaylandClipboardBridge::stop_outbound_worker() {
    stop_push_.store(true);
    push_cv_.notify_all();
    if (push_thread_.joinable()) {
        if (std::this_thread::get_id() != push_thread_.get_id()) {
            push_thread_.join();
        } else {
            push_thread_.detach();
        }
    }
}

void WaylandClipboardBridge::enqueue_outbound_task(OutboundClipTask task) {
    {
        std::lock_guard<std::mutex> lock(push_mutex_);
        if (stop_push_.load()) return;
        // Coalesce burst duplicates in queue tail
        if (!push_queue_.empty() && push_queue_.back().hash == task.hash && !task.bypass_loopback) {
            return;
        }
        push_queue_.push_back(std::move(task));
    }
    push_cv_.notify_one();
}

bool WaylandClipboardBridge::should_suppress_auto_push(const std::array<uint8_t, 32>& hash) {
    std::lock_guard<std::mutex> lock(hash_mutex_);
    if (std::chrono::steady_clock::now() < suppress_until_) {
        return true;
    }
    if (has_applied_hash_ && last_applied_hash_ == hash) {
        return true;
    }
    if (has_applied_png_hash_ && last_applied_png_hash_ == hash) {
        return true;
    }
    if (has_sent_hash_ && last_sent_hash_ == hash) {
        return true;
    }
    return false;
}

bool WaylandClipboardBridge::consume_manual_pull_pending() {
    std::lock_guard<std::mutex> lock(hash_mutex_);
    if (manual_pull_until_ == std::chrono::steady_clock::time_point{}) {
        return false;
    }
    bool active = (std::chrono::steady_clock::now() <= manual_pull_until_);
    manual_pull_until_ = std::chrono::steady_clock::time_point{};
    return active;
}

void WaylandClipboardBridge::outbound_worker_loop() {
    while (!stop_push_.load()) {
        OutboundClipTask task{};
        {
            std::unique_lock<std::mutex> lock(push_mutex_);
            push_cv_.wait(lock, [this] {
                return stop_push_.load() || !push_queue_.empty();
            });
            if (stop_push_.load()) break;
            task = std::move(push_queue_.front());
            push_queue_.pop_front();
        }

        if (!task.bypass_loopback) {
            if (!pc_to_phone_auto_share_.load() ||
                is_applying_remote_.load() ||
                is_selective_pasting_.load() ||
                should_suppress_auto_push(task.hash)) {
                continue;
            }
        }

        bool ok = false;
        switch (task.kind) {
            case OutboundClipTask::Kind::Text: {
                ok = engine_.push_clipboard_text(task.text, task.bypass_loopback, task.is_pull_response);
                if (ok) {
                    std::lock_guard<std::mutex> lock(hash_mutex_);
                    last_sent_hash_ = task.hash;
                    has_sent_hash_ = true;
                }
                if (ok && log_cb_) {
                    std::string preview = task.text.substr(0, std::min<size_t>(40, task.text.size()));
                    log_cb_(task.source_tag + "已同步文本 (" +
                            std::to_string(task.text.size()) + " B): " + preview);
                }
                break;
            }
            case OutboundClipTask::Kind::ImageBytes: {
                ok = engine_.push_clipboard_bytes(
                    task.mime_type, task.bytes, false, task.bypass_loopback, task.is_pull_response);
                if (ok) {
                    std::lock_guard<std::mutex> lock(hash_mutex_);
                    last_sent_hash_ = task.hash;
                    has_sent_hash_ = true;
                }
                if (ok && log_cb_) {
                    log_cb_(task.source_tag + "已同步原始图像 (" + task.mime_type + ", " +
                            std::to_string(task.bytes.size()) + " B)");
                }
                break;
            }
            case OutboundClipTask::Kind::ImageFile: {
                ok = engine_.push_clipboard_file(
                    task.mime_type, task.file_path, false, task.bypass_loopback, task.is_pull_response);
                if (ok) {
                    std::lock_guard<std::mutex> lock(hash_mutex_);
                    last_sent_hash_ = task.hash;
                    has_sent_hash_ = true;
                }
                if (ok && log_cb_) {
                    log_cb_(task.source_tag + "已解析本地图片并同步 (" +
                            task.mime_type + "): " + task.file_path.filename().string());
                }
                break;
            }
        }
    }
}

bool WaylandClipboardBridge::is_valid_mime_type(std::string_view mime) {
    if (mime.empty() || mime.size() > 128) {
        return false;
    }
    bool has_slash = false;
    for (char c : mime) {
        if ((c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '_' || c == '-' || c == '+' || c == '.' || c == '/') {
            if (c == '/') {
                has_slash = true;
            }
        } else {
            return false;
        }
    }
    return has_slash;
}

bool WaylandClipboardBridge::is_wayland_session() {
    const char* wayland_display = std::getenv("WAYLAND_DISPLAY");
    if (wayland_display && wayland_display[0] != '\0') {
        return true;
    }
    const char* session_type = std::getenv("XDG_SESSION_TYPE");
    if (session_type && std::string_view(session_type) == "wayland") {
        return true;
    }
    return false;
}

bool WaylandClipboardBridge::has_wl_clipboard_tools() {
    return check_cmd_in_path("wl-copy");
}

bool WaylandClipboardBridge::has_wl_paste_tools() {
    return check_cmd_in_path("wl-paste");
}

bool WaylandClipboardBridge::has_xdotool() {
    return check_cmd_in_path("xdotool");
}

bool WaylandClipboardBridge::write_to_wl_copy(const std::string& mime_type, const uint8_t* data, size_t size) {
    if (!is_valid_mime_type(mime_type)) {
        return false;
    }
    int pfd[2];
    if (::pipe(pfd) != 0) {
        return false;
    }

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pfd[0]);
        ::close(pfd[1]);
        return false;
    }

    if (pid == 0) {
        ::close(pfd[1]);
        ::dup2(pfd[0], STDIN_FILENO);
        ::close(pfd[0]);

        int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDOUT_FILENO);
            ::dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) {
                ::close(devnull);
            }
        }

        const char* args[] = {"wl-copy", "--type", mime_type.c_str(), nullptr};
        ::execvp("wl-copy", const_cast<char* const*>(args));
        ::_exit(127);
    }

    ::close(pfd[0]);
    if (data && size > 0) {
        size_t written = 0;
        while (written < size) {
            ssize_t nw = ::write(pfd[1], data + written, size - written);
            if (nw < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (nw == 0) break;
            written += static_cast<size_t>(nw);
        }
    }
    ::close(pfd[1]);

    int status = 0;
    ::waitpid(pid, &status, 0);
    return WIFEXITED(status) && (WEXITSTATUS(status) == 0);
}

bool WaylandClipboardBridge::write_file_to_wl_copy(const std::string& mime_type, const std::string& file_path) {
    if (!is_valid_mime_type(mime_type)) {
        return false;
    }
    std::ifstream in(file_path, std::ios::binary);
    if (!in.is_open()) return false;

    int pfd[2];
    if (::pipe(pfd) != 0) return false;

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pfd[0]);
        ::close(pfd[1]);
        return false;
    }

    if (pid == 0) {
        ::close(pfd[1]);
        ::dup2(pfd[0], STDIN_FILENO);
        ::close(pfd[0]);

        int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDOUT_FILENO);
            ::dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) ::close(devnull);
        }

        const char* args[] = {"wl-copy", "--type", mime_type.c_str(), nullptr};
        ::execvp("wl-copy", const_cast<char* const*>(args));
        ::_exit(127);
    }

    ::close(pfd[0]);
    char buf[65536];
    while (in.good()) {
        in.read(buf, sizeof(buf));
        std::streamsize n = in.gcount();
        if (n <= 0) break;
        size_t written = 0;
        while (written < static_cast<size_t>(n)) {
            ssize_t nw = ::write(pfd[1], buf + written, static_cast<size_t>(n) - written);
            if (nw < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (nw == 0) break;
            written += static_cast<size_t>(nw);
        }
    }
    ::close(pfd[1]);

    int status = 0;
    ::waitpid(pid, &status, 0);
    return WIFEXITED(status) && (WEXITSTATUS(status) == 0);
}

void WaylandClipboardBridge::start_gtk_monitoring() {
    gtk_main_thread_id_ = std::this_thread::get_id();
    has_gtk_main_thread_ = true;

    GdkAtom clip_atom = gdk_atom_intern_static_string("CLIPBOARD");
    gtk_clip_ = gtk_clipboard_get(clip_atom);
    if (gtk_clip_) {
        g_signal_connect(gtk_clip_, "owner-change", on_gtk_owner_change, this);
    }

    // Start background XWayland / X11 XFixes monitor for background capture
    start_x11_background_monitor();

    // Spawn wl-paste background watcher if running on Wayland
    if (is_wayland_session()) {
        start_wayland_watcher();
    }
}

void WaylandClipboardBridge::on_gtk_owner_change(
    GtkClipboard* clipboard,
    GdkEvent* /*event*/,
    gpointer user_data) {
    auto* self = static_cast<WaylandClipboardBridge*>(user_data);
    if (!self->pc_to_phone_auto_share_.load()) {
        return;
    }
    if (self->is_applying_remote_.load() || self->is_selective_pasting_.load()) {
        return;
    }

    // Priority 1: Check raw image pixel data
    if (gtk_clipboard_wait_is_image_available(clipboard)) {
        GdkPixbuf* pixbuf = gtk_clipboard_wait_for_image(clipboard);
        if (pixbuf) {
            gchar* buffer = nullptr;
            gsize buffer_size = 0;
            void* error = nullptr;
            if (gdk_pixbuf_save_to_buffer(pixbuf, &buffer, &buffer_size, "png", &error, nullptr) && buffer && buffer_size > 0) {
                auto hash = l2a::sha256_bytes(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(buffer), buffer_size));
                if (!self->should_suppress_auto_push(hash)) {
                    OutboundClipTask task{};
                    task.kind = OutboundClipTask::Kind::ImageBytes;
                    task.mime_type = "image/png";
                    task.bytes.assign(reinterpret_cast<const uint8_t*>(buffer),
                                      reinterpret_cast<const uint8_t*>(buffer) + buffer_size);
                    task.hash = hash;
                    task.source_tag = "[剪贴板 -> 手机] ";
                    self->enqueue_outbound_task(std::move(task));
                }
            }
            if (buffer) g_free(buffer);
            g_object_unref(pixbuf);
            return;
        }
    }

    // Priority 2 & 3: Request text for Nautilus image path resolution or plain text
    gtk_clipboard_request_text(clipboard, on_gtk_text_received, self);
}

void WaylandClipboardBridge::on_gtk_text_received(
    GtkClipboard* /*clipboard*/,
    const gchar* text,
    gpointer user_data) {
    auto* self = static_cast<WaylandClipboardBridge*>(user_data);
    if (!text || text[0] == '\0' || !self->pc_to_phone_auto_share_.load()) {
        return;
    }
    if (self->is_applying_remote_.load() || self->is_selective_pasting_.load()) {
        return;
    }

    std::string text_str(text);

    // Priority 2: Check if text resolves to a local image file (Nautilus URI resolution)
    auto resolved = try_resolve_local_image_file(text_str);
    if (resolved.is_image) {
        auto digest_opt = l2a::sha256_file_range(resolved.path, 0, 0);
        if (digest_opt && !self->should_suppress_auto_push(*digest_opt)) {
            OutboundClipTask task{};
            task.kind = OutboundClipTask::Kind::ImageFile;
            task.mime_type = resolved.mime_type;
            task.file_path = resolved.path;
            task.hash = *digest_opt;
            task.source_tag = "[剪贴板 -> 手机] ";
            self->enqueue_outbound_task(std::move(task));
        }
        return;
    }

    // Priority 3: Plain text
    auto hash = l2a::sha256_string(text_str);
    if (!self->should_suppress_auto_push(hash)) {
        OutboundClipTask task{};
        task.kind = OutboundClipTask::Kind::Text;
        task.mime_type = "text/plain";
        task.text = std::move(text_str);
        task.hash = hash;
        task.source_tag = "[剪贴板 -> 手机] ";
        self->enqueue_outbound_task(std::move(task));
    }
}

bool WaylandClipboardBridge::push_current_clipboard(bool is_pull_response) {
    // 1. Try Wayland wl-paste --list-types first (supports images, URIs, text even without focus)
    if (is_wayland_session() && has_wl_paste_tools()) {
        auto cap = capture_via_wl_paste();
        if (cap.kind == CapturedWaylandClipboard::Kind::ImageBytes) {
            bool ok = engine_.push_clipboard_bytes(
                cap.mime_type, cap.bytes, false, /*bypass_loopback=*/true, is_pull_response);
            if (ok && log_cb_) {
                log_cb_("[手动发送] 已成功向手机发送当前图片剪贴板 (" + cap.mime_type + ", " +
                        std::to_string(cap.bytes.size()) + " B)");
            }
            return ok;
        }
        if (cap.kind == CapturedWaylandClipboard::Kind::ImageFile) {
            bool ok = engine_.push_clipboard_file(
                cap.mime_type, cap.file_path, false, /*bypass_loopback=*/true, is_pull_response);
            if (ok && log_cb_) {
                log_cb_("[手动发送] 已成功发送本地图片本体: " + cap.file_path.filename().string());
            }
            return ok;
        }
        if (cap.kind == CapturedWaylandClipboard::Kind::Text && !cap.text.empty()) {
            bool ok = engine_.push_clipboard_text(cap.text, /*bypass_loopback=*/true, is_pull_response);
            if (ok && log_cb_) {
                std::string preview = cap.text.substr(0, std::min<size_t>(40, cap.text.size()));
                log_cb_("[手动发送] 已成功发送文本剪贴板: " + preview);
            }
            return ok;
        }
    }

    // 2. Fallback to GTK clipboard (marshaled safely onto GTK main thread)
    if (!gtk_clip_) {
        if (has_gtk_main_thread_) {
            run_on_gtk_thread_sync([this]() {
                GdkAtom clip_atom = gdk_atom_intern_static_string("CLIPBOARD");
                gtk_clip_ = gtk_clipboard_get(clip_atom);
            });
        }
    }
    if (!gtk_clip_) {
        return false;
    }

    std::vector<uint8_t> img_bytes;
    std::string clip_text;
    run_on_gtk_thread_sync([this, &img_bytes, &clip_text]() {
        if (!gtk_clip_) return;
        if (gtk_clipboard_wait_is_image_available(gtk_clip_)) {
            GdkPixbuf* pixbuf = gtk_clipboard_wait_for_image(gtk_clip_);
            if (pixbuf) {
                gchar* buffer = nullptr;
                gsize buffer_size = 0;
                void* error = nullptr;
                if (gdk_pixbuf_save_to_buffer(pixbuf, &buffer, &buffer_size, "png", &error, nullptr) &&
                    buffer && buffer_size > 0) {
                    img_bytes.assign(reinterpret_cast<const uint8_t*>(buffer),
                                     reinterpret_cast<const uint8_t*>(buffer) + buffer_size);
                }
                if (buffer) g_free(buffer);
                g_object_unref(pixbuf);
                return;
            }
        }
        gchar* raw_text = gtk_clipboard_wait_for_text(gtk_clip_);
        if (raw_text) {
            clip_text.assign(raw_text);
            g_free(raw_text);
        }
    });

    if (!img_bytes.empty()) {
        bool ok = engine_.push_clipboard_bytes(
            "image/png", img_bytes, false, /*bypass_loopback=*/true, is_pull_response);
        if (ok && log_cb_) {
            log_cb_("[手动发送] 已成功向手机发送当前图片剪贴板 (image/png, " +
                    std::to_string(img_bytes.size()) + " B)");
        }
        return ok;
    }

    if (!clip_text.empty()) {
        auto resolved = try_resolve_local_image_file(clip_text);
        if (resolved.is_image) {
            bool ok = engine_.push_clipboard_file(
                resolved.mime_type, resolved.path, false, /*bypass_loopback=*/true, is_pull_response);
            if (ok && log_cb_) {
                log_cb_("[手动发送] 已成功发送本地图片本体: " + resolved.path.filename().string());
            }
            return ok;
        }
        bool ok = engine_.push_clipboard_text(clip_text, /*bypass_loopback=*/true, is_pull_response);
        if (ok && log_cb_) {
            std::string preview = clip_text.substr(0, std::min<size_t>(40, clip_text.size()));
            log_cb_("[手动发送] 已成功发送文本剪贴板: " + preview);
        }
        return ok;
    }

    return false;
}

bool WaylandClipboardBridge::pull_remote_clipboard() {
    {
        std::lock_guard<std::mutex> lock(hash_mutex_);
        manual_pull_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    }
    return engine_.pull_remote_clipboard();
}

void WaylandClipboardBridge::perform_selective_paste(
    const ClipboardHistoryItem& item_to_paste,
    const std::optional<ClipboardHistoryItem>& item_0) {
    uint64_t seq;
    {
        std::lock_guard<std::mutex> lock(selective_cv_mutex_);
        seq = ++selective_seq_;
        selective_cv_.notify_all();
    }

    is_selective_pasting_.store(true);

    // Apply item_to_paste to clipboard
    if (item_to_paste.type == ClipContentType::Text) {
        if (has_wl_clipboard_tools()) {
            write_to_wl_copy("text/plain", reinterpret_cast<const uint8_t*>(item_to_paste.full_text.data()), item_to_paste.full_text.size());
        }
        if (gtk_clip_) {
            std::string txt = item_to_paste.full_text;
            run_on_gtk_thread_sync([this, txt]() {
                if (gtk_clip_) {
                    gtk_clipboard_set_text(gtk_clip_, txt.c_str(), static_cast<gint>(txt.size()));
                }
            });
        }
    } else if (item_to_paste.type == ClipContentType::Image) {
        std::filesystem::path img_p = item_to_paste.image_path;
        if (img_p.empty() && !item_to_paste.thumbnail_bytes.empty()) {
            std::filesystem::path cache_dir = get_clip_cache_dir();
            img_p = cache_dir / ("img_thumb_" + std::to_string(item_to_paste.id) + ".png");
            std::ofstream out(img_p, std::ios::binary | std::ios::trunc);
            if (out.is_open()) {
                out.write(reinterpret_cast<const char*>(item_to_paste.thumbnail_bytes.data()), item_to_paste.thumbnail_bytes.size());
            }
        }
        if (!img_p.empty()) {
            if (has_wl_clipboard_tools()) {
                write_file_to_wl_copy(item_to_paste.mime_type, img_p.string());
            }
            if (gtk_clip_) {
                std::string p_str = img_p.string();
                run_on_gtk_thread_sync([this, p_str]() {
                    if (!gtk_clip_) return;
                    void* err = nullptr;
                    GdkPixbuf* pix = gdk_pixbuf_new_from_file(p_str.c_str(), &err);
                    if (pix) {
                        gtk_clipboard_set_image(gtk_clip_, pix);
                        g_object_unref(pix);
                    }
                });
            }
        }
    }

    if (log_cb_) {
        log_cb_("[选择性粘贴] 已载入历史条目 #" + std::to_string(item_to_paste.id) +
                " 至剪贴板（3 秒租期，绝不覆盖原最新剪贴板内容）");
    }

    // Attempt synthetic Ctrl+V via xdotool if available
    if (has_xdotool()) {
        auto alive = alive_;
        std::thread([alive]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (alive->load()) {
                ::system("xdotool key --clearmodifiers ctrl+v 2>/dev/null");
            }
        }).detach();
    }

    // Schedule 3-second automatic restoration of original Item #0 non-blockingly
    if (selective_restore_thread_.joinable()) {
        selective_restore_thread_.join();
    }
    selective_restore_thread_ = std::thread([this, item_0, seq]() {
        {
            std::unique_lock<std::mutex> lock(selective_cv_mutex_);
            selective_cv_.wait_for(lock, std::chrono::seconds(3), [this, seq]() {
                return is_destroyed_.load() || selective_seq_.load() != seq;
            });
        }
        if (is_destroyed_.load() || selective_seq_.load() != seq) {
            return;
        }

        if (item_0.has_value()) {
            if (item_0->type == ClipContentType::Text) {
                if (has_wl_clipboard_tools()) {
                    write_to_wl_copy("text/plain", reinterpret_cast<const uint8_t*>(item_0->full_text.data()), item_0->full_text.size());
                }
                if (gtk_clip_) {
                    std::string txt = item_0->full_text;
                    run_on_gtk_thread_sync([this, txt]() {
                        if (gtk_clip_) {
                            gtk_clipboard_set_text(gtk_clip_, txt.c_str(), static_cast<gint>(txt.size()));
                        }
                    });
                }
            } else if (item_0->type == ClipContentType::Image && !item_0->image_path.empty()) {
                if (has_wl_clipboard_tools()) {
                    write_file_to_wl_copy(item_0->mime_type, item_0->image_path.string());
                }
                if (gtk_clip_) {
                    std::string p_str = item_0->image_path.string();
                    run_on_gtk_thread_sync([this, p_str]() {
                        if (!gtk_clip_) return;
                        void* err = nullptr;
                        GdkPixbuf* pix = gdk_pixbuf_new_from_file(p_str.c_str(), &err);
                        if (pix) {
                            gtk_clipboard_set_image(gtk_clip_, pix);
                            g_object_unref(pix);
                        }
                    });
                }
            }
        }
        if (selective_seq_.load() == seq) {
            {
                std::lock_guard<std::mutex> lock(hash_mutex_);
                suppress_until_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            }
            is_selective_pasting_.store(false);
            if (log_cb_) {
                log_cb_("[选择性粘贴] 3 秒租期结束，已自动恢复原剪贴板内容 (条目 #0 保留置顶)");
            }
        }
    });
}

void WaylandClipboardBridge::apply_remote_clipboard(const ReceivedClipboardItem& item) {
    bool is_manual = item.is_pull_response || consume_manual_pull_pending();
    if (!phone_to_pc_auto_share_.load() && !is_manual) {
        if (log_cb_) {
            log_cb_("[手机 -> 电脑] 手机到电脑自动共享已关闭，跳过写入系统剪贴板。");
        }
        return;
    }

    if (is_manual && log_cb_) {
        log_cb_("[手机 -> 电脑] 收到手动拉取的剪贴板内容，写入系统剪贴板。");
    }

    if (!is_valid_mime_type(item.mime_type)) {
        if (log_cb_) {
            log_cb_("[安全拦截] 丢弃非法或包含恶意字符的剪贴板 MIME 类型: " + item.mime_type);
        }
        return;
    }

    // Set anti-loopback hash, reset last_sent_hash_, and arm suppression window
    {
        std::lock_guard<std::mutex> lock(hash_mutex_);
        last_applied_hash_ = item.sha256;
        has_applied_hash_ = true;
        has_applied_png_hash_ = false;
        has_sent_hash_ = false;
        suppress_until_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
    }
    is_applying_remote_.store(true);

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
            if (has_wl_clipboard_tools()) {
                write_to_wl_copy("text/plain", reinterpret_cast<const uint8_t*>(text.data()), text.size());
            }
            if (gtk_clip_) {
                run_on_gtk_thread_sync([this, text]() {
                    if (gtk_clip_) {
                        gtk_clipboard_set_text(gtk_clip_, text.c_str(), static_cast<gint>(text.size()));
                    }
                });
            }

            if (log_cb_) {
                std::string preview = text.substr(0, std::min<size_t>(40, text.size()));
                log_cb_("[手机 -> 剪贴板] 收到文本 (" + std::to_string(text.size()) + " B): " + preview);
            }
        }
    } else if (item.mime_type.rfind("image/", 0) == 0) {
        bool ok = false;
        std::filesystem::path img_path = item.temp_file_path;
        if (img_path.empty() && !item.data.empty() && gtk_clip_) {
            std::filesystem::path cache_dir = get_clip_cache_dir();
            img_path = cache_dir / ("remote_" + l2a::to_hex(item.sha256) + extension_for_mime(item.mime_type));
            std::error_code ec;
            if (!std::filesystem::exists(img_path, ec)) {
                std::ofstream out(img_path, std::ios::binary | std::ios::trunc);
                if (out.is_open()) {
                    out.write(reinterpret_cast<const char*>(item.data.data()),
                              static_cast<std::streamsize>(item.data.size()));
                }
            }
        }

        if (!item.temp_file_path.empty()) {
            ok = write_file_to_wl_copy(item.mime_type, item.temp_file_path.string());
        } else if (!item.data.empty()) {
            ok = write_to_wl_copy(item.mime_type, item.data.data(), item.data.size());
        }

        if (gtk_clip_ && !img_path.empty()) {
            std::string p_str = img_path.string();
            run_on_gtk_thread_sync([this, p_str]() {
                if (!gtk_clip_) return;
                void* err = nullptr;
                GdkPixbuf* pix = gdk_pixbuf_new_from_file(p_str.c_str(), &err);
                if (pix) {
                    // Record re-encoded PNG hash so GTK owner-change won't echo back a re-encoded PNG
                    gchar* png_buf = nullptr;
                    gsize png_sz = 0;
                    void* png_err = nullptr;
                    if (gdk_pixbuf_save_to_buffer(pix, &png_buf, &png_sz, "png", &png_err, nullptr) &&
                        png_buf && png_sz > 0) {
                        auto png_hash = l2a::sha256_bytes(
                            std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(png_buf), png_sz));
                        std::lock_guard<std::mutex> lock(hash_mutex_);
                        last_applied_png_hash_ = png_hash;
                        has_applied_png_hash_ = true;
                    }
                    if (png_buf) g_free(png_buf);
                    gtk_clipboard_set_image(gtk_clip_, pix);
                    g_object_unref(pix);
                }
            });
        }

        if (ok && log_cb_) {
            log_cb_("[手机 -> 剪贴板] 已通过 wl-copy 写入二进制图片 (" +
                    item.mime_type + ", " + std::to_string(item.total_size) + " B)");
        }
    }

    {
        std::lock_guard<std::mutex> lock(hash_mutex_);
        suppress_until_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
    }
    is_applying_remote_.store(false);
}

void WaylandClipboardBridge::start_x11_background_monitor() {
    if (x11_thread_.joinable()) return;
    stop_x11_.store(false);
    x11_thread_ = std::thread([this]() { x11_monitor_loop(); });
}

void WaylandClipboardBridge::stop_x11_background_monitor() {
    stop_x11_.store(true);
    if (x11_thread_.joinable()) {
        x11_thread_.join();
    }
}

void WaylandClipboardBridge::x11_monitor_loop() {
    Display* dpy = ::XOpenDisplay(nullptr);
    if (!dpy) {
        dpy = ::XOpenDisplay(":0");
    }
    if (!dpy) {
        return;  // X11 display not available in current environment
    }

    void* xfixes_lib = ::dlopen("libXfixes.so.3", RTLD_LAZY | RTLD_LOCAL);
    if (!xfixes_lib) {
        xfixes_lib = ::dlopen("libXfixes.so", RTLD_LAZY | RTLD_LOCAL);
    }
    if (!xfixes_lib) {
        ::XCloseDisplay(dpy);
        return;
    }

    auto xfixes_query = reinterpret_cast<PfnXFixesQueryExtension>(::dlsym(xfixes_lib, "XFixesQueryExtension"));
    auto xfixes_select = reinterpret_cast<PfnXFixesSelectSelectionInput>(::dlsym(xfixes_lib, "XFixesSelectSelectionInput"));

    int xfixes_event_base = 0;
    int xfixes_error_base = 0;
    if (!xfixes_query || !xfixes_select || !xfixes_query(dpy, &xfixes_event_base, &xfixes_error_base)) {
        ::dlclose(xfixes_lib);
        ::XCloseDisplay(dpy);
        return;
    }

    Window root = ::XDefaultRootWindow(dpy);
    Window helper_win = ::XCreateSimpleWindow(dpy, root, -10, -10, 1, 1, 0, 0, 0);
    Atom clip_atom = ::XInternAtom(dpy, "CLIPBOARD", False);
    Atom utf8_atom = ::XInternAtom(dpy, "UTF8_STRING", False);
    Atom string_atom = XA_STRING;
    Atom targets_atom = ::XInternAtom(dpy, "TARGETS", False);
    Atom uri_list_atom = ::XInternAtom(dpy, "text/uri-list", False);
    Atom img_png_atom = ::XInternAtom(dpy, "image/png", False);
    Atom img_jpeg_atom = ::XInternAtom(dpy, "image/jpeg", False);
    Atom targets_prop_atom = ::XInternAtom(dpy, "L2A_TARGETS_PROP", False);
    Atom prop_atom = ::XInternAtom(dpy, "L2A_SELECTION_PROP", False);

    xfixes_select(dpy, root, clip_atom, kXFixesSetSelectionOwnerNotifyMask);
    ::XFlush(dpy);

    if (log_cb_) {
        log_cb_("[XFixes/XWayland] 后台全局剪贴板捕获服务已激活 (无焦点无感同步就绪)");
    }

    int x11_fd = ConnectionNumber(dpy);
    Atom pending_target = utf8_atom;

    while (!stop_x11_.load()) {
        while (::XPending(dpy) > 0) {
            XEvent ev;
            ::XNextEvent(dpy, &ev);

            if (ev.type == xfixes_event_base + kXFixesSelectionNotify) {
                auto* notify = reinterpret_cast<XFixesSelectionNotifyEventABI*>(&ev);
                if (notify->owner != helper_win &&
                    !is_applying_remote_.load() &&
                    !is_selective_pasting_.load() &&
                    pc_to_phone_auto_share_.load()) {
                    ::XConvertSelection(dpy, clip_atom, targets_atom, targets_prop_atom, helper_win, CurrentTime);
                    ::XFlush(dpy);
                }
            } else if (ev.type == SelectionNotify) {
                if (is_applying_remote_.load() || is_selective_pasting_.load() || !pc_to_phone_auto_share_.load()) {
                    continue;
                }

                if (ev.xselection.property == targets_prop_atom) {
                    Atom actual_type = None;
                    int actual_format = 0;
                    unsigned long nitems = 0;
                    unsigned long bytes_after = 0;
                    unsigned char* prop_data = nullptr;

                    Atom chosen = utf8_atom;
                    if (::XGetWindowProperty(
                            dpy, helper_win, targets_prop_atom, 0, 4096, False,
                            AnyPropertyType, &actual_type, &actual_format,
                            &nitems, &bytes_after, &prop_data) == Success && prop_data) {
                        if (actual_format == 32 && nitems > 0) {
                            auto* atoms = reinterpret_cast<Atom*>(prop_data);
                            auto has_atom = [&](Atom target) {
                                return std::find(atoms, atoms + nitems, target) != (atoms + nitems);
                            };
                            if (has_atom(img_png_atom)) chosen = img_png_atom;
                            else if (has_atom(img_jpeg_atom)) chosen = img_jpeg_atom;
                            else if (has_atom(uri_list_atom)) chosen = uri_list_atom;
                            else if (has_atom(utf8_atom)) chosen = utf8_atom;
                            else if (has_atom(string_atom)) chosen = string_atom;
                        }
                        ::XFree(prop_data);
                    }
                    ::XDeleteProperty(dpy, helper_win, targets_prop_atom);

                    pending_target = chosen;
                    ::XConvertSelection(dpy, clip_atom, chosen, prop_atom, helper_win, CurrentTime);
                    ::XFlush(dpy);
                } else if (ev.xselection.property == None && ev.xselection.target == targets_atom) {
                    // Selection owner didn't support TARGETS; fall back to UTF8_STRING
                    pending_target = utf8_atom;
                    ::XConvertSelection(dpy, clip_atom, utf8_atom, prop_atom, helper_win, CurrentTime);
                    ::XFlush(dpy);
                } else if (ev.xselection.property == prop_atom) {
                    Atom actual_type = None;
                    int actual_format = 0;
                    unsigned long nitems = 0;
                    unsigned long bytes_after = 0;
                    unsigned char* prop_data = nullptr;

                    if (::XGetWindowProperty(
                            dpy, helper_win, prop_atom, 0,
                            static_cast<long>(kMaxClipboardPayloadBytes / 4), False,
                            AnyPropertyType, &actual_type, &actual_format,
                            &nitems, &bytes_after, &prop_data) == Success && prop_data) {
                        size_t byte_len = nitems * static_cast<size_t>(actual_format / 8);
                        if (byte_len > 0 && actual_format == 8 &&
                            (actual_type == img_png_atom || actual_type == img_jpeg_atom ||
                             pending_target == img_png_atom || pending_target == img_jpeg_atom)) {
                            std::string mime = (actual_type == img_jpeg_atom || pending_target == img_jpeg_atom)
                                                   ? "image/jpeg"
                                                   : "image/png";
                            std::vector<uint8_t> bytes(prop_data, prop_data + byte_len);
                            auto hash = l2a::sha256_bytes(bytes);
                            if (!should_suppress_auto_push(hash)) {
                                OutboundClipTask task{};
                                task.kind = OutboundClipTask::Kind::ImageBytes;
                                task.mime_type = std::move(mime);
                                task.bytes = std::move(bytes);
                                task.hash = hash;
                                task.source_tag = "[XFixes后台 -> 手机] ";
                                enqueue_outbound_task(std::move(task));
                            }
                        } else if (byte_len > 0 && actual_format == 8) {
                            std::string text(reinterpret_cast<const char*>(prop_data), byte_len);
                            auto resolved = try_resolve_local_image_file(text);
                            if (resolved.is_image) {
                                auto digest_opt = l2a::sha256_file_range(resolved.path, 0, 0);
                                if (digest_opt && !should_suppress_auto_push(*digest_opt)) {
                                    OutboundClipTask task{};
                                    task.kind = OutboundClipTask::Kind::ImageFile;
                                    task.mime_type = resolved.mime_type;
                                    task.file_path = resolved.path;
                                    task.hash = *digest_opt;
                                    task.source_tag = "[XFixes后台 -> 手机] ";
                                    enqueue_outbound_task(std::move(task));
                                }
                            } else {
                                auto hash = l2a::sha256_string(text);
                                if (!should_suppress_auto_push(hash)) {
                                    OutboundClipTask task{};
                                    task.kind = OutboundClipTask::Kind::Text;
                                    task.mime_type = "text/plain";
                                    task.text = std::move(text);
                                    task.hash = hash;
                                    task.source_tag = "[XFixes后台 -> 手机] ";
                                    enqueue_outbound_task(std::move(task));
                                }
                            }
                        }
                        ::XFree(prop_data);
                    }
                    ::XDeleteProperty(dpy, helper_win, prop_atom);
                }
            }
        }

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(x11_fd, &fds);
        struct timeval tv { 0, 100000 };  // 100ms
        ::select(x11_fd + 1, &fds, nullptr, nullptr, &tv);
    }

    ::XDestroyWindow(dpy, helper_win);
    ::XCloseDisplay(dpy);
    ::dlclose(xfixes_lib);
}

void WaylandClipboardBridge::start_wayland_watcher() {
    if (watcher_thread_.joinable()) return;
    stop_watcher_.store(false);

    if (!has_wl_paste_tools()) return;

    fifo_path_ = "/tmp/l2a_clip_" + std::to_string(::getpid()) + "_" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".fifo";
    if (::mkfifo(fifo_path_.c_str(), 0600) != 0) return;

    pid_t pid = ::fork();
    if (pid < 0) {
        ::unlink(fifo_path_.c_str());
        fifo_path_.clear();
        return;
    }

    if (pid == 0) {
        int devnull = ::open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            ::dup2(devnull, STDIN_FILENO);
            ::dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) ::close(devnull);
        }

        const char* args[] = {
            "wl-paste",
            "--watch",
            "/bin/sh",
            "-c",
            "echo 1 > \"$1\"",
            "sh",
            fifo_path_.c_str(),
            nullptr
        };
        ::execvp("wl-paste", const_cast<char* const*>(args));
        ::_exit(127);
    }

    watcher_pid_ = pid;
    watcher_thread_ = std::thread([this]() { wayland_watcher_loop(); });
}

void WaylandClipboardBridge::stop_wayland_watcher() {
    stop_watcher_.store(true);
    if (watcher_pid_ > 0) {
        ::kill(watcher_pid_, SIGTERM);
        int status = 0;
        ::waitpid(watcher_pid_, &status, 0);
        watcher_pid_ = 0;
    }

    if (!fifo_path_.empty()) {
        int dummy = ::open(fifo_path_.c_str(), O_WRONLY | O_NONBLOCK);
        if (dummy >= 0) ::close(dummy);
    }

    if (watcher_thread_.joinable()) {
        watcher_thread_.join();
    }

    if (!fifo_path_.empty()) {
        ::unlink(fifo_path_.c_str());
        fifo_path_.clear();
    }
}

void WaylandClipboardBridge::wayland_watcher_loop() {
    while (!stop_watcher_.load()) {
        int fd = ::open(fifo_path_.c_str(), O_RDONLY);
        if (fd < 0) {
            if (stop_watcher_.load()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        char buf[256];
        while (!stop_watcher_.load()) {
            ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n <= 0) break;
        }
        ::close(fd);

        if (stop_watcher_.load()) break;
        if (!pc_to_phone_auto_share_.load() || is_applying_remote_.load() || is_selective_pasting_.load()) {
            continue;
        }

        auto cap = capture_via_wl_paste();
        if (cap.kind == CapturedWaylandClipboard::Kind::ImageBytes) {
            auto hash = l2a::sha256_bytes(cap.bytes);
            if (!should_suppress_auto_push(hash)) {
                OutboundClipTask task{};
                task.kind = OutboundClipTask::Kind::ImageBytes;
                task.mime_type = std::move(cap.mime_type);
                task.bytes = std::move(cap.bytes);
                task.hash = hash;
                task.source_tag = "[Wayland后台 -> 手机] ";
                enqueue_outbound_task(std::move(task));
            }
        } else if (cap.kind == CapturedWaylandClipboard::Kind::ImageFile) {
            auto digest_opt = l2a::sha256_file_range(cap.file_path, 0, 0);
            if (digest_opt && !should_suppress_auto_push(*digest_opt)) {
                OutboundClipTask task{};
                task.kind = OutboundClipTask::Kind::ImageFile;
                task.mime_type = std::move(cap.mime_type);
                task.file_path = std::move(cap.file_path);
                task.hash = *digest_opt;
                task.source_tag = "[Wayland后台 -> 手机] ";
                enqueue_outbound_task(std::move(task));
            }
        } else if (cap.kind == CapturedWaylandClipboard::Kind::Text && !cap.text.empty()) {
            auto hash = l2a::sha256_string(cap.text);
            if (!should_suppress_auto_push(hash)) {
                OutboundClipTask task{};
                task.kind = OutboundClipTask::Kind::Text;
                task.mime_type = "text/plain";
                task.text = std::move(cap.text);
                task.hash = hash;
                task.source_tag = "[Wayland后台 -> 手机] ";
                enqueue_outbound_task(std::move(task));
            }
        }
    }
}

}  // namespace l2a::linux_app
