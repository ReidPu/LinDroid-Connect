# Handoff Report: Milestone M1-C2 (Core & Linux Desktop Remediation: REAL-01 to REAL-04)

- **Agent**: `worker_m1_c2`
- **Role**: Implementer / QA / Specialist (C++ Core & Linux Desktop)
- **Target Audience**: Orchestrator (`af5814b6-fe43-4f2b-b498-cb11fbccdb34`), Reviewer / Challenger, Auditor
- **Date**: 2026-10-23
- **Status**: Completed

---

## 1. Observation

### 1.1 Before Modification
1. **Core Channel On-Demand Connection & Peer Candidates (REAL-01)**:
   - In `core/src/engine.cpp:375-385`, `Engine::send_frame_active()` immediately returned `false` if `active_channel_` was null:
     ```cpp
     if (!ch) {
         return false;
     }
     ```
   - In `core/include/l2a/engine.hpp` and `core/src/engine.cpp:85-96`, UDP peer discovery via `discovery_mgr_->start(...)` reported discovered peers to `listener_->on_peer_discovered(peer)` but did not retain or store candidate endpoints in `Engine`.
   - In `core/src/engine.cpp:213-242`, `Engine::connect_to_peer()` used a standard blocking `::connect()` call with no timeout, which on unreachable IP addresses would stall worker threads for standard TCP SYN timeout (~75 seconds).

2. **Linux Clipboard Push Gate (REAL-02)**:
   - In `linux/src/wayland_clipboard.cpp:263-272` (`on_gtk_text_received`) and lines `461-470` (`wayland_watcher_loop`), clipboard synchronization was gated behind:
     ```cpp
     if (self->engine_.is_connected()) { ... }
     ```
     When the application was started without a prior file transfer, `is_connected()` was false, causing clipboard events to be silently discarded.

3. **Linux Wayland Remote Clipboard Application (REAL-03)**:
   - In `linux/src/wayland_clipboard.cpp:297-306`, `apply_remote_clipboard` had:
     ```cpp
     if (gtk_clip_) {
         gtk_clipboard_set_text(gtk_clip_, text.c_str(), static_cast<gint>(text.size()));
     } else if (has_wl_clipboard_tools()) {
         write_to_wl_copy("text/plain", item.data.data(), item.data.size());
     }
     ```
     Because `gtk_clip_` was initialized during GTK monitoring setup, `has_wl_clipboard_tools()` was never called for text clipboard items. Under Wayland (GNOME Mutter), `gtk_clipboard_set_text` cannot set global compositor selection when the window is unfocused, causing `Ctrl+V` in external applications to fail.
   - Text items larger than 512KB were stored in `item.temp_file_path` with `item.data` empty, which caused `!item.data.empty()` to evaluate to false and bypassed text clipboard handling entirely.

4. **GNOME Desktop Notification Grouping (REAL-04)**:
   - In `linux/src/main.cpp`, there was no logic checking or creating `~/.local/share/applications/l2a-desktop.desktop`.
   - In `linux/src/app_window.cpp:428-435`, `notify-send` was invoked with only `-a "Linux ⇄ Android"`. It lacked `-i network-transmit-receive` and `-h string:desktop-entry:l2a-desktop`.
   - In `linux/src/app_window.cpp:475-479`, `flush_ui_events` applied remote clipboard items but never triggered desktop notifications.

---

## 2. Logic Chain

1. **Resolution of REAL-01 (Core On-Demand Connection & Fallback)**:
   - Added `struct PeerEndpoint { std::string ip; uint16_t port{0}; };` to `namespace l2a` in `core/include/l2a/engine.hpp`.
   - Added `register_candidate_peer(ip, port)`, `ensure_connected()`, `candidate_peers()`, with thread-safe storage `candidate_endpoints_` protected by `candidates_mutex_` and `connect_attempt_mutex_`.
   - In `Engine::start()`, registered default fallback endpoint `register_candidate_peer("127.0.0.1", 53322);` (the Linux ADB port forwarding) and registered each UDP-discovered peer (`peer.ip_address`, `peer.tcp_port`) in the discovery callback.
   - In `Engine::connect_to_peer()`, implemented non-blocking socket connect using `fcntl(fd, F_SETFL, flags | O_NONBLOCK)` followed by `poll(..., 1000)` with 1-second timeout and `getsockopt(SOL_SOCKET, SO_ERROR)`. Restored flags after connection.
   - In `Engine::send_frame_active()`, when `active_channel_` is null, it invokes `ensure_connected()`, which iterates registered candidate peers. If connection succeeds, `active_channel_` is acquired and the frame is delivered.

2. **Resolution of REAL-02 (Linux Clipboard Send Unblock)**:
   - Removed `if (self->engine_.is_connected())` gate in `on_gtk_text_received` and `wayland_watcher_loop`.
   - Offloaded `engine_.push_clipboard_text()` to detached background threads (`std::thread(...).detach()`), preventing any on-demand connection attempt from blocking the GTK UI loop or the Wayland watcher thread.

3. **Resolution of REAL-03 (Linux Wayland Clipboard Receive & Apply)**:
   - In `linux/src/wayland_clipboard.cpp:apply_remote_clipboard`, removed the mutually exclusive `else if`. Under Wayland, if `has_wl_clipboard_tools()` is true, `write_to_wl_copy("text/plain", ...)` is always executed to set the compositor selection without requiring window focus, while `gtk_clipboard_set_text()` is also called so GTK widgets receive the text.
   - Added support for reading text from `item.temp_file_path` when `item.data` is empty (for text items > 512KB), computing `last_applied_hash_`, and writing to both `wl-copy` and GTK.

4. **Resolution of REAL-04 (GNOME Desktop Notification Grouping)**:
   - In `linux/src/main.cpp`, implemented `ensure_desktop_entry_installed(home_dir)` on startup. It ensures `~/.local/share/applications` exists and creates/updates `l2a-desktop.desktop` containing `Name=Linux ⇄ Android 跨端互传`, `Exec=<current_exe_path>`, `Icon=network-transmit-receive`, `Categories=Utility;Network;FileTransfer;`, `StartupNotify=false`.
   - In `linux/src/app_window.cpp:send_desktop_notification`, added `-i network-transmit-receive` and `-h string:desktop-entry:l2a-desktop` to `notify-send` args. Verified that no `-e` (transient) argument is passed.
   - In `linux/src/app_window.cpp:flush_ui_events`, added notification dispatch `send_desktop_notification("已同步手机剪贴板", preview)` whenever a remote clipboard text item is received.

---

## 3. Caveats

1. In sandbox environments where `/home/<user>` is mounted read-only, `ensure_desktop_entry_installed` handles errors gracefully via `std::error_code ec` without crashing or throwing exceptions. On a standard user system, it successfully writes `~/.local/share/applications/l2a-desktop.desktop`.
2. `wl-copy` execution requires the system package `wl-clipboard` to be present on the host; when not present, `has_wl_clipboard_tools()` returns false and fallback to `gtk_clipboard_set_text` occurs smoothly.
3. Tests for dual fallback and initial clipboard sync (REAL-12, REAL-13, REAL-14) are allocated to Milestone M3-C2. Existing 10 test suites pass with zero regressions.

---

## 4. Conclusion

Milestone M1-C2 requirements (REAL-01 to REAL-04) are fully and genuinely implemented in `core/include/l2a/engine.hpp`, `core/src/engine.cpp`, `linux/src/wayland_clipboard.cpp`, `linux/src/app_window.cpp`, and `linux/src/main.cpp`.
- All CMake targets compile with 0 warnings/errors.
- All 10 existing test suites pass (100%).
- Code complies with C++20 conventions, thread safety constraints, and project layout.

---

## 5. Verification Method

### 5.1 Verification Commands & Results

1. **Compilation**:
   ```bash
   cmake -B build -DL2A_BUILD_LINUX=ON && cmake --build build -j4
   ```
   *Result*: Built all targets with 100% success (`libl2a_core.a`, `l2a-desktop`, and all 10 test executables).

2. **Automated Test Suite**:
   ```bash
   ctest --test-dir build --output-on-failure
   ```
   *Result*:
   ```
   100% tests passed, 0 tests failed out of 10
   Total Test time (real) = 1.16 sec
   ```

3. **Desktop Entry Generation Verification**:
   ```bash
   HOME=/tmp/test_home ./build/linux/l2a-desktop --status
   cat /tmp/test_home/.local/share/applications/l2a-desktop.desktop
   ```
   *Result*: Output verified valid `.desktop` file pointing to `Exec=/home/reid-pu/Programming/Linux_to_Android/build/linux/l2a-desktop` with icon `network-transmit-receive`.

### 5.2 Invalidation Conditions
- If `Engine::send_frame_active()` returns false without calling `ensure_connected()` when `active_channel_` is null.
- If `connect_to_peer()` blocks for more than 1000ms on an unreachable candidate IP.
- If `on_gtk_text_received` or `wayland_watcher_loop` drops clipboard events when `is_connected()` is false.
- If `apply_remote_clipboard` ignores `write_to_wl_copy` when `gtk_clip_` is non-null under Wayland.
- If `notify-send` lacks `-h string:desktop-entry:l2a-desktop`.
