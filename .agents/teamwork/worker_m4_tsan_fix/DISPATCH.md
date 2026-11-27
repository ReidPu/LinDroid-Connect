## 2026-10-24T04:58:13Z
You are worker_m4_tsan_fix.
Your working directory is: /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/worker_m4_tsan_fix
The authoritative user request is in: /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/ORIGINAL_REQUEST.md under header `## 2026-10-24T03:09:08Z`.
Read ORIGINAL_REQUEST.md before starting work.
Also read /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/PROJECT.md and /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/challenger_c3_final/handoff.md.

MANDATORY INTEGRITY WARNING:
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A teamwork_preview_auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.

YOUR SPECIFIC REMEDIATION TASK:
Resolve the single ThreadSanitizer data race found by challenger_c3_final in `WaylandClipboardBridge`:
Root Cause:
In `linux/src/wayland_clipboard.cpp:593`, `perform_selective_paste()` starts a detached thread:
`std::thread([this, item_0, seq]() { ... }).detach();`
When `WaylandClipboardBridge` is destroyed on the main thread, `~WaylandClipboardBridge()` calls `selective_cv_.notify_all()` and exits, leading to `selective_cv_.~condition_variable()` (`pthread_cond_destroy`) racing with the waking detached thread in `pthread_cond_clockwait`.

THE FIX:
1. In `linux/src/wayland_clipboard.hpp`:
   Add a member variable:
   `std::thread selective_restore_thread_;`
2. In `linux/src/wayland_clipboard.cpp`:
   - In `perform_selective_paste()` (around line 593):
     Replace `.detach()` by joining any prior thread and assigning to `selective_restore_thread_`:
     ```cpp
     if (selective_restore_thread_.joinable()) {
         selective_restore_thread_.join();
     }
     selective_restore_thread_ = std::thread([this, item_0, seq]() {
         // thread body ...
     });
     ```
   - In `~WaylandClipboardBridge()` (around line 162):
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

VERIFICATION:
1. Recompile with ThreadSanitizer:
   `cmake -B build_tsan -DL2A_BUILD_LINUX=ON -DCMAKE_CXX_FLAGS="-fsanitize=thread -g"`
   `cmake --build build_tsan -j4`
2. Run CTest under ThreadSanitizer:
   `ctest --test-dir build_tsan --output-on-failure -V`
   Verify that all 15/15 tests pass with 0 data races.
3. Rebuild standard build and test:
   `cmake --build build -j4`
   `ctest --test-dir build --output-on-failure`
   Verify 15/15 tests pass.

Write a complete handoff report to:
`/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/worker_m4_tsan_fix/handoff.md`.
