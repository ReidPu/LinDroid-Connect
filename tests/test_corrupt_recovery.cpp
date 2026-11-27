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
        buffer[i] = static_cast<uint8_t>((i * 31 + seed + (i >> 5)) & 0xFFU);
    }
    out.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
}

class CorruptRecoveryListener : public ICoreListener {
public:
    std::mutex mtx;
    std::condition_variable cv;

    bool connected{false};
    std::vector<std::pair<std::filesystem::path, bool>> completed_events;

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
        std::lock_guard<std::mutex> lock(mtx);
        completed_events.emplace_back(saved_path, success);
        if (!success) {
            std::cout << "[INFO] Received expected failure callback: " << error_msg << "\n";
        }
        cv.notify_all();
    }
};

}  // namespace

int main() {
    std::cout << "=========================================================\n";
    std::cout << "Running TEST-05: Corrupted Partial File Recovery Suite\n";
    std::cout << "=========================================================\n";

    auto base_dir = std::filesystem::temp_directory_path() / ("l2a_corrupt_test_" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(base_dir, ec);

    EngineConfig cfg_a{};
    cfg_a.storage_dir = base_dir / "state_a";
    cfg_a.receive_dir = base_dir / "recv_a";
    cfg_a.device_name = "Sender-A";
    cfg_a.platform = "linux";
    cfg_a.listen_port = 0;
    cfg_a.discovery_port = 0;

    EngineConfig cfg_b{};
    cfg_b.storage_dir = base_dir / "state_b";
    cfg_b.receive_dir = base_dir / "recv_b";
    cfg_b.device_name = "Receiver-B";
    cfg_b.platform = "android";
    cfg_b.listen_port = 0;
    cfg_b.discovery_port = 0;

    CorruptRecoveryListener listener_a;
    CorruptRecoveryListener listener_b;

    Engine engine_a(cfg_a, &listener_a);
    Engine engine_b(cfg_b, &listener_b);

    // 1. Establish loopback connection
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
    std::cout << "[PASS] 1. Connected Peer A and Peer B.\n";

    // ------------------------------------------------------------------------
    // Part A: Corrupted Tail in .l2apart -> Hash Mismatch -> Sender Refuses Resume ->
    // Receiver Truncates to 0 -> Clean Recovery
    // ------------------------------------------------------------------------
    constexpr size_t kFileSizeA = 320 * 1024;  // 5 x 64KB
    auto src_dir_a = base_dir / "src_test_a";
    auto src_file_a = src_dir_a / "document.bin";
    create_test_file(src_file_a, kFileSizeA, 0x11);
    auto src_hash_a = sha256_file_range(src_file_a);

    // Intentionally create a corrupted .l2apart on receiver with 128KB data where tail bytes are garbage
    auto part_path_a = cfg_b.receive_dir / "src_test_a" / "document.bin.l2apart";
    auto final_path_a = cfg_b.receive_dir / "src_test_a" / "document.bin";
    std::filesystem::create_directories(part_path_a.parent_path());
    {
        std::ofstream out(part_path_a, std::ios::binary | std::ios::trunc);
        std::vector<uint8_t> corrupted_part(128 * 1024, 0xEE);  // All garbage
        out.write(reinterpret_cast<const char*>(corrupted_part.data()),
                  static_cast<std::streamsize>(corrupted_part.size()));
    }
    TEST_CHECK(std::filesystem::file_size(part_path_a) == 128 * 1024);
    std::cout << "[INFO] Pre-seeded corrupted .l2apart (" << part_path_a << ", 128KB garbage).\n";

    // Initiate transfer: receiver will send tail hash of the garbage data.
    // Sender will notice local_tail != rentry.tail_block_sha256 and start from offset 0.
    // Receiver will receive chunk offset 0, truncate .l2apart to 0, and cleanly complete the file.
    std::vector<std::filesystem::path> paths_a = {src_dir_a};
    uint64_t b1 = engine_a.send_files(paths_a);
    TEST_CHECK(b1 > 0);

    {
        std::unique_lock<std::mutex> lock(listener_b.mtx);
        bool done = listener_b.cv.wait_for(lock, std::chrono::seconds(10), [&] {
            return !listener_b.completed_events.empty();
        });
        TEST_CHECK(done);
        TEST_CHECK(listener_b.completed_events.back().second == true);  // Success!
    }

    TEST_CHECK(std::filesystem::exists(final_path_a));
    TEST_CHECK(!std::filesystem::exists(part_path_a));  // Cleaned up
    TEST_CHECK(sha256_file_range(final_path_a) == src_hash_a);
    std::cout << "[PASS] 2. Corrupted tail .l2apart detected, truncated, and cleanly recovered with 100% SHA-256 match.\n";

    // ------------------------------------------------------------------------
    // Part B: Corrupted Partial File with Matching Tail -> Verification Fails -> Part File Discarded (CORE-11)
    // ------------------------------------------------------------------------
    constexpr size_t kFileSizeB = 256 * 1024;  // 4 x 64KB
    auto src_dir_b = base_dir / "src_test_b";
    auto src_file_b = src_dir_b / "database.sqlite";
    create_test_file(src_file_b, kFileSizeB, 0x77);

    // Pre-create 128KB (2 x 64KB chunks) .l2apart on receiver where chunk 1 (64..128KB) matches,
    // but chunk 0 (0..64KB) is corrupted with garbage!
    auto part_path_b = cfg_b.receive_dir / "src_test_b" / "database.sqlite.l2apart";
    auto final_path_b = cfg_b.receive_dir / "src_test_b" / "database.sqlite";
    std::filesystem::create_directories(part_path_b.parent_path());
    {
        // Read valid source file up to 128KB
        std::ifstream in(src_file_b, std::ios::binary);
        std::vector<uint8_t> buf(128 * 1024);
        in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));

        // Corrupt first 64KB with garbage
        for (size_t i = 0; i < 64 * 1024; ++i) {
            buf[i] ^= 0xAA;
        }

        std::ofstream out(part_path_b, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    }
    TEST_CHECK(std::filesystem::exists(part_path_b));
    std::cout << "[INFO] Pre-seeded head-corrupted 128KB partial .l2apart (" << part_path_b << ").\n";

    size_t prev_events = listener_b.completed_events.size();

    // Send paths: because tail block matches, receiver thinks it's complete, but whole-file SHA-256 verification fails!
    // Receiver MUST discard the corrupted .l2apart file (std::filesystem::remove(part_path)).
    std::vector<std::filesystem::path> paths_b = {src_dir_b};
    uint64_t b2 = engine_a.send_files(paths_b);
    TEST_CHECK(b2 > 0);

    {
        std::unique_lock<std::mutex> lock(listener_b.mtx);
        bool done = listener_b.cv.wait_for(lock, std::chrono::seconds(10), [&] {
            return listener_b.completed_events.size() > prev_events;
        });
        TEST_CHECK(done);
        // The verification failed as expected
        TEST_CHECK(listener_b.completed_events.back().second == false);
    }

    // Verify CORE-11: the corrupted .l2apart MUST be deleted to prevent infinite resume loops!
    TEST_CHECK(!std::filesystem::exists(part_path_b));
    TEST_CHECK(!std::filesystem::exists(final_path_b));
    std::cout << "[PASS] 3. Corrupted .l2apart on verification failure was immediately deleted from disk (CORE-11).\n";

    // Now re-send: transfer starts clean and succeeds
    size_t prev_events2 = listener_b.completed_events.size();
    uint64_t b3 = engine_a.send_files(paths_b);
    TEST_CHECK(b3 > 0);

    {
        std::unique_lock<std::mutex> lock(listener_b.mtx);
        bool done = listener_b.cv.wait_for(lock, std::chrono::seconds(10), [&] {
            return listener_b.completed_events.size() > prev_events2;
        });
        TEST_CHECK(done);
        TEST_CHECK(listener_b.completed_events.back().second == true);  // Clean success!
    }

    TEST_CHECK(std::filesystem::exists(final_path_b));
    TEST_CHECK(!std::filesystem::exists(part_path_b));
    TEST_CHECK(sha256_file_range(final_path_b) == sha256_file_range(src_file_b));
    std::cout << "[PASS] 4. Fresh re-transfer completed successfully after corrupted part was purged.\n";

    engine_a.stop();
    engine_b.stop();
    std::filesystem::remove_all(base_dir, ec);

    std::cout << "All TEST-05 corrupted partial file recovery tests PASSED!\n";
    return 0;
}
