# Linux Desktop Subsystem & Core Integration Architectural Survey and Defect Report

## 1. Observation

### 1.1 Architecture & Key Files Overview
The Linux Desktop Subsystem resides under `linux/` and integrates directly with `core/` (linked as `l2a_core`). It provides a lightweight native desktop UI, system tray integration, Wayland/X11 clipboard synchronization, ADB port forwarding/permission automation, and touchpad NFC (NTAG5332) I2C hardware communication.

- **`linux/src/gtk_abi.hpp`**: Minimal C-compatible ABI declarations for GTK3, GDK3, GLib, GObject, and Ayatana-AppIndicator3, allowing dynamic linking against system runtime libraries without requiring heavy GTK dev headers.
- **`linux/src/main.cpp`**: Desktop entry point. Manages CLI `--status` inspection, PATH configuration, GTK initialization (`gtk_init_check`), and main loop execution (`gtk_main`).
- **`linux/src/app_window.{hpp,cpp}`**: Implements `ICoreListener` and the main GTK window. Manages drag-and-drop URI targets, status indicators, file picker dialogs, transfer progress bars, text logs, desktop notifications via `notify-send`, and periodic UI event flushing (`flush_ui_events`).
- **`linux/src/tray_indicator.{hpp,cpp}`**: Manages the Ayatana AppIndicator status icon, dynamic status labels, and context menus for window toggling, manual file selection, clipboard toggling, and ADB setup.
- **`linux/src/wayland_clipboard.{hpp,cpp}`**: Interfaces with the GTK/GDK clipboard and Wayland command-line utilities (`wl-copy`). Listens to `owner-change` and requests/pushes text and binary MIME payloads.
- **`linux/src/adb_helper.{hpp,cpp}`**: Discovers local or system `adb`, grants background clipboard and overlay permissions via `cmd appops`, and configures bidirectional TCP port forwarding (`adb forward` and `adb reverse`).
- **`linux/src/nfc_ntag5332.{hpp,cpp}`**: Probes `/sys/bus/i2c/devices/i2c-NTAG5332:00` for hardware presence, builds `l2a://tap` DeepLink URIs, encodes NFC Forum Type 5 NDEF TLV payloads, and writes blocks to `/dev/i2c-0`.
- **`core/src/file_transfer.cpp` & `core/src/engine.cpp`**: Core transport engine handling TLS 1.3 channels, frame dispatching, and file chunk streaming.

---

### 1.2 Verbatim Code Observations

#### Observation O1: Command Injection via Unsanitized Remote MIME Type
In `linux/src/wayland_clipboard.cpp`, lines 81-85:
```cpp
    // For images or binary MIME types: stream to wl-copy if available
    if (has_wl_clipboard_tools()) {
        std::string cmd = "wl-copy --type '" + item.mime_type + "'";
        FILE* p = ::popen(cmd.c_str(), "w");
```
`item.mime_type` originates directly from the remote peer across the network (`kClipOffer` / `ReceivedClipboardItem`). It is directly concatenated into a shell command string executed via `popen` (`/bin/sh -c`).

#### Observation O2: Command Injection in Desktop Notifications
In `linux/src/app_window.cpp`, lines 336-348:
```cpp
    if (success) {
        append_log("[文件完成] SHA-256 校验通过: " + saved_path.string());
        send_desktop_notification("文件传输完成", saved_path.filename().string());
    } ...
void AppWindow::send_desktop_notification(const std::string& title, const std::string& body) {
    std::string cmd = "notify-send -a 'Linux ⇄ Android' '" + title + "' '" + body + "' >/dev/null 2>&1 &";
    int rc = ::system(cmd.c_str());
    (void)rc;
}
```
`saved_path.filename().string()` is derived from remote file metadata. Any file name containing single quotes or shell metacharacters triggers arbitrary shell execution.

#### Observation O3: Missing SIGPIPE Handling Across Process
No signal handling for `SIGPIPE` exists across `linux/src/` or `core/src/`:
- `linux/src/wayland_clipboard.cpp:70, 92, 95`: `::fwrite(...)` to `FILE* p` opened by `popen`.
- `linux/src/adb_helper.cpp:15`: `popen` execution.
- `core/src/tls_channel.cpp:110`: socket write operations.
If the child process (`wl-copy` or `adb`) exits prematurely or a TCP socket breaks, the default `SIGPIPE` signal immediately terminates the entire application.

#### Observation O4: Background Wayland Clipboard Loss
In `linux/src/wayland_clipboard.cpp`, lines 19-25:
```cpp
void WaylandClipboardBridge::start_gtk_monitoring() {
    GdkAtom clip_atom = gdk_atom_intern_static_string("CLIPBOARD");
    gtk_clip_ = gtk_clipboard_get(clip_atom);
    if (gtk_clip_) {
        g_signal_connect(gtk_clip_, "owner-change", on_gtk_owner_change, this);
    }
}
```
In `linux/src/app_window.cpp`, lines 170-178:
```cpp
gboolean AppWindow::on_window_delete(GtkWidget* widget, GdkEvent* /*event*/, gpointer user_data) {
    auto* self = static_cast<AppWindow*>(user_data);
    gtk_widget_hide(widget);
    self->append_log("[托盘] 窗口已最小化至系统托盘，后台剪贴板与文件接收继续运行。");
    return TRUE_VAL;
}
```
Under pure Wayland compositors (GNOME Wayland, KDE Plasma Wayland, wlroots/Sway), the Wayland security model denies clipboard access and stops delivering `owner-change` events when no application window has active focus. Minimizing to tray causes clipboard monitoring to silently halt.

#### Observation O5: Clipboard Echo / Feedback Loop & Missing Local Origin Tracking
In `linux/src/wayland_clipboard.cpp`, lines 27-56 and 63-78:
Setting remote text via `gtk_clipboard_set_text(gtk_clip_, ...)` triggers GTK's `owner-change` signal. `on_gtk_owner_change` requests text via `gtk_clipboard_request_text`, which invokes `on_gtk_text_received` and immediately pushes the text back to `Engine::push_clipboard_text`. Although `core` tracks `last_received_hash_`, `WaylandClipboardBridge` lacks local guard flags (`is_applying_remote_`) or hash caching, generating redundant GTK requests and ping-pong risks upon whitespace/encoding changes.

#### Observation O6: Use-After-Free via Discarded `g_timeout_add` Source
In `linux/src/app_window.cpp`, line 141:
```cpp
    // Periodic UI event pump (every 50ms)
    g_timeout_add(50, flush_ui_events, this);
```
The returned `guint` source ID is discarded. In `AppWindow::~AppWindow()` (lines 22-24):
```cpp
AppWindow::~AppWindow() {
    engine_.stop();
}
```
`g_source_remove` is never called. If `AppWindow` is destroyed, the timeout callback continues running on the `GMainContext` and dereferences a dangling `this` pointer.

#### Observation O7: Synchronous Blocking Operations Freezing GTK UI Thread
In `linux/src/app_window.cpp`:
- Lines 250-258 (`run_adb_setup`): Runs synchronous `popen` commands and `engine_.connect_to_peer("127.0.0.1", 53322)` (which calls blocking `::connect()`).
- Lines 272-277 (`on_btn_nfc_write`): Calls `NfcNtag5332Controller::try_write_via_i2c`, which executes multiple `::write()` calls and `::usleep(5000)` per block on the UI thread.
- Lines 206-215 & 238-246: Calls `engine_.send_files(paths)` directly on the UI thread, which scans directories and synchronously computes SHA-256 for entire files before returning.

#### Observation O8: Indefinite Hang in `run_cmd_capture` (No Subprocess Timeout)
In `linux/src/adb_helper.cpp`, lines 13-24:
```cpp
int run_cmd_capture(const std::string& cmd, std::string& out_text) {
    out_text.clear();
    FILE* pipe = ::popen((cmd + " 2>&1").c_str(), "r");
    if (!pipe) return -1;
    std::array<char, 256> buf{};
    while (::fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) {
        out_text += buf.data();
    }
    return ::pclose(pipe);
}
```
`popen` / `fgets` blocks synchronously. If `adb` hangs (e.g. device unauthorized, daemon restart deadlock), the calling thread hangs indefinitely.

#### Observation O9: Flawed Exit Code Evaluation (`pclose` status vs exit code)
In `linux/src/adb_helper.cpp`, line 23:
`return ::pclose(pipe);`
On POSIX systems, `pclose` returns the termination status from `wait4`. To inspect the exit code, `WIFEXITED` and `WEXITSTATUS` must be evaluated.

#### Observation O10: Asymmetric Port Forwarding Success Check and Target Port Mismatch
In `linux/src/adb_helper.cpp`, lines 78-95:
```cpp
    std::string out_fwd;
    int rc_fwd = run_cmd_capture(
        adb_bin + " forward tcp:53322 tcp:" + std::to_string(transfer_port), out_fwd);

    std::string out_rev;
    int rc_rev = run_cmd_capture(
        adb_bin + " reverse tcp:53323 tcp:" + std::to_string(transfer_port), out_rev);

    if (rc_fwd == 0 || rc_rev == 0) {
        res.success = true;
```
1. If `rc_fwd` fails while `rc_rev` succeeds, `res.success` is set to `true`. In `AppWindow::run_adb_setup()`, it immediately attempts `engine_.connect_to_peer("127.0.0.1", 53322)`. This always fails because forwarding was not established.
2. In `adb forward tcp:53322 tcp:<port>`, the target port is on the Android device (where `L2AService` listens on port 53321). Passing PC's local `transfer_port` (which may be dynamic or different) causes connection failure.

#### Observation O11: Hardcoded `/dev/i2c-0` and Missing Retry Backoff in NFC Driver
In `linux/src/nfc_ntag5332.cpp`, lines 31, 98, 123-129:
- Hardcodes `/dev/i2c-0`, ignoring sysfs device parent adapter numbering.
- On block write failure, immediately aborts without retrying or backoff on transient bus errors (`EBUSY`, `EAGAIN`), risking partial EEPROM corruption.

#### Observation O12: Integer Overflow on NDEF URI Length Byte
In `linux/src/nfc_ntag5332.cpp`, lines 67, 85:
```cpp
    uint8_t payload_len = static_cast<uint8_t>(1 + uri.size());
    ...
    tlv.push_back(static_cast<uint8_t>(ndef_record.size()));
```
If `uri.size() >= 255`, `payload_len` wraps around due to truncation, corrupting the NDEF TLV structure.

#### Observation O13: Core Receive Thread Deadlocked in Synchronous File Streaming
In `core/src/file_transfer.cpp`, lines 220-237:
```cpp
        case FrameType::kFileResumeOffset: {
            ...
            if (found) {
                stream_outgoing_files(batch_copy, *resume);
            }
            break;
        }
```
`handle_frame` is called on `recv_thread_` in `Engine::channel_recv_loop`. `stream_outgoing_files` synchronously reads from disk and sends chunks for the entire batch. During this entire transfer, `recv_thread_` cannot process any incoming frames (e.g., Ping/Pong heartbeats, clipboard updates, cancel frames, or `kFileVerifiedAck`). In bidirectional transfers, both ends block in `stream_outgoing_files`, causing a complete distributed deadlock.

---

## 2. Logic Chain

```
[Observation O1 & O2: Unescaped popen/system command execution]
  ↳ Input originates from remote peer (MIME type / file name)
  ↳ Shell metacharacters (e.g. quotes, semicolons) escape command boundaries
  ↳ Remote Code Execution (RCE) vulnerability exists on Linux host.

[Observation O3: Zero SIGPIPE handling in codebase]
  ↳ Subprocess pipes (wl-copy, adb) or TCP sockets can close unexpectedly
  ↳ Next write operation triggers SIGPIPE
  ↳ Default signal action terminates process immediately -> Desktop crash.

[Observation O4: Wayland focus requirement for GdkClipboard]
  ↳ Application window hidden to tray on delete event
  ↳ Window loses keyboard and surface focus
  ↳ Wayland compositor suppresses clipboard events to unfocused clients
  ↳ Clipboard sync silently ceases when running in background.

[Observation O6: g_timeout_add source ID discarded]
  ↳ AppWindow destroyed without g_source_remove
  ↳ Timer callback remains registered in GMainContext
  ↳ flush_ui_events runs with dangling pointer -> Use-After-Free / crash.

[Observation O7: Heavy I/O & hashing run synchronously on GTK main thread]
  ↳ GTK main loop cannot iterate while popen, I2C writes, or file hashing execute
  ↳ Window does not repaint, input events queue up, OS flags "Window not responding".

[Observation O8 & O9: popen without timeout and unhandled wait4 status]
  ↳ Hanging ADB command freezes thread permanently
  ↳ Raw status value returned rather than WEXITSTATUS, leading to false checks.

[Observation O10: ADB forward/reverse logic error]
  ↳ rc_fwd failure masked by rc_rev success in '||' condition
  ↳ Host local port passed to forward instead of Android service port (53321)
  ↳ Direct USB connection attempts fail.

[Observation O11: Hardcoded i2c-0 & no retry backoff]
  ↳ Modern laptops route touchpad NFC through varying I2C buses (i2c-1, i2c-2, etc.)
  ↳ Transient I2C NACKs during EEPROM write cycle cause premature abort and corrupt tag memory.

[Observation O13: stream_outgoing_files executed on recv_thread_]
  ↳ Frame handling in Engine::channel_recv_loop is single-threaded
  ↳ Synchronous disk read + chunk sending starves receive loop of processing ACKs/Pings
  ↳ Simultaneous bidirectional transfers mutually block -> Complete deadlock.
```

---

## 3. Categorized Defect Findings

| ID | Category | Severity | File & Line | Summary |
|---|---|---|---|---|
| **DEF-01** | Core Transport | **CRITICAL** | `core/src/file_transfer.cpp:235` | Receive thread blocked during file chunk streaming; causes bidirectional deadlock |
| **DEF-02** | Security | **HIGH** | `linux/src/wayland_clipboard.cpp:83` | Remote Command Execution via shell injection in `wl-copy --type '<mime>'` |
| **DEF-03** | Security | **HIGH** | `linux/src/app_window.cpp:345` | Remote Command Execution via shell injection in `notify-send` filename argument |
| **DEF-04** | Stability | **HIGH** | `linux/src/wayland_clipboard.cpp:19` | Wayland background clipboard monitoring ceases when minimized to tray |
| **DEF-05** | Memory Safety | **HIGH** | `linux/src/app_window.cpp:141` | Use-After-Free via unmanaged `g_timeout_add` source upon `AppWindow` destruction |
| **DEF-06** | UI Responsiveness | **HIGH** | `linux/src/app_window.cpp:210, 242` | Synchronous file SHA-256 calculation freezes GTK UI during drag-and-drop |
| **DEF-07** | UI Responsiveness | **HIGH** | `linux/src/app_window.cpp:250-258` | Synchronous ADB execution and TCP connection freezes GTK main thread |
| **DEF-08** | Crash / Signals | **MEDIUM** | `linux/src/main.cpp` & throughout | Unhandled `SIGPIPE` causes immediate process termination on broken pipes |
| **DEF-09** | Subprocess | **MEDIUM** | `linux/src/adb_helper.cpp:15-24` | Indefinite hang in `run_cmd_capture` due to lack of timeout mechanism |
| **DEF-10** | Subprocess | **MEDIUM** | `linux/src/adb_helper.cpp:23` | Exit status incorrectly evaluated from `pclose` without `WEXITSTATUS` macro |
| **DEF-11** | Connectivity | **MEDIUM** | `linux/src/adb_helper.cpp:82-90` | Asymmetric forward/reverse check and invalid target port in ADB forward |
| **DEF-12** | Clipboard | **MEDIUM** | `linux/src/wayland_clipboard.cpp:27-56` | Missing local echo suppression flag in Wayland clipboard bridge |
| **DEF-13** | Hardware Fault | **MEDIUM** | `linux/src/nfc_ntag5332.cpp:98, 123` | Hardcoded `/dev/i2c-0` path and lack of retry backoff on transient bus errors |
| **DEF-14** | Buffer / Memory | **LOW** | `linux/src/app_window.cpp:385` | Unbounded growth of GTK log text buffer in long-running sessions |
| **DEF-15** | Data Integrity | **LOW** | `linux/src/nfc_ntag5332.cpp:67` | Integer overflow in NDEF TLV payload length byte for URIs >= 255 bytes |

---

## 4. Concrete Fix Strategies

### Fix for DEF-01 (Core Receive Loop Deadlock)
Offload `stream_outgoing_files` to a dedicated sender worker thread / `std::async`:
```cpp
case FrameType::kFileResumeOffset: {
    ...
    if (found) {
        std::thread([this, batch = std::move(batch_copy), res = *resume]() {
            stream_outgoing_files(batch, res);
        }).detach();
    }
    break;
}
```

### Fix for DEF-02 & DEF-03 (Shell Injection in Clipboard & Notifications)
1. In `wayland_clipboard.cpp`, sanitize `item.mime_type` against `^[a-zA-Z0-9_\-+./]+$`. Reject any MIME containing characters outside this set. Prefer `fork` + `execvp` (`{"wl-copy", "--type", mime_type.c_str(), nullptr}`) over `/bin/sh`.
2. In `app_window.cpp`, replace `::system(cmd.c_str())` with direct execution using `fork` + `execvp` (`{"notify-send", "-a", "Linux ⇄ Android", title.c_str(), body.c_str(), nullptr}`) to eliminate shell parsing.

### Fix for DEF-04 (Wayland Background Clipboard)
Add support for Wayland background clipboard capture using `wl-paste --watch`:
```cpp
// Check if running on Wayland; spawn background watcher thread running:
// "wl-paste --watch [internal pipe reader]" to capture events without window focus.
```

### Fix for DEF-05 (GSource Lifetime Management)
Store `guint timeout_source_id_{0}` in `AppWindow`. In `AppWindow::~AppWindow()`:
```cpp
if (timeout_source_id_ != 0) {
    g_source_remove(timeout_source_id_);
    timeout_source_id_ = 0;
}
```

### Fix for DEF-07 & DEF-06 (GTK Thread Offloading)
1. Wrap ADB setup in a background `std::thread`, dispatching status messages back to `AppWindow` via `g_idle_add`.
2. Asynchronously scan and hash dropped files in a worker thread, posting progress back to the GTK main context.

### Fix for DEF-08 (SIGPIPE Handling)
In `main.cpp` before any I/O:
```cpp
::signal(SIGPIPE, SIG_IGN);
```

### Fix for DEF-09 & DEF-10 (Subprocess Runner Robustness)
Implement a non-blocking pipe runner with `poll()` / `select()` and a 5-second timeout. Unpack exit codes using `WIFEXITED(st) ? WEXITSTATUS(st) : -1`.

### Fix for DEF-11 (ADB Port Forwarding Correction)
```cpp
// Ensure PC forwards 53322 to Android's fixed service port 53321:
adb_bin + " forward tcp:53322 tcp:53321";
// Ensure Android reverse connects back to Linux's active bound port:
adb_bin + " reverse tcp:53323 tcp:" + std::to_string(transfer_port);
// Require both commands to succeed:
if (rc_fwd == 0 && rc_rev == 0) { ... }
```

### Fix for DEF-13 (NFC I2C Hardware Robustness)
1. Dynamically read adapter bus number from `/sys/bus/i2c/devices/i2c-NTAG5332:00`.
2. Implement 3-retry exponential backoff (2ms, 5ms, 10ms) on `EAGAIN` / `EBUSY` / `ENXIO`.
3. Wrap file descriptors in an RAII `UniqueFd` class.

---

## 5. Linux-specific Testability & Verification Strategy

### Current Test Baseline
- Build Command: `cmake -B build -DL2A_BUILD_LINUX=ON && cmake --build build -j4` (Clean, 0 errors).
- Test Command: `ctest --test-dir build --output-on-failure -V` (2/2 tests pass: `test_protocol`, `test_loopback_transfer`).

### Proposed Linux Automated Test Suite (`tests/test_linux_subsystem.cpp`)
To achieve robust regression coverage, implement automated tests covering:
1. **`test_adb_helper`**:
   - Verify `run_cmd_capture` extracts status codes via `WEXITSTATUS` correctly.
   - Verify timeout aborts a command that sleeps > 5s without hanging test runner.
   - Verify port string construction for `forward` (device port 53321) vs `reverse` (PC port).
2. **`test_nfc_encoding_and_fault_tolerance`**:
   - Verify `encode_type5_ndef_tlv` with URI strings of 10, 100, 250, and 500 characters, asserting correct formatting and absence of truncation.
   - Verify simulated block write retries with synthetic bus busy returns.
3. **`test_wayland_clipboard_safety`**:
   - Verify MIME type sanitization rejects malicious command injection strings (`image/png'; rm -rf /; '`).
   - Verify anti-echo suppression: setting local clipboard ignores immediate self-notification.
4. **`test_concurrent_transfer_no_deadlock`**:
   - Spin up two loopback `Engine` instances and initiate simultaneous bidirectional transfers of multi-megabyte streams to verify `recv_thread_` does not deadlock.

---

## 6. Suggested Milestones & Dependencies

1. **Milestone M1 (Critical Transport & Signal Safety)**:
   - Fix DEF-01 (decouple `stream_outgoing_files` from `recv_thread_`).
   - Add `SIGPIPE` ignoring (DEF-08).
   - *Dependency*: Core engine and channel tests.
2. **Milestone M2 (Security & Memory Hardening)**:
   - Eliminate command injection in `wayland_clipboard.cpp` (DEF-02) and `app_window.cpp` (DEF-03).
   - Fix `g_timeout_add` resource leak (DEF-05) and GTK log buffer limits (DEF-14).
   - *Dependency*: GTK3 runtime linking.
3. **Milestone M3 (Subprocess & Hardware Subsystems)**:
   - Overhaul `AdbHelper` with non-blocking timeout runner, corrected port mapping, and robust exit status (DEF-09, DEF-10, DEF-11).
   - Harden NFC I2C driver with dynamic sysfs adapter resolution, retries, and URI length validation (DEF-12, DEF-13, DEF-15).
   - *Dependency*: POSIX process APIs and I2C dev headers.
4. **Milestone M4 (Desktop Responsiveness & Wayland Support)**:
   - Offload heavy tasks from GTK main thread (DEF-06, DEF-07).
   - Enhance Wayland clipboard background daemon support (DEF-04).
   - Implement `test_linux_subsystem` test suite in `tests/`.

---

## 7. Caveats
- No direct hardware NFC NTAG5332 chip was attached during test execution; hardware tests rely on sysfs mocks and interface tests.
- Physical Wayland clipboard background tests require active compositors (e.g. Sway / Mutter); headless CI verification can mock `wl-paste` behavior.

---

## 8. Conclusion
The Linux Desktop Subsystem is well-architected for modular integration with the C++20 core, but contains 1 critical deadlock in core frame reception during file streaming, 2 high-severity remote command injection vulnerabilities from unescaped shell commands, and multiple GUI thread-blocking and subprocess exception handling flaws. All identified issues have clear, localized fix strategies that preserve API compatibility.

---

## 9. Verification Method
1. Build verification:
   `cmake -B build -DL2A_BUILD_LINUX=ON && cmake --build build -j4`
2. Test verification:
   `ctest --test-dir build --output-on-failure -V`
3. Inspection:
   Verify `handoff.md` and check lines cited across `linux/` and `core/`.
