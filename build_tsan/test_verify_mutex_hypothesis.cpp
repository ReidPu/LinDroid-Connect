#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <cassert>

// Test model of the un-mutexed vs mutexed pattern
struct UnlockedModel {
    std::atomic<bool> is_destroyed{false};
    std::atomic<uint64_t> seq{0};
    std::mutex mtx;
    std::condition_variable cv;
    std::thread th;

    void start() {
        uint64_t s = ++seq;
        cv.notify_all(); // UNLOCKED NOTIFY
        if (th.joinable()) th.join();
        th = std::thread([this, s]() {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait_for(lock, std::chrono::seconds(1), [this, s]() {
                return is_destroyed.load() || seq.load() != s;
            });
        });
    }

    void destroy() {
        is_destroyed.store(true);
        cv.notify_all(); // UNLOCKED NOTIFY
        if (th.joinable()) th.join();
    }
};

struct LockedModel {
    std::atomic<bool> is_destroyed{false};
    std::atomic<uint64_t> seq{0};
    std::mutex mtx;
    std::condition_variable cv;
    std::thread th;

    void start() {
        uint64_t s;
        {
            std::lock_guard<std::mutex> lock(mtx);
            s = ++seq;
            cv.notify_all(); // LOCKED NOTIFY
        }
        if (th.joinable()) th.join();
        th = std::thread([this, s]() {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait_for(lock, std::chrono::seconds(1), [this, s]() {
                return is_destroyed.load() || seq.load() != s;
            });
        });
    }

    void destroy() {
        {
            std::lock_guard<std::mutex> lock(mtx);
            is_destroyed.store(true);
            cv.notify_all(); // LOCKED NOTIFY
        }
        if (th.joinable()) th.join();
    }
};

int main() {
    std::cout << "Testing Unlocked Model (current implementation)...\n";
    int unlocked_slow = 0;
    for (int i = 0; i < 50; ++i) {
        UnlockedModel m;
        m.start();
        auto t0 = std::chrono::steady_clock::now();
        m.destroy();
        auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0).count();
        if (dt >= 100) {
            unlocked_slow++;
        }
    }
    std::cout << "Unlocked Model slow destructions: " << unlocked_slow << " / 50\n";

    std::cout << "\nTesting Locked Model (proposed fix)...\n";
    int locked_slow = 0;
    for (int i = 0; i < 50; ++i) {
        LockedModel m;
        m.start();
        auto t0 = std::chrono::steady_clock::now();
        m.destroy();
        auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0).count();
        if (dt >= 100) {
            locked_slow++;
        }
    }
    std::cout << "Locked Model slow destructions: " << locked_slow << " / 50\n";
    return 0;
}
