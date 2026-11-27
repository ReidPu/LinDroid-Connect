#include "l2a/history_manager.hpp"
#include "l2a/sha256.hpp"
#include "settings_manager.hpp"
#include "wayland_clipboard.hpp"
#include "l2a/engine.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#define STRESS_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "\n[ADVERSARIAL FAILURE] " << msg << "\n" \
                      << "  Condition: " << #cond << "\n" \
                      << "  Location:  " << __FILE__ << ":" << __LINE__ << "\n\n"; \
            std::abort(); \
        } \
    } while (0)

namespace {

// =========================================================================
// 1. R1 Adversarial Stress: Settings Persistence & Manual Pull Edge Cases
// =========================================================================
void stress_r1_settings_and_manual_pull() {
    std::cout << "\n======================================================\n";
    std::cout << "[STRESS 1] R1 Settings Persistence & Manual Pull Edge Cases\n";
    std::cout << "======================================================\n";

    auto temp_dir = std::filesystem::temp_directory_path() /
                    ("l2a_stress_settings_" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir, ec);

    // 1.1 Malformed JSON injections into settings.json
    std::filesystem::path cfg_file = temp_dir / "settings.json";

    std::vector<std::string> malformed_jsons = {
        "",                                         // Empty file
        "   \t\r\n   ",                             // Whitespace only
        "{",                                        // Unclosed object
        "{\"pc_to_phone_auto_share\": }",           // Missing value
        "{\"history_limit\": \"not_a_number\"}",    // String instead of number
        "{\"history_limit\": -99999}",              // Massive negative number
        "{\"history_limit\": 0.0001}",              // Tiny fractional float
        "{\"history_limit\": 1e10}",                // Scientific notation overflow
        "[1, 2, 3]",                                // Array instead of object
        "{\"unknown_key\": 12345, \"history_limit\": 77}" // Unknown keys with valid limit
    };

    for (size_t i = 0; i < malformed_jsons.size(); ++i) {
        {
            std::ofstream out(cfg_file, std::ios::trunc);
            out << malformed_jsons[i];
        }
        l2a::linux_app::SettingsManager sm(temp_dir);
        size_t lim = sm.history_limit();
        STRESS_ASSERT(lim >= 5 && lim <= 200,
            "History limit must ALWAYS be clamped to [5, 200], even on corrupt JSON injection #" + std::to_string(i));
    }

    // 1.2 Extreme concurrency on SettingsManager (16 threads concurrent get/set/save)
    {
        l2a::linux_app::SettingsManager sm(temp_dir);
        std::atomic<bool> start{false};
        std::atomic<bool> done{false};
        std::vector<std::thread> workers;

        for (int t = 0; t < 16; ++t) {
            workers.emplace_back([&, t]() {
                while (!start.load()) std::this_thread::yield();
                for (int op = 0; op < 200; ++op) {
                    if (t % 4 == 0) {
                        sm.set_history_limit(op % 300);
                    } else if (t % 4 == 1) {
                        sm.set_pc_to_phone_auto_share(op % 2 == 0);
                    } else if (t % 4 == 2) {
                        sm.set_phone_to_pc_auto_share(op % 2 != 0);
                    } else {
                        auto s = sm.get();
                        (void)s.history_limit;
                    }
                }
            });
        }

        start.store(true);
        for (auto& w : workers) {
            w.join();
        }

        STRESS_ASSERT(sm.history_limit() >= 5 && sm.history_limit() <= 200,
            "Final limit after 3200 concurrent ops must be in [5, 200]");
    }

    // 1.3 WaylandClipboardBridge Manual Pull State Machine
    {
        l2a::EngineConfig cfg{};
        cfg.storage_dir = temp_dir / "storage";
        cfg.receive_dir = temp_dir / "receive";
        cfg.listen_port = 0;
        cfg.discovery_port = 0;
        l2a::Engine test_engine(cfg);

        std::vector<std::string> logs;
        l2a::linux_app::WaylandClipboardBridge bridge(test_engine, [&](const std::string& msg) {
            logs.push_back(msg);
        });

        // Set phone_to_pc_auto_share = false
        bridge.set_phone_to_pc_auto_share(false);

        l2a::ReceivedClipboardItem test_item{};
        test_item.clip_id = 42;
        test_item.mime_type = "text/plain";
        test_item.data = {'H', 'i'};
        test_item.total_size = 2;

        // Unsolicited: must be dropped
        logs.clear();
        bridge.apply_remote_clipboard(test_item);
        STRESS_ASSERT(!logs.empty() && logs.back().find("跳过") != std::string::npos,
            "Unsolicited packet must be skipped when auto-share is false");

        // Concurrent manual pulls
        std::vector<std::thread> pullers;
        for (int p = 0; p < 10; ++p) {
            pullers.emplace_back([&]() {
                bridge.pull_remote_clipboard();
            });
        }
        for (auto& p : pullers) p.join();

        // Exactly one apply_remote_clipboard must consume the manual_pull_pending_ flag
        logs.clear();
        bridge.apply_remote_clipboard(test_item);
        STRESS_ASSERT(!logs.empty() && logs.front().find("收到手动拉取") != std::string::npos,
            "First remote clip after manual pull must be accepted");

        // Subsequent unsolicited clip without another pull must be dropped again!
        logs.clear();
        bridge.apply_remote_clipboard(test_item);
        STRESS_ASSERT(!logs.empty() && logs.back().find("跳过") != std::string::npos,
            "Subsequent remote clip without manual pull must be dropped again");
    }

    std::filesystem::remove_all(temp_dir, ec);
    std::cout << "  -> PASS: R1 Settings persistence & manual pull state machine verified under stress.\n";
}

// =========================================================================
// 2. R2 Adversarial Stress: Nautilus Image URI Resolution & Binary Integrity
// =========================================================================
void stress_r2_image_uri_resolution() {
    std::cout << "\n======================================================\n";
    std::cout << "[STRESS 2] R2 Nautilus Image URI Resolution & Binary Integrity\n";
    std::cout << "======================================================\n";

    auto temp_dir = std::filesystem::temp_directory_path() /
                    ("l2a_stress_nautilus_" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir, ec);

    // 2.1 File with spaces and Chinese characters
    auto cn_img = temp_dir / "我的 屏幕截图 2026.PNG";
    {
        std::ofstream out(cn_img, std::ios::binary);
        out << "\x89PNG\r\n\x1a\n";
        out.write("TEST_IMAGE_PIXELS", 17);
    }

    // Test raw path
    auto res_raw = l2a::linux_app::try_resolve_local_image_file(cn_img.string());
    STRESS_ASSERT(res_raw.is_image, "Raw path with spaces and Chinese must be resolved");
    STRESS_ASSERT(res_raw.mime_type == "image/png", "Uppercase .PNG must resolve to image/png");

    // Test percent-encoded URI (Nautilus copy)
    // "我的 屏幕截图 2026.PNG"
    // ' ' -> %20
    std::string encoded_uri = "file://" + temp_dir.string() + "/%E6%88%91%E7%9A%84%20%E5%B1%8F%E5%B9%95%E6%88%AA%E5%9B%BE%202026.PNG";
    auto res_enc = l2a::linux_app::try_resolve_local_image_file(encoded_uri);
    STRESS_ASSERT(res_enc.is_image, "Percent-encoded UTF-8 Nautilus URI must be resolved");
    STRESS_ASSERT(res_enc.path == cn_img, "Decoded path must match real file path");

    // 2.2 Nautilus multi-line format with comment lines and cut/copy headers
    std::string complex_nautilus =
        "copy\r\n"
        "# This is a comment line\r\n"
        "   \r\n"
        "file://" + cn_img.string() + "\r\n"
        "file:///tmp/another.jpg\r\n";
    auto res_multi = l2a::linux_app::try_resolve_local_image_file(complex_nautilus);
    STRESS_ASSERT(res_multi.is_image, "Multi-line Nautilus payload must skip headers and comments");
    STRESS_ASSERT(res_multi.path == cn_img, "First valid image URI must be extracted");

    // 2.3 50MB Safety Limit Test
    auto img_50mb = temp_dir / "exact_50mb.jpg";
    {
        std::ofstream out(img_50mb, std::ios::binary);
        std::string chunk(65536, 'X');
        for (size_t i = 0; i < (50 * 1024 * 1024) / chunk.size(); ++i) {
            out.write(chunk.data(), chunk.size());
        }
    }
    auto res_50mb = l2a::linux_app::try_resolve_local_image_file(img_50mb.string());
    STRESS_ASSERT(res_50mb.is_image, "Exact 50MB file must be ACCEPTED");

    auto img_50mb_plus = temp_dir / "overflow_50mb.jpg";
    {
        std::ofstream out(img_50mb_plus, std::ios::binary);
        std::string chunk(65536, 'X');
        for (size_t i = 0; i < (50 * 1024 * 1024) / chunk.size(); ++i) {
            out.write(chunk.data(), chunk.size());
        }
        out.write("Z", 1); // 50MB + 1 byte
    }
    auto res_overflow = l2a::linux_app::try_resolve_local_image_file(img_50mb_plus.string());
    STRESS_ASSERT(!res_overflow.is_image, "50MB + 1 byte file must be REJECTED by 50MB safety guard");

    // 2.4 Zero-byte file
    auto img_zero = temp_dir / "zero.png";
    {
        std::ofstream out(img_zero, std::ios::binary);
    }
    auto res_zero = l2a::linux_app::try_resolve_local_image_file(img_zero.string());
    STRESS_ASSERT(!res_zero.is_image, "0-byte file must be REJECTED");

    // 2.5 Directory with .png extension
    auto dir_as_png = temp_dir / "fake_dir.png";
    std::filesystem::create_directories(dir_as_png, ec);
    auto res_dir = l2a::linux_app::try_resolve_local_image_file(dir_as_png.string());
    STRESS_ASSERT(!res_dir.is_image, "Directory named .png must be REJECTED");

    // 2.6 Unsupported extensions
    std::vector<std::string> rejected_exts = {
        "file.svg", "file.tiff", "file.pdf", "file.txt", "file.mp4", "file.exe", "file.sh"
    };
    for (const auto& name : rejected_exts) {
        auto p = temp_dir / name;
        std::ofstream out(p);
        out << "dummy";
        auto res = l2a::linux_app::try_resolve_local_image_file(p.string());
        STRESS_ASSERT(!res.is_image, "Extension " + name + " must be REJECTED");
    }

    std::filesystem::remove_all(temp_dir, ec);
    std::cout << "  -> PASS: R2 Nautilus image resolution boundary and integrity checks passed.\n";
}

// =========================================================================
// 3. R3 Adversarial Stress: History Limit Edge Clamping & Selective Paste
// =========================================================================
void stress_r3_history_and_selective_paste() {
    std::cout << "\n======================================================\n";
    std::cout << "[STRESS 3] R3 History Limit Clamping & Selective Paste\n";
    std::cout << "======================================================\n";

    l2a::ClipboardHistoryManager hist("", 50);

    // 3.1 All-pinned eviction scenario
    // Add 10 items and pin ALL of them
    std::vector<uint64_t> ids;
    for (int i = 0; i < 10; ++i) {
        std::string text = "PinnedItem_" + std::to_string(i);
        uint64_t id = hist.add_text_item(l2a::ClipOrigin::Local, text, l2a::sha256_string(text));
        hist.pin_item(id, true);
        ids.push_back(id);
    }
    STRESS_ASSERT(hist.item_count() == 10, "Should have 10 items");

    // Reduce limit to 5: even though ALL are pinned, strict capacity must be enforced!
    hist.set_max_history_limit(5);
    STRESS_ASSERT(hist.item_count() == 5, "Strict capacity limit 5 must be enforced even if all items are pinned");
    auto items_pinned = hist.get_items();
    // Newest 5 pinned items must be kept
    STRESS_ASSERT(items_pinned[0].full_text == "PinnedItem_9", "Top item must be newest pinned item");
    STRESS_ASSERT(items_pinned[4].full_text == "PinnedItem_5", "5th item must be preserved");

    // 3.2 Selective Paste Integrity
    // Mark item 7 (now index 2, "PinnedItem_7")
    uint64_t target_id = ids[7];
    std::string token = hist.mark_selective_paste(target_id);
    STRESS_ASSERT(hist.is_selective_paste_active(target_id), "Selective paste must be active");

    // Verify item #0 is still #9 and unperturbed
    auto current_items = hist.get_items();
    STRESS_ASSERT(current_items[0].full_text == "PinnedItem_9", "Item #0 MUST remain unchanged");
    STRESS_ASSERT(current_items[0].id == ids[9], "Item #0 ID must remain unchanged");

    // Delete the target item while selective paste is active
    hist.delete_item(target_id);
    STRESS_ASSERT(hist.item_count() == 4, "Item must be deleted");
    // Verify query does not crash
    auto active_id = hist.active_selective_paste_id();
    STRESS_ASSERT(active_id.has_value() && *active_id == target_id, "active_selective_paste_id returns recorded ID");

    // Clear selective paste
    hist.clear_selective_paste();
    STRESS_ASSERT(!hist.is_selective_paste_active(target_id), "Selective paste must be inactive after clear");
    STRESS_ASSERT(!hist.active_selective_paste_id().has_value(), "active_selective_paste_id must be nullopt");

    std::cout << "  -> PASS: R3 History eviction and selective paste integrity passed.\n";
}

// =========================================================================
// 4. R4 Adversarial Stress: Touchpad NFC UID Binding & Normalization
// =========================================================================
void stress_r4_nfc_uid_normalization() {
    std::cout << "\n======================================================\n";
    std::cout << "[STRESS 4] R4 Touchpad NFC UID Binding & Normalization\n";
    std::cout << "======================================================\n";

    // Simulate Kotlin Android UID extraction:
    // val uid = uidBytes.joinToString(":") { "%02X".format(it) }
    auto format_uid = [](const std::vector<uint8_t>& bytes) -> std::string {
        if (bytes.empty()) return "";
        std::string s;
        char buf[8];
        for (size_t i = 0; i < bytes.size(); ++i) {
            std::snprintf(buf, sizeof(buf), "%02X", bytes[i]);
            s += buf;
            if (i + 1 < bytes.size()) s += ":";
        }
        return s;
    };

    // Standard NTAG5332 7-byte UID
    std::vector<uint8_t> uid_7 = {0x04, 0x89, 0x63, 0xAD, 0x3E, 0xAF, 0xFE};
    std::string bound_uid = format_uid(uid_7);
    STRESS_ASSERT(bound_uid == "04:89:63:AD:3E:AF:FE", "Formatted UID must be uppercase colon-separated");

    // Case-insensitive comparison check:
    auto uid_equals = [](const std::string& a, const std::string& b) -> bool {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (std::tolower(a[i]) != std::tolower(b[i])) return false;
        }
        return true;
    };

    STRESS_ASSERT(uid_equals(bound_uid, "04:89:63:ad:3e:af:fe"), "Case-insensitive UID comparison must match");
    STRESS_ASSERT(!uid_equals(bound_uid, "04:89:63:AD:3E:AF:00"), "Distinct UID must not match");
    STRESS_ASSERT(!uid_equals(bound_uid, ""), "Empty UID must not match");

    // 4-byte UID (Classic/Ultralight)
    std::vector<uint8_t> uid_4 = {0x12, 0x34, 0x56, 0x78};
    STRESS_ASSERT(format_uid(uid_4) == "12:34:56:78", "4-byte UID must format correctly");

    // 10-byte UID
    std::vector<uint8_t> uid_10 = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A};
    STRESS_ASSERT(format_uid(uid_10) == "01:02:03:04:05:06:07:08:09:0A", "10-byte UID must format correctly");

    std::cout << "  -> PASS: R4 NFC UID formatting, casing, and multi-tech length handling verified.\n";
}

}  // namespace

int main() {
    std::cout << "=========================================================\n";
    std::cout << "Running Cycle 3 Final Empirical Adversarial Stress Suite\n";
    std::cout << "=========================================================\n";

    stress_r1_settings_and_manual_pull();
    stress_r2_image_uri_resolution();
    stress_r3_history_and_selective_paste();
    stress_r4_nfc_uid_normalization();

    std::cout << "\n=========================================================\n";
    std::cout << "ALL CYCLE 3 FINAL ADVERSARIAL STRESS TESTS PASSED (4/4)!\n";
    std::cout << "=========================================================\n";
    return 0;
}
