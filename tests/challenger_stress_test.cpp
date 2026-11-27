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
#include <thread>
#include <unistd.h>
#include <vector>

using namespace l2a;

#define CHALLENGER_CHECK(cond, msg)                                                \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << "[CHALLENGER-FAIL] " << __FILE__ << ":" << __LINE__        \
                      << " (" << #cond << "): " << msg << "\n";                    \
            std::abort();                                                           \
        }                                                                           \
    } while (0)

namespace {

class StressListener : public ICoreListener {
public:
    std::mutex mtx;
    std::condition_variable cv;

    bool connected{false};
    std::string peer_name;
    std::vector<ReceivedClipboardItem> clips;
    std::vector<std::filesystem::path> completed_files;

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

    void on_file_completed(
        uint64_t /*batch_id*/,
        uint32_t /*file_index*/,
        const std::filesystem::path& saved_path,
        bool success,
        const std::string& error_msg) override {
        CHALLENGER_CHECK(success, "File completion failed: " + error_msg);
        std::lock_guard<std::mutex> lock(mtx);
        completed_files.push_back(saved_path);
        cv.notify_all();
    }
};

uint16_t find_free_port() {
    int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    CHALLENGER_CHECK(sock >= 0, "socket creation failed");
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    CHALLENGER_CHECK(::bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0, "bind failed");
    socklen_t len = sizeof(addr);
    CHALLENGER_CHECK(::getsockname(sock, reinterpret_cast<sockaddr*>(&addr), &len) == 0, "getsockname failed");
    uint16_t port = ntohs(addr.sin_port);
    ::close(sock);
    return port;
}

void generate_file(const std::filesystem::path& path, size_t size_bytes, uint8_t seed) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    CHALLENGER_CHECK(out.is_open(), "Cannot open file for writing: " + path.string());
    constexpr size_t kChunk = 64 * 1024;
    std::vector<uint8_t> buffer(kChunk);
    size_t written = 0;
    while (written < size_bytes) {
        size_t to_write = std::min(kChunk, size_bytes - written);
        for (size_t i = 0; i < to_write; ++i) {
            buffer[i] = static_cast<uint8_t>(((written + i) * 31 + seed + ((written + i) >> 7)) & 0xFFU);
        }
        out.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(to_write));
        written += to_write;
    }
}

// -----------------------------------------------------------------------------
// Test 1: Rapid Repetitions & Port Recycling Stress (30 Cycles)
// -----------------------------------------------------------------------------
void challenge_1_rapid_cycles() {
    std::cout << "\n[CHALLENGE 1] Rapid Repetitions & Port Recycling (30 iterations)...\n";
    auto base_dir = std::filesystem::temp_directory_path() /
                    ("l2a_chal1_" + std::to_string(::getpid()));
    std::error_code ec;

    auto t_start = std::chrono::steady_clock::now();
    for (int i = 0; i < 30; ++i) {
        std::filesystem::remove_all(base_dir, ec);

        EngineConfig cfg_a{};
        cfg_a.storage_dir = base_dir / "a_state";
        cfg_a.receive_dir = base_dir / "a_recv";
        cfg_a.device_name = "Desktop-Cycler";
        cfg_a.platform = "linux";
        cfg_a.listen_port = 0;
        cfg_a.discovery_port = 0;

        EngineConfig cfg_b{};
        cfg_b.storage_dir = base_dir / "b_state";
        cfg_b.receive_dir = base_dir / "b_recv";
        cfg_b.device_name = "Phone-Cycler";
        cfg_b.platform = "android";
        cfg_b.listen_port = 0;
        cfg_b.discovery_port = 0;

        StressListener la;
        StressListener lb;

        Engine node_a(cfg_a, &la);
        Engine node_b(cfg_b, &lb);

        CHALLENGER_CHECK(node_b.start(), "node_b start failed at cycle " + std::to_string(i));
        uint16_t port_b = node_b.bound_tcp_port();
        CHALLENGER_CHECK(port_b > 0, "bound_tcp_port must be > 0");

        CHALLENGER_CHECK(node_a.start(), "node_a start failed at cycle " + std::to_string(i));
        node_a.register_candidate_peer("127.0.0.1", port_b);

        std::string payload = "Cycle #" + std::to_string(i) + " verification text";
        CHALLENGER_CHECK(node_a.push_clipboard_text(payload), "push_clipboard_text failed at cycle " + std::to_string(i));

        {
            std::unique_lock<std::mutex> lock(lb.mtx);
            bool ok = lb.cv.wait_for(lock, std::chrono::seconds(3), [&] { return !lb.clips.empty(); });
            CHALLENGER_CHECK(ok, "Timeout waiting for clipboard in cycle " + std::to_string(i));
        }

        CHALLENGER_CHECK(node_a.is_connected(), "node_a must be connected");
        CHALLENGER_CHECK(node_b.is_connected(), "node_b must be connected");
        CHALLENGER_CHECK(lb.clips.size() == 1, "Expected 1 clip");
        std::string got(reinterpret_cast<const char*>(lb.clips[0].data.data()), lb.clips[0].data.size());
        CHALLENGER_CHECK(got == payload, "Payload mismatch in cycle " + std::to_string(i));

        node_a.stop();
        node_b.stop();
    }
    auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t_start).count();
    std::filesystem::remove_all(base_dir, ec);
    std::cout << "[CHALLENGE 1 PASSED] 30 rapid iterations completed in " << dur_ms
              << " ms (avg " << (dur_ms / 30.0) << " ms/cycle) with 0 port errors.\n";
}

// -----------------------------------------------------------------------------
// Test 2: Concurrency Stress on Initial Sync (8 simultaneous threads)
// -----------------------------------------------------------------------------
void challenge_2_concurrent_initial_sync() {
    std::cout << "\n[CHALLENGE 2] Concurrency Stress: 8 threads concurrently triggering initial sync...\n";
    auto base_dir = std::filesystem::temp_directory_path() /
                    ("l2a_chal2_" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(base_dir, ec);

    EngineConfig cfg_a{};
    cfg_a.storage_dir = base_dir / "a_state";
    cfg_a.receive_dir = base_dir / "a_recv";
    cfg_a.device_name = "Desktop-Parallel";
    cfg_a.platform = "linux";
    cfg_a.listen_port = 0;
    cfg_a.discovery_port = 0;

    EngineConfig cfg_b{};
    cfg_b.storage_dir = base_dir / "b_state";
    cfg_b.receive_dir = base_dir / "b_recv";
    cfg_b.device_name = "Phone-Parallel";
    cfg_b.platform = "android";
    cfg_b.listen_port = 0;
    cfg_b.discovery_port = 0;

    StressListener la;
    StressListener lb;

    Engine node_a(cfg_a, &la);
    Engine node_b(cfg_b, &lb);

    CHALLENGER_CHECK(node_b.start(), "node_b start failed");
    uint16_t port_b = node_b.bound_tcp_port();
    CHALLENGER_CHECK(port_b > 0, "port_b invalid");

    CHALLENGER_CHECK(node_a.start(), "node_a start failed");
    node_a.register_candidate_peer("127.0.0.1", port_b);
    CHALLENGER_CHECK(!node_a.is_connected(), "node_a must initially be disconnected");

    constexpr int kThreadCount = 8;
    std::vector<std::thread> workers;
    std::atomic<bool> start_gate{false};
    std::atomic<int> success_count{0};

    for (int t = 0; t < kThreadCount; ++t) {
        workers.emplace_back([&, t]() {
            while (!start_gate.load()) {
                std::this_thread::yield();
            }
            std::string msg = "Thread-" + std::to_string(t) + "-Concurrent-Data-" + std::string(100, static_cast<char>('A' + t));
            if (node_a.push_clipboard_text(msg)) {
                success_count.fetch_add(1);
            }
        });
    }

    start_gate.store(true);
    for (auto& w : workers) {
        w.join();
    }

    CHALLENGER_CHECK(success_count.load() == kThreadCount, "Not all threads pushed successfully");

    {
        std::unique_lock<std::mutex> lock(lb.mtx);
        bool ok = lb.cv.wait_for(lock, std::chrono::seconds(5), [&] {
            return lb.clips.size() == kThreadCount;
        });
        CHALLENGER_CHECK(ok, "Timeout waiting for all concurrent clips, received: " + std::to_string(lb.clips.size()));
    }

    CHALLENGER_CHECK(node_a.is_connected(), "node_a must be connected");
    CHALLENGER_CHECK(node_b.is_connected(), "node_b must be connected");
    CHALLENGER_CHECK(lb.clips.size() == kThreadCount, "Expected exactly 8 clips");

    node_a.stop();
    node_b.stop();
    std::filesystem::remove_all(base_dir, ec);
    std::cout << "[CHALLENGE 2 PASSED] 8 concurrent threads initiated on-demand sync without deadlock (8/8 received).\n";
}

// -----------------------------------------------------------------------------
// Test 3: Large Payload Multi-Chunk Slicing & Integrity (1MB & 2MB)
// -----------------------------------------------------------------------------
void challenge_3_large_payload_integrity() {
    std::cout << "\n[CHALLENGE 3] Large Payload Integrity: 1MB and 2MB clipboard payload over initial sync...\n";
    auto base_dir = std::filesystem::temp_directory_path() /
                    ("l2a_chal3_" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(base_dir, ec);

    EngineConfig cfg_a{};
    cfg_a.storage_dir = base_dir / "a_state";
    cfg_a.receive_dir = base_dir / "a_recv";
    cfg_a.device_name = "Desktop-BigClip";
    cfg_a.platform = "linux";
    cfg_a.listen_port = 0;
    cfg_a.discovery_port = 0;

    EngineConfig cfg_b{};
    cfg_b.storage_dir = base_dir / "b_state";
    cfg_b.receive_dir = base_dir / "b_recv";
    cfg_b.device_name = "Phone-BigClip";
    cfg_b.platform = "android";
    cfg_b.listen_port = 0;
    cfg_b.discovery_port = 0;

    StressListener la;
    StressListener lb;

    Engine node_a(cfg_a, &la);
    Engine node_b(cfg_b, &lb);

    CHALLENGER_CHECK(node_b.start(), "node_b start failed");
    uint16_t port_b = node_b.bound_tcp_port();
    CHALLENGER_CHECK(node_a.start(), "node_a start failed");
    node_a.register_candidate_peer("127.0.0.1", port_b);

    // 1MB payload
    size_t sz_1mb = 1024 * 1024;
    std::vector<uint8_t> data_1mb(sz_1mb);
    for (size_t i = 0; i < sz_1mb; ++i) {
        data_1mb[i] = static_cast<uint8_t>((i * 17 + 101) & 0xFFU);
    }
    auto expected_sha_1mb = sha256_bytes(data_1mb);

    std::cout << "[INFO] Pushing 1MB payload over on-demand connection...\n";
    CHALLENGER_CHECK(node_a.push_clipboard_bytes("application/octet-stream", data_1mb, false), "push 1mb failed");

    {
        std::unique_lock<std::mutex> lock(lb.mtx);
        bool ok = lb.cv.wait_for(lock, std::chrono::seconds(10), [&] {
            return !lb.clips.empty();
        });
        CHALLENGER_CHECK(ok, "Timeout waiting for 1MB clip");
    }

    CHALLENGER_CHECK(lb.clips.size() == 1, "Expected 1 clip");
    CHALLENGER_CHECK(lb.clips[0].total_size == sz_1mb, "Total size mismatch on 1MB clip");
    CHALLENGER_CHECK(lb.clips[0].sha256 == expected_sha_1mb, "Receiver hasher SHA-256 mismatch on 1MB clip");
    if (!lb.clips[0].temp_file_path.empty()) {
        std::error_code fec;
        auto actual_fsz = std::filesystem::file_size(lb.clips[0].temp_file_path, fec);
        CHALLENGER_CHECK(!fec && actual_fsz == sz_1mb, "Temp file size mismatch on 1MB clip");
        auto f_sha = *sha256_file_range(lb.clips[0].temp_file_path, 0, 0);
        CHALLENGER_CHECK(f_sha == expected_sha_1mb, "Temp file SHA-256 mismatch on 1MB clip");
        std::cout << "[PASS] 1MB payload streamed to temp file (" << lb.clips[0].temp_file_path.filename()
                  << ") and verified with 100% SHA-256 match.\n";
    } else {
        CHALLENGER_CHECK(lb.clips[0].data.size() == sz_1mb, "Memory buffer size mismatch on 1MB clip");
        auto mem_sha = sha256_bytes(lb.clips[0].data);
        CHALLENGER_CHECK(mem_sha == expected_sha_1mb, "Memory buffer SHA-256 mismatch on 1MB clip");
        std::cout << "[PASS] 1MB payload verified in memory with 100% SHA-256 match.\n";
    }

    // 2MB payload
    size_t sz_2mb = 2 * 1024 * 1024;
    std::vector<uint8_t> data_2mb(sz_2mb);
    for (size_t i = 0; i < sz_2mb; ++i) {
        data_2mb[i] = static_cast<uint8_t>((i * 29 + 47) & 0xFFU);
    }
    auto expected_sha_2mb = sha256_bytes(data_2mb);

    std::cout << "[INFO] Pushing 2MB payload...\n";
    CHALLENGER_CHECK(node_a.push_clipboard_bytes("application/octet-stream", data_2mb, false), "push 2mb failed");

    {
        std::unique_lock<std::mutex> lock(lb.mtx);
        bool ok = lb.cv.wait_for(lock, std::chrono::seconds(15), [&] {
            return lb.clips.size() == 2;
        });
        CHALLENGER_CHECK(ok, "Timeout waiting for 2MB clip");
    }

    CHALLENGER_CHECK(lb.clips.size() == 2, "Expected 2 clips");
    CHALLENGER_CHECK(lb.clips[1].total_size == sz_2mb, "Total size mismatch on 2MB clip");
    CHALLENGER_CHECK(lb.clips[1].sha256 == expected_sha_2mb, "Receiver hasher SHA-256 mismatch on 2MB clip");
    if (!lb.clips[1].temp_file_path.empty()) {
        std::error_code fec;
        auto actual_fsz = std::filesystem::file_size(lb.clips[1].temp_file_path, fec);
        CHALLENGER_CHECK(!fec && actual_fsz == sz_2mb, "Temp file size mismatch on 2MB clip");
        auto f_sha = *sha256_file_range(lb.clips[1].temp_file_path, 0, 0);
        CHALLENGER_CHECK(f_sha == expected_sha_2mb, "Temp file SHA-256 mismatch on 2MB clip");
        std::cout << "[PASS] 2MB payload streamed to temp file (" << lb.clips[1].temp_file_path.filename()
                  << ") and verified with 100% SHA-256 match.\n";
    } else {
        CHALLENGER_CHECK(lb.clips[1].data.size() == sz_2mb, "Memory buffer size mismatch on 2MB clip");
        auto mem_sha = sha256_bytes(lb.clips[1].data);
        CHALLENGER_CHECK(mem_sha == expected_sha_2mb, "Memory buffer SHA-256 mismatch on 2MB clip");
        std::cout << "[PASS] 2MB payload verified in memory with 100% SHA-256 match.\n";
    }

    node_a.stop();
    node_b.stop();
    std::filesystem::remove_all(base_dir, ec);
    std::cout << "[CHALLENGE 3 PASSED] 1MB and 2MB multi-chunk clipboard payloads verified.\n";
}

// -----------------------------------------------------------------------------
// Test 4: Non-Blocking Connect Timeout & Fallback Bounds
// -----------------------------------------------------------------------------
void challenge_4_timeout_bounds_and_fallback() {
    std::cout << "\n[CHALLENGE 4] Non-blocking Connect Timeout Bounds & Graceful Fallback...\n";
    auto base_dir = std::filesystem::temp_directory_path() /
                    ("l2a_chal4_" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(base_dir, ec);

    EngineConfig srv_cfg{};
    srv_cfg.storage_dir = base_dir / "srv_state";
    srv_cfg.receive_dir = base_dir / "srv_recv";
    srv_cfg.device_name = "Server-Desk";
    srv_cfg.platform = "linux";
    srv_cfg.listen_port = 0;
    srv_cfg.discovery_port = 0;

    EngineConfig cli_cfg{};
    cli_cfg.storage_dir = base_dir / "cli_state";
    cli_cfg.receive_dir = base_dir / "cli_recv";
    cli_cfg.device_name = "Client-Phone";
    cli_cfg.platform = "android";
    cli_cfg.listen_port = 0;
    cli_cfg.discovery_port = 0;

    StressListener srv_listener;
    StressListener cli_listener;

    Engine server(srv_cfg, &srv_listener);
    Engine client(cli_cfg, &cli_listener);

    CHALLENGER_CHECK(server.start(), "server start failed");
    uint16_t srv_port = server.bound_tcp_port();

    // 4A: Fast Closed Port Fallback (RST)
    std::cout << "[INFO] Testing 4A: Closed loopback port fallback (immediate ECONNREFUSED)...\n";
    uint16_t closed_port = find_free_port();
    while (closed_port == srv_port) {
        closed_port = find_free_port();
    }

    client.register_candidate_peer("127.0.0.1", srv_port);     // secondary
    client.register_candidate_peer("127.0.0.1", closed_port);  // primary (prepended)

    CHALLENGER_CHECK(client.start(), "client start failed");

    auto t0 = std::chrono::steady_clock::now();
    CHALLENGER_CHECK(client.push_clipboard_text("RST fallback test"), "push_clipboard_text failed");

    {
        std::unique_lock<std::mutex> lock(srv_listener.mtx);
        bool ok = srv_listener.cv.wait_for(lock, std::chrono::seconds(3), [&] {
            return !srv_listener.clips.empty();
        });
        CHALLENGER_CHECK(ok, "Timeout on RST fallback");
    }

    auto rst_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    std::cout << "[PASS] 4A: RST fallback took " << rst_elapsed_ms << " ms (< 200 ms threshold).\n";
    CHALLENGER_CHECK(rst_elapsed_ms < 200, "RST fallback should be nearly instantaneous (< 200ms)");

    client.stop();
    server.stop();

    // 4B: Dropping / Non-responsive IP Fallback (SYN timeout ~1000ms via poll)
    std::cout << "[INFO] Testing 4B: Non-responsive IP fallback (SYN drop with 1000ms poll timeout)...\n";
    StressListener srv_listener_b;
    StressListener cli_listener_b;
    Engine server_b(srv_cfg, &srv_listener_b);
    Engine client_b(cli_cfg, &cli_listener_b);

    CHALLENGER_CHECK(server_b.start(), "server_b start failed");
    uint16_t srv_port_b = server_b.bound_tcp_port();

    // 192.0.2.1 is RFC 5737 TEST-NET-1 (unassigned/blackholed on standard networks)
    // Connecting to it will cause SYN packet to be sent without RST response, triggering poll timeout!
    client_b.register_candidate_peer("127.0.0.1", srv_port_b);    // secondary
    client_b.register_candidate_peer("192.0.2.1", 53322);         // primary (dropped)

    CHALLENGER_CHECK(client_b.start(), "client_b start failed");

    auto t1 = std::chrono::steady_clock::now();
    CHALLENGER_CHECK(client_b.push_clipboard_text("SYN drop fallback test"), "push failed");

    {
        std::unique_lock<std::mutex> lock(srv_listener_b.mtx);
        bool ok = srv_listener_b.cv.wait_for(lock, std::chrono::seconds(5), [&] {
            return !srv_listener_b.clips.empty();
        });
        CHALLENGER_CHECK(ok, "Timeout on SYN drop fallback");
    }

    auto syn_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t1).count();
    std::cout << "[PASS] 4B: Non-responsive IP fallback took " << syn_elapsed_ms
              << " ms (< 1000ms bound).\n";
    CHALLENGER_CHECK(syn_elapsed_ms < 1000, "Fallback must be non-blocking and complete in < 1000ms");

    // 4C: Completely Unreachable (No valid candidate)
    std::cout << "[INFO] Testing 4C: Completely unreachable destination returns false cleanly...\n";
    StressListener cli_listener_c;
    Engine client_c(cli_cfg, &cli_listener_c);
    client_c.register_candidate_peer("127.0.0.1", closed_port);
    CHALLENGER_CHECK(client_c.start(), "client_c start failed");

    bool push_res = client_c.push_clipboard_text("Should fail gracefully");
    CHALLENGER_CHECK(!push_res, "Push must return false when no candidates can connect");
    CHALLENGER_CHECK(!client_c.is_connected(), "Must remain disconnected");

    client_b.stop();
    server_b.stop();
    client_c.stop();
    std::filesystem::remove_all(base_dir, ec);
    std::cout << "[CHALLENGE 4 PASSED] Non-blocking timeouts strictly bounded under both RST and SYN drops.\n";
}

// -----------------------------------------------------------------------------
// Test 5: Reverse Transfer High-Stress File Integrity (5MB + 10 Files)
// -----------------------------------------------------------------------------
void challenge_5_reverse_file_transfer_integrity() {
    std::cout << "\n[CHALLENGE 5] Reverse File Transfer Stress & 100% SHA-256 Integrity (5MB + 10 files)...\n";
    auto base_dir = std::filesystem::temp_directory_path() /
                    ("l2a_chal5_" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(base_dir, ec);

    EngineConfig srv_cfg{};
    srv_cfg.storage_dir = base_dir / "srv_state";
    srv_cfg.receive_dir = base_dir / "srv_recv";
    srv_cfg.device_name = "Desktop-Receiver";
    srv_cfg.platform = "linux";
    srv_cfg.listen_port = 0;
    srv_cfg.discovery_port = 0;

    EngineConfig cli_cfg{};
    cli_cfg.storage_dir = base_dir / "cli_state";
    cli_cfg.receive_dir = base_dir / "cli_recv";
    cli_cfg.device_name = "Phone-Reverse-Sender";
    cli_cfg.platform = "android";
    cli_cfg.listen_port = 0;
    cli_cfg.discovery_port = 0;

    StressListener srv_listener;
    StressListener cli_listener;

    Engine server(srv_cfg, &srv_listener);
    Engine client(cli_cfg, &cli_listener);

    CHALLENGER_CHECK(server.start(), "server start failed");
    uint16_t srv_port = server.bound_tcp_port();

    client.register_candidate_peer("127.0.0.1", srv_port);
    CHALLENGER_CHECK(client.start(), "client start failed");

    auto src_dir = base_dir / "send_files";
    std::vector<std::filesystem::path> batch;
    std::vector<Sha256Digest> expected_hashes;

    // 1x 5MB file
    auto f_5mb = src_dir / "large_5mb.bin";
    generate_file(f_5mb, 5 * 1024 * 1024, 7);
    batch.push_back(f_5mb);
    expected_hashes.push_back(*sha256_file_range(f_5mb, 0, 0));

    // 10x varied size files (4KB to 256KB)
    for (int i = 0; i < 10; ++i) {
        auto f_i = src_dir / ("file_" + std::to_string(i) + ".dat");
        size_t sz = (i + 1) * 24 * 1024; // 24KB to 240KB
        generate_file(f_i, sz, static_cast<uint8_t>(i * 13 + 5));
        batch.push_back(f_i);
        expected_hashes.push_back(*sha256_file_range(f_i, 0, 0));
    }

    std::cout << "[INFO] Initiating send_files() for batch of 11 files (total ~6.3 MB) over on-demand reverse connection...\n";
    auto t0 = std::chrono::steady_clock::now();
    uint64_t batch_id = client.send_files(batch);
    CHALLENGER_CHECK(batch_id > 0, "send_files returned 0 batch_id");

    {
        std::unique_lock<std::mutex> lock(srv_listener.mtx);
        bool ok = srv_listener.cv.wait_for(lock, std::chrono::seconds(25), [&] {
            return srv_listener.completed_files.size() == batch.size();
        });
        CHALLENGER_CHECK(ok, "Timeout waiting for 11 files, received: " + std::to_string(srv_listener.completed_files.size()));
    }

    auto xfer_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    CHALLENGER_CHECK(srv_listener.completed_files.size() == batch.size(), "Mismatch in completed files count");

    // Verify 100% SHA-256 match for every file
    for (size_t i = 0; i < batch.size(); ++i) {
        auto filename = batch[i].filename().string();
        bool found = false;
        for (const auto& completed : srv_listener.completed_files) {
            if (completed.filename().string() == filename) {
                found = true;
                auto got_hash = *sha256_file_range(completed, 0, 0);
                CHALLENGER_CHECK(got_hash == expected_hashes[i], "SHA-256 hash mismatch for file: " + filename);
                break;
            }
        }
        CHALLENGER_CHECK(found, "Completed file not found for: " + filename);
    }

    client.stop();
    server.stop();
    std::filesystem::remove_all(base_dir, ec);
    std::cout << "[CHALLENGE 5 PASSED] Batch of 11 files (6.3MB total) transferred in "
              << xfer_elapsed_ms << " ms with 100% SHA-256 bit-exact match.\n";
}

}  // namespace

int main() {
    std::cout << "=================================================================\n";
    std::cout << "CHALLENGER EMPIRICAL ADVERSARIAL STRESS TEST SUITE\n";
    std::cout << "Target: test_initial_clipboard_sync & test_reverse_transfer_and_fallback\n";
    std::cout << "=================================================================\n";

    challenge_1_rapid_cycles();
    challenge_2_concurrent_initial_sync();
    challenge_3_large_payload_integrity();
    challenge_4_timeout_bounds_and_fallback();
    challenge_5_reverse_file_transfer_integrity();

    std::cout << "\n=================================================================\n";
    std::cout << "ALL 5 CHALLENGER STRESS TESTS COMPLETED WITH 100% PASS RATE!\n";
    std::cout << "=================================================================\n";
    return 0;
}
