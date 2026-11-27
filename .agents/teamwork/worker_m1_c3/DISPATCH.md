## 2026-10-24T03:25:02Z
You are worker_m1_c3.
Your working directory is: /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/worker_m1_c3
The authoritative user request is in: /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/ORIGINAL_REQUEST.md under header `## 2026-10-24T03:09:08Z`.
Read ORIGINAL_REQUEST.md before starting work.
Also read /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/PROJECT.md.

Read the detailed survey reports prepared by our survey explorers:
1. /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/explorer_survey_c3_1/handoff.md (Settings & Wayland/XFixes background sync, pull remote clipboard)
2. /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/explorer_survey_c3_2/handoff.md (3-tier image priority, Nautilus URI resolution, Linux image clipboard)
3. /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/explorer_survey_c3_3/handoff.md (ClipboardHistoryManager, non-destructive selective paste)

FILE WRITE OWNERSHIP:
You exclusively own and will implement code in:
- `core/include/l2a/history_manager.hpp` (NEW)
- `core/src/history_manager.cpp` (NEW)
- `core/include/l2a/engine.hpp`
- `core/src/engine.cpp`
- `core/src/clipboard_stream.cpp`
- `core/CMakeLists.txt`
- `linux/src/settings_manager.hpp` (NEW)
- `linux/src/settings_manager.cpp` (NEW)
- `linux/src/wayland_clipboard.hpp`
- `linux/src/wayland_clipboard.cpp`
- `linux/src/app_window.hpp`
- `linux/src/app_window.cpp`
- `linux/src/tray_indicator.cpp`
- `linux/CMakeLists.txt`
- `android/app/src/main/cpp/jni_bridge.cpp` (add JNI hooks for nativePushClipboardBytes, nativePullRemoteClipboard, history manager)

MANDATORY INTEGRITY WARNING:
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A teamwork_preview_auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.

SPECIFIC IMPLEMENTATION REQUIREMENTS:
1. C++ Core History Manager (`history_manager.hpp` and `.cpp`):
   - Capacity limit N in [5, 200], default 20. Evicts oldest non-pinned items when exceeded.
   - Dynamic limit adjustment via `set_max_history_limit(size_t)`.
   - Records text preview, image info, origin (Local/Remote), timestamp, sha256.
   - Thread-safe access. Supports disk persistence (`~/.config/linux_to_android/history.json`).
   - Selective paste token tracking: mark an item for non-destructive selective paste.
2. Protocol & Engine:
   - In `clipboard_stream.cpp`: handle `clip_id == 0` in `kClipPullReq` by finding latest item and sending `CLIP_OFFER` back to remote.
   - In `engine.cpp`: add `pull_remote_clipboard()` sending `clip_id == 0`.
   - In `jni_bridge.cpp`: implement `nativePushClipboardBytes(mimeType, data)`, `nativePullRemoteClipboard()`, `nativeSetMaxHistoryLimit()`.
3. Linux Settings Manager (`settings_manager.hpp` and `.cpp`):
   - Path: `~/.config/linux_to_android/settings.json`.
   - Fields: `pc_to_phone_auto_share` (bool, default true), `phone_to_pc_auto_share` (bool, default true), `history_limit` (size_t, default 20).
4. Wayland & XWayland Background Clipboard Bridge (`wayland_clipboard.hpp` and `.cpp`):
   - Implement XWayland / X11 XFixes background capture (`XFixesSelectSelectionInput` on `:0` root window) so clipboard is captured even when `l2a-desktop` window is unfocused or minimized!
   - 3-tier capture priority:
     - Priority 1: Raw image pixel data -> `image/png` binary bytes.
     - Priority 2: File manager copied image resolution. Detect `file:///` or local path in `text/uri-list` / `text/plain` for `.png`, `.jpg`, `.jpeg`, `.webp`, `.gif`, `.bmp`. Read image binary bytes directly and push as `image/*` binary, NOT string path!
     - Priority 3: Plain text.
   - Check `pc_to_phone_auto_share` before pushing to phone.
   - Non-destructive selective paste: when user pastes item #k, temporarily set clipboard to item #k with guard flag `is_selective_pasting_` (to prevent loopback push to phone and preserving history #0 item), and schedule automatic restoration of original #0 item after 3 seconds.
5. Linux GTK UI (`app_window.hpp`, `app_window.cpp`, `tray_indicator.cpp`):
   - Add Settings UI: directional auto-share switches, history limit (5-200).
   - Add manual action buttons: "立即发送当前剪贴板" and "拉取手机剪贴板".
   - Add Clipboard History Viewer UI: list of history items, previews, origin badge, timestamp, "选择性粘贴" button, "删除" button, "清空" button.
6. Verify Build & Tests:
   - Run `cmake -B build -DL2A_BUILD_LINUX=ON && cmake --build build -j4`
   - Run `ctest --test-dir build --output-on-failure`
   - Ensure all existing tests pass and build is 100% clean.
   - Write comprehensive report to `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/worker_m1_c3/handoff.md`.
