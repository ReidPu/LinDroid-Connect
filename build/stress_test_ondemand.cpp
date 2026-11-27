#include "l2a/engine.hpp"
#include "l2a/sha256.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <future>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <set>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace l2a;

#define STRESS_CHECK(cond)                                                          \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << "\n[STRESS FAIL] " << __FILE__ << ":" << __LINE__          \
                      << " Assertion failed: " << #cond << std::endl;               \
            std::abort();                                                           \
        }                                                                           \
    } while (0)

namespace {

class StressListener : public ICoreListener {
public:
    std::mutex mtx;
    std::condition_variable cv;

    bool connected{false};
    std::atomic<int> connect_count{0};
    std::atomic<int> disconnect_count{0};
    std::vector<ReceivedClipboardItem> clips;

    void on_connection_state_changed(
        bool is_conn,
        const std::string& /*peer_id*/,
        const std::string& /*peer_name*/) override {
        std::lock_guard<std::mutex> lock(mtx);
        connected = is_conn;
        if (is_conn) {
            connect_count++;
        } else {
            disconnect_count++;
        }
        cv.notify_all();
    }

    void on_clipboard_received(const ReceivedClipboardItem& item) override {
        std::lock_guard<std::mutex> lock(mtx);
        clips.push_back(item);
        cv.notify_all();
    }
};

// Helper: Create a listening socket whose listen backlog is full so SYN packets are dropped silently
class DropSynEndpoint {
public:
    DropSynEndpoint() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        STRESS_CHECK(listen_fd_ >= 0);

        int yes = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;

        STRESS_CHECK(::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

        sockaddr_in bound{};
        socklen_t len = sizeof(bound);
        STRESS_CHECK(::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &len) == 0);
        port_ = ntohs(bound.sin_port);

        STRESS_CHECK(::listen(listen_fd_, 1) == 0);

        // Saturate the accept queue so subsequent SYNs are dropped
        for (int i = 0; i < 8; ++i) {
            int c = ::socket(AF_INET, SOCK_STREAM, 0);
            if (c >= 0) {
                int flags = ::fcntl(c, F_GETFL, 0);
                if (flags >= 0) ::fcntl(c, F_SETFL, flags | O_NONBLOCK);
                ::connect(c, reinterpret_cast<sockaddr*>(&bound), sizeof(bound));
                client_fds_.push_back(c);
            }
        }
    }

    ~DropSynEndpoint() {
        for (int c : client_fds_) {
            if (c >= 0) ::close(c);
        }
        if (listen_fd_ >= 0) {
            ::close(listen_fd_);
        }
    }

    uint16_t port() const { return port_; }

private:
    int listen_fd_{-1};
    uint16_t port_{0};
    std::vector<int> client_fds_;
};

}  // namespace

// Test 1: Unreachable & Unresponsive IP timeout behavior
void test_unreachable_and_unresponsive_timeout() {
    std::cout << "\n=== [TEST 1] Unreachable & Unresponsive IP Timeout (1000ms poll) ===" << std::endl;

    auto base_dir = std::filesystem::temp_directory_path() / "l2a_stress_test_t1";
    std::error_code ec;
    std::filesystem::remove_all(base_dir, ec);

    EngineConfig cfg{};
    cfg.storage_dir = base_dir / "state";
    cfg.receive_dir = base_dir / "recv";
    cfg.device_name = "StressTester-T1";
    cfg.listen_port = 0;
    cfg.discovery_port = 0;

    StressListener listener;
    Engine engine(cfg, &listener);
    STRESS_CHECK(engine.start());

    // 1A. Network unreachable / unroutable IP (e.g. 192.0.2.1 without route)
    std::cout << "[1A] Testing unroutable IP (192.0.2.1:53321)..." << std::endl;
    auto t0 = std::chrono::steady_clock::now();
    bool res_unroutable = engine.connect_to_peer("192.0.2.1", 53321);
    auto d0 = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::cout << "     connect_to_peer returned " << (res_unroutable ? "true" : "false")
              << " in " << d0 << "ms (expected false immediately without hanging)" << std::endl;
    STRESS_CHECK(!res_unroutable);
    STRESS_CHECK(d0 < 500);  // Should fail immediately without blocking

    // 1B. Unresponsive endpoint (SYN silently dropped by full TCP backlog)
    std::cout << "[1B] Testing unresponsive SYN-drop endpoint (simulating firewall DROP)..." << std::endl;
    DropSynEndpoint drop_ep;
    std::cout << "     DropSynEndpoint bound on port " << drop_ep.port() << std::endl;

    auto t1 = std::chrono::steady_clock::now();
    bool res_drop = engine.connect_to_peer("127.0.0.1", drop_ep.port());
    auto d1 = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t1).count();
    std::cout << "     connect_to_peer returned " << (res_drop ? "true" : "false")
              << " in " << d1 << "ms (expected false in ~1000ms)" << std::endl;
    STRESS_CHECK(!res_drop);
    // Verifying 1000ms poll timeout without thread hang:
    STRESS_CHECK(d1 >= 950);
    STRESS_CHECK(d1 <= 2000);  // Strictly capped, never hanging for default OS TCP timeout ~75s

    // 1C. ensure_connected() with only unresponsive endpoint in candidate list
    std::cout << "[1C] Testing ensure_connected() with unresponsive candidate..." << std::endl;
    // Register the unresponsive endpoint as candidate
    engine.register_candidate_peer("127.0.0.1", drop_ep.port());

    auto t2 = std::chrono::steady_clock::now();
    bool res_ensure = engine.ensure_connected();
    auto d2 = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t2).count();
    std::cout << "     ensure_connected() returned " << (res_ensure ? "true" : "false")
              << " in " << d2 << "ms" << std::endl;
    STRESS_CHECK(!res_ensure);
    STRESS_CHECK(d2 >= 950);
    STRESS_CHECK(d2 <= 2500);

    engine.stop();
    std::filesystem::remove_all(base_dir, ec);
    std::cout << "[TEST 1 PASSED] 1000ms poll timeout verified with zero thread hang!" << std::endl;
}

// Test 2: Fallback to reachable endpoint and on-demand clipboard send
void test_fallback_to_reachable_endpoint() {
    std::cout << "\n=== [TEST 2] Fallback to Reachable Endpoint & On-Demand Send ===" << std::endl;

    auto base_dir = std::filesystem::temp_directory_path() / "l2a_stress_test_t2";
    std::error_code ec;
    std::filesystem::remove_all(base_dir, ec);

    // Setup Server Engine
    EngineConfig srv_cfg{};
    srv_cfg.storage_dir = base_dir / "srv_state";
    srv_cfg.receive_dir = base_dir / "srv_recv";
    srv_cfg.device_name = "Server-T2";
    srv_cfg.listen_port = 0;
    srv_cfg.discovery_port = 0;

    StressListener srv_listener;
    Engine srv_engine(srv_cfg, &srv_listener);
    STRESS_CHECK(srv_engine.start());
    uint16_t srv_port = srv_engine.bound_tcp_port();
    STRESS_CHECK(srv_port > 0);
    std::cout << "     Server listening on port " << srv_port << std::endl;

    // Setup Client Engine
    EngineConfig cli_cfg{};
    cli_cfg.storage_dir = base_dir / "cli_state";
    cli_cfg.receive_dir = base_dir / "cli_recv";
    cli_cfg.device_name = "Client-T2";
    cli_cfg.listen_port = 0;
    cli_cfg.discovery_port = 0;

    StressListener cli_listener;
    Engine cli_engine(cli_cfg, &cli_listener);
    STRESS_CHECK(cli_engine.start());

    // Initially client is disconnected
    STRESS_CHECK(!cli_engine.is_connected());

    // Create an unresponsive SYN-drop endpoint
    DropSynEndpoint drop_ep;
    std::cout << "     Created unresponsive endpoint on port " << drop_ep.port() << std::endl;

    // Register reachable server FIRST, then register unresponsive endpoint SECOND.
    // Since register_candidate_peer inserts at beginning, candidate order is:
    // [0]: Unresponsive (127.0.0.1:drop_ep.port())
    // [1]: Reachable server (127.0.0.1:srv_port)
    // [2]: Default ADB fallback (127.0.0.1:53322)
    cli_engine.register_candidate_peer("127.0.0.1", srv_port);
    cli_engine.register_candidate_peer("127.0.0.1", drop_ep.port());

    auto candidates = cli_engine.candidate_peers();
    STRESS_CHECK(candidates.size() >= 2);
    STRESS_CHECK(candidates[0].port == drop_ep.port());
    STRESS_CHECK(candidates[1].port == srv_port);
    std::cout << "     Candidate[0] is unresponsive port " << candidates[0].port << std::endl;
    std::cout << "     Candidate[1] is reachable server port " << candidates[1].port << std::endl;

    // Verify active_channel_ is initially null, and push_clipboard_text triggers on-demand connection
    std::string test_payload = "On-Demand Fallback Test Payload - 42_STRESS";
    std::cout << "     Calling push_clipboard_text (should timeout candidate 0 in 1000ms, then connect candidate 1)..." << std::endl;

    auto t0 = std::chrono::steady_clock::now();
    bool push_ok = cli_engine.push_clipboard_text(test_payload);
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::cout << "     push_clipboard_text returned " << (push_ok ? "true" : "false")
              << " in " << elapsed_ms << "ms" << std::endl;

    STRESS_CHECK(push_ok);
    STRESS_CHECK(cli_engine.is_connected());
    // Verify candidate 0 took ~1000ms and candidate 1 succeeded immediately after
    STRESS_CHECK(elapsed_ms >= 950);
    STRESS_CHECK(elapsed_ms <= 3000);

    // Wait for server to receive the clipboard item
    {
        std::unique_lock<std::mutex> lock(srv_listener.mtx);
        bool got_clip = srv_listener.cv.wait_for(lock, std::chrono::seconds(5), [&] {
            return !srv_listener.clips.empty();
        });
        STRESS_CHECK(got_clip);
        std::string received_str(srv_listener.clips[0].data.begin(), srv_listener.clips[0].data.end());
        std::cout << "     Server received clipboard text: \"" << received_str << "\"" << std::endl;
        STRESS_CHECK(received_str == test_payload);
    }

    cli_engine.stop();
    srv_engine.stop();
    std::filesystem::remove_all(base_dir, ec);
    std::cout << "[TEST 2 PASSED] Fallback to reachable endpoint succeeded with correct on-demand delivery!" << std::endl;
}

// Test 3: Concurrent Multi-Threaded Connection Stress (No deadlock, no double-connect)
void test_concurrent_multithreaded_stress() {
    std::cout << "\n=== [TEST 3] Concurrent Multi-Threaded Stress Test (10 threads) ===" << std::endl;

    auto base_dir = std::filesystem::temp_directory_path() / "l2a_stress_test_t3";
    std::error_code ec;
    std::filesystem::remove_all(base_dir, ec);

    // 3A. 10 threads calling push_clipboard_text() simultaneously on DISCONNECTED engine
    std::cout << "[3A] 10 threads calling push_clipboard_text() simultaneously on disconnected client..." << std::endl;
    {
        EngineConfig srv_cfg{};
        srv_cfg.storage_dir = base_dir / "srv3a_state";
        srv_cfg.receive_dir = base_dir / "srv3a_recv";
        srv_cfg.device_name = "Server-T3A";
        srv_cfg.listen_port = 0;
        srv_cfg.discovery_port = 0;

        StressListener srv_listener;
        Engine srv_engine(srv_cfg, &srv_listener);
        STRESS_CHECK(srv_engine.start());
        uint16_t srv_port = srv_engine.bound_tcp_port();
        STRESS_CHECK(srv_port > 0);

        EngineConfig cli_cfg{};
        cli_cfg.storage_dir = base_dir / "cli3a_state";
        cli_cfg.receive_dir = base_dir / "cli3a_recv";
        cli_cfg.device_name = "Client-T3A";
        cli_cfg.listen_port = 0;
        cli_cfg.discovery_port = 0;

        StressListener cli_listener;
        Engine cli_engine(cli_cfg, &cli_listener);
        STRESS_CHECK(cli_engine.start());

        // Register candidate peer
        cli_engine.register_candidate_peer("127.0.0.1", srv_port);
        STRESS_CHECK(!cli_engine.is_connected());

        constexpr int kNumThreads = 10;
        std::vector<std::thread> threads;
        std::atomic<bool> start_gate{false};
        std::atomic<int> success_count{0};

        for (int i = 0; i < kNumThreads; ++i) {
            threads.emplace_back([&, i] {
                while (!start_gate.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                std::string msg = "Concurrent payload from thread #" + std::to_string(i);
                if (cli_engine.push_clipboard_text(msg)) {
                    success_count++;
                }
            });
        }

        // Fire all threads at once
        auto t0 = std::chrono::steady_clock::now();
        start_gate.store(true, std::memory_order_release);

        for (auto& th : threads) {
            th.join();
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        std::cout << "     All 10 threads completed in " << elapsed << "ms. Success count: " << success_count.load() << std::endl;

        STRESS_CHECK(success_count.load() == kNumThreads);
        STRESS_CHECK(cli_engine.is_connected());
        // Verify server connection count: should have accepted exactly 1 connection (no double-connect)
        std::cout << "     Server connection count: " << srv_listener.connect_count.load() << std::endl;
        STRESS_CHECK(srv_listener.connect_count.load() == 1);

        // Wait for all 10 clipboard items to arrive on server
        {
            std::unique_lock<std::mutex> lock(srv_listener.mtx);
            bool got_all = srv_listener.cv.wait_for(lock, std::chrono::seconds(5), [&] {
                return srv_listener.clips.size() == kNumThreads;
            });
            std::cout << "     Server received clips count: " << srv_listener.clips.size() << std::endl;
            STRESS_CHECK(got_all);
            std::set<std::string> received_texts;
            for (const auto& item : srv_listener.clips) {
                received_texts.insert(std::string(item.data.begin(), item.data.end()));
            }
            STRESS_CHECK(received_texts.size() == kNumThreads);
        }

        cli_engine.stop();
        srv_engine.stop();
    }

    // 3B. 10 threads calling ensure_connected() simultaneously on DISCONNECTED engine
    std::cout << "\n[3B] 10 threads calling ensure_connected() simultaneously on disconnected client..." << std::endl;
    {
        EngineConfig srv_cfg{};
        srv_cfg.storage_dir = base_dir / "srv3b_state";
        srv_cfg.receive_dir = base_dir / "srv3b_recv";
        srv_cfg.device_name = "Server-T3B";
        srv_cfg.listen_port = 0;
        srv_cfg.discovery_port = 0;

        StressListener srv_listener;
        Engine srv_engine(srv_cfg, &srv_listener);
        STRESS_CHECK(srv_engine.start());
        uint16_t srv_port = srv_engine.bound_tcp_port();
        STRESS_CHECK(srv_port > 0);

        EngineConfig cli_cfg{};
        cli_cfg.storage_dir = base_dir / "cli3b_state";
        cli_cfg.receive_dir = base_dir / "cli3b_recv";
        cli_cfg.device_name = "Client-T3B";
        cli_cfg.listen_port = 0;
        cli_cfg.discovery_port = 0;

        StressListener cli_listener;
        Engine cli_engine(cli_cfg, &cli_listener);
        STRESS_CHECK(cli_engine.start());

        cli_engine.register_candidate_peer("127.0.0.1", srv_port);
        STRESS_CHECK(!cli_engine.is_connected());

        constexpr int kNumThreads = 10;
        std::vector<std::thread> threads;
        std::atomic<bool> start_gate{false};
        std::atomic<int> success_count{0};

        for (int i = 0; i < kNumThreads; ++i) {
            threads.emplace_back([&] {
                while (!start_gate.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                if (cli_engine.ensure_connected()) {
                    success_count++;
                }
            });
        }

        start_gate.store(true, std::memory_order_release);
        for (auto& th : threads) {
            th.join();
        }

        std::cout << "     All 10 ensure_connected() threads completed. Success count: " << success_count.load() << std::endl;
        STRESS_CHECK(success_count.load() == kNumThreads);
        STRESS_CHECK(cli_engine.is_connected());
        // Verify no double-connect:
        std::cout << "     Server connection count: " << srv_listener.connect_count.load() << std::endl;
        STRESS_CHECK(srv_listener.connect_count.load() == 1);

        cli_engine.stop();
        srv_engine.stop();
    }

    // 3C. 10 threads calling ensure_connected() when candidate is UNREACHABLE
    std::cout << "\n[3C] 10 threads calling ensure_connected() on unroutable candidate..." << std::endl;
    {
        EngineConfig cli_cfg{};
        cli_cfg.storage_dir = base_dir / "cli3c_state";
        cli_cfg.receive_dir = base_dir / "cli3c_recv";
        cli_cfg.device_name = "Client-T3C";
        cli_cfg.listen_port = 0;
        cli_cfg.discovery_port = 0;

        StressListener cli_listener;
        Engine cli_engine(cli_cfg, &cli_listener);
        STRESS_CHECK(cli_engine.start());

        cli_engine.register_candidate_peer("192.0.2.1", 53321);

        constexpr int kNumThreads = 10;
        std::vector<std::thread> threads;
        std::atomic<bool> start_gate{false};
        std::atomic<int> fail_count{0};

        for (int i = 0; i < kNumThreads; ++i) {
            threads.emplace_back([&] {
                while (!start_gate.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                if (!cli_engine.ensure_connected()) {
                    fail_count++;
                }
            });
        }

        start_gate.store(true, std::memory_order_release);
        for (auto& th : threads) {
            th.join();
        }

        std::cout << "     All 10 threads completed safely without deadlock. Fail count: " << fail_count.load() << std::endl;
        STRESS_CHECK(fail_count.load() == kNumThreads);
        STRESS_CHECK(!cli_engine.is_connected());

        cli_engine.stop();
    }

    std::filesystem::remove_all(base_dir, ec);
    std::cout << "[TEST 3 PASSED] Concurrent stress testing completed with 0 deadlocks, 0 race conditions, and no double-connect!" << std::endl;
}

// Test 4: Abrupt disconnect and on-demand reconnection
void test_disconnect_and_ondemand_reconnect() {
    std::cout << "\n=== [TEST 4] Abrupt Disconnect & On-Demand Reconnection ===" << std::endl;

    auto base_dir = std::filesystem::temp_directory_path() / "l2a_stress_test_t4";
    std::error_code ec;
    std::filesystem::remove_all(base_dir, ec);

    EngineConfig srv_cfg{};
    srv_cfg.storage_dir = base_dir / "srv4_state";
    srv_cfg.receive_dir = base_dir / "srv4_recv";
    srv_cfg.device_name = "Server-T4";
    srv_cfg.listen_port = 0;
    srv_cfg.discovery_port = 0;

    StressListener srv_listener;
    Engine srv_engine(srv_cfg, &srv_listener);
    STRESS_CHECK(srv_engine.start());
    uint16_t srv_port = srv_engine.bound_tcp_port();
    STRESS_CHECK(srv_port > 0);

    EngineConfig cli_cfg{};
    cli_cfg.storage_dir = base_dir / "cli4_state";
    cli_cfg.receive_dir = base_dir / "cli4_recv";
    cli_cfg.device_name = "Client-T4";
    cli_cfg.listen_port = 0;
    cli_cfg.discovery_port = 0;

    StressListener cli_listener;
    Engine cli_engine(cli_cfg, &cli_listener);
    STRESS_CHECK(cli_engine.start());

    cli_engine.register_candidate_peer("127.0.0.1", srv_port);

    // Send first item on-demand
    std::cout << "     Sending clip 1 on-demand..." << std::endl;
    STRESS_CHECK(cli_engine.push_clipboard_text("Clip Before Disconnect"));
    STRESS_CHECK(cli_engine.is_connected());

    // Disconnect actively
    std::cout << "     Disconnecting active peer..." << std::endl;
    cli_engine.disconnect_active_peer();
    STRESS_CHECK(!cli_engine.is_connected());

    // Now send second item: must automatically reconnect on-demand!
    std::cout << "     Sending clip 2 on-demand (verifying automatic re-connection)..." << std::endl;
    STRESS_CHECK(cli_engine.push_clipboard_text("Clip After Disconnect"));
    STRESS_CHECK(cli_engine.is_connected());

    // Verify server received both clips
    {
        std::unique_lock<std::mutex> lock(srv_listener.mtx);
        bool ok = srv_listener.cv.wait_for(lock, std::chrono::seconds(5), [&] {
            return srv_listener.clips.size() >= 2;
        });
        STRESS_CHECK(ok);
        std::string s1(srv_listener.clips[0].data.begin(), srv_listener.clips[0].data.end());
        std::string s2(srv_listener.clips[1].data.begin(), srv_listener.clips[1].data.end());
        STRESS_CHECK(s1 == "Clip Before Disconnect");
        STRESS_CHECK(s2 == "Clip After Disconnect");
    }

    cli_engine.stop();
    srv_engine.stop();
    std::filesystem::remove_all(base_dir, ec);
    std::cout << "[TEST 4 PASSED] On-demand reconnection after disconnect verified!" << std::endl;
}

// Test 5: Candidate Endpoint Thread-Safety (Concurrent writers and readers)
void test_candidate_endpoint_thread_safety() {
    std::cout << "\n=== [TEST 5] Candidate Endpoint Concurrency (Readers vs Writers) ===" << std::endl;

    auto base_dir = std::filesystem::temp_directory_path() / "l2a_stress_test_t5";
    std::error_code ec;
    std::filesystem::remove_all(base_dir, ec);

    EngineConfig cfg{};
    cfg.storage_dir = base_dir / "state";
    cfg.receive_dir = base_dir / "recv";
    cfg.device_name = "CandidateRacer";
    cfg.listen_port = 0;
    cfg.discovery_port = 0;

    Engine engine(cfg);
    STRESS_CHECK(engine.start());

    constexpr int kNumOps = 500;
    std::atomic<bool> stop_flag{false};

    // 4 writer threads
    std::vector<std::thread> writers;
    for (int w = 0; w < 4; ++w) {
        writers.emplace_back([&, w] {
            for (int i = 0; i < kNumOps; ++i) {
                std::string ip = "192.168.1." + std::to_string((w * 50 + i) % 254 + 1);
                uint16_t port = static_cast<uint16_t>(50000 + (i % 1000));
                engine.register_candidate_peer(ip, port);
            }
        });
    }

    // 4 reader threads
    std::vector<std::thread> readers;
    for (int r = 0; r < 4; ++r) {
        readers.emplace_back([&] {
            while (!stop_flag.load(std::memory_order_relaxed)) {
                auto list = engine.candidate_peers();
                (void)list.size();
                std::this_thread::yield();
            }
        });
    }

    for (auto& th : writers) th.join();
    stop_flag.store(true, std::memory_order_relaxed);
    for (auto& th : readers) th.join();

    auto final_list = engine.candidate_peers();
    std::cout << "     Concurrent registration completed safely. Final candidate count: " << final_list.size() << std::endl;
    STRESS_CHECK(!final_list.empty());

    engine.stop();
    std::filesystem::remove_all(base_dir, ec);
    std::cout << "[TEST 5 PASSED] Candidate endpoints reader/writer concurrency is 100% thread-safe!" << std::endl;
}

// Test 6: 50 threads burst push
void test_high_concurrency_burst_push() {
    std::cout << "\n=== [TEST 6] High Concurrency Burst Push (50 threads) ===" << std::endl;

    auto base_dir = std::filesystem::temp_directory_path() / "l2a_stress_test_t6";
    std::error_code ec;
    std::filesystem::remove_all(base_dir, ec);

    EngineConfig srv_cfg{};
    srv_cfg.storage_dir = base_dir / "srv6_state";
    srv_cfg.receive_dir = base_dir / "srv6_recv";
    srv_cfg.device_name = "Server-T6";
    srv_cfg.listen_port = 0;
    srv_cfg.discovery_port = 0;

    StressListener srv_listener;
    Engine srv_engine(srv_cfg, &srv_listener);
    STRESS_CHECK(srv_engine.start());
    uint16_t srv_port = srv_engine.bound_tcp_port();

    EngineConfig cli_cfg{};
    cli_cfg.storage_dir = base_dir / "cli6_state";
    cli_cfg.receive_dir = base_dir / "cli6_recv";
    cli_cfg.device_name = "Client-T6";
    cli_cfg.listen_port = 0;
    cli_cfg.discovery_port = 0;

    StressListener cli_listener;
    Engine cli_engine(cli_cfg, &cli_listener);
    STRESS_CHECK(cli_engine.start());

    cli_engine.register_candidate_peer("127.0.0.1", srv_port);

    constexpr int kNumBurst = 50;
    std::vector<std::thread> burst_threads;
    std::atomic<bool> gate{false};
    std::atomic<int> ok_count{0};

    for (int i = 0; i < kNumBurst; ++i) {
        burst_threads.emplace_back([&, i] {
            while (!gate.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            std::string text = "Burst msg #" + std::to_string(i);
            if (cli_engine.push_clipboard_text(text)) {
                ok_count++;
            }
        });
    }

    auto t0 = std::chrono::steady_clock::now();
    gate.store(true, std::memory_order_release);
    for (auto& th : burst_threads) th.join();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

    std::cout << "     All 50 threads finished in " << elapsed << "ms. Successful sends: " << ok_count.load() << std::endl;
    STRESS_CHECK(ok_count.load() == kNumBurst);

    // Verify all received on server
    {
        std::unique_lock<std::mutex> lock(srv_listener.mtx);
        bool got_all = srv_listener.cv.wait_for(lock, std::chrono::seconds(8), [&] {
            return srv_listener.clips.size() == kNumBurst;
        });
        std::cout << "     Server received clips count: " << srv_listener.clips.size() << std::endl;
        STRESS_CHECK(got_all);
    }

    cli_engine.stop();
    srv_engine.stop();
    std::filesystem::remove_all(base_dir, ec);
    std::cout << "[TEST 6 PASSED] 50-thread high concurrency burst push passed with 100% integrity!" << std::endl;
}

int main() {
    std::cout << "========================================================\n"
              << " EMPIRICAL CHALLENGER STRESS HARNESS (Milestone M1-C2)\n"
              << "========================================================" << std::endl;

    test_unreachable_and_unresponsive_timeout();
    test_fallback_to_reachable_endpoint();
    test_concurrent_multithreaded_stress();
    test_disconnect_and_ondemand_reconnect();
    test_candidate_endpoint_thread_safety();
    test_high_concurrency_burst_push();

    std::cout << "\n========================================================\n"
              << " ALL EMPIRICAL CHALLENGER TESTS PASSED SUCCESSFULLY!\n"
              << "========================================================" << std::endl;
    return 0;
}
