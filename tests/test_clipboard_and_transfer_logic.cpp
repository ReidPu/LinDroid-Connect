#include "l2a/engine.hpp"
#include "l2a/sha256.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <string>
#include <sys/socket.h>
#include <thread>
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

struct FileEvent {
    uint64_t batch_id{0};
    uint32_t file_index{0};
    std::filesystem::path path;
    bool is_sending{false};
    bool success{false};
    std::string error;
};

class LogicTestListener : public ICoreListener {
public:
    Engine* owner{nullptr};
    std::atomic<bool> live_pull_enabled{false};
    std::string live_pull_text;

    std::atomic<bool> interrupt_on_rx_chunk{false};
    std::atomic<bool> interrupt_done{false};

    std::mutex mtx;
    std::condition_variable cv;
    bool connected{false};
    std::vector<ReceivedClipboardItem> clips;
    std::vector<FileEvent> file_events;

    void on_connection_state_changed(
        bool is_conn,
        const std::string& /*peer_id*/,
        const std::string& /*peer_name*/) override {
        std::lock_guard<std::mutex> lock(mtx);
        connected = is_conn;
        cv.notify_all();
    }

    void on_clipboard_received(const ReceivedClipboardItem& item) override {
        std::lock_guard<std::mutex> lock(mtx);
        clips.push_back(item);
        cv.notify_all();
    }

    bool on_clipboard_pull_requested_sync() override {
        if (live_pull_enabled.load() && owner && !live_pull_text.empty()) {
            return owner->push_clipboard_text(live_pull_text, /*bypass_loopback=*/true, /*is_pull_response=*/true);
        }
        return false;
    }

    void on_file_progress(const TransferProgress& progress) override {
        if (!progress.is_sending &&
            interrupt_on_rx_chunk.load() &&
            progress.bytes_transferred >= 128 * 1024 &&
            owner) {
            if (!interrupt_done.exchange(true)) {
                owner->disconnect_active_peer();
            }
        }
    }

    void on_file_transfer_completed(
        uint64_t batch_id,
        uint32_t file_index,
        const std::filesystem::path& saved_path,
        bool is_sending,
        bool success,
        const std::string& error_msg) override {
        std::lock_guard<std::mutex> lock(mtx);
        file_events.push_back({batch_id, file_index, saved_path, is_sending, success, error_msg});
        cv.notify_all();
    }
};

void connect_via_socketpair(Engine& a, LogicTestListener& la, Engine& b, LogicTestListener& lb) {
    int sv[2]{-1, -1};
    TEST_CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    auto srv_fut = std::async(std::launch::async, [&] {
        return a.connect_with_fd(sv[0], /*is_client=*/false);
    });
    TEST_CHECK(b.connect_with_fd(sv[1], /*is_client=*/true));
    TEST_CHECK(srv_fut.get());
    {
        std::unique_lock<std::mutex> lock(la.mtx);
        TEST_CHECK(la.cv.wait_for(lock, std::chrono::seconds(5), [&] { return la.connected; }));
    }
    {
        std::unique_lock<std::mutex> lock(lb.mtx);
        TEST_CHECK(lb.cv.wait_for(lock, std::chrono::seconds(5), [&] { return lb.connected; }));
    }
}

void write_deterministic_file(const std::filesystem::path& p, size_t bytes, uint8_t seed) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    TEST_CHECK(out.is_open());
    std::vector<uint8_t> buf(bytes);
    for (size_t i = 0; i < bytes; ++i) {
        buf[i] = static_cast<uint8_t>((i * 29 + seed + (i >> 7)) & 0xFFU);
    }
    out.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
}

void test_clipboard_loopback_pull_and_history_persistence() {
    std::cout << "[TEST] 1-3. Clipboard bypass_loopback, live pull vs cache fallback, and >512KB image history persistence...\n";

    auto base = std::filesystem::temp_directory_path() / ("l2a_logic_clip_" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(base, ec);

    EngineConfig cfg_a{base / "a_state", base / "a_recv", "Linux-Host", "linux", 0, 0};
    EngineConfig cfg_b{base / "b_state", base / "b_recv", "Android-Phone", "android", 0, 0};

    LogicTestListener la, lb;
    Engine a(cfg_a, &la);
    Engine b(cfg_b, &lb);
    la.owner = &a;
    lb.owner = &b;

    // 9a. Platform ADB default candidate port check
    TEST_CHECK(a.start());
    TEST_CHECK(b.start());
    auto cand_a = a.candidate_peers();
    auto cand_b = b.candidate_peers();
    TEST_CHECK(!cand_a.empty() && cand_a.back().port == 53322);
    TEST_CHECK(!cand_b.empty() && cand_b.back().port == 53323);

    connect_via_socketpair(a, la, b, lb);

    // 1. Auto push vs bypass_loopback + history non-pollution
    TEST_CHECK(a.push_clipboard_text("AlphaClip"));
    {
        std::unique_lock<std::mutex> lock(lb.mtx);
        TEST_CHECK(lb.cv.wait_for(lock, std::chrono::seconds(3), [&] { return lb.clips.size() == 1; }));
        TEST_CHECK(!lb.clips[0].is_pull_response);
    }

    size_t hist_before = b.history_manager().item_count();
    // Auto-echo must be rejected and NOT added to history
    TEST_CHECK(!b.push_clipboard_text("AlphaClip", /*bypass_loopback=*/false));
    TEST_CHECK(b.history_manager().item_count() == hist_before);

    // Manual push (bypass_loopback=true) MUST succeed and deliver to A
    TEST_CHECK(b.push_clipboard_text("AlphaClip", /*bypass_loopback=*/true));
    {
        std::unique_lock<std::mutex> lock(la.mtx);
        TEST_CHECK(la.cv.wait_for(lock, std::chrono::seconds(3), [&] { return la.clips.size() == 1; }));
    }

    // 2. Live pull callback vs cached fallback + is_pull_response flag
    la.live_pull_enabled.store(true);
    la.live_pull_text = "LiveOSClipboard2026";
    TEST_CHECK(b.pull_remote_clipboard());
    {
        std::unique_lock<std::mutex> lock(lb.mtx);
        TEST_CHECK(lb.cv.wait_for(lock, std::chrono::seconds(3), [&] { return lb.clips.size() == 2; }));
        TEST_CHECK(lb.clips.back().is_pull_response == true);
        std::string got(reinterpret_cast<const char*>(lb.clips.back().data.data()), lb.clips.back().data.size());
        TEST_CHECK(got == "LiveOSClipboard2026");
    }

    // When live pull returns false, fallback to cached latest_local_item_ with is_pull_response == true
    la.live_pull_enabled.store(false);
    TEST_CHECK(b.pull_remote_clipboard());
    {
        std::unique_lock<std::mutex> lock(lb.mtx);
        TEST_CHECK(lb.cv.wait_for(lock, std::chrono::seconds(3), [&] { return lb.clips.size() == 3; }));
        TEST_CHECK(lb.clips.back().is_pull_response == true);
        std::string got(reinterpret_cast<const char*>(lb.clips.back().data.data()), lb.clips.back().data.size());
        TEST_CHECK(got == "LiveOSClipboard2026");
    }

    // 3. Large (>512KB) image history persistence survives TempFileGuard cleanup
    std::vector<uint8_t> big_img(600 * 1024, 0xAB);
    const uint8_t png_sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::memcpy(big_img.data(), png_sig, sizeof(png_sig));
    auto expected_img_hash = sha256_bytes(big_img);

    TEST_CHECK(a.push_clipboard_bytes("image/png", big_img));
    {
        std::unique_lock<std::mutex> lock(lb.mtx);
        TEST_CHECK(lb.cv.wait_for(lock, std::chrono::seconds(5), [&] { return lb.clips.size() == 4; }));
        TEST_CHECK(lb.clips.back().data.empty());
        TEST_CHECK(!lb.clips.back().temp_file_path.empty());
    }
    // Drop received item's TempFileGuard by clearing lb.clips
    {
        std::lock_guard<std::mutex> lock(lb.mtx);
        lb.clips.clear();
    }
    auto b_hist = b.history_manager().get_items();
    TEST_CHECK(!b_hist.empty());
    TEST_CHECK(b_hist.front().type == ClipContentType::Image);
    TEST_CHECK(std::filesystem::exists(b_hist.front().image_path));
    TEST_CHECK(sha256_file_range(b_hist.front().image_path, 0, 0) == expected_img_hash);

    a.stop();
    b.stop();
    std::filesystem::remove_all(base, ec);
    std::cout << "  -> PASSED.\n";
}

void test_full_part_resume_conflict_rename_and_auto_resume() {
    std::cout << "[TEST] 4-6, 9b. Full .l2apart deadlock fix, filename conflict auto-rename, and auto-resume on reconnect...\n";

    auto base = std::filesystem::temp_directory_path() / ("l2a_logic_xfer_" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(base, ec);

    EngineConfig cfg_a{base / "a_state", base / "a_recv", "Sender", "linux", 0, 0};
    EngineConfig cfg_b{base / "b_state", base / "b_recv", "Receiver", "android", 0, 0};

    LogicTestListener la, lb;
    Engine a(cfg_a, &la);
    Engine b(cfg_b, &lb);
    la.owner = &a;
    lb.owner = &b;

    connect_via_socketpair(a, la, b, lb);

    // 4a. Valid full-size .l2apart (part_size == file_size) -> immediate finalize & ACK
    auto src1 = base / "src" / "full_valid.bin";
    write_deterministic_file(src1, 192 * 1024, 0x12);
    auto h1 = *sha256_file_range(src1);
    auto part1 = cfg_b.receive_dir / "full_valid.bin.l2apart";
    std::filesystem::create_directories(cfg_b.receive_dir);
    std::filesystem::copy_file(src1, part1);

    std::vector<std::filesystem::path> p1 = {src1};
    TEST_CHECK(a.send_files(p1) > 0);
    {
        std::unique_lock<std::mutex> lock(lb.mtx);
        TEST_CHECK(lb.cv.wait_for(lock, std::chrono::seconds(5), [&] { return lb.file_events.size() == 1; }));
        TEST_CHECK(lb.file_events[0].success && !lb.file_events[0].is_sending);
    }
    {
        std::unique_lock<std::mutex> lock(la.mtx);
        TEST_CHECK(la.cv.wait_for(lock, std::chrono::seconds(5), [&] { return la.file_events.size() == 1; }));
        TEST_CHECK(la.file_events[0].success && la.file_events[0].is_sending);
    }
    TEST_CHECK(sha256_file_range(cfg_b.receive_dir / "full_valid.bin") == h1);
    TEST_CHECK(!std::filesystem::exists(part1));

    // 4b. Corrupted full-size .l2apart -> purge and cleanly re-transfer from 0
    auto src2 = base / "src" / "full_corrupt.bin";
    write_deterministic_file(src2, 192 * 1024, 0x34);
    auto h2 = *sha256_file_range(src2);
    auto part2 = cfg_b.receive_dir / "full_corrupt.bin.l2apart";
    write_deterministic_file(part2, 192 * 1024, 0x99);  // different content, same size!

    std::vector<std::filesystem::path> p2 = {src2};
    TEST_CHECK(a.send_files(p2) > 0);
    {
        std::unique_lock<std::mutex> lock(lb.mtx);
        TEST_CHECK(lb.cv.wait_for(lock, std::chrono::seconds(5), [&] { return lb.file_events.size() == 2; }));
        TEST_CHECK(lb.file_events.back().success);
    }
    TEST_CHECK(sha256_file_range(cfg_b.receive_dir / "full_corrupt.bin") == h2);

    // 5. Filename conflict auto-rename: existing dup.bin with different SHA-256 -> dup (1).bin
    auto src3_v1 = base / "src_v1" / "dup.bin";
    auto src3_v2 = base / "src_v2" / "dup.bin";
    write_deterministic_file(src3_v1, 8192, 0x55);
    write_deterministic_file(src3_v2, 8192, 0x66);
    auto h3_v1 = *sha256_file_range(src3_v1);
    auto h3_v2 = *sha256_file_range(src3_v2);
    std::filesystem::copy_file(src3_v1, cfg_b.receive_dir / "dup.bin");

    std::vector<std::filesystem::path> p3 = {src3_v2};
    TEST_CHECK(a.send_files(p3) > 0);
    {
        std::unique_lock<std::mutex> lock(lb.mtx);
        TEST_CHECK(lb.cv.wait_for(lock, std::chrono::seconds(5), [&] { return lb.file_events.size() == 3; }));
        TEST_CHECK(lb.file_events.back().success);
        TEST_CHECK(lb.file_events.back().path.filename() == "dup (1).bin");
    }
    TEST_CHECK(sha256_file_range(cfg_b.receive_dir / "dup.bin") == h3_v1);
    TEST_CHECK(sha256_file_range(cfg_b.receive_dir / "dup (1).bin") == h3_v2);

    // 6. Disconnect mid-transfer + automatic resume on reconnect WITHOUT re-calling send_files()
    auto src4 = base / "src" / "auto_resume.bin";
    write_deterministic_file(src4, 512 * 1024, 0x78);
    auto h4 = *sha256_file_range(src4);
    lb.interrupt_on_rx_chunk.store(true);

    std::vector<std::filesystem::path> p4 = {src4};
    TEST_CHECK(a.send_files(p4) > 0);
    {
        std::unique_lock<std::mutex> lock(la.mtx);
        TEST_CHECK(la.cv.wait_for(lock, std::chrono::seconds(5), [&] { return !la.connected; }));
    }
    {
        std::unique_lock<std::mutex> lock(lb.mtx);
        TEST_CHECK(lb.cv.wait_for(lock, std::chrono::seconds(5), [&] { return !lb.connected; }));
    }
    TEST_CHECK(lb.interrupt_done.load());
    lb.interrupt_on_rx_chunk.store(false);

    // Reconnect ONLY — do NOT call send_files again!
    connect_via_socketpair(a, la, b, lb);
    {
        std::unique_lock<std::mutex> lock(lb.mtx);
        TEST_CHECK(lb.cv.wait_for(lock, std::chrono::seconds(8), [&] { return lb.file_events.size() == 4; }));
        TEST_CHECK(lb.file_events.back().success);
    }
    {
        std::unique_lock<std::mutex> lock(la.mtx);
        TEST_CHECK(la.cv.wait_for(lock, std::chrono::seconds(8), [&] { return la.file_events.size() == 4; }));
        TEST_CHECK(la.file_events.back().success && la.file_events.back().is_sending);
    }
    TEST_CHECK(sha256_file_range(cfg_b.receive_dir / "auto_resume.bin") == h4);

    a.stop();
    b.stop();
    std::filesystem::remove_all(base, ec);
    std::cout << "  -> PASSED.\n";
}

void test_dual_connect_arbitration_and_priority_queue() {
    std::cout << "[TEST] 7-8. Simultaneous dual-connect arbitration and clipboard priority over large file...\n";

    auto base = std::filesystem::temp_directory_path() / ("l2a_logic_arb_" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(base, ec);

    EngineConfig cfg_a{base / "a_state", base / "a_recv", "Node-A", "linux", 0, 0};
    EngineConfig cfg_b{base / "b_state", base / "b_recv", "Node-B", "android", 0, 0};

    LogicTestListener la, lb;
    Engine a(cfg_a, &la);
    Engine b(cfg_b, &lb);

    // 7. Simultaneous dual-connect using two socketpairs (A client + B client)
    int sv1[2]{-1, -1};
    int sv2[2]{-1, -1};
    TEST_CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv1) == 0);
    TEST_CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv2) == 0);

    // First connection: A is client, B is server
    auto f1 = std::async(std::launch::async, [&] { return b.connect_with_fd(sv1[1], false); });
    TEST_CHECK(a.connect_with_fd(sv1[0], true));
    TEST_CHECK(f1.get());

    // Second simultaneous connection in opposite direction: B is client, A is server
    auto f2 = std::async(std::launch::async, [&] { return a.connect_with_fd(sv2[0], false); });
    TEST_CHECK(b.connect_with_fd(sv2[1], true));
    TEST_CHECK(f2.get());

    // Both engines must remain connected and converge to one working channel
    TEST_CHECK(a.is_connected());
    TEST_CHECK(b.is_connected());
    TEST_CHECK(a.push_clipboard_text("DualConnectConverged"));
    {
        std::unique_lock<std::mutex> lock(lb.mtx);
        TEST_CHECK(lb.cv.wait_for(lock, std::chrono::seconds(3), [&] { return lb.clips.size() == 1; }));
    }

    // 8. Priority send queue: stream a 4MB file while sending a clipboard text immediately after
    auto big_file = base / "src" / "stream_4mb.bin";
    write_deterministic_file(big_file, 4 * 1024 * 1024, 0xAA);
    std::vector<std::filesystem::path> pf = {big_file};
    TEST_CHECK(a.send_files(pf) > 0);
    TEST_CHECK(a.push_clipboard_text("HighPriorityClipboard"));

    {
        std::unique_lock<std::mutex> lock(lb.mtx);
        TEST_CHECK(lb.cv.wait_for(lock, std::chrono::seconds(3), [&] { return lb.clips.size() == 2; }));
        std::string got(reinterpret_cast<const char*>(lb.clips.back().data.data()), lb.clips.back().data.size());
        TEST_CHECK(got == "HighPriorityClipboard");
    }
    {
        std::unique_lock<std::mutex> lock(lb.mtx);
        TEST_CHECK(lb.cv.wait_for(lock, std::chrono::seconds(10), [&] { return lb.file_events.size() == 1; }));
        TEST_CHECK(lb.file_events[0].success);
    }

    a.stop();
    b.stop();
    std::filesystem::remove_all(base, ec);
    std::cout << "  -> PASSED.\n";
}

}  // namespace

int main() {
    std::cout << "=========================================================\n";
    std::cout << "Running Regression Suite: Clipboard, Transfer & Arbitration\n";
    std::cout << "=========================================================\n";

    test_clipboard_loopback_pull_and_history_persistence();
    test_full_part_resume_conflict_rename_and_auto_resume();
    test_dual_connect_arbitration_and_priority_queue();

    std::cout << "=========================================================\n";
    std::cout << "All Clipboard & Transfer Logic Regression Tests PASSED!\n";
    std::cout << "=========================================================\n";
    return 0;
}
