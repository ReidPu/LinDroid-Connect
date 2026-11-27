#include "l2a/engine.hpp"
#include "l2a/sha256.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
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

class InitialClipListener : public ICoreListener {
public:
    std::mutex mtx;
    std::condition_variable cv;

    bool connected{false};
    std::string peer_name;
    std::vector<ReceivedClipboardItem> clips;

    void on_connection_state_changed(
        bool is_conn,
        const std::string& /*peer_id*/,
        const std::string& name) override {
        std::lock_guard<std::mutex> lock(mtx);
        connected = is_conn;
        peer_name = name;
        cv.notify_all();
    }

    void on_clipboard_received(const ReceivedClipboardItem& item) override {
        std::lock_guard<std::mutex> lock(mtx);
        clips.push_back(item);
        cv.notify_all();
    }
};

}  // namespace

int main() {
    std::cout << "=========================================================\n";
    std::cout << "Running REAL-12: Initial Clipboard Sync Test Suite (R1)\n";
    std::cout << "=========================================================\n";

    auto base_dir = std::filesystem::temp_directory_path() /
                    ("l2a_init_clip_test_" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(base_dir, ec);

    EngineConfig cfg_a{};
    cfg_a.storage_dir = base_dir / "linux_state";
    cfg_a.receive_dir = base_dir / "linux_recv";
    cfg_a.device_name = "Desktop-Computer";
    cfg_a.platform = "linux";
    cfg_a.listen_port = 0;
    cfg_a.discovery_port = 0;

    EngineConfig cfg_b{};
    cfg_b.storage_dir = base_dir / "android_state";
    cfg_b.receive_dir = base_dir / "android_recv";
    cfg_b.device_name = "Xiaomi-14-Pro";
    cfg_b.platform = "android";
    cfg_b.listen_port = 0;
    cfg_b.discovery_port = 0;

    InitialClipListener listener_a;
    InitialClipListener listener_b;

    Engine node_a(cfg_a, &listener_a);
    Engine node_b(cfg_b, &listener_b);

    // 1. Start Node B (receiver listener)
    TEST_CHECK(node_b.start());
    uint16_t port_b = node_b.bound_tcp_port();
    TEST_CHECK(port_b > 0);

    // 2. Start Node A (sender)
    TEST_CHECK(node_a.start());

    // 3. Assert initial disconnected state
    TEST_CHECK(!node_a.is_connected());
    TEST_CHECK(!node_b.is_connected());

    // 4. Node A registers Node B as candidate peer
    node_a.register_candidate_peer("127.0.0.1", port_b);
    auto candidates = node_a.candidate_peers();
    TEST_CHECK(candidates.size() >= 1);
    TEST_CHECK(candidates[0].ip == "127.0.0.1");
    TEST_CHECK(candidates[0].port == port_b);
    TEST_CHECK(!node_a.is_connected());

    // 5. Node A calls push_clipboard_text() WITHOUT initiating any file transfer
    std::string initial_text = "Hello from Initial Clipboard Sync!";
    std::cout << "[INFO] Triggering initial push_clipboard_text() on unconnected node...\n";
    TEST_CHECK(node_a.push_clipboard_text(initial_text));

    // 6. Node B's clipboard callback captures the received item
    {
        std::unique_lock<std::mutex> lock(listener_b.mtx);
        TEST_CHECK(listener_b.cv.wait_for(
            lock, std::chrono::seconds(5), [&] { return !listener_b.clips.empty(); }));
    }

    // Assert that node_a.is_connected() becomes true and Node B receives the exact text
    TEST_CHECK(node_a.is_connected());
    TEST_CHECK(node_b.is_connected());
    TEST_CHECK(listener_b.clips.size() == 1);
    TEST_CHECK(listener_b.clips[0].mime_type == "text/plain");
    std::string got_text(reinterpret_cast<const char*>(listener_b.clips[0].data.data()),
                         listener_b.clips[0].data.size());
    TEST_CHECK(got_text == initial_text);
    std::cout << "[PASS] 1. Initial on-demand clipboard sync without prior file transfer verified.\n";

    // 7. Boundary Test: Rapid consecutive clipboard pushes
    std::cout << "[INFO] Testing boundary: Rapid consecutive clipboard pushes...\n";
    constexpr int kRapidCount = 10;
    for (int i = 0; i < kRapidCount; ++i) {
        std::string msg = "Rapid clip burst #" + std::to_string(i) +
                          " - timestamp=" + std::to_string(i * 100);
        TEST_CHECK(node_a.push_clipboard_text(msg));
    }

    {
        std::unique_lock<std::mutex> lock(listener_b.mtx);
        TEST_CHECK(listener_b.cv.wait_for(
            lock, std::chrono::seconds(5), [&] {
                return listener_b.clips.size() == 1 + kRapidCount;
            }));
    }

    for (int i = 0; i < kRapidCount; ++i) {
        std::string expected = "Rapid clip burst #" + std::to_string(i) +
                               " - timestamp=" + std::to_string(i * 100);
        std::string got(reinterpret_cast<const char*>(listener_b.clips[1 + i].data.data()),
                        listener_b.clips[1 + i].data.size());
        TEST_CHECK(got == expected);
    }
    std::cout << "[PASS] 2. Rapid consecutive clipboard burst (" << kRapidCount
              << "/" << kRapidCount << ") delivered in order.\n";

    // 8. Boundary Test: Large clipboard payload > 64KB (130KB text)
    std::cout << "[INFO] Testing boundary: Large clipboard payload > 64KB (130KB text)...\n";
    std::string large_text;
    large_text.reserve(130 * 1024);
    for (size_t i = 0; i < 130 * 1024; ++i) {
        large_text.push_back(static_cast<char>('A' + (i % 26)));
    }
    auto sent_sha256 = sha256_string(large_text);

    TEST_CHECK(node_a.push_clipboard_text(large_text));

    {
        std::unique_lock<std::mutex> lock(listener_b.mtx);
        TEST_CHECK(listener_b.cv.wait_for(
            lock, std::chrono::seconds(5), [&] {
                return listener_b.clips.size() == 2 + kRapidCount;
            }));
    }

    const auto& large_clip = listener_b.clips.back();
    TEST_CHECK(large_clip.data.size() == large_text.size());
    auto recv_sha256 = sha256_bytes(large_clip.data);
    TEST_CHECK(recv_sha256 == sent_sha256);
    std::cout << "[PASS] 3. Large multi-chunk clipboard payload (>64KB: 130KB) verified with 100% SHA-256 match.\n";

    // 9. Clean shutdown and temporary file cleanup
    node_a.stop();
    node_b.stop();
    std::filesystem::remove_all(base_dir, ec);

    std::cout << "=========================================================\n";
    std::cout << "All REAL-12 Initial Clipboard Sync Tests PASSED (3/3)!\n";
    std::cout << "=========================================================\n";
    return 0;
}
