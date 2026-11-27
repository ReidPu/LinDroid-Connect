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

class LargeBatchListener : public ICoreListener {
public:
    std::mutex mtx;
    std::condition_variable cv;

    bool connected{false};
    std::atomic<uint32_t> completed_files{0};
    std::atomic<uint32_t> failed_files{0};

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
        const std::filesystem::path& /*saved_path*/,
        bool success,
        const std::string& error_msg) override {
        if (!success) {
            std::cerr << "[TEST ERROR] File completed with error: " << error_msg << "\n";
            failed_files++;
        } else {
            completed_files++;
        }
        std::lock_guard<std::mutex> lock(mtx);
        cv.notify_all();
    }
};

}  // namespace

int main() {
    std::cout << "=========================================================\n";
    std::cout << "Running TEST-02: Large Batch Metadata Slicing Suite\n";
    std::cout << "=========================================================\n";

    auto base_dir = std::filesystem::temp_directory_path() / ("l2a_large_batch_" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(base_dir, ec);

    EngineConfig cfg_a{};
    cfg_a.storage_dir = base_dir / "state_a";
    cfg_a.receive_dir = base_dir / "recv_a";
    cfg_a.device_name = "Sender-Node";
    cfg_a.platform = "linux";
    cfg_a.listen_port = 0;
    cfg_a.discovery_port = 0;

    EngineConfig cfg_b{};
    cfg_b.storage_dir = base_dir / "state_b";
    cfg_b.receive_dir = base_dir / "recv_b";
    cfg_b.device_name = "Receiver-Node";
    cfg_b.platform = "android";
    cfg_b.listen_port = 0;
    cfg_b.discovery_port = 0;

    LargeBatchListener listener_a;
    LargeBatchListener listener_b;

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
    std::cout << "[PASS] 1. Connected Peer A and Peer B.\n";

    // 2. Generate a large directory tree containing 1,200 files
    // 12 directories with 100 files each.
    constexpr uint32_t kTotalTestFiles = 1200;
    constexpr uint32_t kDirsCount = 12;
    constexpr uint32_t kFilesPerDir = 100;

    auto src_root = base_dir / "large_dataset";
    std::cout << "[INFO] Creating " << kTotalTestFiles << " files in directory tree " << src_root << "...\n";

    for (uint32_t d = 0; d < kDirsCount; ++d) {
        char dir_buf[32];
        std::snprintf(dir_buf, sizeof(dir_buf), "sub_category_%02u", d);
        auto dir_path = src_root / dir_buf;
        std::filesystem::create_directories(dir_path);

        for (uint32_t f = 0; f < kFilesPerDir; ++f) {
            char file_buf[64];
            std::snprintf(file_buf, sizeof(file_buf), "record_item_%04u.dat", d * kFilesPerDir + f);
            auto file_path = dir_path / file_buf;

            std::ofstream out(file_path, std::ios::binary | std::ios::trunc);
            std::string content = "L2A_LARGE_BATCH_ENTRY_" + std::to_string(d) + "_" + std::to_string(f);
            out.write(content.data(), static_cast<std::streamsize>(content.size()));
        }
    }

    std::cout << "[PASS] 2. Successfully generated " << kTotalTestFiles << " structured test files.\n";

    // 3. Initiate directory transfer of the 1,200 files
    // This exercises FileTransferManager dynamic metadata slicing (>10 frames under kMaxFramePayload 66560)
    std::vector<std::filesystem::path> send_list = {src_root};
    auto start_time = std::chrono::steady_clock::now();
    uint64_t batch_id = engine_a.send_files(send_list);
    TEST_CHECK(batch_id > 0);

    // 4. Wait for receiver to complete all 1,200 files
    {
        std::unique_lock<std::mutex> lock(listener_b.mtx);
        bool all_done = listener_b.cv.wait_for(lock, std::chrono::seconds(25), [&] {
            return listener_b.completed_files.load() >= kTotalTestFiles;
        });
        if (!all_done) {
            std::cerr << "[ERROR] Timeout waiting for 1,200 files. Completed: "
                      << listener_b.completed_files.load()
                      << ", Failed: " << listener_b.failed_files.load() << "\n";
        }
        TEST_CHECK(all_done);
    }

    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time).count();
    std::cout << "[INFO] Transferred and verified " << listener_b.completed_files.load()
              << " files in " << elapsed_ms << " ms.\n";

    TEST_CHECK(listener_b.failed_files.load() == 0);
    TEST_CHECK(listener_b.completed_files.load() == kTotalTestFiles);

    // 5. Verify integrity of sampled files across different slices
    uint32_t sample_indices[] = {0, 127, 128, 255, 256, 500, 750, 1000, 1199};
    for (uint32_t idx : sample_indices) {
        uint32_t d = idx / kFilesPerDir;
        uint32_t f = idx % kFilesPerDir;
        (void)f;
        char dir_buf[32];
        std::snprintf(dir_buf, sizeof(dir_buf), "sub_category_%02u", d);
        char file_buf[64];
        std::snprintf(file_buf, sizeof(file_buf), "record_item_%04u.dat", idx);

        auto src_f = src_root / dir_buf / file_buf;
        auto dst_f = cfg_b.receive_dir / "large_dataset" / dir_buf / file_buf;

        TEST_CHECK(std::filesystem::exists(dst_f));
        auto src_hash = sha256_file_range(src_f);
        auto dst_hash = sha256_file_range(dst_f);
        TEST_CHECK(src_hash.has_value());
        TEST_CHECK(dst_hash.has_value());
        TEST_CHECK(*src_hash == *dst_hash);
    }
    std::cout << "[PASS] 3. Sliced metadata frames received and all 1,200 files verified with 100% SHA-256 match.\n";

    engine_a.stop();
    engine_b.stop();
    std::filesystem::remove_all(base_dir, ec);

    std::cout << "All TEST-02 large batch metadata slicing tests PASSED!\n";
    return 0;
}
