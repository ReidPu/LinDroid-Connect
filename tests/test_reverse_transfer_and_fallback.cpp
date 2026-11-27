#include "l2a/engine.hpp"
#include "l2a/sha256.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
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

class ReverseTestListener : public ICoreListener {
public:
    std::mutex mtx;
    std::condition_variable cv;

    bool connected{false};
    std::vector<ReceivedClipboardItem> clips;
    std::vector<std::filesystem::path> completed_files;

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

    void on_file_completed(
        uint64_t /*batch_id*/,
        uint32_t /*file_index*/,
        const std::filesystem::path& saved_path,
        bool success,
        const std::string& error_msg) override {
        if (!success) {
            std::cerr << "[TEST ERROR] File transfer failed: " << error_msg << "\n";
        }
        TEST_CHECK(success);
        std::lock_guard<std::mutex> lock(mtx);
        completed_files.push_back(saved_path);
        cv.notify_all();
    }
};

void create_test_file(const std::filesystem::path& path, size_t size_bytes, uint8_t seed) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    TEST_CHECK(out.is_open());
    std::vector<uint8_t> buffer(size_bytes);
    for (size_t i = 0; i < size_bytes; ++i) {
        buffer[i] = static_cast<uint8_t>((i * 23 + seed + (i >> 5)) & 0xFFU);
    }
    out.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
}

uint16_t find_unbound_port() {
    int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    TEST_CHECK(sock >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    TEST_CHECK(::bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    TEST_CHECK(::getsockname(sock, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    uint16_t port = ntohs(addr.sin_port);
    ::close(sock);
    return port;
}

void test_scenario_a_direct_lan() {
    std::cout << "\n--- Scenario A: Direct LAN IP Reverse Transfer (R2/R3) ---\n";

    auto base_dir = std::filesystem::temp_directory_path() /
                    ("l2a_rev_scen_a_" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(base_dir, ec);

    EngineConfig srv_cfg{};
    srv_cfg.storage_dir = base_dir / "server_state";
    srv_cfg.receive_dir = base_dir / "server_recv";
    srv_cfg.device_name = "Linux-Host-Desktop";
    srv_cfg.platform = "linux";
    srv_cfg.listen_port = 0;
    srv_cfg.discovery_port = 0;

    EngineConfig cli_cfg{};
    cli_cfg.storage_dir = base_dir / "client_state";
    cli_cfg.receive_dir = base_dir / "client_recv";
    cli_cfg.device_name = "Android-Phone-Sender";
    cli_cfg.platform = "android";
    cli_cfg.listen_port = 0;
    cli_cfg.discovery_port = 0;

    ReverseTestListener srv_listener;
    ReverseTestListener cli_listener;

    Engine server(srv_cfg, &srv_listener);
    Engine client(cli_cfg, &cli_listener);

    // 1. Start Server on loopback
    TEST_CHECK(server.start());
    uint16_t srv_port = server.bound_tcp_port();
    TEST_CHECK(srv_port > 0);

    // 2. Start Client and register Server endpoint
    TEST_CHECK(client.start());
    TEST_CHECK(!client.is_connected());
    TEST_CHECK(!server.is_connected());

    client.register_candidate_peer("127.0.0.1", srv_port);
    auto candidates = client.candidate_peers();
    TEST_CHECK(candidates.size() >= 1);
    TEST_CHECK(candidates[0].ip == "127.0.0.1");
    TEST_CHECK(candidates[0].port == srv_port);

    // 3. Client sends clipboard text -> triggers ensure_connected() on demand
    std::string clip_text = "Scenario A: Direct LAN clipboard from Phone to Desktop";
    std::cout << "[INFO] Client pushing clipboard text to trigger on-demand reverse connection...\n";
    TEST_CHECK(client.push_clipboard_text(clip_text));

    {
        std::unique_lock<std::mutex> lock(srv_listener.mtx);
        TEST_CHECK(srv_listener.cv.wait_for(
            lock, std::chrono::seconds(5), [&] { return !srv_listener.clips.empty(); }));
    }

    TEST_CHECK(client.is_connected());
    TEST_CHECK(server.is_connected());
    TEST_CHECK(srv_listener.clips.size() == 1);
    std::string got_clip(reinterpret_cast<const char*>(srv_listener.clips[0].data.data()),
                         srv_listener.clips[0].data.size());
    TEST_CHECK(got_clip == clip_text);
    std::cout << "[PASS] Clipboard text successfully received on Server.\n";

    // 4. Client sends a batch of 3 files (4KB, 64KB, 256KB)
    auto src_dir = base_dir / "client_files";
    auto file1 = src_dir / "doc.txt";
    auto file2 = src_dir / "photo.png";
    auto file3 = src_dir / "binary.dat";

    create_test_file(file1, 4 * 1024, 11);
    create_test_file(file2, 64 * 1024, 22);
    create_test_file(file3, 256 * 1024, 33);

    auto h1 = *sha256_file_range(file1, 0, 0);
    auto h2 = *sha256_file_range(file2, 0, 0);
    auto h3 = *sha256_file_range(file3, 0, 0);

    std::vector<std::filesystem::path> batch = {file1, file2, file3};
    std::cout << "[INFO] Client sending batch of 3 files (4KB, 64KB, 256KB)...\n";
    uint64_t batch_id = client.send_files(batch);
    TEST_CHECK(batch_id > 0);

    {
        std::unique_lock<std::mutex> lock(srv_listener.mtx);
        TEST_CHECK(srv_listener.cv.wait_for(
            lock, std::chrono::seconds(10), [&] { return srv_listener.completed_files.size() == 3; }));
    }

    // 5. Verify received file integrity on Server
    for (const auto& completed : srv_listener.completed_files) {
        auto filename = completed.filename().string();
        auto got_hash = *sha256_file_range(completed, 0, 0);
        if (filename == "doc.txt") {
            TEST_CHECK(got_hash == h1);
        } else if (filename == "photo.png") {
            TEST_CHECK(got_hash == h2);
        } else if (filename == "binary.dat") {
            TEST_CHECK(got_hash == h3);
        } else {
            TEST_CHECK(false && "Unexpected completed file filename");
        }
    }

    client.stop();
    server.stop();
    std::filesystem::remove_all(base_dir, ec);
    std::cout << "[PASS] Scenario A: Direct LAN connection, clipboard sync, and file batch integrity verified.\n";
}

void test_scenario_b_dual_candidate_fallback() {
    std::cout << "\n--- Scenario B: Dual Candidate Fallback (Unreachable Primary -> Secondary Tunnel) ---\n";

    auto base_dir = std::filesystem::temp_directory_path() /
                    ("l2a_rev_scen_b_" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(base_dir, ec);

    EngineConfig srv_cfg{};
    srv_cfg.storage_dir = base_dir / "server_state";
    srv_cfg.receive_dir = base_dir / "server_recv";
    srv_cfg.device_name = "Linux-Host-Desktop";
    srv_cfg.platform = "linux";
    srv_cfg.listen_port = 0;
    srv_cfg.discovery_port = 0;

    EngineConfig cli_cfg{};
    cli_cfg.storage_dir = base_dir / "client_state";
    cli_cfg.receive_dir = base_dir / "client_recv";
    cli_cfg.device_name = "Android-Phone-Sender";
    cli_cfg.platform = "android";
    cli_cfg.listen_port = 0;
    cli_cfg.discovery_port = 0;

    ReverseTestListener srv_listener;
    ReverseTestListener cli_listener;

    Engine server(srv_cfg, &srv_listener);
    Engine client(cli_cfg, &cli_listener);

    // 1. Server listens on fallback port (simulating USB ADB reverse tunnel 127.0.0.1:53323)
    TEST_CHECK(server.start());
    uint16_t secondary_port = server.bound_tcp_port();
    TEST_CHECK(secondary_port > 0);

    // 2. Identify an unreachable primary port (closed socket on loopback)
    uint16_t primary_unreachable_port = find_unbound_port();
    while (primary_unreachable_port == secondary_port) {
        primary_unreachable_port = find_unbound_port();
    }

    // 3. Register candidates: register secondary first, then primary
    // Candidate list prepends so index 0 is primary, index 1 is secondary fallback
    client.register_candidate_peer("127.0.0.1", secondary_port);
    client.register_candidate_peer("127.0.0.1", primary_unreachable_port);

    auto candidates = client.candidate_peers();
    TEST_CHECK(candidates.size() >= 2);
    TEST_CHECK(candidates[0].port == primary_unreachable_port);
    TEST_CHECK(candidates[1].port == secondary_port);

    TEST_CHECK(client.start());
    TEST_CHECK(!client.is_connected());
    TEST_CHECK(!server.is_connected());

    // 4. Client sends clipboard text -> primary unreachable, must fall back to secondary
    std::string fallback_clip = "Scenario B: Fallback connection succeeded via secondary endpoint!";
    std::cout << "[INFO] Triggering client push_clipboard_text with primary candidate down...\n";

    auto start_time = std::chrono::steady_clock::now();
    TEST_CHECK(client.push_clipboard_text(fallback_clip));

    {
        std::unique_lock<std::mutex> lock(srv_listener.mtx);
        TEST_CHECK(srv_listener.cv.wait_for(
            lock, std::chrono::seconds(5), [&] { return !srv_listener.clips.empty(); }));
    }

    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time).count();

    // Assert prompt fallback without blocking/hanging
    TEST_CHECK(elapsed_ms < 3000 && "Fallback must connect in < 3000ms");
    TEST_CHECK(client.is_connected());
    TEST_CHECK(server.is_connected());
    TEST_CHECK(srv_listener.clips.size() == 1);
    std::string got_clip(reinterpret_cast<const char*>(srv_listener.clips[0].data.data()),
                         srv_listener.clips[0].data.size());
    TEST_CHECK(got_clip == fallback_clip);
    std::cout << "[PASS] Fallback connection established in " << elapsed_ms
              << " ms and clipboard delivered.\n";

    // 5. Send file batch over the fallback connection
    auto src_dir = base_dir / "client_fallback_files";
    auto file_a = src_dir / "report.pdf";
    auto file_b = src_dir / "archive.tar";

    create_test_file(file_a, 32 * 1024, 77);
    create_test_file(file_b, 128 * 1024, 88);

    auto ha = *sha256_file_range(file_a, 0, 0);
    auto hb = *sha256_file_range(file_b, 0, 0);

    std::vector<std::filesystem::path> batch = {file_a, file_b};
    std::cout << "[INFO] Client sending batch of 2 files over fallback connection...\n";
    uint64_t batch_id = client.send_files(batch);
    TEST_CHECK(batch_id > 0);

    {
        std::unique_lock<std::mutex> lock(srv_listener.mtx);
        TEST_CHECK(srv_listener.cv.wait_for(
            lock, std::chrono::seconds(10), [&] { return srv_listener.completed_files.size() == 2; }));
    }

    for (const auto& completed : srv_listener.completed_files) {
        auto filename = completed.filename().string();
        auto got_hash = *sha256_file_range(completed, 0, 0);
        if (filename == "report.pdf") {
            TEST_CHECK(got_hash == ha);
        } else if (filename == "archive.tar") {
            TEST_CHECK(got_hash == hb);
        } else {
            TEST_CHECK(false && "Unexpected completed file filename in Scenario B");
        }
    }

    client.stop();
    server.stop();
    std::filesystem::remove_all(base_dir, ec);
    std::cout << "[PASS] Scenario B: Dual candidate fallback, non-blocking timeout, clipboard and files verified.\n";
}

}  // namespace

int main() {
    std::cout << "=========================================================\n";
    std::cout << "Running REAL-13: Reverse Transfer & Dual Fallback Suite\n";
    std::cout << "=========================================================\n";

    test_scenario_a_direct_lan();
    test_scenario_b_dual_candidate_fallback();

    std::cout << "=========================================================\n";
    std::cout << "All REAL-13 Reverse Transfer & Fallback Tests PASSED (2/2)!\n";
    std::cout << "=========================================================\n";
    return 0;
}
