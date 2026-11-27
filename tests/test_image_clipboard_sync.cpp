#include "l2a/engine.hpp"
#include "l2a/sha256.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

using namespace l2a;

#define TEST_CHECK(cond) \
    do { \
        if (!(cond)) { \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << ": " << #cond << "\n"; \
            std::abort(); \
        } \
    } while (0)

namespace {

class ClipSyncListener : public ICoreListener {
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

std::vector<uint8_t> generate_png_payload(size_t size) {
    std::vector<uint8_t> data(size);
    // Standard PNG signature
    const uint8_t png_header[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::memcpy(data.data(), png_header, sizeof(png_header));
    for (size_t i = sizeof(png_header); i < size; ++i) {
        data[i] = static_cast<uint8_t>((i * 7 + 13) % 256);
    }
    return data;
}

}  // namespace

int main() {
    std::cout << "=========================================================\n";
    std::cout << "Running TEST-02: Image Clipboard Sync & Pull Req Test\n";
    std::cout << "=========================================================\n";

    auto base_dir = std::filesystem::temp_directory_path() /
                    ("l2a_img_clip_test_" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(base_dir, ec);

    EngineConfig cfg_a{};
    cfg_a.storage_dir = base_dir / "pc_state";
    cfg_a.receive_dir = base_dir / "pc_recv";
    cfg_a.device_name = "Linux-Workstation";
    cfg_a.platform = "linux";
    cfg_a.listen_port = 0;
    cfg_a.discovery_port = 0;

    EngineConfig cfg_b{};
    cfg_b.storage_dir = base_dir / "phone_state";
    cfg_b.receive_dir = base_dir / "phone_recv";
    cfg_b.device_name = "Android-Phone";
    cfg_b.platform = "android";
    cfg_b.listen_port = 0;
    cfg_b.discovery_port = 0;

    ClipSyncListener listener_a;
    ClipSyncListener listener_b;

    Engine node_a(cfg_a, &listener_a);
    Engine node_b(cfg_b, &listener_b);

    TEST_CHECK(node_b.start());
    uint16_t port_b = node_b.bound_tcp_port();
    TEST_CHECK(port_b > 0);

    TEST_CHECK(node_a.start());

    node_a.register_candidate_peer("127.0.0.1", port_b);
    TEST_CHECK(node_a.ensure_connected());

    // Wait for connection handshake
    {
        std::unique_lock<std::mutex> lock(listener_a.mtx);
        TEST_CHECK(listener_a.cv.wait_for(lock, std::chrono::seconds(5), [&] {
            return listener_a.connected;
        }));
    }
    {
        std::unique_lock<std::mutex> lock(listener_b.mtx);
        TEST_CHECK(listener_b.cv.wait_for(lock, std::chrono::seconds(5), [&] {
            return listener_b.connected;
        }));
    }

    std::cout << "[INFO] Both nodes connected over TLS.\n";

    // 1. Test binary image clipboard sync (Priority 1)
    std::cout << "[TEST] 1. Push raw image/png binary bytes...\n";
    auto png_data = generate_png_payload(64 * 1024);  // 64 KB
    auto expected_sha256 = sha256_bytes(png_data);

    TEST_CHECK(node_a.push_clipboard_bytes("image/png", png_data));

    // Wait for Node B to receive
    {
        std::unique_lock<std::mutex> lock(listener_b.mtx);
        TEST_CHECK(listener_b.cv.wait_for(lock, std::chrono::seconds(5), [&] {
            return !listener_b.clips.empty();
        }));
    }

    TEST_CHECK(listener_b.clips.size() == 1);
    const auto& rec1 = listener_b.clips[0];
    TEST_CHECK(rec1.mime_type == "image/png");
    TEST_CHECK(rec1.data.size() == png_data.size());
    TEST_CHECK(sha256_bytes(rec1.data) == expected_sha256);

    // Verify history recorded on both nodes
    auto a_history = node_a.history_manager().get_items();
    TEST_CHECK(!a_history.empty());
    TEST_CHECK(a_history.front().type == ClipContentType::Image);
    TEST_CHECK(a_history.front().mime_type == "image/png");

    auto b_history = node_b.history_manager().get_items();
    TEST_CHECK(!b_history.empty());
    TEST_CHECK(b_history.front().type == ClipContentType::Image);
    TEST_CHECK(b_history.front().origin == ClipOrigin::Remote);

    std::cout << "  -> PASSED: Binary image/png transferred and recorded in history managers with SHA-256 match.\n";

    // 2. Test clip_id == 0 pull remote clipboard
    std::cout << "[TEST] 2. Pull remote clipboard with clip_id == 0 from Node B to Node A...\n";
    size_t count_before = listener_b.clips.size();

    // Node B calls pull_remote_clipboard()
    TEST_CHECK(node_b.pull_remote_clipboard());

    // Node A will receive kClipPullReq with clip_id == 0, find latest item, send CLIP_OFFER + stream back to Node B
    {
        std::unique_lock<std::mutex> lock(listener_b.mtx);
        TEST_CHECK(listener_b.cv.wait_for(lock, std::chrono::seconds(5), [&] {
            return listener_b.clips.size() > count_before;
        }));
    }

    TEST_CHECK(listener_b.clips.size() == count_before + 1);
    const auto& pulled_item = listener_b.clips.back();
    TEST_CHECK(pulled_item.mime_type == "image/png");
    TEST_CHECK(pulled_item.data.size() == png_data.size());
    TEST_CHECK(sha256_bytes(pulled_item.data) == expected_sha256);
    std::cout << "  -> PASSED: Remote clipboard pull (clip_id == 0) returned latest item with matching payload.\n";

    // 3. Test File-backed image push (push_clipboard_file)
    std::cout << "[TEST] 3. Push image file via push_clipboard_file...\n";
    auto temp_img_file = base_dir / "sample_photo.jpeg";
    {
        std::ofstream ofs(temp_img_file, std::ios::binary);
        const uint8_t jpeg_header[4] = {0xFF, 0xD8, 0xFF, 0xE0};
        ofs.write(reinterpret_cast<const char*>(jpeg_header), sizeof(jpeg_header));
        std::string dummy(32 * 1024, 'X');
        ofs.write(dummy.data(), dummy.size());
        ofs.close();
    }
    auto file_sha256 = sha256_file_range(temp_img_file, 0, 0);
    TEST_CHECK(file_sha256.has_value());

    count_before = listener_b.clips.size();
    TEST_CHECK(node_a.push_clipboard_file("image/jpeg", temp_img_file));

    {
        std::unique_lock<std::mutex> lock(listener_b.mtx);
        TEST_CHECK(listener_b.cv.wait_for(lock, std::chrono::seconds(5), [&] {
            return listener_b.clips.size() > count_before;
        }));
    }

    const auto& rec_file_clip = listener_b.clips.back();
    TEST_CHECK(rec_file_clip.mime_type == "image/jpeg");
    if (!rec_file_clip.data.empty()) {
        TEST_CHECK(sha256_bytes(rec_file_clip.data) == *file_sha256);
    } else {
        TEST_CHECK(!rec_file_clip.temp_file_path.empty());
        auto rec_sha = sha256_file_range(rec_file_clip.temp_file_path, 0, 0);
        TEST_CHECK(rec_sha.has_value() && *rec_sha == *file_sha256);
    }
    std::cout << "  -> PASSED: File-backed image push transferred and verified with matching SHA-256.\n";

    // 4. Test Anti-loopback suppression
    std::cout << "[TEST] 4. Anti-loopback suppression on received payload echo...\n";
    auto echo_test_bytes = generate_png_payload(16 * 1024);
    TEST_CHECK(node_a.push_clipboard_bytes("image/png", echo_test_bytes));

    // Wait for Node B to receive echo_test_bytes
    {
        std::unique_lock<std::mutex> lock(listener_b.mtx);
        TEST_CHECK(listener_b.cv.wait_for(lock, std::chrono::seconds(5), [&] {
            return !listener_b.clips.empty() && listener_b.clips.back().data.size() == echo_test_bytes.size();
        }));
    }

    size_t a_count_before = listener_a.clips.size();

    // Now Node B's last_received_hash_ is precisely sha256_bytes(echo_test_bytes).
    // When Node B attempts to echo it back to Node A, is_loopback_digest() MUST reject this push!
    bool push_res = node_b.push_clipboard_bytes("image/png", echo_test_bytes);
    TEST_CHECK(!push_res && "Echoing received payload back MUST be blocked by anti-loopback!");

    // Wait a brief duration and verify Node A did NOT receive an echo
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    {
        std::unique_lock<std::mutex> lock(listener_a.mtx);
        TEST_CHECK(listener_a.clips.size() == a_count_before && "Node A must not receive loopback echo!");
    }
    std::cout << "  -> PASSED: Loopback echo correctly blocked by receiver is_loopback_digest check.\n";

    // Cleanup
    node_a.stop();
    node_b.stop();
    std::filesystem::remove_all(base_dir, ec);

    std::cout << "=========================================================\n";
    std::cout << "All Image Clipboard Sync Tests PASSED (4/4)!\n";
    std::cout << "=========================================================\n";
    return 0;
}
