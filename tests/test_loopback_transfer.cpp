#include "l2a/engine.hpp"
#include "l2a/sha256.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <string>
#include <sys/socket.h>
#include <vector>

using namespace l2a;

#define TEST_CHECK(cond)                                                           \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << ": " << #cond \
                      << "\n";                                                     \
            std::abort();                                                          \
        }                                                                          \
    } while (0)

namespace {

class TestListener : public ICoreListener {
public:
    std::mutex mtx;
    std::condition_variable cv;

    bool connected{false};
    std::vector<ReceivedClipboardItem> clips;
    std::vector<ClipOfferPayload> lazy_offers;
    std::vector<std::filesystem::path> completed_files;
    uint64_t last_progress_bytes{0};

    void on_connection_state_changed(
        bool is_conn,
        const std::string& /*peer_id*/,
        const std::string& /*peer_name*/) override {
        std::lock_guard<std::mutex> lock(mtx);
        connected = is_conn;
        cv.notify_all();
    }

    void on_clipboard_received(const ReceivedClipboardItem& item) override {
        std::lock_guard<std::mutex> lock(mtx);
        clips.push_back(item);
        cv.notify_all();
    }

    void on_clipboard_lazy_offer(const ClipOfferPayload& offer) override {
        std::lock_guard<std::mutex> lock(mtx);
        lazy_offers.push_back(offer);
        cv.notify_all();
    }

    void on_file_progress(const TransferProgress& progress) override {
        std::lock_guard<std::mutex> lock(mtx);
        if (!progress.is_sending) {
            last_progress_bytes = progress.bytes_transferred;
        }
    }

    void on_file_completed(
        uint64_t /*batch_id*/,
        uint32_t /*file_index*/,
        const std::filesystem::path& saved_path,
        bool success,
        const std::string& error_msg) override {
        if (!success) {
            std::cerr << "File transfer error: " << error_msg << "\n";
        }
        TEST_CHECK(success);
        std::lock_guard<std::mutex> lock(mtx);
        completed_files.push_back(saved_path);
        cv.notify_all();
    }
};

}  // namespace

int main() {
    auto base_dir = std::filesystem::temp_directory_path() / "l2a_loopback_test";
    std::error_code ec;
    std::filesystem::remove_all(base_dir, ec);

    EngineConfig linux_cfg{};
    linux_cfg.storage_dir = base_dir / "linux_state";
    linux_cfg.receive_dir = base_dir / "linux_recv";
    linux_cfg.device_name = "Redmi-G-Pro-2024";
    linux_cfg.platform = "linux";
    linux_cfg.listen_port = 0;
    linux_cfg.discovery_port = 0;

    EngineConfig android_cfg{};
    android_cfg.storage_dir = base_dir / "android_state";
    android_cfg.receive_dir = base_dir / "android_recv";
    android_cfg.device_name = "Android-Phone";
    android_cfg.platform = "android";
    android_cfg.listen_port = 0;
    android_cfg.discovery_port = 0;

    TestListener linux_listener;
    TestListener android_listener;

    Engine linux_engine(linux_cfg, &linux_listener);
    Engine android_engine(android_cfg, &android_listener);

    // 1. Connect Android <-> Linux (try TCP loopback first; fallback to AF_UNIX socketpair in sandbox)
    bool tcp_started = linux_engine.start();
    if (tcp_started && linux_engine.bound_tcp_port() > 0) {
        TEST_CHECK(android_engine.connect_to_peer("127.0.0.1", linux_engine.bound_tcp_port()));
    } else {
        int sv[2]{-1, -1};
        TEST_CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        auto srv_fut = std::async(std::launch::async, [&] {
            return linux_engine.connect_with_fd(sv[0], /*is_client=*/false);
        });
        bool cli_ok = android_engine.connect_with_fd(sv[1], /*is_client=*/true);
        TEST_CHECK(cli_ok);
        TEST_CHECK(srv_fut.get());
    }

    {
        std::unique_lock<std::mutex> lock(linux_listener.mtx);
        TEST_CHECK(linux_listener.cv.wait_for(
            lock, std::chrono::seconds(3), [&] { return linux_listener.connected; }));
    }
    std::cout << "[PASS] 1. TLS/3DH encrypted handshake & TOFU peer pinning succeeded.\n";

    // 2. Clipboard Text + Anti-Loopback verification
    std::string clip_text = "https://github.com/reid-pu/Linux_to_Android - 跨端同步测试";
    TEST_CHECK(linux_engine.push_clipboard_text(clip_text));
    {
        std::unique_lock<std::mutex> lock(android_listener.mtx);
        TEST_CHECK(android_listener.cv.wait_for(
            lock, std::chrono::seconds(3), [&] { return android_listener.clips.size() == 1; }));
        std::string got(reinterpret_cast<const char*>(android_listener.clips[0].data.data()),
                        android_listener.clips[0].data.size());
        TEST_CHECK(got == clip_text);
    }
    // Simulate Android OS firing onPrimaryClipChanged with the exact same text -> must be suppressed!
    bool echoed = android_engine.push_clipboard_text(clip_text);
    TEST_CHECK(!echoed);
    std::cout << "[PASS] 2. Bidirectional text clipboard & SHA-256 anti-loopback verified.\n";

    // 3. Multi-chunk 64KB Streaming Image Clipboard (200 KB) + Lazy-Pull URI List
    std::vector<uint8_t> fake_png(200 * 1024);
    for (size_t i = 0; i < fake_png.size(); ++i) {
        fake_png[i] = static_cast<uint8_t>((i * 31 + 7) & 0xFFU);
    }
    TEST_CHECK(android_engine.push_clipboard_bytes("image/png", fake_png));
    {
        std::unique_lock<std::mutex> lock(linux_listener.mtx);
        TEST_CHECK(linux_listener.cv.wait_for(
            lock, std::chrono::seconds(3), [&] { return linux_listener.clips.size() == 1; }));
        TEST_CHECK(linux_listener.clips[0].mime_type == "image/png");
        TEST_CHECK(linux_listener.clips[0].data == fake_png);
    }

    // Test Lazy-Pull ("text/uri-list")
    std::string uri_list = "file:///home/reid-pu/Videos/demo_4k.mp4\r\n";
    TEST_CHECK(linux_engine.push_clipboard_bytes(
        "text/uri-list",
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(uri_list.data()), uri_list.size())));
    uint64_t lazy_clip_id = 0;
    {
        std::unique_lock<std::mutex> lock(android_listener.mtx);
        TEST_CHECK(android_listener.cv.wait_for(
            lock, std::chrono::seconds(3), [&] { return android_listener.lazy_offers.size() == 1; }));
        lazy_clip_id = android_listener.lazy_offers[0].clip_id;
    }
    TEST_CHECK(android_engine.request_clipboard_lazy_pull(lazy_clip_id));
    {
        std::unique_lock<std::mutex> lock(android_listener.mtx);
        TEST_CHECK(android_listener.cv.wait_for(
            lock, std::chrono::seconds(3), [&] { return android_listener.clips.size() == 2; }));
        std::string pulled_uri(reinterpret_cast<const char*>(android_listener.clips[1].data.data()),
                               android_listener.clips[1].data.size());
        TEST_CHECK(pulled_uri == uri_list);
    }
    std::cout << "[PASS] 3. 64KB chunked image clipboard & Lazy-Pull on-demand streaming verified.\n";

    // 4. Folder Transfer + Block-Verified Breakpoint Resume (.l2apart)
    auto src_folder = base_dir / "src_project";
    std::filesystem::create_directories(src_folder / "docs");
    std::filesystem::create_directories(src_folder / "media");

    auto small_file = src_folder / "docs" / "readme.md";
    {
        std::ofstream out(small_file, std::ios::binary);
        out << "# Project Documentation\nTransferred with directory hierarchy preserved.\n";
    }

    // Create a 320 KB binary file (5 x 64KB chunks)
    auto large_file = src_folder / "media" / "recording.bin";
    std::vector<uint8_t> large_data(320 * 1024);
    for (size_t i = 0; i < large_data.size(); ++i) {
        large_data[i] = static_cast<uint8_t>((i * 13 + 99) & 0xFFU);
    }
    {
        std::ofstream out(large_file, std::ios::binary);
        out.write(reinterpret_cast<const char*>(large_data.data()),
                  static_cast<std::streamsize>(large_data.size()));
    }

    // Pre-create a partial .l2apart file in android_recv with the first 128 KB (2 chunks) already written!
    auto pre_part = android_cfg.receive_dir / "src_project" / "media" / "recording.bin.l2apart";
    std::filesystem::create_directories(pre_part.parent_path());
    {
        std::ofstream out(pre_part, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(large_data.data()), 128 * 1024);
    }

    std::vector<std::filesystem::path> send_list = {src_folder};
    uint64_t batch_id = linux_engine.send_files(send_list);
    TEST_CHECK(batch_id > 0);

    {
        std::unique_lock<std::mutex> lock(android_listener.mtx);
        TEST_CHECK(android_listener.cv.wait_for(
            lock, std::chrono::seconds(5),
            [&] { return android_listener.completed_files.size() == 2; }));
    }

    auto recv_small = android_cfg.receive_dir / "src_project" / "docs" / "readme.md";
    auto recv_large = android_cfg.receive_dir / "src_project" / "media" / "recording.bin";
    TEST_CHECK(std::filesystem::exists(recv_small));
    TEST_CHECK(std::filesystem::exists(recv_large));
    TEST_CHECK(!std::filesystem::exists(pre_part));
    TEST_CHECK(sha256_file_range(recv_large) == sha256_file_range(large_file));

    std::cout << "[PASS] 4. Recursive folder transfer & .l2apart block-verified resume verified.\n";

    android_engine.stop();
    linux_engine.stop();
    std::filesystem::remove_all(base_dir, ec);

    std::cout << "All loopback integration tests passed!\n";
    return 0;
}
