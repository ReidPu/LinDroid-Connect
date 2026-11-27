# Linux Desktop Subsystem Remediation (Milestone 2) Handoff Report

## 1. Observation

All 15 defects (LINUX-01 through LINUX-15) identified in the Defect Diagnostic Report (`DEFECT_REVIEW_REPORT.md`) have been systematically audited and remediated in the `linux/` codebase:

1. **LINUX-01 (`linux/src/wayland_clipboard.cpp`)**:
   - *Previous state*: `std::string cmd = "wl-copy --type '" + item.mime_type + "'"; FILE* p = ::popen(cmd.c_str(), "w");` permitted remote arbitrary shell command injection via crafted MIME headers.
   - *Direct observation after fix*: Added `WaylandClipboardBridge::is_valid_mime_type(std::string_view mime)` validating against `^[a-zA-Z0-9_\-+./]+$` (must contain `/`). Replaced `popen` shell execution with direct `fork` + `execvp("wl-copy", args)` with standard input streamed directly from file descriptors, eliminating any shell interpretation.

2. **LINUX-02 (`linux/src/app_window.cpp`)**:
   - *Previous state*: `std::string cmd = "notify-send -a 'Linux ⇄ Android' '" + title + "' '" + body + "' >/dev/null 2>&1 &"; ::system(cmd.c_str());` allowed arbitrary code execution via file names with single quotes or shell metacharacters.
   - *Direct observation after fix*: Replaced `::system` with daemonized double-fork calling `execvp("notify-send", {"notify-send", "-a", "Linux ⇄ Android", title.c_str(), body.c_str(), nullptr})` without shell evaluation.

3. **LINUX-03 (`linux/src/main.cpp`)**:
   - *Previous state*: Process terminated immediately on `SIGPIPE` when broken pipes or disconnected sockets occurred.
   - *Direct observation after fix*: Added `::signal(SIGPIPE, SIG_IGN);` at the entry of `main()` before initializing GTK or network subsystems.

4. **LINUX-04 (`linux/src/wayland_clipboard.cpp`)**:
   - *Previous state*: Pure Wayland compositors (GNOME/KDE/wlroots) suppressed GTK `owner-change` signals when the window was minimized or hidden to the system tray.
   - *Direct observation after fix*: Added `WaylandClipboardBridge::is_wayland_session()` checking `WAYLAND_DISPLAY` and `XDG_SESSION_TYPE`. Implemented a background watcher thread spawning `wl-paste --type text/plain --watch` writing to a private FIFO pipe (`/tmp/l2a_clip_<pid>_<ts>.fifo`), continuously capturing clipboard events in the background while safely unlinking the FIFO upon shutdown.

5. **LINUX-05 (`linux/src/app_window.cpp`, `app_window.hpp`)**:
   - *Previous state*: Return value of `g_timeout_add(50, flush_ui_events, this)` was discarded, causing Use-After-Free if `AppWindow` was destroyed while the timeout remained active.
   - *Direct observation after fix*: Stored `timeout_source_id_` and called `g_source_remove(timeout_source_id_)` in `AppWindow::~AppWindow()`.

6. **LINUX-06 (`linux/src/app_window.cpp`)**:
   - *Previous state*: `AppWindow::on_drag_data_received` called `engine_.send_files(paths)` directly on the GTK main loop thread, synchronously scanning directories and computing SHA-256 for large files, freezing the UI.
   - *Direct observation after fix*: Immediately finished drag context on GTK thread, then offloaded directory scanning, SHA-256 computation, and batch dispatch to `std::thread`, dispatching UI logs back via `post_ui_log` (`g_idle_add`).

7. **LINUX-07 (`linux/src/app_window.cpp`)**:
   - *Previous state*: `run_adb_setup` and `on_btn_nfc_write` executed synchronous subprocesses, I2C writes, and socket `connect()` on the GTK main thread.
   - *Direct observation after fix*: Offloaded both ADB port forwarding / peer connection and I2C NFC operations to background `std::thread` workers, posting updates via `g_idle_add`.

8. **LINUX-08 (`linux/src/adb_helper.cpp`, `adb_helper.hpp`)**:
   - *Previous state*: `run_cmd_capture` relied on blocking `popen`/`fgets` without timeout, hanging indefinitely if the ADB daemon froze.
   - *Direct observation after fix*: Replaced `popen` with a non-blocking pipe runner with `poll()` supporting customizable timeouts (default 5000ms). When timed out, the child process is sent `SIGKILL`, reaped with `waitpid`, and `-1` is returned.

9. **LINUX-09 (`linux/src/adb_helper.cpp`)**:
   - *Previous state*: Returned raw `pclose` exit status, failing to parse actual process exit codes.
   - *Direct observation after fix*: Evaluated status with `WIFEXITED(status) ? WEXITSTATUS(status) : -1` (or `128 + WTERMSIG(status)` on termination by signal).

10. **LINUX-10 (`linux/src/adb_helper.cpp`)**:
    - *Previous state*: `rc_fwd == 0 || rc_rev == 0` allowed forward failures to be masked, and forwarded to local PC `transfer_port` rather than Android fixed service port 53321.
    - *Direct observation after fix*: Corrected to `adb forward tcp:53322 tcp:53321` and `adb reverse tcp:53323 tcp:<transfer_port>`. Required `rc_fwd == 0 && rc_rev == 0` for `res.success = true`.

11. **LINUX-11 (`linux/src/wayland_clipboard.cpp`)**:
    - *Previous state*: Applying remote clipboard content into GTK immediately fired local `owner-change`, re-broadcasting received text back to the remote peer.
    - *Direct observation after fix*: Introduced dual defense: `is_applying_remote_` atomic guard flag and SHA-256 hash tracking (`last_applied_hash_`, `last_sent_hash_`) to suppress echo loops.

12. **LINUX-12 (`linux/src/nfc_ntag5332.cpp`, `nfc_ntag5332.hpp`)**:
    - *Previous state*: Hardcoded `/dev/i2c-0` and aborted immediately on bus errors without retry.
    - *Direct observation after fix*: Implemented `NfcNtag5332Controller::resolve_i2c_dev_path()` parsing the sysfs device parent adapter (`/sys/bus/i2c/devices/i2c-NTAG5332:00 -> i2c-X`), falling back to `/dev/i2c-0`. Added 3-retry exponential backoff (2ms, 4ms, 8ms) on transient bus errors (`EBUSY`, `EAGAIN`, `ENXIO`, `ETIMEDOUT`).

13. **LINUX-13 (`linux/src/nfc_ntag5332.cpp`)**:
    - *Previous state*: `payload_len = static_cast<uint8_t>(1 + uri.size())` suffered 8-bit integer truncation overflow if URI exceeded 254 bytes, corrupting NDEF structures.
    - *Direct observation after fix*: Enforced `if (uri.empty() || uri.size() > 253) return {};` before casting to `uint8_t`.

14. **LINUX-14 (`linux/src/app_window.cpp`, `app_window.hpp`)**:
    - *Previous state*: GTK text buffer appended logs without limit, leading to unbounded memory usage in long sessions.
    - *Direct observation after fix*: Implemented sliding window pruning: when `gtk_text_buffer_get_line_count > 1000`, the oldest `(count - 1000)` lines are removed with `gtk_text_buffer_delete`.

15. **LINUX-15 (`linux/CMakeLists.txt`)**:
    - *Previous state*: Hardcoded `/usr/lib/x86_64-linux-gnu` breaking ARM64, Fedora, Arch, and Alpine.
    - *Direct observation after fix*: Replaced with `find_package(PkgConfig QUIET)` and multiarch `find_library(LIB_GTK3 ... PATHS ${SYSTEM_LIB_SEARCH_PATHS})`. Added `test_linux_subsystem` test executable and CTest registration.

---

## 2. Logic Chain

```
[Observation: Remote MIME & file names passed to popen/system]
  ↳ Remotely controlled strings contained unescaped quotes/semicolons
  ↳ Directly invoking /bin/sh allowed remote command execution
  ↳ By validating MIME against ^[a-zA-Z0-9_\-+./]+$ and replacing shell invocation with direct fork + execvp,
    arbitrary command execution is eliminated (LINUX-01, LINUX-02).

[Observation: Broken pipes trigger SIGPIPE]
  ↳ Subprocess pipes or peer sockets closing prematurely terminated the process
  ↳ ::signal(SIGPIPE, SIG_IGN) ensures the application gracefully handles EPIPE (LINUX-03).

[Observation: Wayland suppresses owner-change without window focus]
  ↳ Hiding window to system tray cuts off GTK clipboard event delivery
  ↳ Spawning wl-paste --watch piped to a private FIFO enables continuous background capture (LINUX-04).

[Observation: Discarded timeout ID dereferenced destroyed AppWindow]
  ↳ g_timeout_add continues firing after AppWindow destruction
  ↳ Storing timeout_source_id_ and calling g_source_remove eliminates the Use-After-Free (LINUX-05).

[Observation: Heavy I/O & hashing run synchronously on GTK main loop]
  ↳ GTK event loop freezes while file scanning, ADB commands, or I2C writes execute
  ↳ Delegating all blocking operations to worker threads and posting back via g_idle_add
    keeps the desktop UI permanently responsive (LINUX-06, LINUX-07).

[Observation: popen without timeout and raw pclose return]
  ↳ Hung subprocess freezes the calling thread permanently; exit codes were misinterpreted
  ↳ A poll-based non-blocking pipe with 5s timeout and WIFEXITED/WEXITSTATUS parsing
    guarantees timely aborts and accurate status evaluation (LINUX-08, LINUX-09).

[Observation: ADB port mismatch and OR logic in forwarding check]
  ↳ Forwarding pointed to PC port instead of Android 53321; reverse failure was masked
  ↳ Setting forward to 53321 and requiring both forward and reverse succeed restores USB connectivity (LINUX-10).

[Observation: Setting GTK clipboard re-fires owner-change]
  ↳ Remote clipboard updates triggered local reflection loops
  ↳ Combining is_applying_remote_ guard and SHA-256 hash comparison breaks echo loops (LINUX-11).

[Observation: Hardcoded i2c-0, transient bus NACKs, and 8-bit length cast]
  ↳ Touchpad NFC adapters vary across laptop models; EEPROM write cycles require retry; URI > 254 bytes overflowed
  ↳ Sysfs adapter parsing, exponential backoff retries, and strict URI length validation (< 254 bytes)
    resolve hardware communication and data corruption risks (LINUX-12, LINUX-13).

[Observation: GTK log buffer grew indefinitely]
  ↳ Long-running desktop client leaked memory via unbounded text buffer
  ↳ Pruning oldest lines above 1000 lines bounds memory consumption (LINUX-14).

[Observation: Hardcoded x86_64 Ubuntu library paths in CMake]
  ↳ Non-Ubuntu or ARM64 architectures failed to build
  ↳ find_package and multiarch find_library search paths provide distribution-neutral builds (LINUX-15).
```

---

## 3. Caveats

1. **Wayland Compositor Runtime**: Background clipboard watching with `wl-paste --watch` requires `wl-clipboard` to be installed on the host. If `wl-paste` is absent on a Wayland session, the bridge logs a helpful prompt and falls back safely without crashing or hanging.
2. **Physical Touchpad NFC Hardware**: In environments without a physical NTAG5332 ACPI/I2C touchpad chip, `probe_hardware()` accurately detects the absence and falls back to RF mode / LAN discovery. Unit tests validate the sysfs resolution and NDEF TLV encoding boundaries independently of physical hardware.
3. **ADB Executable**: If ADB is not installed or no Android device is connected via USB, `setup_android_permissions_and_usb_tunnel` returns cleanly with an explanatory status message and does not crash or block.

---

## 4. Conclusion

All 15 Linux Desktop Subsystem defects (LINUX-01 through LINUX-15) have been completely and genuinely remediated:
- Remote command injection vulnerabilities are eliminated.
- GTK main thread UI freezes are eradicated via background worker threads and `g_idle_add`.
- Subprocess execution is hardened with poll-based timeouts and standard POSIX exit status parsing.
- Use-After-Free and unbounded log memory growth are resolved.
- Pure Wayland background clipboard sync and anti-echo mechanisms are implemented.
- Dynamic I2C device resolution, retry backoff, and NDEF length boundary validation are functional.
- Cross-distribution CMake library resolution is clean and portable.

---

## 5. Verification Method

### 1. Build Verification
```bash
cmake -B build -DL2A_BUILD_LINUX=ON && cmake --build build -j4
```
**Result**: Clean compilation with **0 errors and 0 warnings** under `-Wall -Wextra -Wpedantic`.

### 2. Automated Test Suite Verification
```bash
ctest --test-dir build --output-on-failure -V
```
**Result**: **100% tests passed (3/3)**:
- `ProtocolAndCryptoTest`: Passed (0.02s)
- `LoopbackTransferAndResumeTest`: Passed (0.09s)
- `LinuxSubsystemTest`: Passed (0.30s)
  - `LINUX-01`: Safe MIME types accepted; all 16 malicious injection vectors rejected.
  - `LINUX-08 & LINUX-09`: Exit codes 0, 1, 42, 127 verified; 250ms timeout promptly aborted 10s command in 251ms.
  - `LINUX-12 & LINUX-13`: Dynamic I2C device resolution verified; NDEF length boundaries (253 vs 254 vs 500) validated.
  - `LINUX-11`: SHA-256 anti-echo hash defense verified.

### 3. CLI Status Verification
```bash
./build/linux/l2a-desktop --status
```
**Result**: Output verified clean device identity, pubkey fingerprint, NFC hardware status, and valid Type 5 NDEF TLV hex payload.

### Invalidation Conditions
- Any return to unescaped `popen` or `::system` in `wayland_clipboard.cpp` or `app_window.cpp`.
- Blocking operations re-introduced into GTK main event loop.
- Modifying files in `core/` or `android/` (which belong exclusively to other milestone workers).
