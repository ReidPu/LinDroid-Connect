# DISPATCH — worker_m1_c2

## Task Assignment
**Role**: C++ Core & Linux Desktop Implementation Worker
**Working Directory**: `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/worker_m1_c2`
**Project Plan**: `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/PROJECT.md`
**Source of Truth**: `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/ORIGINAL_REQUEST.md` (read section `## 2026-10-23T12:34:22Z`)
**Explorer Reports to Read**:
- `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/explorer_survey_r2_1/handoff.md`
- `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/explorer_survey_r2_3/handoff.md`

## File Ownership
You exclusively own and may modify:
- `core/include/l2a/engine.hpp`
- `core/src/engine.cpp`
- `linux/src/wayland_clipboard.cpp`
- `linux/src/app_window.cpp`
- `linux/src/main.cpp`

## Requirements to Implement (M1-C2: REAL-01 to REAL-04)
1. **REAL-01 (Core On-demand Connection & Fallback)**:
   - In `core/include/l2a/engine.hpp` and `core/src/engine.cpp`:
     - Add candidate endpoints list (`candidate_endpoints_`, protected by `std::mutex candidates_mutex_`).
     - Method `void register_candidate_peer(const std::string& ip, uint16_t port);`.
     - Method `bool ensure_connected();` that checks if `connected_.load()`, otherwise iterates candidate endpoints and connects via `connect_to_peer()`.
     - In `Engine::start()`, register default fallback candidate `register_candidate_peer("127.0.0.1", 53322);` (the ADB port forwarding on Linux).
     - In UDP discovery callback, register discovered peer `register_candidate_peer(peer.ip_address, peer.tcp_port);`.
     - In `Engine::send_frame_active()`: if `active_channel_` is null, call `ensure_connected()` to establish on-demand connection, re-acquire `active_channel_`, and proceed.
     - Non-blocking timeout in `connect_to_peer()`: use non-blocking socket connect + `poll()` with 1000ms timeout so candidate endpoints are checked quickly.
2. **REAL-02 (Linux Clipboard Send Unblock)**:
   - In `linux/src/wayland_clipboard.cpp`:
     - In `on_gtk_text_received` and `wayland_watcher_loop`, remove `if (self->engine_.is_connected())` gate! Offload `engine_.push_clipboard_text()` to a background thread (`std::thread(...).detach()`) so if on-demand connection is needed, it establishes without blocking the GTK UI.
3. **REAL-03 (Linux Wayland Clipboard Receive & Apply)**:
   - In `linux/src/wayland_clipboard.cpp:apply_remote_clipboard`:
     - Under Wayland, `gtk_clipboard_set_text` does NOT update the compositor global selection when the window is unfocused. Therefore, whenever `has_wl_clipboard_tools()` is true, ALWAYS execute `write_to_wl_copy("text/plain", ...)` regardless of `gtk_clip_`!
     - Also set `gtk_clipboard_set_text` so GTK widgets receive it.
     - Support large text (>512KB) from `item.temp_file_path`: read text, calculate `last_applied_hash_`, and write to `wl-copy` and GTK.
4. **REAL-04 (GNOME Desktop Notification Grouping)**:
   - In `linux/src/main.cpp`:
     - On startup, ensure `~/.local/share/applications/l2a-desktop.desktop` exists. Create `~/.local/share/applications` if missing, and write `l2a-desktop.desktop` with `Name=Linux ⇄ Android 跨端互传`, `Exec=<current_exe_path>`, `Icon=network-transmit-receive`, `Categories=Utility;Network;FileTransfer;`, `StartupNotify=false`.
   - In `linux/src/app_window.cpp:send_desktop_notification`:
     - Add `-h string:desktop-entry:l2a-desktop` and `-i network-transmit-receive` to `notify-send` args. Ensure NO `-e` (transient) argument is passed.
   - When clipboard text is received from Android in `flush_ui_events`, send desktop notification: `send_desktop_notification("已同步手机剪贴板", preview)`.

## Verification Commands
After implementing, verify build and existing tests:
```bash
cmake -B build -DL2A_BUILD_LINUX=ON
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

## Mandatory Integrity Warning
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A teamwork_preview_auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.

Produce `handoff.md` in your directory with verification results.


## 2026-10-23T12:55:22Z
You are worker_m1_c2.
Your working directory is: `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/worker_m1_c2`
You MUST read:
- `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/ORIGINAL_REQUEST.md` (section `## 2026-10-23T12:34:22Z`)
- `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/PROJECT.md`
- `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/worker_m1_c2/DISPATCH.md`
- Explorer reports:
  - `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/explorer_survey_r2_1/handoff.md`
  - `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/explorer_survey_r2_3/handoff.md`

Your Task:
Implement Milestone M1-C2 (Core & Linux Desktop Remediation: REAL-01 to REAL-04):
1. `core/include/l2a/engine.hpp` & `core/src/engine.cpp`: candidate endpoints registration, `ensure_connected()`, on-demand connection in `send_frame_active()`, non-blocking connect timeout.
2. `linux/src/wayland_clipboard.cpp`: remove `is_connected()` gate on clipboard copy, offload to background thread, always call `write_to_wl_copy` for text under Wayland + GTK clipboard setter, handle text > 512KB from temp file.
3. `linux/src/main.cpp`: auto-install `~/.local/share/applications/l2a-desktop.desktop` on startup.
4. `linux/src/app_window.cpp`: add `-h string:desktop-entry:l2a-desktop` and `-i network-transmit-receive` to `notify-send`, add notification on receiving clipboard text.
