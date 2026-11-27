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

class ConcurrencyListener : public ICoreListener {
public:
    std::mutex mtx;
    std::condition_variable cv;

    bool connected{false};
    std::vector<std::filesystem::path> received_files;
    std::vector<std::filesystem::path> sent_completed_files;
    std::atomic<size_t> failed_transfers{0};

    void on_connection_state_changed(
        bool is_conn,
        const std::string& /*peer_id*/,
        const std::string& /*peer_name*/) override {
        std::lock_guard<std::mutex> lock(mtx);
        connected = is_conn;
        cv.notify_all();
    }

    void on_file_completed(
        uint64_t /*batch_id*/,
        uint32_t /*file_index*/,
        const std::filesystem::path& saved_path,
        bool success,
        const std::string& error_msg) override {
        if (!success) {
            std::cerr << "[TEST ERROR] File transfer failed: " << error_msg << " for " << saved_path << "\n";
            failed_transfers++;
        }
        std::lock_guard<std::mutex> lock(mtx);
        // Note: For sender, saved_path is the local source path. For receiver, it is the received path.
        received_files.push_back(saved_path);
        cv.notify_all();
    }
};

void create_test_file(const std::filesystem::path& path, size_t size_bytes, uint8_t seed) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    TEST_CHECK(out.is_open());
    std::vector<uint8_t> buffer(size_bytes);
    for (size_t i = 0; i < size_bytes; ++i) {
        buffer[i] = static_cast<uint8_t>((i * 17 + seed + (i >> 8)) & 0xFFU);
    }
    out.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
}

}  // namespace

int main() {
    std::cout << "=========================================================\n";
    std::cout << "Running TEST-01: Bidirectional Transfer Concurrency Suite\n";
    std::cout << "=========================================================\n";

    auto base_dir = std::filesystem::temp_directory_path() / ("l2a_concurrency_test_" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(base_dir, ec);

    EngineConfig cfg_a{};
    cfg_a.storage_dir = base_dir / "state_a";
    cfg_a.receive_dir = base_dir / "recv_a";
    cfg_a.device_name = "Device-Peer-A";
    cfg_a.platform = "linux";
    cfg_a.listen_port = 0;
    cfg_a.discovery_port = 0;

    EngineConfig cfg_b{};
    cfg_b.storage_dir = base_dir / "state_b";
    cfg_b.receive_dir = base_dir / "recv_b";
    cfg_b.device_name = "Device-Peer-B";
    cfg_b.platform = "android";
    cfg_b.listen_port = 0;
    cfg_b.discovery_port = 0;

    ConcurrencyListener listener_a;
    ConcurrencyListener listener_b;

    Engine engine_a(cfg_a, &listener_a);
    Engine engine_b(cfg_b, &listener_b);

    // 1. Establish loopback connection
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
    std::cout << "[PASS] 1. Connected Peer A and Peer B over loopback.\n";

    // 2. Prepare concurrent test batches
    auto src_dir_a = base_dir / "src_a";
    auto file_a1 = src_dir_a / "doc1.pdf";
    auto file_a2 = src_dir_a / "audio.flac";
    auto file_a3 = src_dir_a / "sub" / "data.bin";
    create_test_file(file_a1, 15 * 1024, 0x11);
    create_test_file(file_a2, 140 * 1024, 0x22);
    create_test_file(file_a3, 310 * 1024, 0x33);

    auto src_dir_b = base_dir / "src_b";
    auto file_b1 = src_dir_b / "image.png";
    auto file_b2 = src_dir_b / "sub" / "archive.tar";
    auto file_b3 = src_dir_b / "nested" / "deep" / "blob.dat";
    create_test_file(file_b1, 9 * 1024, 0xAA);
    create_test_file(file_b2, 85 * 1024, 0xBB);
    create_test_file(file_b3, 260 * 1024, 0xCC);

    std::vector<std::filesystem::path> paths_a = {src_dir_a};
    std::vector<std::filesystem::path> paths_b = {src_dir_b};

    // 3. Initiate bidirectional transfer simultaneously in separate threads
    std::cout << "[INFO] Initiating concurrent bidirectional transfer across worker threads...\n";
    auto fut_a = std::async(std::launch::async, [&] {
        return engine_a.send_files(paths_a);
    });
    auto fut_b = std::async(std::launch::async, [&] {
        return engine_b.send_files(paths_b);
    });

    uint64_t batch_id_a = fut_a.get();
    uint64_t batch_id_b = fut_b.get();
    TEST_CHECK(batch_id_a > 0);
    TEST_CHECK(batch_id_b > 0);

    // 4. Wait for both engines to receive all 3 incoming files + 3 outgoing acks
    // On Peer A: receives 3 files from B, and receives 3 ACKs for batch_a. Total on_file_completed callbacks = 6.
    // On Peer B: receives 3 files from A, and receives 3 ACKs for batch_b. Total on_file_completed callbacks = 6.
    {
        std::unique_lock<std::mutex> lock(listener_a.mtx);
        bool ok = listener_a.cv.wait_for(lock, std::chrono::seconds(15), [&] {
            return listener_a.received_files.size() >= 6;
        });
        TEST_CHECK(ok);
    }
    {
        std::unique_lock<std::mutex> lock(listener_b.mtx);
        bool ok = listener_b.cv.wait_for(lock, std::chrono::seconds(15), [&] {
            return listener_b.received_files.size() >= 6;
        });
        TEST_CHECK(ok);
    }

    TEST_CHECK(listener_a.failed_transfers.load() == 0);
    TEST_CHECK(listener_b.failed_transfers.load() == 0);

    // 5. Verify 100% SHA-256 match for all transferred files in both directions
    auto recv_b_a1 = cfg_b.receive_dir / "src_a" / "doc1.pdf";
    auto recv_b_a2 = cfg_b.receive_dir / "src_a" / "audio.flac";
    auto recv_b_a3 = cfg_b.receive_dir / "src_a" / "sub" / "data.bin";
    TEST_CHECK(std::filesystem::exists(recv_b_a1));
    TEST_CHECK(std::filesystem::exists(recv_b_a2));
    TEST_CHECK(std::filesystem::exists(recv_b_a3));
    TEST_CHECK(sha256_file_range(recv_b_a1) == sha256_file_range(file_a1));
    TEST_CHECK(sha256_file_range(recv_b_a2) == sha256_file_range(file_a2));
    TEST_CHECK(sha256_file_range(recv_b_a3) == sha256_file_range(file_a3));

    auto recv_a_b1 = cfg_a.receive_dir / "src_b" / "image.png";
    auto recv_a_b2 = cfg_a.receive_dir / "src_b" / "sub" / "archive.tar";
    auto recv_a_b3 = cfg_a.receive_dir / "src_b" / "nested" / "deep" / "blob.dat";
    TEST_CHECK(std::filesystem::exists(recv_a_b1));
    TEST_CHECK(std::filesystem::exists(recv_a_b2));
    TEST_CHECK(std::filesystem::exists(recv_a_b3));
    TEST_CHECK(sha256_file_range(recv_a_b1) == sha256_file_range(file_b1));
    TEST_CHECK(sha256_file_range(recv_a_b2) == sha256_file_range(file_b2));
    TEST_CHECK(sha256_file_range(recv_a_b3) == sha256_file_range(file_b3));

    std::cout << "[PASS] 2. 100% SHA-256 match verified across all concurrent bidirectional files.\n";

    // 6. Verify clean batch pruning and subsequent transfer capability
    auto second_file_a = base_dir / "second_file_a.txt";
    create_test_file(second_file_a, 4096, 0x77);
    std::vector<std::filesystem::path> p2 = {second_file_a};

    size_t prev_b_count = 0;
    {
        std::lock_guard<std::mutex> lock(listener_b.mtx);
        prev_b_count = listener_b.received_files.size();
    }

    uint64_t b2_id = engine_a.send_files(p2);
    TEST_CHECK(b2_id > 0);

    {
        std::unique_lock<std::mutex> lock(listener_b.mtx);
        bool ok = listener_b.cv.wait_for(lock, std::chrono::seconds(5), [&] {
            return listener_b.received_files.size() > prev_b_count;
        });
        TEST_CHECK(ok);
    }

    auto recv_second = cfg_b.receive_dir / "second_file_a.txt";
    TEST_CHECK(std::filesystem::exists(recv_second));
    TEST_CHECK(sha256_file_range(recv_second) == sha256_file_range(second_file_a));

    std::cout << "[PASS] 3. Clean batch pruning & subsequent transfer verified without state collision.\n";

    engine_a.stop();
    engine_b.stop();
    std::filesystem::remove_all(base_dir, ec);

    std::cout << "All TEST-01 bidirectional transfer concurrency checks PASSED!\n";
    return 0;
}
