# Remediation Handoff Report: Elimination of Lost Wakeup on `selective_cv_` in `WaylandClipboardBridge`

- **Agent**: `worker_m4_mutex_remediation`
- **Roles**: implementer, qa, specialist
- **Milestone**: M4-C3 Mutex Remediation
- **Date**: 2026-10-24
- **Parent Conversation ID**: `8eeb0c45-a89c-4e54-a47f-ce415553534a`

---

## 1. Observation

### 1.1 Baseline Defect Reproduction
Prior to applying the remediation, 20 iterations of `./build_tsan/linux/test_linux_subsystem` were executed on the unpatched codebase. Two runs failed with `rc=-6` (SIGABRT due to assertion failure `duration_ms < 100` at line 493 of `test_linux_subsystem.cpp`) and lasted ~3.5 seconds:
```
Run 0: PASSED time=0.558s
...
Run 11: PASSED time=0.558s
Run 12: FAILED rc=-6 time=3.567s
Run 13: PASSED time=0.541s
...
Run 17: PASSED time=0.558s
Run 18: FAILED rc=-6 time=3.495s
Run 19: PASSED time=0.521s
Total failures: 2/20 (10.0% failure rate)
```

### 1.2 Unsynchronized Code in `linux/src/wayland_clipboard.cpp`
In the unpatched code:
1. In `~WaylandClipboardBridge()` (lines 162–170):
   ```cpp
   WaylandClipboardBridge::~WaylandClipboardBridge() {
       is_destroyed_.store(true);
       selective_cv_.notify_all();
       if (selective_restore_thread_.joinable()) {
           selective_restore_thread_.join();
       }
       stop_x11_background_monitor();
       stop_wayland_watcher();
   }
   ```
2. In `perform_selective_paste()` (lines 544–547):
   ```cpp
   uint64_t seq = ++selective_seq_;
   selective_cv_.notify_all();
   is_selective_pasting_.store(true);
   ```
Both calls to `selective_cv_.notify_all()` and predicate modifications (`is_destroyed_`, `selective_seq_`) occurred without acquiring `selective_cv_mutex_`.

### 1.3 Implemented Code Modifications
In `linux/src/wayland_clipboard.cpp`:
1. In `WaylandClipboardBridge::~WaylandClipboardBridge()` (lines 162–173):
   ```cpp
   WaylandClipboardBridge::~WaylandClipboardBridge() {
       {
           std::lock_guard<std::mutex> lock(selective_cv_mutex_);
           is_destroyed_.store(true);
           selective_cv_.notify_all();
       }
       if (selective_restore_thread_.joinable()) {
           selective_restore_thread_.join();
       }
       stop_x11_background_monitor();
       stop_wayland_watcher();
   }
   ```
2. In `WaylandClipboardBridge::perform_selective_paste()` (lines 544–555):
   ```cpp
   void WaylandClipboardBridge::perform_selective_paste(
       const ClipboardHistoryItem& item_to_paste,
       const std::optional<ClipboardHistoryItem>& item_0) {
       uint64_t seq;
       {
           std::lock_guard<std::mutex> lock(selective_cv_mutex_);
           seq = ++selective_seq_;
           selective_cv_.notify_all();
       }

       is_selective_pasting_.store(true);
       ...
   ```

### 1.4 Post-Remediation Test Results
1. **ThreadSanitizer 50-Iteration Stress Loop 1**:
   ```
   Run 0: PASSED time=0.552s
   Run 10: PASSED time=0.585s
   Run 20: PASSED time=0.550s
   Run 30: PASSED time=0.559s
   Run 40: PASSED time=0.539s

   Final Summary: 50/50 passed. Failures = 0
   Average duration: 0.556s, Max duration: 0.597s
   ```
2. **ThreadSanitizer 50-Iteration Stress Loop 2**:
   ```
   50 iterations test: 50/50 passed. Failures = 0
   Max time: 0.601s
   ```
3. **Adversarial Stress Test Harness (`build_tsan/test_adversarial_tsan_bridge`)**:
   ```
   [TEST 1] Testing Consecutive Selective Paste Calls for UI Freeze / Lost Wakeup...
     -> Result: 0 / 30 calls suffered UI thread freeze (> 100ms)!
     -> Passed smoothly in this run.

   [TEST 2] Testing Rapid Bridge Destruction vs Active Selective Paste Thread...
     -> Destroyed 30 bridges. Slow destructions: 0

   [TEST 3] Testing Natural 3-Second Expiry & Restoration State...
     -> PASSED: Natural 3-second expiration cleared is_selective_pasting_ flag.
   ```
4. **Full CTest Suites**:
   - `ctest --test-dir build_tsan --output-on-failure -V`: 15/15 passed (8.50s real time, 0 data races).
   - `cmake --build build -j4 && ctest --test-dir build --output-on-failure`: 15/15 passed (2.10s real time).
   - `cmake --build build_asan -j4 && ctest --test-dir build_asan --output-on-failure`: 15/15 passed (5.16s real time).
5. **Android APK Compilation**:
   - `cd android && ./gradlew assembleDebug`: `BUILD SUCCESSFUL in 6s` (41 actionable tasks, `app-debug.apk` up-to-date).

---

## 2. Logic Chain

1. **Root Cause Analysis (Observation 1.1 & 1.2)**:
   - In `perform_selective_paste()`, the background restoration thread runs:
     ```cpp
     std::unique_lock<std::mutex> lock(selective_cv_mutex_);
     selective_cv_.wait_for(lock, std::chrono::seconds(3), [this, seq]() {
         return is_destroyed_.load() || selective_seq_.load() != seq;
     });
     ```
   - When calling `perform_selective_paste()` again or destroying the bridge without acquiring `selective_cv_mutex_`, a thread interleaving window exists:
     - The background thread holds `selective_cv_mutex_`, checks the predicate (evaluates to `false`), but has not yet enrolled in the underlying kernel condition variable wait queue.
     - The caller thread updates `selective_seq_` or `is_destroyed_` and issues `selective_cv_.notify_all()` to an empty wait queue.
     - The background thread then transitions into `pthread_cond_clockwait` and blocks for the full 3.00 seconds.
     - The caller thread subsequently invokes `selective_restore_thread_.join()` and hangs for 3.0 seconds, failing `TEST_ASSERT(duration_ms < 100)`.

2. **Remediation Mechanism (Observation 1.3)**:
   - By locking `selective_cv_mutex_` around the predicate update (`is_destroyed_ = true` or `seq = ++selective_seq_`) and `selective_cv_.notify_all()`:
     - **Case A**: The background thread acquires `selective_cv_mutex_` first. It enters `wait_for` and atomically releases `selective_cv_mutex_` while enrolling in the condvar wait queue. The caller acquires `selective_cv_mutex_`, updates the state, and calls `notify_all()`. The background thread is guaranteed to be in the wait queue and is woken immediately.
     - **Case B**: The caller thread acquires `selective_cv_mutex_` first. It updates the state and releases the lock. When the background thread subsequently acquires `selective_cv_mutex_` and evaluates `pred()`, `is_destroyed_` is already `true` or `selective_seq_ != seq`. The predicate evaluates to `true` and the thread never enters the wait state at all.
   - In both cases, lost wakeups and 3-second hangs are mathematically eliminated.

3. **Empirical Confirmation (Observation 1.4)**:
   - 100 consecutive executions of `test_linux_subsystem` across two stress batches achieved a 100% pass rate with zero hangs (maximum test time 0.601s vs 3.567s prior to fix).
   - The adversarial harness specifically targeting UI thread freeze and rapid destructor teardown recorded 0/30 blocked calls and 0/30 slow destructions.
   - All 15 CTest tests pass across standard, ThreadSanitizer, and AddressSanitizer builds.

---

## 3. Caveats

No caveats. The defect mechanism was verified mathematically, reproduced experimentally on unpatched code (10.0% failure rate), eliminated with the fix (0.0% failure rate across 100 runs), and confirmed across all sanitizer suites.

---

## 4. Conclusion

The lost wakeup on `selective_cv_` in `WaylandClipboardBridge` is completely resolved. Mutex synchronization guarantees safe predicate evaluation and notification ordering between the UI/caller thread and the background selective paste restoration thread. The fix is minimal, robust, regression-free, and fully verified.

---

## 5. Verification Method

To independently verify this remediation:

```bash
# 1. Build under ThreadSanitizer
cmake -B build_tsan -DL2A_BUILD_LINUX=ON -DCMAKE_CXX_FLAGS="-fsanitize=thread -g"
cmake --build build_tsan -j4

# 2. Run 50 iterations of test_linux_subsystem
python3 -c '
import subprocess, time
failures = 0
for i in range(50):
    res = subprocess.run(["./build_tsan/linux/test_linux_subsystem"], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if res.returncode != 0:
        failures += 1
print(f"Result: {failures}/50 failures")
assert failures == 0
'

# 3. Run full CTest suites
ctest --test-dir build_tsan --output-on-failure -V
cmake --build build -j4 && ctest --test-dir build --output-on-failure
cmake --build build_asan -j4 && ctest --test-dir build_asan --output-on-failure
```
