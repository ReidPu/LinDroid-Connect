#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <atomic>
#include <cassert>
#include <filesystem>
#include "l2a/engine.hpp"
#include "l2a/history_manager.hpp"
#include "wayland_clipboard.hpp"

using namespace l2a;
using namespace l2a::linux_app;

int main() {
    std::cout << "=========================================================\n";
    std::cout << "Adversarial Stress Test: WaylandClipboardBridge Concurrency\n";
    std::cout << "=========================================================\n";

    auto temp_dir = std::filesystem::temp_directory_path() / ("l2a_adv_stress_" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::create_directories(temp_dir, ec);

    EngineConfig cfg{};
    cfg.storage_dir = temp_dir / "storage";
    cfg.receive_dir = temp_dir / "receive";
    cfg.listen_port = 0;
    cfg.discovery_port = 0;
    Engine engine(cfg);

    // =========================================================================
    // SUITE 1: Rapid Consecutive Selective Paste (UI Freeze Detection)
    // =========================================================================
    std::cout << "\n[TEST 1] Testing Consecutive Selective Paste Calls for UI Freeze / Lost Wakeup...\n";
    {
        WaylandClipboardBridge bridge(engine, [](const std::string&) {});

        ClipboardHistoryItem item_top{};
        item_top.id = 1;
        item_top.mime_type = "text/plain";
        item_top.full_text = "Top original";

        int slow_count = 0;
        const int iterations = 30;

        for (int i = 0; i < iterations; ++i) {
            ClipboardHistoryItem item_k{};
            item_k.id = 100 + i;
            item_k.mime_type = "text/plain";
            item_k.full_text = "Item " + std::to_string(item_k.id);

            auto t0 = std::chrono::steady_clock::now();
            bridge.perform_selective_paste(item_k, item_top);
            auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t0).count();

            if (dt >= 100) {
                std::cout << "  [WARNING] Call " << i << " BLOCKED for " << dt << " ms (expected < 100ms)!\n";
                slow_count++;
            }
            // Small pause between clicks simulating rapid user interaction
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        std::cout << "  -> Result: " << slow_count << " / " << iterations 
                  << " calls suffered UI thread freeze (> 100ms)!\n";
        if (slow_count > 0) {
            std::cout << "  -> [VULNERABILITY CONFIRMED] UI thread freeze due to lost-wakeup / synchronous join.\n";
        } else {
            std::cout << "  -> Passed smoothly in this run.\n";
        }
    }

    // =========================================================================
    // SUITE 2: Rapid Lifetime Destruction vs Active Selective Paste
    // =========================================================================
    std::cout << "\n[TEST 2] Testing Rapid Bridge Destruction vs Active Selective Paste Thread...\n";
    {
        int destroyed_count = 0;
        int slow_destruct_count = 0;
        const int iterations = 30;

        for (int i = 0; i < iterations; ++i) {
            auto t0 = std::chrono::steady_clock::now();
            {
                WaylandClipboardBridge bridge(engine, [](const std::string&) {});
                ClipboardHistoryItem item_k{};
                item_k.id = 200 + i;
                item_k.mime_type = "text/plain";
                item_k.full_text = "Rapid Kill " + std::to_string(i);

                ClipboardHistoryItem item_0{};
                item_0.id = 1;
                item_0.mime_type = "text/plain";
                item_0.full_text = "Original";

                bridge.perform_selective_paste(item_k, item_0);
                // Immediately destruct the bridge while the restoration thread is starting/waiting
            }
            auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t0).count();
            if (dt >= 100) {
                std::cout << "  [WARNING] Destructor in iter " << i << " BLOCKED for " << dt << " ms!\n";
                slow_destruct_count++;
            }
            destroyed_count++;
        }
        std::cout << "  -> Destroyed " << destroyed_count << " bridges. Slow destructions: " 
                  << slow_destruct_count << "\n";
    }

    // =========================================================================
    // SUITE 3: Full 3-second Expiry & Integrity of Restoration
    // =========================================================================
    std::cout << "\n[TEST 3] Testing Natural 3-Second Expiry & Restoration State...\n";
    {
        WaylandClipboardBridge bridge(engine, [](const std::string&) {});
        ClipboardHistoryItem item_k{};
        item_k.id = 999;
        item_k.mime_type = "text/plain";
        item_k.full_text = "Selective 999";

        ClipboardHistoryItem item_0{};
        item_0.id = 1;
        item_0.mime_type = "text/plain";
        item_0.full_text = "Top Original";

        bridge.perform_selective_paste(item_k, item_0);
        assert(bridge.is_selective_pasting() == true);

        // Wait 3.5s for natural expiration
        std::this_thread::sleep_for(std::chrono::milliseconds(3500));
        assert(bridge.is_selective_pasting() == false);
        std::cout << "  -> PASSED: Natural 3-second expiration cleared is_selective_pasting_ flag.\n";
    }

    std::filesystem::remove_all(temp_dir, ec);
    std::cout << "\n=========================================================\n";
    std::cout << "Adversarial Stress Test Complete.\n";
    std::cout << "=========================================================\n";
    return 0;
}
