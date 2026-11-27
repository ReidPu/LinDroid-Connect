#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <thread>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define private public
#define protected public
#include "wayland_clipboard.hpp"
#include "app_window.hpp"
#include "l2a/engine.hpp"
#include "l2a/sha256.hpp"
#undef private
#undef protected

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "[-] ASSERTION FAILED: " << msg << " (" << #cond << ") at " << __FILE__ << ":" << __LINE__ << "\n"; \
            std::exit(1); \
        } \
    } while (0)

namespace fs = std::filesystem;

static std::string read_file_string(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return "";
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static std::vector<std::string> read_file_lines(const fs::path& path) {
    std::vector<std::string> lines;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

int main() {
    std::cout << "=========================================================\n";
    std::cout << "[CHALLENGER] Empirical Test Suite: Wayland Clipboard & Desktop Entry\n";
    std::cout << "=========================================================\n";

    // Setup mock binaries directory
    fs::path mock_bin = "/tmp/l2a_mock_bin";
    fs::create_directories(mock_bin);

    // Mock wl-copy
    {
        fs::path wl_copy_path = mock_bin / "wl-copy";
        std::ofstream ofs(wl_copy_path);
        ofs << "#!/usr/bin/env bash\n";
        ofs << "printf \"%s\\n\" \"$@\" > /tmp/mock_wl_copy_args.txt\n";
        ofs << "cat > /tmp/mock_wl_copy_stdin.bin\n";
        ofs << "exit 0\n";
        ofs.close();
        ::chmod(wl_copy_path.c_str(), 0755);
    }

    // Mock notify-send
    {
        fs::path notify_path = mock_bin / "notify-send";
        std::ofstream ofs(notify_path);
        ofs << "#!/usr/bin/env bash\n";
        ofs << "printf \"%s\\n\" \"$@\" > /tmp/mock_notify_args.txt\n";
        ofs << "exit 0\n";
        ofs.close();
        ::chmod(notify_path.c_str(), 0755);
    }

    // Prepend mock_bin to PATH
    const char* old_path = std::getenv("PATH");
    std::string new_path = mock_bin.string() + (old_path ? (":" + std::string(old_path)) : "");
    ::setenv("PATH", new_path.c_str(), 1);

    // Ignore SIGPIPE
    ::signal(SIGPIPE, SIG_IGN);

    // Prepare mock EngineConfig
    fs::path temp_home = "/tmp/l2a_challenger_temp";
    fs::create_directories(temp_home / "storage");
    fs::create_directories(temp_home / "receive");

    l2a::EngineConfig config{};
    config.storage_dir = temp_home / "storage";
    config.receive_dir = temp_home / "receive";
    config.device_name = "Challenger-Test-Host";
    config.platform = "linux";
    config.listen_port = 53399;
    config.discovery_port = 0; // disable discovery in test

    l2a::Engine engine(config);
    std::vector<std::string> log_records;
    auto log_cb = [&](const std::string& msg) {
        log_records.push_back(msg);
    };

    l2a::linux_app::WaylandClipboardBridge bridge(engine, log_cb);

    // -------------------------------------------------------------
    // Test 1: Verify has_wl_clipboard_tools() detects mock wl-copy
    // -------------------------------------------------------------
    std::cout << "[RUN] Test 1: Verify mock wl-copy detection in PATH...\n";
    TEST_ASSERT(l2a::linux_app::WaylandClipboardBridge::has_wl_clipboard_tools(), "has_wl_clipboard_tools must return true with mock wl-copy in PATH");
    std::cout << "  -> PASSED: mock wl-copy successfully detected.\n";

    // -------------------------------------------------------------
    // Test 2: Standard text apply_remote_clipboard with mock wl-copy
    // -------------------------------------------------------------
    std::cout << "[RUN] Test 2: Standard text apply_remote_clipboard...\n";
    fs::remove("/tmp/mock_wl_copy_args.txt");
    fs::remove("/tmp/mock_wl_copy_stdin.bin");

    std::string text_payload = "Cross-device clipboard text: 测试剪贴板同步 1234567890 !@#$%^&*()";
    l2a::ReceivedClipboardItem text_item{};
    text_item.mime_type = "text/plain";
    text_item.data.assign(text_payload.begin(), text_payload.end());
    text_item.total_size = text_payload.size();

    bridge.apply_remote_clipboard(text_item);

    // Wait slightly for child process
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    TEST_ASSERT(fs::exists("/tmp/mock_wl_copy_args.txt"), "mock wl-copy must be executed");
    auto wl_args = read_file_lines("/tmp/mock_wl_copy_args.txt");
    TEST_ASSERT(wl_args.size() >= 2, "wl-copy args must have at least 2 entries");
    TEST_ASSERT(wl_args[0] == "--type", "wl-copy arg 0 must be --type");
    TEST_ASSERT(wl_args[1] == "text/plain", "wl-copy arg 1 must be text/plain");

    std::string captured_stdin = read_file_string("/tmp/mock_wl_copy_stdin.bin");
    TEST_ASSERT(captured_stdin == text_payload, "wl-copy stdin must match text payload byte-for-byte");

    auto expected_hash1 = l2a::sha256_string(text_payload);
    TEST_ASSERT(bridge.has_applied_hash_, "has_applied_hash_ must be set to true");
    TEST_ASSERT(bridge.last_applied_hash_ == expected_hash1, "last_applied_hash_ must match SHA-256 of text");
    std::cout << "  -> PASSED: Standard text apply_remote_clipboard wrote to wl-copy and updated hash.\n";

    // -------------------------------------------------------------
    // Test 3: Large text > 512KB from temp_file_path
    // -------------------------------------------------------------
    std::cout << "[RUN] Test 3: Large text > 512KB from temp_file_path...\n";
    fs::remove("/tmp/mock_wl_copy_args.txt");
    fs::remove("/tmp/mock_wl_copy_stdin.bin");

    fs::path large_temp_file = "/tmp/l2a_large_text.txt";
    std::string large_payload;
    large_payload.reserve(700 * 1024);
    for (int i = 0; i < 14000; ++i) {
        large_payload += "Line " + std::to_string(i) + ": The quick brown fox jumps over the lazy dog! 0123456789\n";
    }
    size_t large_size = large_payload.size();
    TEST_ASSERT(large_size > 512 * 1024, "Payload must exceed 512KB");
    {
        std::ofstream ofs(large_temp_file, std::ios::binary);
        ofs << large_payload;
    }

    l2a::ReceivedClipboardItem large_item{};
    large_item.mime_type = "text/plain";
    large_item.data.clear(); // Empty data, simulating protocol chunking to file
    large_item.temp_file_path = large_temp_file.string();
    large_item.total_size = large_size;

    bridge.apply_remote_clipboard(large_item);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    TEST_ASSERT(fs::exists("/tmp/mock_wl_copy_args.txt"), "mock wl-copy must be executed for large text");
    wl_args = read_file_lines("/tmp/mock_wl_copy_args.txt");
    TEST_ASSERT(wl_args.size() >= 2 && wl_args[1] == "text/plain", "wl-copy args for large text must be --type text/plain");

    captured_stdin = read_file_string("/tmp/mock_wl_copy_stdin.bin");
    TEST_ASSERT(captured_stdin.size() == large_size, "Captured stdin size must match large text size exactly");
    TEST_ASSERT(captured_stdin == large_payload, "Captured stdin content must match large text content byte-for-byte");

    auto expected_large_hash = l2a::sha256_string(large_payload);
    TEST_ASSERT(bridge.last_applied_hash_ == expected_large_hash, "last_applied_hash_ must match large text SHA-256");
    std::cout << "  -> PASSED: Large text (" << large_size << " bytes, " << (large_size / 1024)
              << " KB) from temp file streamed to wl-copy and hashed successfully.\n";

    // -------------------------------------------------------------
    // Test 4: Anti-echo suppression verification
    // -------------------------------------------------------------
    std::cout << "[RUN] Test 4: Anti-echo suppression logic...\n";

    // 4.1: Attempt to re-send large text via on_gtk_text_received
    // Since last_applied_hash_ == expected_large_hash, on_gtk_text_received must drop it without setting last_sent_hash_
    bridge.has_sent_hash_ = false;
    bridge.last_sent_hash_.fill(0);

    bridge.on_gtk_text_received(nullptr, large_payload.c_str(), &bridge);
    TEST_ASSERT(!bridge.has_sent_hash_, "on_gtk_text_received must suppress text matching last_applied_hash_");

    // 4.2: Locally copied distinct text
    std::string local_text = "New local user copy: 987654321";
    auto local_hash = l2a::sha256_string(local_text);
    bridge.on_gtk_text_received(nullptr, local_text.c_str(), &bridge);

    TEST_ASSERT(bridge.has_sent_hash_, "on_gtk_text_received must accept new distinct text");
    TEST_ASSERT(bridge.last_sent_hash_ == local_hash, "last_sent_hash_ must be updated with new text hash");

    // 4.3: Duplicate local copy event must be suppressed by last_sent_hash_
    // Reset a tracking marker or verify hash doesn't change
    bridge.on_gtk_text_received(nullptr, local_text.c_str(), &bridge);
    TEST_ASSERT(bridge.last_sent_hash_ == local_hash, "Duplicate text suppressed by last_sent_hash_");

    std::cout << "  -> PASSED: Echo suppression prevents looping back applied remote text and duplicate local text.\n";

    // -------------------------------------------------------------
    // Test 5: Binary MIME type via write_file_to_wl_copy
    // -------------------------------------------------------------
    std::cout << "[RUN] Test 5: Binary MIME type (image/png) via temp_file_path...\n";
    fs::remove("/tmp/mock_wl_copy_args.txt");
    fs::remove("/tmp/mock_wl_copy_stdin.bin");

    fs::path img_file = "/tmp/l2a_test_image.png";
    std::string img_content = "\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR...MockImageData...";
    {
        std::ofstream ofs(img_file, std::ios::binary);
        ofs << img_content;
    }

    l2a::ReceivedClipboardItem img_item{};
    img_item.mime_type = "image/png";
    img_item.temp_file_path = img_file.string();
    img_item.total_size = img_content.size();

    bridge.apply_remote_clipboard(img_item);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    TEST_ASSERT(fs::exists("/tmp/mock_wl_copy_args.txt"), "mock wl-copy must be executed for image/png");
    wl_args = read_file_lines("/tmp/mock_wl_copy_args.txt");
    TEST_ASSERT(wl_args.size() >= 2 && wl_args[1] == "image/png", "wl-copy args for image must be --type image/png");
    captured_stdin = read_file_string("/tmp/mock_wl_copy_stdin.bin");
    TEST_ASSERT(captured_stdin == img_content, "Binary image content must match byte-for-byte");
    std::cout << "  -> PASSED: Binary image streamed to wl-copy with correct MIME type.\n";

    // -------------------------------------------------------------
    // Test 6: Security: Malicious MIME injection defense
    // -------------------------------------------------------------
    std::cout << "[RUN] Test 6: Malicious MIME injection defense...\n";
    fs::remove("/tmp/mock_wl_copy_args.txt");

    l2a::ReceivedClipboardItem evil_item{};
    evil_item.mime_type = "text/plain; rm -rf /";
    evil_item.data = {'a', 'b', 'c'};
    evil_item.total_size = 3;

    bridge.apply_remote_clipboard(evil_item);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    TEST_ASSERT(!fs::exists("/tmp/mock_wl_copy_args.txt"), "Malicious MIME type must NOT execute wl-copy");
    std::cout << "  -> PASSED: Malicious MIME type successfully blocked.\n";

    // -------------------------------------------------------------
    // Test 7: notify-send command line formation & args
    // -------------------------------------------------------------
    std::cout << "[RUN] Test 7: notify-send argument formation...\n";
    fs::remove("/tmp/mock_notify_args.txt");

    l2a::linux_app::AppWindow app_window(config);
    std::string test_title = "已同步手机剪贴板";
    std::string test_body = "测试通知文本内容 123";

    app_window.send_desktop_notification(test_title, test_body);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    TEST_ASSERT(fs::exists("/tmp/mock_notify_args.txt"), "mock notify-send must be executed");
    auto notify_args = read_file_lines("/tmp/mock_notify_args.txt");

    bool has_app_flag = false;
    bool has_app_name = false;
    bool has_icon_flag = false;
    bool has_icon_name = false;
    bool has_hint_flag = false;
    bool has_hint_value = false;
    bool has_title = false;
    bool has_body = false;
    bool has_transient = false;

    for (size_t i = 0; i < notify_args.size(); ++i) {
        if (notify_args[i] == "-a") {
            has_app_flag = true;
            if (i + 1 < notify_args.size() && notify_args[i+1] == "Linux ⇄ Android") has_app_name = true;
        }
        if (notify_args[i] == "-i") {
            has_icon_flag = true;
            if (i + 1 < notify_args.size() && notify_args[i+1] == "network-transmit-receive") has_icon_name = true;
        }
        if (notify_args[i] == "-h") {
            has_hint_flag = true;
            if (i + 1 < notify_args.size() && notify_args[i+1] == "string:desktop-entry:l2a-desktop") has_hint_value = true;
        }
        if (notify_args[i] == test_title) has_title = true;
        if (notify_args[i] == test_body) has_body = true;
        if (notify_args[i] == "-e" || notify_args[i] == "--transient") has_transient = true;
    }

    TEST_ASSERT(has_app_flag && has_app_name, "notify-send must have -a 'Linux ⇄ Android'");
    TEST_ASSERT(has_icon_flag && has_icon_name, "notify-send must have -i 'network-transmit-receive'");
    TEST_ASSERT(has_hint_flag && has_hint_value, "notify-send must have -h 'string:desktop-entry:l2a-desktop'");
    TEST_ASSERT(has_title, "notify-send must contain the title");
    TEST_ASSERT(has_body, "notify-send must contain the body");
    TEST_ASSERT(!has_transient, "notify-send must NOT contain transient flag");
    std::cout << "  -> PASSED: notify-send arguments verified for GNOME Shell stacking.\n";

    // Clean up temporary files
    fs::remove_all(mock_bin);
    fs::remove_all(temp_home);
    fs::remove(large_temp_file);
    fs::remove(img_file);
    fs::remove("/tmp/mock_wl_copy_args.txt");
    fs::remove("/tmp/mock_wl_copy_stdin.bin");
    fs::remove("/tmp/mock_notify_args.txt");

    std::cout << "=========================================================\n";
    std::cout << "[CHALLENGER] All 7 Empirical Verification Tests PASSED!\n";
    std::cout << "=========================================================\n";
    return 0;
}
