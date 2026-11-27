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

#define CHALLENGE_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "\n[CHALLENGE FAILURE] " << msg << "\n" \
                      << "  Condition: " << #cond << "\n" \
                      << "  Location:  " << __FILE__ << ":" << __LINE__ << "\n\n"; \
            std::abort(); \
        } \
    } while (0)

namespace {

// =========================================================================
// 1. Edge Capacity Limits & Strict Clamping [5, 200]
// =========================================================================
void challenge_edge_capacity_limits() {
    std::cout << "\n======================================================\n";
    std::cout << "[CHALLENGE 1] Edge Capacity Limits & Strict Clamping [5, 200]\n";
    std::cout << "======================================================\n";

    // 1.1 ClipboardHistoryManager Clamping
    {
        l2a::ClipboardHistoryManager hist("", 20);

        // Sub-minimum clamping: 0 -> 5
        hist.set_max_history_limit(0);
        CHALLENGE_ASSERT(hist.max_history_limit() == 5, "set_max_history_limit(0) must clamp to 5");

        // Sub-minimum clamping: 1 -> 5
        hist.set_max_history_limit(1);
        CHALLENGE_ASSERT(hist.max_history_limit() == 5, "set_max_history_limit(1) must clamp to 5");

        // Sub-minimum clamping: 4 -> 5
        hist.set_max_history_limit(4);
        CHALLENGE_ASSERT(hist.max_history_limit() == 5, "set_max_history_limit(4) must clamp to 5");

        // Boundary exact: 5 -> 5
        hist.set_max_history_limit(5);
        CHALLENGE_ASSERT(hist.max_history_limit() == 5, "set_max_history_limit(5) boundary must be 5");

        // Normal range: 50 -> 50
        hist.set_max_history_limit(50);
        CHALLENGE_ASSERT(hist.max_history_limit() == 50, "set_max_history_limit(50) must be 50");

        // Boundary exact: 200 -> 200
        hist.set_max_history_limit(200);
        CHALLENGE_ASSERT(hist.max_history_limit() == 200, "set_max_history_limit(200) boundary must be 200");

        // Super-maximum clamping: 201 -> 200
        hist.set_max_history_limit(201);
        CHALLENGE_ASSERT(hist.max_history_limit() == 200, "set_max_history_limit(201) must clamp to 200");

        // Extreme overflow clamping: 1,000,000 -> 200
        hist.set_max_history_limit(1'000'000);
        CHALLENGE_ASSERT(hist.max_history_limit() == 200, "set_max_history_limit(1000000) must clamp to 200");

        // Cast of negative value (e.g. static_cast<size_t>(-1))
        hist.set_max_history_limit(static_cast<size_t>(-1));
        CHALLENGE_ASSERT(hist.max_history_limit() == 200, "static_cast<size_t>(-1) must clamp to 200");
    }

    // 1.2 SettingsManager Clamping
    auto temp_dir = std::filesystem::temp_directory_path() /
                    ("l2a_challenger_settings_" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir, ec);

    {
        l2a::linux_app::SettingsManager sm(temp_dir);
        CHALLENGE_ASSERT(sm.history_limit() == 20, "SettingsManager default history_limit must be 20");

        sm.set_history_limit(0);
        CHALLENGE_ASSERT(sm.history_limit() == 5, "SettingsManager limit 0 must clamp to 5");

        sm.set_history_limit(4);
        CHALLENGE_ASSERT(sm.history_limit() == 5, "SettingsManager limit 4 must clamp to 5");

        sm.set_history_limit(5);
        CHALLENGE_ASSERT(sm.history_limit() == 5, "SettingsManager limit 5 must be 5");

        sm.set_history_limit(200);
        CHALLENGE_ASSERT(sm.history_limit() == 200, "SettingsManager limit 200 must be 200");

        sm.set_history_limit(201);
        CHALLENGE_ASSERT(sm.history_limit() == 200, "SettingsManager limit 201 must clamp to 200");

        sm.set_history_limit(99999);
        CHALLENGE_ASSERT(sm.history_limit() == 200, "SettingsManager limit 99999 must clamp to 200");
    }

    // 1.3 Malformed Settings File on Disk (Direct JSON injection)
    {
        std::filesystem::path cfg_file = temp_dir / "settings.json";

        // Inject limit = 1 into settings.json
        {
            std::ofstream out(cfg_file, std::ios::trunc);
            out << "{\n  \"history_limit\": 1\n}\n";
        }
        l2a::linux_app::SettingsManager sm_loaded(temp_dir);
        CHALLENGE_ASSERT(sm_loaded.history_limit() == 5,
            "SettingsManager loaded from disk with limit=1 must clamp to 5");

        // Inject limit = 9999 into settings.json
        {
            std::ofstream out(cfg_file, std::ios::trunc);
            out << "{\n  \"history_limit\": 9999\n}\n";
        }
        l2a::linux_app::SettingsManager sm_loaded2(temp_dir);
        CHALLENGE_ASSERT(sm_loaded2.history_limit() == 200,
            "SettingsManager loaded from disk with limit=9999 must clamp to 200");
    }

    std::filesystem::remove_all(temp_dir, ec);
    std::cout << "  -> PASS: All edge boundary conditions [5, 200] and clamping verified.\n";
}

// =========================================================================
// 2. Dynamic Pruning: 30 items -> limit to 10 -> verify 10 newest items
// =========================================================================
void challenge_dynamic_pruning() {
    std::cout << "\n======================================================\n";
    std::cout << "[CHALLENGE 2] Dynamic Pruning (30 items -> limit 10)\n";
    std::cout << "======================================================\n";

    l2a::ClipboardHistoryManager hist("", 50);
    std::vector<uint64_t> item_ids;

    // Add 30 items sequentially
    for (int i = 0; i < 30; ++i) {
        std::string text = "Sequential_Dynamic_Item_" + std::to_string(i);
        uint64_t id = hist.add_text_item(l2a::ClipOrigin::Local, text, l2a::sha256_string(text));
        item_ids.push_back(id);
    }

    CHALLENGE_ASSERT(hist.item_count() == 30, "Must have exactly 30 items before pruning");
    auto before_items = hist.get_items();
    CHALLENGE_ASSERT(before_items.size() == 30, "before_items size must be 30");

    // Index 0 must be item #29 (the latest item pushed)
    CHALLENGE_ASSERT(before_items[0].full_text == "Sequential_Dynamic_Item_29",
        "Index 0 must be the newest item (#29)");
    // Index 29 must be item #0 (the oldest item pushed)
    CHALLENGE_ASSERT(before_items[29].full_text == "Sequential_Dynamic_Item_0",
        "Index 29 must be the oldest item (#0)");

    std::cout << "  Added 30 items. Now applying dynamic pruning: set_max_history_limit(10)...\n";
    hist.set_max_history_limit(10);

    CHALLENGE_ASSERT(hist.max_history_limit() == 10, "max_history_limit must now be 10");
    CHALLENGE_ASSERT(hist.item_count() == 10, "item_count must now be exactly 10");

    auto after_items = hist.get_items();
    CHALLENGE_ASSERT(after_items.size() == 10, "after_items must contain exactly 10 items");

    // Verify EXACT contents: items 20 through 29 in reverse-chronological order
    for (size_t idx = 0; idx < 10; ++idx) {
        int expected_num = 29 - static_cast<int>(idx);
        std::string expected_text = "Sequential_Dynamic_Item_" + std::to_string(expected_num);
        CHALLENGE_ASSERT(after_items[idx].full_text == expected_text,
            "Pruned item at index " + std::to_string(idx) + " must be '" + expected_text + "'");
        CHALLENGE_ASSERT(after_items[idx].id == item_ids[expected_num],
            "Pruned item ID must match expected ID");
    }

    // Verify older items (#0 through #19) are completely pruned and not retrievable
    for (int i = 0; i < 20; ++i) {
        auto opt = hist.get_item(item_ids[i]);
        CHALLENGE_ASSERT(!opt.has_value(),
            "Pruned older item #" + std::to_string(i) + " must not exist in history");
    }

    // Dynamic expansion test: increase limit back to 20, items count stays 10
    hist.set_max_history_limit(20);
    CHALLENGE_ASSERT(hist.max_history_limit() == 20, "max_history_limit must expand to 20");
    CHALLENGE_ASSERT(hist.item_count() == 10, "item_count must remain 10 after limit expansion");

    std::cout << "  -> PASS: Dynamic pruning retained exactly 10 newest items in strict order.\n";
}

// =========================================================================
// 3. Pinned Items Preservation Under Extreme Eviction
// =========================================================================
void challenge_pinned_items_preservation() {
    std::cout << "\n======================================================\n";
    std::cout << "[CHALLENGE 3] Pinned Items Preservation Under Eviction Pressure\n";
    std::cout << "======================================================\n";

    // Scenario A: Standard capacity eviction when pushing new items
    {
        std::cout << "  [Scenario 3A] Capacity limit=5: Pinning 2 items, pushing 100 new items...\n";
        l2a::ClipboardHistoryManager hist("", 5);

        // Add 5 initial items
        std::vector<uint64_t> ids;
        for (int i = 0; i < 5; ++i) {
            std::string text = "Initial_Item_" + std::to_string(i);
            ids.push_back(hist.add_text_item(l2a::ClipOrigin::Local, text, l2a::sha256_string(text)));
        }

        // Pin the oldest item (#0) and a middle item (#2)
        uint64_t pinned_oldest = ids[0];
        uint64_t pinned_middle = ids[2];
        CHALLENGE_ASSERT(hist.pin_item(pinned_oldest, true), "Pin item 0 must succeed");
        CHALLENGE_ASSERT(hist.pin_item(pinned_middle, true), "Pin item 2 must succeed");

        // Push 100 new unpinned items
        for (int i = 100; i < 200; ++i) {
            std::string text = "Eviction_Pressure_Item_" + std::to_string(i);
            hist.add_text_item(l2a::ClipOrigin::Local, text, l2a::sha256_string(text));
        }

        // Capacity must be strictly 5
        CHALLENGE_ASSERT(hist.item_count() == 5, "Capacity must be capped at 5");

        auto items = hist.get_items();
        bool found_oldest = false;
        bool found_middle = false;

        for (const auto& item : items) {
            if (item.id == pinned_oldest) {
                found_oldest = true;
                CHALLENGE_ASSERT(item.is_pinned, "Oldest item must retain is_pinned flag");
                CHALLENGE_ASSERT(item.full_text == "Initial_Item_0", "Oldest item text preserved");
            }
            if (item.id == pinned_middle) {
                found_middle = true;
                CHALLENGE_ASSERT(item.is_pinned, "Middle item must retain is_pinned flag");
                CHALLENGE_ASSERT(item.full_text == "Initial_Item_2", "Middle item text preserved");
            }
        }

        CHALLENGE_ASSERT(found_oldest, "Pinned oldest item MUST NOT be evicted under 100x capacity pressure");
        CHALLENGE_ASSERT(found_middle, "Pinned middle item MUST NOT be evicted under 100x capacity pressure");
    }

    // Scenario B: Dynamic limit reduction with pinned items
    {
        std::cout << "  [Scenario 3B] Dynamic limit reduction (25 -> 5) with 4 pinned items...\n";
        l2a::ClipboardHistoryManager hist("", 25);

        std::vector<uint64_t> ids;
        for (int i = 0; i < 25; ++i) {
            std::string text = "DynPin_Item_" + std::to_string(i);
            ids.push_back(hist.add_text_item(l2a::ClipOrigin::Local, text, l2a::sha256_string(text)));
        }

        // Pin items at different positions: #1 (very old), #6 (old), #15 (middle), #23 (recent)
        uint64_t p1 = ids[1];
        uint64_t p6 = ids[6];
        uint64_t p15 = ids[15];
        uint64_t p23 = ids[23];

        hist.pin_item(p1, true);
        hist.pin_item(p6, true);
        hist.pin_item(p15, true);
        hist.pin_item(p23, true);

        // Dynamically trim down to 5
        hist.set_max_history_limit(5);
        CHALLENGE_ASSERT(hist.item_count() == 5, "Trimmed size must be exactly 5");

        auto items = hist.get_items();
        int pinned_found = 0;
        for (const auto& item : items) {
            if (item.id == p1 || item.id == p6 || item.id == p15 || item.id == p23) {
                pinned_found++;
                CHALLENGE_ASSERT(item.is_pinned, "Item must be marked is_pinned");
            }
        }

        CHALLENGE_ASSERT(pinned_found == 4,
            "All 4 pinned items must survive dynamic limit reduction to 5 slots");
        // Index 0 should be the newest unpinned item (#24)
        CHALLENGE_ASSERT(items[0].full_text == "DynPin_Item_24",
            "Index 0 should remain the newest item");
    }

    std::cout << "  -> PASS: Pinned items are reliably preserved during capacity eviction and dynamic trimming.\n";
}

// =========================================================================
// 4. Non-Destructive Selective Paste Token Semantics
// =========================================================================
void challenge_non_destructive_selective_paste() {
    std::cout << "\n======================================================\n";
    std::cout << "[CHALLENGE 4] Non-Destructive Selective Paste Token Semantics\n";
    std::cout << "======================================================\n";

    l2a::ClipboardHistoryManager hist("", 15);

    // Push 10 distinct items
    std::vector<uint64_t> ids;
    for (int i = 0; i < 10; ++i) {
        std::string text = "History_Entry_#" + std::to_string(i);
        ids.push_back(hist.add_text_item(l2a::ClipOrigin::Local, text, l2a::sha256_string(text)));
    }

    // Take exact pre-marking snapshot
    auto snapshot_before = hist.get_items();
    CHALLENGE_ASSERT(snapshot_before.size() == 10, "History must have 10 items");
    uint64_t top_id_before = snapshot_before[0].id;
    CHALLENGE_ASSERT(top_id_before == ids[9], "Top item #0 must be the 9th pushed item");

    // Choose item #k in the middle (index 5, corresponding to pushed item #4)
    uint64_t target_k_id = snapshot_before[5].id;
    std::string target_k_text = snapshot_before[5].full_text;
    CHALLENGE_ASSERT(target_k_text == "History_Entry_#4", "Target item #k text match");

    std::cout << "  Marking item #k (ID=" << target_k_id << ") for selective paste...\n";
    std::string token = hist.mark_selective_paste(target_k_id);
    CHALLENGE_ASSERT(!token.empty(), "Generated token must not be empty");
    CHALLENGE_ASSERT(token.rfind("sel_paste_" + std::to_string(target_k_id), 0) == 0,
        "Token must prefix with sel_paste_<id>");

    // 1. Verify token status
    CHALLENGE_ASSERT(hist.is_selective_paste_active(target_k_id),
        "is_selective_paste_active must be true for target_k_id");
    CHALLENGE_ASSERT(!hist.is_selective_paste_active(top_id_before),
        "is_selective_paste_active must be false for top_id");
    CHALLENGE_ASSERT(!hist.is_selective_paste_active(99999),
        "is_selective_paste_active must be false for non-existent ID");

    auto active_opt = hist.active_selective_paste_id();
    CHALLENGE_ASSERT(active_opt.has_value() && *active_opt == target_k_id,
        "active_selective_paste_id must return target_k_id");

    // 2. CRUCIAL: Verify that marking did NOT alter the history stack order or displace #0
    auto snapshot_after = hist.get_items();
    CHALLENGE_ASSERT(snapshot_after.size() == snapshot_before.size(),
        "History size must not change after mark_selective_paste");

    for (size_t idx = 0; idx < snapshot_before.size(); ++idx) {
        CHALLENGE_ASSERT(snapshot_after[idx].id == snapshot_before[idx].id,
            "History item ID at index " + std::to_string(idx) + " must not change");
        CHALLENGE_ASSERT(snapshot_after[idx].full_text == snapshot_before[idx].full_text,
            "History item text at index " + std::to_string(idx) + " must not change");
        CHALLENGE_ASSERT(snapshot_after[idx].timestamp_ms == snapshot_before[idx].timestamp_ms,
            "History item timestamp at index " + std::to_string(idx) + " must not change");
    }

    auto latest_item = hist.get_latest_item();
    CHALLENGE_ASSERT(latest_item.has_value() && latest_item->id == top_id_before,
        "Top item #0 must remain 100% unchanged and undisplaced by selective paste marking");

    // 3. Clear selective paste
    hist.clear_selective_paste();
    CHALLENGE_ASSERT(!hist.is_selective_paste_active(target_k_id),
        "is_selective_paste_active must be false after clear");
    CHALLENGE_ASSERT(!hist.active_selective_paste_id().has_value(),
        "active_selective_paste_id must be nullopt after clear");

    // Verify order STILL unmodified after clear
    auto snapshot_final = hist.get_items();
    CHALLENGE_ASSERT(snapshot_final[0].id == top_id_before, "Top item must still be #0");
    CHALLENGE_ASSERT(snapshot_final[5].id == target_k_id, "Target item must still be at index 5");

    std::cout << "  -> PASS: Non-destructive selective paste token marked without displacing #0 or altering order.\n";
}

// =========================================================================
// 5. Extreme Concurrency & Multi-Thread Safety
// =========================================================================
void challenge_concurrency_stress() {
    std::cout << "\n======================================================\n";
    std::cout << "[CHALLENGE 5] Extreme Concurrency & Multi-Thread Safety\n";
    std::cout << "======================================================\n";

    auto temp_file = std::filesystem::temp_directory_path() /
                     ("l2a_challenger_hist_conc_" + std::to_string(::getpid()) + ".json");
    std::error_code ec;
    std::filesystem::remove(temp_file, ec);

    constexpr int kNumThreads = 16;
    constexpr int kOpsPerThread = 200;
    constexpr size_t kLimit = 50;

    l2a::ClipboardHistoryManager hist(temp_file, kLimit);
    std::atomic<bool> start_flag{false};
    std::atomic<int> completed_threads{0};
    std::vector<std::thread> threads;

    std::cout << "  Spawning " << kNumThreads << " threads performing "
              << kOpsPerThread << " concurrent mixed operations...\n";

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&hist, &start_flag, &completed_threads, t]() {
            while (!start_flag.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (int i = 0; i < kOpsPerThread; ++i) {
                int op = (t * kOpsPerThread + i) % 6;
                std::string text = "Thread_" + std::to_string(t) + "_Item_" + std::to_string(i);
                auto sha = l2a::sha256_string(text);

                switch (op) {
                    case 0:
                    case 1:
                    case 2: {
                        uint64_t id = hist.add_text_item(
                            (i % 2 == 0) ? l2a::ClipOrigin::Local : l2a::ClipOrigin::Remote,
                            text, sha);
                        if (i % 10 == 0) {
                            hist.pin_item(id, true);
                        }
                        break;
                    }
                    case 3: {
                        auto items = hist.get_items();
                        CHALLENGE_ASSERT(items.size() <= 50,
                            "items.size() must not exceed max_limit_ during concurrent operations");
                        if (!items.empty()) {
                            auto latest = hist.get_latest_item();
                            CHALLENGE_ASSERT(latest.has_value(), "get_latest_item must return a value when items non-empty");
                        }
                        break;
                    }
                    case 4: {
                        hist.set_max_history_limit(5 + (i % 46));
                        break;
                    }
                    case 5: {
                        auto items = hist.get_items();
                        if (!items.empty()) {
                            uint64_t victim_id = items.back().id;
                            hist.delete_item(victim_id);
                        }
                        break;
                    }
                }
            }
            completed_threads.fetch_add(1, std::memory_order_release);
        });
    }

    start_flag.store(true, std::memory_order_release);

    for (auto& th : threads) {
        th.join();
    }

    CHALLENGE_ASSERT(completed_threads.load() == kNumThreads, "All threads must complete");
    size_t final_count = hist.item_count();
    size_t final_limit = hist.max_history_limit();
    std::cout << "  Completed: 16 threads, " << (kNumThreads * kOpsPerThread)
              << " operations. Final items=" << final_count
              << ", limit=" << final_limit << "\n";
    CHALLENGE_ASSERT(final_count <= final_limit, "Final count must respect final limit");
    CHALLENGE_ASSERT(final_limit >= 5 && final_limit <= 200, "Final limit must remain within [5, 200]");

    std::filesystem::remove(temp_file, ec);
    std::cout << "  -> PASS: Extreme concurrency survived without crash or deadlock.\n";
}

// =========================================================================
// 6. JSON Robustness & Memory Boundary Fuzzing
// =========================================================================
void challenge_json_robustness_and_corrupt_recovery() {
    std::cout << "\n======================================================\n";
    std::cout << "[CHALLENGE 6] JSON Serialization Robustness & Corrupt Recovery\n";
    std::cout << "======================================================\n";

    l2a::ClipboardHistoryManager hist("", 10);

    // Push complex unicode, emojis, control characters, quotes, and newlines
    std::string complex_text = "Hello \"World\"! \nLine2 \tTab \\Backslash \r\nEmoji: 🚀🎉🔥 中文测试";
    uint64_t id = hist.add_text_item(l2a::ClipOrigin::Local, complex_text, l2a::sha256_string(complex_text));

    std::string json = hist.to_json();
    CHALLENGE_ASSERT(!json.empty(), "to_json must not produce empty string");

    // Deserialize into fresh manager
    l2a::ClipboardHistoryManager hist2("", 10);
    bool ok = hist2.from_json(json);
    CHALLENGE_ASSERT(ok, "from_json must parse serialized complex text successfully");

    auto item_opt = hist2.get_item(id);
    CHALLENGE_ASSERT(item_opt.has_value(), "Recovered item must exist");
    CHALLENGE_ASSERT(item_opt->full_text == complex_text,
        "Recovered text must byte-for-byte match original complex text");

    // Test malformed JSON recovery
    CHALLENGE_ASSERT(!hist2.from_json(""), "Empty JSON string must return false");
    CHALLENGE_ASSERT(!hist2.from_json("{ not an array }"), "Non-array JSON must return false");
    CHALLENGE_ASSERT(!hist2.from_json("[{\"id\": 123, \"full_text\": \"incomplete"), "Malformed JSON must return false");

    std::cout << "  -> PASS: JSON serialization handles quotes, newlines, UTF-8, and rejects corrupt payloads gracefully.\n";
}

}  // namespace

int main() {
    std::cout << "=========================================================\n";
    std::cout << "L2A Milestone M1-C3 Empirical Challenger Test Harness\n";
    std::cout << "=========================================================\n";

    challenge_edge_capacity_limits();
    challenge_dynamic_pruning();
    challenge_pinned_items_preservation();
    challenge_non_destructive_selective_paste();
    challenge_concurrency_stress();
    challenge_json_robustness_and_corrupt_recovery();

    std::cout << "\n=========================================================\n";
    std::cout << "ALL EMPIRICAL CHALLENGER TESTS PASSED (6/6)!\n";
    std::cout << "=========================================================\n";
    return 0;
}
