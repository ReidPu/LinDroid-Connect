## 2026-10-24T05:13:57Z
You are worker_m4_mutex_remediation.
Your working directory is: /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/worker_m4_mutex_remediation
The authoritative user request is in: /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/ORIGINAL_REQUEST.md under header `## 2026-10-24T03:09:08Z`.
Read ORIGINAL_REQUEST.md before starting work.
Also read /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/PROJECT.md and /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/challenger_c3_final_2/handoff.md.

MANDATORY INTEGRITY WARNING:
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A teamwork_preview_auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.

YOUR SPECIFIC REMEDIATION TASK:
Fix the lost wakeup on `selective_cv_` in `linux/src/wayland_clipboard.cpp` reported by `challenger_c3_final_2`:
Root Cause:
Calling `selective_cv_.notify_all()` without holding `selective_cv_mutex_` creates a window where the background thread evaluates `pred()` (false) and before enrolling in the condvar wait queue, `notify_all()` is fired to an empty queue. The background thread then waits for the full 3.0s timeout, causing `selective_restore_thread_.join()` to block for 3.5s and failing `duration_ms < 100` (10% reproduction rate).

THE FIX:
In `linux/src/wayland_clipboard.cpp`:
1. In `~WaylandClipboardBridge()` (lines 162–170):
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
2. In `perform_selective_paste()` (around line 544):
   ```cpp
   uint64_t seq;
   {
       std::lock_guard<std::mutex> lock(selective_cv_mutex_);
       seq = ++selective_seq_;
       selective_cv_.notify_all();
   }
   is_selective_pasting_.store(true);
   ```

VERIFICATION:
1. Recompile with ThreadSanitizer:
   `cmake -B build_tsan -DL2A_BUILD_LINUX=ON -DCMAKE_CXX_FLAGS="-fsanitize=thread -g"`
   `cmake --build build_tsan -j4`
2. Run a loop of 50 iterations of `./build_tsan/linux/test_linux_subsystem` and verify 0 failures (50/50 pass, no 3-second stalls).
3. Run `ctest --test-dir build_tsan --output-on-failure -V` (15/15 tests pass).
4. Run `cmake --build build -j4 && ctest --test-dir build --output-on-failure` (15/15 pass).
5. Run `cmake --build build_asan -j4 && ctest --test-dir build_asan --output-on-failure` (15/15 pass).

Write a complete report to:
`/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/worker_m4_mutex_remediation/handoff.md`.
