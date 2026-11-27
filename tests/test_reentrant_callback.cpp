#include "l2a/engine.hpp"
#include "l2a/sha256.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
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

void create_test_file(const std::filesystem::path& path, size_t size_bytes, uint8_t seed) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    TEST_CHECK(out.is_open());
    std::vector<uint8_t> buffer(size_bytes);
    for (size_t i = 0; i < size_bytes; ++i) {
        buffer[i] = static_cast<uint8_t>((i * 23 + seed) & 0xFFU);
    }
    out.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
}

class ReentrantListener : public ICoreListener {
public:
    Engine* my_engine{nullptr};
    std::filesystem::path reentrant_file_to_send;
    std::atomic<bool> trigger_reentrant_send{false};
    std::atomic<bool> reentrant_send_done{false};
    std::atomic<uint64_t> reentrant_batch_id{0};

    // Reconnection test state
    std::atomic<bool> auto_reconnect_on_disconnect{false};
    std::atomic<bool> reconnect_attempted{false};
    std::atomic<bool> reconnect_succeeded{false};
    int reconnect_sv[2]{-1, -1};
    Engine* peer_engine{nullptr};

    std::mutex mtx;
    std::condition_variable cv;

    bool connected{false};
    std::vector<ReceivedClipboardItem> clips;
    std::vector<std::filesystem::path> completed_files;

    void on_connection_state_changed(
        bool is_conn,
        const std::string& /*peer_id*/,
        const std::string& /*peer_name*/) override {
        {
            std::lock_guard<std::mutex> lock(mtx);
            connected = is_conn;
            cv.notify_all();
        }

        // Test CORE-03: Re-entrant reconnection from inside on_connection_state_changed(false)
        if (!is_conn && auto_reconnect_on_disconnect.load() && my_engine && peer_engine) {
            reconnect_attempted.store(true);
            std::cout << "[INFO] on_connection_state_changed(false) triggered. Attempting re-entrant attach_channel on recv_thread_...\n";

            // Create new socketpair for reconnection
            if (::socketpair(AF_UNIX, SOCK_STREAM, 0, reconnect_sv) == 0) {
                // Connect peer asynchronously
                auto peer_fut = std::async(std::launch::async, [&] {
                    return peer_engine->connect_with_fd(reconnect_sv[1], /*is_client=*/false);
                });

                // Directly invoke connect_with_fd -> attach_channel inside the recv_thread_ callback!
                bool ok = my_engine->connect_with_fd(reconnect_sv[0], /*is_client=*/true);
                bool peer_ok = peer_fut.get();
                reconnect_succeeded.store(ok && peer_ok);
                std::cout << "[INFO] Re-entrant connection attempt completed. Result: " << (ok && peer_ok ? "SUCCESS" : "FAIL") << "\n";
            }
        }
    }

    void on_clipboard_received(const ReceivedClipboardItem& item) override {
        std::lock_guard<std::mutex> lock(mtx);
        clips.push_back(item);
        cv.notify_all();
    }

    void on_file_completed(
        uint64_t /*batch_id*/,
        uint32_t file_index,
        const std::filesystem::path& saved_path,
        bool success,
        const std::string& /*error_msg*/) override {
        {
            std::lock_guard<std::mutex> lock(mtx);
            if (success) {
                completed_files.push_back(saved_path);
            }
            cv.notify_all();
        }

        // Test CORE-02: Re-entrant operations from inside on_file_completed
        if (success && my_engine && trigger_reentrant_send.exchange(false)) {
            std::cout << "[INFO] on_file_completed invoked. Triggering re-entrant operations on engine...\n";

            // 1. Re-entrant clipboard push
            bool clip_ok = my_engine->push_clipboard_text("REENTRANT_CLIPBOARD_FROM_FILE_COMPLETED_" + std::to_string(file_index));
            TEST_CHECK(clip_ok);

            // 2. Re-entrant file transfer send_files()
            if (!reentrant_file_to_send.empty()) {
                std::vector<std::filesystem::path> p = {reentrant_file_to_send};
                uint64_t bid = my_engine->send_files(p);
                reentrant_batch_id.store(bid);
                reentrant_send_done.store(bid > 0);
            }
        }
    }
};

}  // namespace

int main() {
    std::cout << "=========================================================\n";
    std::cout << "Running TEST-03: Re-entrant Callback Deadlock Suite\n";
    std::cout << "=========================================================\n";

    auto base_dir = std::filesystem::temp_directory_path() / ("l2a_reentrant_" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(base_dir, ec);

    EngineConfig cfg_a{};
    cfg_a.storage_dir = base_dir / "state_a";
    cfg_a.receive_dir = base_dir / "recv_a";
    cfg_a.device_name = "Reentrant-Node-A";
    cfg_a.platform = "linux";
    cfg_a.listen_port = 0;
    cfg_a.discovery_port = 0;

    EngineConfig cfg_b{};
    cfg_b.storage_dir = base_dir / "state_b";
    cfg_b.receive_dir = base_dir / "recv_b";
    cfg_b.device_name = "Reentrant-Node-B";
    cfg_b.platform = "android";
    cfg_b.listen_port = 0;
    cfg_b.discovery_port = 0;

    ReentrantListener listener_a;
    ReentrantListener listener_b;

    Engine engine_a(cfg_a, &listener_a);
    Engine engine_b(cfg_b, &listener_b);

    listener_a.my_engine = &engine_a;
    listener_b.my_engine = &engine_b;
    listener_a.peer_engine = &engine_b;
    listener_b.peer_engine = &engine_a;

    // Connect Node A and Node B
    bool tcp_started = engine_a.start();
    if (tcp_started && engine_a.bound_tcp_port() > 0) {
        TEST_CHECK(engine_b.connect_to_peer("127.0.0.1", engine_a.bound_tcp_port()));
    } else {
        int sv[2]{-1, -1};
        TEST_CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        auto srv_fut = std::async(std::launch::async, [&] {
            return engine_a.connect_with_fd(sv[0], /*is_client=*/false);
        });
        bool cli_ok = engine_b.connect_with_fd(sv[1], /*is_client=*/true);
        TEST_CHECK(cli_ok);
        TEST_CHECK(srv_fut.get());
    }

    {
        std::unique_lock<std::mutex> lock(listener_a.mtx);
        TEST_CHECK(listener_a.cv.wait_for(lock, std::chrono::seconds(5), [&] { return listener_a.connected; }));
    }
    {
        std::unique_lock<std::mutex> lock(listener_b.mtx);
        TEST_CHECK(listener_b.cv.wait_for(lock, std::chrono::seconds(5), [&] { return listener_b.connected; }));
    }
    std::cout << "[PASS] 1. Initial loopback connection established.\n";

    // ------------------------------------------------------------------------
    // Part A: Verify on_file_completed Re-entrancy (push clipboard & send_files)
    // ------------------------------------------------------------------------
    auto initial_file = base_dir / "initial.txt";
    create_test_file(initial_file, 8192, 0x55);

    auto second_file = base_dir / "chained_reentrant.bin";
    create_test_file(second_file, 32768, 0x88);

    // When Node B finishes receiving initial_file, Node B will re-entrantly send
    // a clipboard text AND initiate transfer of second_file back to Node A!
    listener_b.reentrant_file_to_send = second_file;
    listener_b.trigger_reentrant_send.store(true);

    std::vector<std::filesystem::path> initial_paths = {initial_file};
    uint64_t b1 = engine_a.send_files(initial_paths);
    TEST_CHECK(b1 > 0);

    // Wait for Node B to receive initial.txt and execute re-entrant send_files
    {
        std::unique_lock<std::mutex> lock(listener_b.mtx);
        bool done = listener_b.cv.wait_for(lock, std::chrono::seconds(8), [&] {
            return listener_b.completed_files.size() >= 1 && listener_b.reentrant_send_done.load();
        });
        TEST_CHECK(done);
    }
    std::cout << "[PASS] 2. Re-entrant operations inside on_file_completed executed without deadlock.\n";

    // Verify Node A received the re-entrant clipboard item
    {
        std::unique_lock<std::mutex> lock(listener_a.mtx);
        bool clip_arrived = listener_a.cv.wait_for(lock, std::chrono::seconds(5), [&] {
            return !listener_a.clips.empty();
        });
        TEST_CHECK(clip_arrived);
        std::string clip_str(reinterpret_cast<const char*>(listener_a.clips[0].data.data()),
                             listener_a.clips[0].data.size());
        TEST_CHECK(clip_str.find("REENTRANT_CLIPBOARD_FROM_FILE_COMPLETED") != std::string::npos);
    }
    std::cout << "[PASS] 3. Re-entrant clipboard push successfully received by peer.\n";

    // Verify Node A received the re-entrantly dispatched second file
    {
        std::unique_lock<std::mutex> lock(listener_a.mtx);
        bool file_arrived = listener_a.cv.wait_for(lock, std::chrono::seconds(8), [&] {
            return !listener_a.completed_files.empty();
        });
        TEST_CHECK(file_arrived);
    }
    auto recv_chained = cfg_a.receive_dir / "chained_reentrant.bin";
    TEST_CHECK(std::filesystem::exists(recv_chained));
    TEST_CHECK(sha256_file_range(recv_chained) == sha256_file_range(second_file));
    std::cout << "[PASS] 4. Re-entrant file transfer completed with 100% SHA-256 match.\n";

    // ------------------------------------------------------------------------
    // Part B: Verify on_connection_state_changed Re-entrancy (Reconnect without self-join deadlock)
    // ------------------------------------------------------------------------
    // Enable auto-reconnection in listener_a on disconnect
    listener_a.auto_reconnect_on_disconnect.store(true);

    std::cout << "[INFO] Severing connection from Node B to trigger on_connection_state_changed(false) on Node A...\n";
    // Disconnect from Node B side. This drops the channel. Node A's channel_recv_loop terminates and fires callback.
    engine_b.disconnect_active_peer();

    // Give time for re-entrant reconnection to fire and establish
    int attempts = 0;
    while (!listener_a.reconnect_succeeded.load() && attempts++ < 50) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    TEST_CHECK(listener_a.reconnect_attempted.load());
    TEST_CHECK(listener_a.reconnect_succeeded.load());
    TEST_CHECK(engine_a.is_connected());
    TEST_CHECK(engine_b.is_connected());
    std::cout << "[PASS] 5. Self-join deadlock avoided in on_connection_state_changed(false); re-entrant reconnect succeeded.\n";

    // Verify reconnected channel is operational by pushing another clipboard message
    listener_a.auto_reconnect_on_disconnect.store(false);
    std::string post_reconnect_msg = "CHANNEL_FUNCTIONAL_AFTER_REENTRANT_RECONNECT";
    TEST_CHECK(engine_a.push_clipboard_text(post_reconnect_msg));

    {
        std::unique_lock<std::mutex> lock(listener_b.mtx);
        bool post_arrived = listener_b.cv.wait_for(lock, std::chrono::seconds(5), [&] {
            return !listener_b.clips.empty();
        });
        TEST_CHECK(post_arrived);
        std::string got(reinterpret_cast<const char*>(listener_b.clips.back().data.data()),
                        listener_b.clips.back().data.size());
        TEST_CHECK(got == post_reconnect_msg);
    }
    std::cout << "[PASS] 6. Reconnected channel verified fully operational.\n";

    engine_a.stop();
    engine_b.stop();
    std::filesystem::remove_all(base_dir, ec);

    std::cout << "All TEST-03 re-entrant callback deadlock checks PASSED!\n";
    return 0;
}
