## 2026-10-24T04:04:49Z
You are worker_m1_c3_remediation.
Your working directory is: /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/worker_m1_c3_remediation
The authoritative user request is in: /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/ORIGINAL_REQUEST.md under header `## 2026-10-24T03:09:08Z`.
Read ORIGINAL_REQUEST.md before starting work.
Also read /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/PROJECT.md.

Read the review and challenge reports for Milestone M1-C3:
1. /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/reviewer_m1_c3_1/handoff.md
2. /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/reviewer_m1_c3_2/handoff.md
3. /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/challenger_m1_c3_1/handoff.md
4. /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/challenger_m1_c3_2/handoff.md

MANDATORY INTEGRITY WARNING:
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A teamwork_preview_auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.

SPECIFIC REMEDIATION TASKS:
1. Manual Remote Pull Response when `phone_to_pc_auto_share` is false:
   - In `linux/src/wayland_clipboard.hpp` and `.cpp`: When user calls `pull_remote_clipboard()`, set `manual_pull_pending_ = true`. In `apply_remote_clipboard()`, if `manual_pull_pending_.exchange(false)` is true, proceed to write to system clipboard even if `phone_to_pc_auto_share` is disabled!
2. Non-blocking Selective Paste in GTK UI:
   - In `linux/src/wayland_clipboard.cpp:perform_selective_paste`: Remove the blocking `selective_restore_thread_.join()` from the GTK main UI thread. Use a generation/sequence counter with a detached thread or GLib `g_timeout_add` to restore Item #0 after 3 seconds without blocking or freezing the GTK UI.
3. Raw In-Memory Pixel Image Disk Caching for Selective Paste:
   - In `linux/src/wayland_clipboard.cpp`: When capturing raw image pixels (Priority 1), save the PNG bytes to `~/.config/linux_to_android/clip_cache/img_<sha256>.png` and populate `item.image_path`, ensuring that selective paste can successfully re-apply the image to system clipboard and `wl-copy`.
4. Fix ThreadSanitizer Data Race on `ClipboardHistoryManager` and `SettingsManager`:
   - In `core/include/l2a/history_manager.hpp` and `core/src/history_manager.cpp`: Separate `to_json_locked()` and `from_json_locked()` from public `to_json()` / `from_json()`, ensuring public methods lock `mutex_`.
   - In `linux/src/settings_manager.cpp`: Ensure `save()` holds `mutex_` while reading settings.
5. In `core/src/clipboard_stream.cpp`:
   - Fix redundant double-send when handling `kClipPullReq` with `clip_id == 0`.

VERIFICATION:
- Rebuild via `cmake -B build -DL2A_BUILD_LINUX=ON && cmake --build build -j4`
- Run `ctest --test-dir build --output-on-failure -V`
- Run `cmake -B build_android_test -S android/app/src/main/cpp && cmake --build build_android_test && ./build_android_test/test_android_jni`
- Document all changes and test outputs in `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/worker_m1_c3_remediation/handoff.md`.
