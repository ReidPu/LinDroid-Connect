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

void create_test_file(const std::filesystem::path& path, size_t size_bytes, uint8_t seed) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    TEST_CHECK(out.is_open());
    std::vector<uint8_t> buffer(size_bytes);
    for (size_t i = 0; i < size_bytes; ++i) {
        buffer[i] = static_cast<uint8_t>((i * 19 + seed + (i >> 6)) & 0xFFU);
    }
    out.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
}

class ResumeListener : public ICoreListener {
public:
    Engine* interrupt_engine{nullptr};
    std::atomic<bool> interrupt_triggered{false};
    std::atomic<uint64_t> interrupted_at_bytes{0};

    std::atomic<uint64_t> sender_first_progress_bytes{0};
    std::atomic<bool> sender_first_chunk_recorded{false};

    std::mutex mtx;
    std::condition_variable cv;

    bool connected{false};
    std::vector<std::filesystem::path> completed_files;
    std::atomic<size_t> failures{0};

    void on_connection_state_changed(
        bool is_conn,
        const std::string& /*peer_id*/,
        const std::string& /*peer_name*/) override {
        std::lock_guard<std::mutex> lock(mtx);
        connected = is_conn;
        cv.notify_all();
    }

    void on_file_progress(const TransferProgress& progress) override {
        if (!progress.is_sending) {
            // Receiver progress: check if we should trigger abrupt disconnect
            if (!interrupt_triggered.load() && progress.bytes_transferred >= 128 * 1024 && interrupt_engine) {
                if (!interrupt_triggered.exchange(true)) {
                    interrupted_at_bytes.store(progress.bytes_transferred);
                    std::cout << "[INFO] Receiver reached " << progress.bytes_transferred
                              << " bytes. Simulating abrupt network failure by severing channel...\n";
                    interrupt_engine->disconnect_active_peer();
                    {
                        std::lock_guard<std::mutex> lock(mtx);
                        connected = false;
                        cv.notify_all();
                    }
                }
            }
        } else {
            // Sender progress
            if (!sender_first_chunk_recorded.exchange(true)) {
                sender_first_progress_bytes.store(progress.bytes_transferred);
            }
        }
    }

    void on_file_completed(
        uint64_t /*batch_id*/,
        uint32_t /*file_index*/,
        const std::filesystem::path& saved_path,
        bool success,
        const std::string& error_msg) override {
        if (!success) {
            std::cerr << "[TEST ERROR] File completed with error: " << error_msg << "\n";
            failures++;
        }
        std::lock_guard<std::mutex> lock(mtx);
        if (success) {
            completed_files.push_back(saved_path);
        }
        cv.notify_all();
    }
};

}  // namespace

int main() {
    std::cout << "=========================================================\n";
    std::cout << "Running TEST-04: Abrupt Disconnect & Reconnect Resume Suite\n";
    std::cout << "=========================================================\n";

    auto base_dir = std::filesystem::temp_directory_path() / ("l2a_disconnect_resume_" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(base_dir, ec);

    EngineConfig cfg_a{};
    cfg_a.storage_dir = base_dir / "state_a";
    cfg_a.receive_dir = base_dir / "recv_a";
    cfg_a.device_name = "Sender-Device";
    cfg_a.platform = "linux";
    cfg_a.listen_port = 0;
    cfg_a.discovery_port = 0;

    EngineConfig cfg_b{};
    cfg_b.storage_dir = base_dir / "state_b";
    cfg_b.receive_dir = base_dir / "recv_b";
    cfg_b.device_name = "Receiver-Device";
    cfg_b.platform = "android";
    cfg_b.listen_port = 0;
    cfg_b.discovery_port = 0;

    ResumeListener listener_a;
    ResumeListener listener_b;

    Engine engine_a(cfg_a, &listener_a);
    Engine engine_b(cfg_b, &listener_b);

    listener_b.interrupt_engine = &engine_b;

    // Connect Engine A and Engine B
    auto connect_peers = [&]() {
        int sv[2]{-1, -1};
        TEST_CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        auto srv_fut = std::async(std::launch::async, [&] {
            return engine_a.connect_with_fd(sv[0], /*is_client=*/false);
        });
        bool cli_ok = engine_b.connect_with_fd(sv[1], /*is_client=*/true);
        TEST_CHECK(cli_ok);
        TEST_CHECK(srv_fut.get());

        {
            std::unique_lock<std::mutex> lock(listener_a.mtx);
            TEST_CHECK(listener_a.cv.wait_for(lock, std::chrono::seconds(5), [&] { return listener_a.connected; }));
        }
        {
            std::unique_lock<std::mutex> lock(listener_b.mtx);
            TEST_CHECK(listener_b.cv.wait_for(lock, std::chrono::seconds(5), [&] { return listener_b.connected; }));
        }
    };

    connect_peers();
    std::cout << "[PASS] 1. Initial connection established.\n";

    // 2. Create a 512KB binary file (8 x 64KB chunks)
    constexpr size_t kFileSize = 512 * 1024;
    auto src_dir = base_dir / "src_folder";
    auto src_file = src_dir / "video_stream.bin";
    create_test_file(src_file, kFileSize, 0x42);
    auto src_hash = sha256_file_range(src_file);
    TEST_CHECK(src_hash.has_value());

    // 3. Start transfer — listener_b will simulate abrupt disconnect mid-transfer
    std::cout << "[INFO] Initiating transfer of 512KB file with mid-transfer disconnect trigger...\n";
    std::vector<std::filesystem::path> paths = {src_dir};
    uint64_t b1 = engine_a.send_files(paths);
    TEST_CHECK(b1 > 0);

    // 4. Wait for disconnect event
    {
        std::unique_lock<std::mutex> lock(listener_a.mtx);
        bool dropped = listener_a.cv.wait_for(lock, std::chrono::seconds(8), [&] {
            return !listener_a.connected;
        });
        TEST_CHECK(dropped);
    }
    {
        std::unique_lock<std::mutex> lock(listener_b.mtx);
        bool dropped = listener_b.cv.wait_for(lock, std::chrono::seconds(8), [&] {
            return !listener_b.connected;
        });
        TEST_CHECK(dropped);
    }
    TEST_CHECK(listener_b.interrupt_triggered.load());

    // 5. Inspect partial file (.l2apart)
    auto part_file = cfg_b.receive_dir / "src_folder" / "video_stream.bin.l2apart";
    auto final_file = cfg_b.receive_dir / "src_folder" / "video_stream.bin";
    TEST_CHECK(std::filesystem::exists(part_file));
    TEST_CHECK(!std::filesystem::exists(final_file));

    uint64_t part_size = std::filesystem::file_size(part_file);
    std::cout << "[PASS] 2. Abrupt disconnect caught. Partial file preserved: "
              << part_file << " (size: " << part_size << " bytes, expected: >=128KB and <512KB).\n";
    TEST_CHECK(part_size >= 128 * 1024);
    TEST_CHECK(part_size < kFileSize);

    // 6. Re-establish connection between Peer A and Peer B
    std::cout << "[INFO] Re-establishing connection for resume...\n";
    listener_b.interrupt_engine = nullptr;  // Do not interrupt second transfer
    listener_a.sender_first_chunk_recorded.store(false);
    connect_peers();
    std::cout << "[PASS] 3. Reconnection established successfully.\n";

    // 7. Re-send batch: engine should resume from existing .l2apart offset
    std::cout << "[INFO] Re-invoking send_files to resume from offset " << part_size << "...\n";
    uint64_t b2 = engine_a.send_files(paths);
    TEST_CHECK(b2 > 0);

    // 8. Wait for transfer completion on Node B
    {
        std::unique_lock<std::mutex> lock(listener_b.mtx);
        bool completed = listener_b.cv.wait_for(lock, std::chrono::seconds(10), [&] {
            return !listener_b.completed_files.empty();
        });
        TEST_CHECK(completed);
    }

    // 9. Verify resumption behavior and file integrity
    TEST_CHECK(std::filesystem::exists(final_file));
    TEST_CHECK(!std::filesystem::exists(part_file));  // .l2apart must be cleaned up / renamed

    auto final_hash = sha256_file_range(final_file);
    TEST_CHECK(final_hash.has_value());
    TEST_CHECK(*final_hash == *src_hash);

    // Verify sender did not re-transfer from offset 0
    uint64_t first_sent_progress = listener_a.sender_first_progress_bytes.load();
    std::cout << "[INFO] First chunk progress on sender reported at offset: " << first_sent_progress
              << " bytes (original part size: " << part_size << " bytes).\n";
    TEST_CHECK(first_sent_progress >= part_size);

    std::cout << "[PASS] 4. Resumption verified: skipped " << part_size
              << " bytes; final file matches 100% SHA-256.\n";

    engine_a.stop();
    engine_b.stop();
    std::filesystem::remove_all(base_dir, ec);

    std::cout << "All TEST-04 disconnect and resume tests PASSED!\n";
    return 0;
}
