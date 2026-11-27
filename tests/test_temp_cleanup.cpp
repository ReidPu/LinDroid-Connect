#include "l2a/clipboard_stream.hpp"
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

class TempCleanupListener : public ICoreListener {
public:
    std::mutex mtx;
    std::condition_variable cv;

    bool connected{false};
    std::filesystem::path observed_temp_path;
    bool temp_file_existed_during_callback{false};
    Sha256Digest received_hash{};
    std::atomic<bool> clipboard_done{false};

    void on_connection_state_changed(
        bool is_conn,
        const std::string& /*peer_id*/,
        const std::string& /*peer_name*/) override {
        std::lock_guard<std::mutex> lock(mtx);
        connected = is_conn;
        cv.notify_all();
    }

    void on_clipboard_received(const ReceivedClipboardItem& item) override {
        // Notice: While item is in scope, item.file_guard holds ownership of the temporary file!
        std::lock_guard<std::mutex> lock(mtx);
        observed_temp_path = item.temp_file_path;
        received_hash = item.sha256;

        std::error_code ec;
        if (!observed_temp_path.empty() && std::filesystem::exists(observed_temp_path, ec)) {
            temp_file_existed_during_callback = true;
        }
        clipboard_done.store(true);
        cv.notify_all();
        // At the end of this scope, if item is not copied/saved, item.file_guard destroys and unlinks the file!
    }
};

size_t count_tmp_files(const std::filesystem::path& dir) {
    size_t count = 0;
    std::error_code ec;
    if (std::filesystem::exists(dir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (entry.is_regular_file(ec) && entry.path().extension() == ".tmp") {
                count++;
            }
        }
    }
    return count;
}

}  // namespace

int main() {
    std::cout << "=========================================================\n";
    std::cout << "Running TEST-07: Temporary File Cleanup and Leak Suite\n";
    std::cout << "=========================================================\n";

    auto base_dir = std::filesystem::temp_directory_path() / ("l2a_temp_cleanup_" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(base_dir, ec);

    auto temp_dir_b = base_dir / "custom_temp_b";
    std::filesystem::create_directories(temp_dir_b);

    // ------------------------------------------------------------------------
    // Part A: Normal Streaming of Clipboard > 512KB and RAII Guard Cleanup
    // ------------------------------------------------------------------------
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

    TempCleanupListener listener_a;
    TempCleanupListener listener_b;

    Engine engine_a(cfg_a, &listener_a);
    Engine engine_b(cfg_b, &listener_b);

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

    // Create 600 KB payload (> 512 KB threshold to trigger temp file streaming)
    constexpr size_t kLargeClipSize = 600 * 1024;
    std::vector<uint8_t> large_data(kLargeClipSize);
    for (size_t i = 0; i < large_data.size(); ++i) {
        large_data[i] = static_cast<uint8_t>((i * 13 + 7) & 0xFFU);
    }
    Sha256Digest original_hash = sha256_bytes(large_data);

    std::cout << "[INFO] Pushing 600KB clipboard item from Node A to Node B...\n";
    TEST_CHECK(engine_a.push_clipboard_bytes("image/png", large_data));

    std::filesystem::path observed_path;
    {
        std::unique_lock<std::mutex> lock(listener_b.mtx);
        bool arrived = listener_b.cv.wait_for(lock, std::chrono::seconds(8), [&] {
            return listener_b.clipboard_done.load();
        });
        TEST_CHECK(arrived);
        TEST_CHECK(listener_b.temp_file_existed_during_callback);
        TEST_CHECK(listener_b.received_hash == original_hash);
        observed_path = listener_b.observed_temp_path;
    }

    // Now that the callback has finished and handle_frame exits, ReceivedClipboardItem
    // goes out of scope on the receiver worker thread and TempFileGuard removes the file.
    TEST_CHECK(!observed_path.empty());
    bool deleted = false;
    for (int i = 0; i < 200; ++i) {
        if (!std::filesystem::exists(observed_path)) {
            deleted = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    TEST_CHECK(deleted);
    std::cout << "[PASS] 2. Verified temp file " << observed_path
              << " was created for >512KB clip, held during callback, and deleted by TempFileGuard on exit.\n";

    engine_a.stop();
    engine_b.stop();

    // ------------------------------------------------------------------------
    // Part B: Direct ClipboardStreamManager Aborted Stream Cleanup
    // ------------------------------------------------------------------------
    std::cout << "[INFO] Testing aborted stream cleanup in ClipboardStreamManager...\n";
    std::vector<Frame> sent_frames;
    auto mock_send = [&](const Frame& f) {
        sent_frames.push_back(f);
        return true;
    };

    auto clip_mgr = std::make_unique<ClipboardStreamManager>(mock_send, temp_dir_b);

    // Send a ClipOffer for 700KB
    ClipOfferPayload offer{};
    offer.clip_id = 888;
    offer.mime_type = "text/plain";
    offer.total_size = 700 * 1024;
    offer.sha256 = original_hash;
    offer.lazy_pull = false;

    Frame offer_frame{};
    offer_frame.header.type = FrameType::kClipOffer;
    offer_frame.header.stream_id = 50;
    offer_frame.payload = offer.encode();
    clip_mgr->handle_frame(offer_frame);

    // Verify temp file was allocated in temp_dir_b
    auto expected_temp = temp_dir_b / "l2a_clip_888.tmp";
    TEST_CHECK(std::filesystem::exists(expected_temp));

    // Send first 64KB chunk
    ClipDataChunkPayload chunk1{};
    chunk1.clip_id = 888;
    chunk1.offset = 0;
    chunk1.data.assign(large_data.begin(), large_data.begin() + 64 * 1024);

    Frame chunk1_frame{};
    chunk1_frame.header.type = FrameType::kClipDataChunk;
    chunk1_frame.header.stream_id = 50;
    chunk1_frame.payload = chunk1.encode();
    clip_mgr->handle_frame(chunk1_frame);

    TEST_CHECK(std::filesystem::file_size(expected_temp) == 64 * 1024);

    // Now send an invalid chunk with bad offset (simulate protocol disruption / stream drop)
    ClipDataChunkPayload bad_chunk{};
    bad_chunk.clip_id = 888;
    bad_chunk.offset = 200 * 1024;  // Mismatched offset!
    bad_chunk.data.assign(1024, 0x00);

    Frame bad_frame{};
    bad_frame.header.type = FrameType::kClipDataChunk;
    bad_frame.header.stream_id = 50;
    bad_frame.payload = bad_chunk.encode();
    clip_mgr->handle_frame(bad_frame);

    // Mismatched offset triggers stream deletion: incoming_streams_.erase(it)
    // When incoming_streams_ entry is erased, TempFileGuard destructor automatically deletes expected_temp!
    TEST_CHECK(!std::filesystem::exists(expected_temp));
    TEST_CHECK(count_tmp_files(temp_dir_b) == 0);
    std::cout << "[PASS] 3. Interrupted/mismatched stream erased and partial temp file automatically unlinked.\n";

    clip_mgr.reset();

    // ------------------------------------------------------------------------
    // Part C: Startup Sweep of Orphaned Files
    // ------------------------------------------------------------------------
    std::cout << "[INFO] Testing startup sweep of orphaned .tmp files...\n";

    // Create 3 orphaned files simulating crash of prior run
    auto orphan1 = temp_dir_b / "l2a_clip_101.tmp";
    auto orphan2 = temp_dir_b / "l2a_clip_102.tmp";
    auto orphan3 = temp_dir_b / "l2a_clip_crash.tmp";
    auto keep_file = temp_dir_b / "important_document.txt";

    {
        std::ofstream(orphan1) << "abandoned data 1";
        std::ofstream(orphan2) << "abandoned data 2";
        std::ofstream(orphan3) << "abandoned data 3";
        std::ofstream(keep_file) << "must not be deleted";
    }

    TEST_CHECK(std::filesystem::exists(orphan1));
    TEST_CHECK(std::filesystem::exists(orphan2));
    TEST_CHECK(std::filesystem::exists(orphan3));
    TEST_CHECK(std::filesystem::exists(keep_file));
    TEST_CHECK(count_tmp_files(temp_dir_b) == 3);

    // Instantiate a new ClipboardStreamManager pointing to temp_dir_b
    // Its constructor must sweep and delete any l2a_clip_*.tmp files!
    auto new_clip_mgr = std::make_unique<ClipboardStreamManager>(mock_send, temp_dir_b);

    TEST_CHECK(!std::filesystem::exists(orphan1));
    TEST_CHECK(!std::filesystem::exists(orphan2));
    TEST_CHECK(!std::filesystem::exists(orphan3));
    TEST_CHECK(std::filesystem::exists(keep_file));  // Preserved
    TEST_CHECK(count_tmp_files(temp_dir_b) == 0);

    std::cout << "[PASS] 4. Startup sweep successfully wiped all orphaned .tmp files while preserving non-temp files.\n";

    new_clip_mgr.reset();
    std::filesystem::remove_all(base_dir, ec);

    std::cout << "All TEST-07 temporary file cleanup and leak checks PASSED!\n";
    return 0;
}
