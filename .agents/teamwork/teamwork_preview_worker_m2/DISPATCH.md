## 2026-10-21T03:41:00Z
You are teamwork_preview_worker_m2.
Your working directory is: /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/teamwork_preview_worker_m2/
Project root is: /home/reid-pu/Programming/Linux_to_Android
Parent Orchestrator ID: 046339d7-ff3b-4ec5-91cc-957d813227d5

MANDATORY FIRST STEP: Read /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/ORIGINAL_REQUEST.md.
Also read:
- Master Scope: /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/PROJECT.md
- Defect Diagnostic Report: /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/DEFECT_REVIEW_REPORT.md
- Explorer 2 Linux Subsystem Report: /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/teamwork_preview_explorer_survey_2/handoff.md

MANDATORY INTEGRITY WARNING:
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A teamwork_preview_auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.

EXCLUSIVE FILE WRITE OWNERSHIP:
You own exclusively files under `linux/`:
- `linux/src/main.cpp`
- `linux/src/app_window.hpp`, `linux/src/app_window.cpp`
- `linux/src/wayland_clipboard.hpp`, `linux/src/wayland_clipboard.cpp`
- `linux/src/adb_helper.hpp`, `linux/src/adb_helper.cpp`
- `linux/src/nfc_ntag5332.hpp`, `linux/src/nfc_ntag5332.cpp`
- `linux/CMakeLists.txt`
Do NOT modify files in `core/` or `android/`.

YOUR TASK (Milestone 2: Linux Desktop Subsystem Remediation):
Implement genuine, robust, production-grade C++20 fixes for LINUX-01 through LINUX-15:
1. LINUX-01: Prevent Command Injection in `linux/src/wayland_clipboard.cpp`. Validate `item.mime_type` against `^[a-zA-Z0-9_\-+./]+$` and replace `popen` shell execution with direct `fork` + `execvp` (`{"wl-copy", "--type", mime_type.c_str(), nullptr}`) to eliminate arbitrary command execution via crafted remote MIME headers.
2. LINUX-02: Prevent Command Injection in `linux/src/app_window.cpp`. In `send_desktop_notification`, eliminate `::system("notify-send ...")` with shell concatenation. Use direct `fork` + `execvp` (`{"notify-send", "-a", "Linux ⇄ Android", title.c_str(), body.c_str(), nullptr}`) so arbitrary remote file names cannot execute shell commands.
3. LINUX-03: Signal safety in `linux/src/main.cpp`. Add `::signal(SIGPIPE, SIG_IGN)` at the start of `main()` before initializing GTK or sockets to prevent process termination on broken pipes or abruptly disconnected sockets.
4. LINUX-04: Support pure Wayland background clipboard monitoring in `linux/src/wayland_clipboard.cpp`. Detect Wayland environment (`WAYLAND_DISPLAY` or `XDG_SESSION_TYPE==wayland`). Implement support to monitor clipboard events via a background watcher thread (e.g., executing `wl-paste --watch` piped into the bridge or fallback monitoring) so clipboard capture continues when the window is hidden/minimized to system tray.
5. LINUX-05: Eliminate Use-After-Free in `linux/src/app_window.cpp`. Store the `guint` source ID returned by `g_timeout_add(50, flush_ui_events, this)`. In `AppWindow::~AppWindow()`, remove the source via `g_source_remove(timeout_source_id_)` if non-zero.
6. LINUX-06: Offload drag-and-drop file scanning & SHA-256 calculation from GTK main thread. In `AppWindow::on_drag_data_received`, spawn a background worker thread (`std::thread`) to scan dropped directories, gather file metadata, and initiate transfer, posting UI status updates back via `g_idle_add`.
7. LINUX-07: Offload synchronous ADB execution and connection from GTK main thread. In `AppWindow::run_adb_setup` and `on_btn_nfc_write`, run ADB subprocess commands and `engine_.connect_to_peer` on a background thread so the GTK event loop never freezes.
8. LINUX-08: Non-blocking subprocess execution with timeout in `AdbHelper::run_cmd_capture`. Replace indefinite `popen`/`fgets` with a robust pipe runner supporting a timeout (e.g. 5 seconds) using `poll()` or non-blocking I/O so hung ADB commands never hang the application.
9. LINUX-09: Correct exit code parsing in `linux/src/adb_helper.cpp`. Evaluate subprocess exit status using `WIFEXITED(st) ? WEXITSTATUS(st) : -1` rather than raw `pclose` return.
10. LINUX-10: Correct ADB port forwarding logic in `linux/src/adb_helper.cpp`. In `forward`, map local TCP port 53322 to Android target service port 53321 (`adb forward tcp:53322 tcp:53321`). In `reverse`, map Android port 53323 to PC transfer port (`adb reverse tcp:53323 tcp:<port>`). Require both `rc_fwd == 0 && rc_rev == 0` for `res.success = true`.
11. LINUX-11: Suppress local clipboard echo loops in `linux/src/wayland_clipboard.cpp`. Track the hash of locally applied remote clipboard text/data or set an `is_applying_remote_` guard flag to prevent GTK `owner-change` callbacks from re-transmitting received content back across the network.
12. LINUX-12: Dynamic I2C adapter resolution and bus retry backoff in `linux/src/nfc_ntag5332.cpp`. Parse sysfs parent adapter number rather than hardcoding `/dev/i2c-0`, and add exponential backoff retry on transient bus errors (`EBUSY`, `EAGAIN`).
13. LINUX-13: Validate NDEF URI length in `linux/src/nfc_ntag5332.cpp`. Enforce URI length limits (< 255 bytes) and check for truncation before casting to `uint8_t` to prevent payload length byte integer overflow.
14. LINUX-14: Bound GTK log text buffer in `linux/src/app_window.cpp`. Limit text buffer to 1000 lines (prune oldest lines from top when line count exceeds 1000) to prevent unbounded memory growth during long-running sessions.
15. LINUX-15: Clean CMake library resolution in `linux/CMakeLists.txt`. Ensure dynamic loading or package resolution conforms cleanly across Linux distributions without invalid hardcoded paths.

VERIFICATION REQUIREMENTS:
Run:
`cmake -B build -DL2A_BUILD_LINUX=ON && cmake --build build -j4`
`ctest --test-dir build --output-on-failure -V`
Both MUST compile cleanly with 0 errors and 0 warnings.
Write empirical verification unit/stress tests to prove that command injection is eliminated, timeouts function properly, and exit codes are correctly parsed.

OUTPUT:
Deliver a comprehensive handoff report to:
`/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/teamwork_preview_worker_m2/handoff.md`
Detailing all files modified, diff summary, verification commands, and test output.
When finished, send a completion message to the parent orchestrator via send_message.
