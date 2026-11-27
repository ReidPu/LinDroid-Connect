#include "l2a/history_manager.hpp"
#include "l2a/sha256.hpp"
#include "settings_manager.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#define TEST_ASSERT(cond) \
    do { \
        if (!(cond)) { \
            std::cerr << "Assertion failed: " << #cond << " at " << __FILE__ << ":" << __LINE__ << "\n"; \
            std::abort(); \
        } \
    } while (0)

namespace {

void test_settings_manager_defaults_and_clamping() {
    std::cout << "[TEST] 1. SettingsManager defaults, clamping, and persistence...\n";

    auto temp_dir = std::filesystem::temp_directory_path() /
                    ("l2a_test_settings_" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir, ec);

    {
        l2a::linux_app::SettingsManager mgr(temp_dir);
        // Defaults
        TEST_ASSERT(mgr.pc_to_phone_auto_share() == true);
        TEST_ASSERT(mgr.phone_to_pc_auto_share() == true);
        TEST_ASSERT(mgr.history_limit() == 20);

        // Clamping checks [5, 200]
        mgr.set_history_limit(0);
        TEST_ASSERT(mgr.history_limit() == 5);

        mgr.set_history_limit(4);
        TEST_ASSERT(mgr.history_limit() == 5);

        mgr.set_history_limit(201);
        TEST_ASSERT(mgr.history_limit() == 200);

        mgr.set_history_limit(9999);
        TEST_ASSERT(mgr.history_limit() == 200);

        mgr.set_history_limit(50);
        TEST_ASSERT(mgr.history_limit() == 50);

        // Toggles
        mgr.set_pc_to_phone_auto_share(false);
        mgr.set_phone_to_pc_auto_share(false);
        TEST_ASSERT(mgr.pc_to_phone_auto_share() == false);
        TEST_ASSERT(mgr.phone_to_pc_auto_share() == false);
    }

    // Verify reload from disk
    {
        l2a::linux_app::SettingsManager mgr2(temp_dir);
        TEST_ASSERT(mgr2.pc_to_phone_auto_share() == false);
        TEST_ASSERT(mgr2.phone_to_pc_auto_share() == false);
        TEST_ASSERT(mgr2.history_limit() == 50);
    }

    std::filesystem::remove_all(temp_dir, ec);
    std::cout << "  -> PASSED: SettingsManager defaults, [5, 200] clamping and disk reload verified.\n";
}

void test_clipboard_history_manager_capacity_and_lru() {
    std::cout << "[TEST] 2. ClipboardHistoryManager capacity clamping and LRU eviction...\n";

    auto temp_file = std::filesystem::temp_directory_path() /
                     ("l2a_test_hist_cap_" + std::to_string(::getpid()) + ".json");
    std::error_code ec;
    std::filesystem::remove(temp_file, ec);

    {
        l2a::ClipboardHistoryManager hist(temp_file, 20);
        TEST_ASSERT(hist.max_history_limit() == 20);

        // Clamping test
        hist.set_max_history_limit(2);
        TEST_ASSERT(hist.max_history_limit() == 5);

        hist.set_max_history_limit(500);
        TEST_ASSERT(hist.max_history_limit() == 200);

        hist.set_max_history_limit(10);
        TEST_ASSERT(hist.max_history_limit() == 10);

        // Push 15 items
        for (int i = 0; i < 15; ++i) {
            std::string content = "History Item #" + std::to_string(i);
            hist.add_text_item(
                l2a::ClipOrigin::Local,
                content,
                l2a::sha256_string(content)
            );
        }

        auto items = hist.get_items();
        TEST_ASSERT(items.size() == 10);
        // Latest item (#14) must be at index 0
        TEST_ASSERT(items.front().text_preview == "History Item #14");
        // Oldest preserved item (#5) must be at the end
        TEST_ASSERT(items.back().text_preview == "History Item #5");
    }

    std::filesystem::remove(temp_file, ec);
    std::cout << "  -> PASSED: Capacity clamping and LRU eviction order verified.\n";
}

void test_clipboard_history_manager_pinning_and_dynamic_trim() {
    std::cout << "[TEST] 3. ClipboardHistoryManager pinning preservation and dynamic trimming...\n";

    auto temp_file = std::filesystem::temp_directory_path() /
                     ("l2a_test_hist_pin_" + std::to_string(::getpid()) + ".json");
    std::error_code ec;
    std::filesystem::remove(temp_file, ec);

    {
        l2a::ClipboardHistoryManager hist(temp_file, 5);
        for (int i = 1; i <= 5; ++i) {
            std::string content = "PinnedCandidate #" + std::to_string(i);
            hist.add_text_item(l2a::ClipOrigin::Local, content, l2a::sha256_string(content));
        }

        auto items = hist.get_items();
        TEST_ASSERT(items.size() == 5);

        // Pin the oldest item ("PinnedCandidate #1")
        uint64_t pinned_id = items.back().id;
        TEST_ASSERT(hist.pin_item(pinned_id, true));

        // Push 10 new items
        for (int i = 6; i <= 15; ++i) {
            std::string content = "NewItem #" + std::to_string(i);
            hist.add_text_item(l2a::ClipOrigin::Local, content, l2a::sha256_string(content));
        }

        items = hist.get_items();
        TEST_ASSERT(items.size() == 5);
        bool found_pinned = false;
        for (const auto& it : items) {
            if (it.id == pinned_id) {
                found_pinned = true;
                TEST_ASSERT(it.is_pinned == true);
                TEST_ASSERT(it.text_preview == "PinnedCandidate #1");
            }
        }
        TEST_ASSERT(found_pinned && "Pinned item must NOT be evicted even under capacity pressure");

        // Dynamic trimming test
        hist.set_max_history_limit(20);
        for (int i = 16; i <= 30; ++i) {
            std::string content = "Item20 #" + std::to_string(i);
            hist.add_text_item(l2a::ClipOrigin::Local, content, l2a::sha256_string(content));
        }
        TEST_ASSERT(hist.get_items().size() == 20);

        // Dynamically trim down to 5
        hist.set_max_history_limit(5);
        items = hist.get_items();
        TEST_ASSERT(items.size() == 5);

        // Pinned item must still be alive!
        found_pinned = false;
        for (const auto& it : items) {
            if (it.id == pinned_id) found_pinned = true;
        }
        TEST_ASSERT(found_pinned && "Pinned item must survive dynamic capacity reduction");
    }

    std::filesystem::remove(temp_file, ec);
    std::cout << "  -> PASSED: Pinning preservation and dynamic limit reduction verified.\n";
}

void test_clipboard_history_manager_selective_paste_token() {
    std::cout << "[TEST] 4. ClipboardHistoryManager selective paste token tracking...\n";

    l2a::ClipboardHistoryManager hist("", 10);
    std::string content = "Selective Paste Target";
    uint64_t target_id = hist.add_text_item(l2a::ClipOrigin::Remote, content, l2a::sha256_string(content));

    TEST_ASSERT(!hist.is_selective_paste_active(target_id));
    std::string token = hist.mark_selective_paste(target_id);
    TEST_ASSERT(!token.empty());

    TEST_ASSERT(hist.is_selective_paste_active(target_id));
    auto active_id = hist.active_selective_paste_id();
    TEST_ASSERT(active_id.has_value() && *active_id == target_id);

    hist.clear_selective_paste();
    TEST_ASSERT(!hist.is_selective_paste_active(target_id));
    TEST_ASSERT(!hist.active_selective_paste_id().has_value());

    std::cout << "  -> PASSED: Selective paste token set, query, active check, and clear verified.\n";
}

void test_clipboard_history_manager_thread_safety() {
    std::cout << "[TEST] 5. ClipboardHistoryManager multi-threaded stress and safety...\n";

    l2a::ClipboardHistoryManager hist("", 50);
    std::vector<std::thread> workers;

    for (int t = 0; t < 6; ++t) {
        workers.emplace_back([&hist, t]() {
            for (int i = 0; i < 100; ++i) {
                std::string text = "Thread " + std::to_string(t) + " Msg " + std::to_string(i);
                uint64_t id = hist.add_text_item(l2a::ClipOrigin::Local, text, l2a::sha256_string(text));
                if (i % 20 == 0) {
                    hist.pin_item(id, true);
                }
                if (i % 30 == 0) {
                    (void)hist.to_json();
                }
            }
        });
    }

    for (auto& w : workers) {
        w.join();
    }

    TEST_ASSERT(hist.get_items().size() <= 50);
    std::cout << "  -> PASSED: Multi-threaded stress (6 threads x 100 ops) completed without deadlock or crash.\n";
}

void test_clipboard_history_manager_persistence() {
    std::cout << "[TEST] 6. ClipboardHistoryManager JSON serialization and disk persistence...\n";

    auto temp_file = std::filesystem::temp_directory_path() /
                     ("l2a_test_hist_persist_" + std::to_string(::getpid()) + ".json");
    std::error_code ec;
    std::filesystem::remove(temp_file, ec);

    uint64_t text_id{0};
    uint64_t img_id{0};
    {
        l2a::ClipboardHistoryManager hist(temp_file, 20);

        // Push text
        std::string txt = "Hello Persistent World";
        text_id = hist.add_text_item(l2a::ClipOrigin::Local, txt, l2a::sha256_string(txt));

        // Push image
        std::vector<uint8_t> png_bytes = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x01};
        img_id = hist.add_image_item(l2a::ClipOrigin::Remote, "image/png", png_bytes, "", l2a::sha256_bytes(png_bytes));

        auto items = hist.get_items();
        TEST_ASSERT(items.size() == 2);
        // Index 0 is newest (img_id), index 1 is older (text_id)
        TEST_ASSERT(items[0].id == img_id);
        TEST_ASSERT(items[1].id == text_id);

        hist.pin_item(text_id, true);
    }

    // Load in fresh manager instance
    {
        l2a::ClipboardHistoryManager hist2(temp_file, 20);
        auto items = hist2.get_items();
        TEST_ASSERT(items.size() == 2);

        TEST_ASSERT(items[0].id == img_id);
        TEST_ASSERT(items[0].mime_type == "image/png");
        TEST_ASSERT(items[0].origin == l2a::ClipOrigin::Remote);
        TEST_ASSERT(items[0].type == l2a::ClipContentType::Image);

        TEST_ASSERT(items[1].id == text_id);
        TEST_ASSERT(items[1].mime_type == "text/plain");
        TEST_ASSERT(items[1].origin == l2a::ClipOrigin::Local);
        TEST_ASSERT(items[1].type == l2a::ClipContentType::Text);
        TEST_ASSERT(items[1].text_preview == "Hello Persistent World");
        TEST_ASSERT(items[1].is_pinned == true);

        // Test delete single item
        TEST_ASSERT(hist2.delete_item(img_id));
        TEST_ASSERT(hist2.get_items().size() == 1);

        // Test clear all
        hist2.clear_all();
        TEST_ASSERT(hist2.get_items().empty());
    }

    std::filesystem::remove(temp_file, ec);
    std::cout << "  -> PASSED: Full round-trip disk persistence, item reload, remove, and clear verified.\n";
}

}  // namespace

int main() {
    std::cout << "=========================================================\n";
    std::cout << "Running Settings & History Manager Test Suite (M1-C3)\n";
    std::cout << "=========================================================\n";

    test_settings_manager_defaults_and_clamping();
    test_clipboard_history_manager_capacity_and_lru();
    test_clipboard_history_manager_pinning_and_dynamic_trim();
    test_clipboard_history_manager_selective_paste_token();
    test_clipboard_history_manager_thread_safety();
    test_clipboard_history_manager_persistence();

    std::cout << "=========================================================\n";
    std::cout << "All Settings & History Tests PASSED (6/6)!\n";
    std::cout << "=========================================================\n";
    return 0;
}
