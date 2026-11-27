# Project: Linux_to_Android (L2A) Defect Remediation & Verification

## Architecture
Linux_to_Android is a high-performance cross-platform file, directory, and clipboard synchronization system connecting Linux desktops and Android mobile devices.
- **Core Library (`core/`)**: Modern C++20 shared library (`l2a_core`). Zero-external-dependency cryptography (RFC 7748 X25519, RFC 8439 ChaCha20-Poly1305, SHA-256), binary framing protocol, TLS 1.3-like authenticated handshake, UDP beacon peer discovery, file chunk streaming with resumption, and clipboard streaming.
- **Linux Desktop Subsystem (`linux/`)**: GTK3 and Ayatana AppIndicator UI, Wayland clipboard bridge with `wl-copy`/`wl-paste` fallback, ADB process pipe automation, and NTAG5332 touchpad NFC I2C hardware integration.
- **Android Subsystem (`android/`)**: Kotlin Android app (`com.reid.l2a`) with JNI C++ bridge (`jni_bridge.cpp`), `L2AService` foreground service (`dataSync`), `NfcTriggerActivity` for one-touch touchpad NFC tap synchronization, and `ShareReceiverActivity` for system share integration.
- **Tests (`tests/`)**: CTest-driven test suite validating cryptography, binary protocols, TLS handshake, and loopback end-to-end transfers.

---

## Feature & Defect Inventory
Every item identified during the comprehensive Survey phase is inventoried and mapped to an implementation milestone.

| # | Item ID | Description | Milestone | Source |
|---|---|---|---|---|
| 1 | CORE-01 | Decouple `stream_outgoing_files` from `recv_thread_` into async task to eliminate bidirectional transfer deadlock | M1 | Survey (core/) |
| 2 | CORE-02 | Release `FileTransferManager::mutex_` before computing SHA-256, sending ACKs, and calling listener callback | M1 | Survey (core/) |
| 3 | CORE-03 | Prevent self-join deadlock in `Engine::attach_channel` when reconnecting from `on_connection_state_changed` | M1 | Survey (core/) |
| 4 | CORE-04 | Fix `kMaxFramePayload` frame overflow when directory contains >750 files by chunking metadata | M1 | Survey (core/) |
| 5 | CORE-05 | Implement RAII cleanup and startup sweep for temporary clipboard files (`l2a_clip_*.tmp`) > 512KB | M1 | Survey (core/) |
| 6 | CORE-06 | Prune completed batches from `outgoing_batches_`, `incoming_batches_`, and `lazy_sources_` | M1 | Survey (core/) |
| 7 | CORE-07 | Enable periodic `kPing`/`kPong` heartbeats and configure socket timeouts (`SO_RCVTIMEO`, `SO_SNDTIMEO`, `SO_KEEPALIVE`) | M1 | Survey (core/) |
| 8 | CORE-08 | Sanitize and escape whitespace/newlines in `trusted_peers.tsv` and `identity.cfg` parsing | M1 | Survey (core/) |
| 9 | CORE-09 | Use `std::atomic<int>` for `sock_fd_` and synchronize socket close lifecycle | M1 | Survey (core/) |
| 10 | CORE-10 | Enforce bounds check (`count <= remaining / min_size`) before `p.files.reserve()` to prevent DoS | M1 | Survey (core/) |
| 11 | CORE-11 | Clean up or truncate corrupt `.l2apart` on SHA-256 verification failure to avoid infinite resume loops | M1 | Survey (core/) |
| 12 | CORE-12 | Constant-time HMAC comparison in TLS Finished verification | M1 | Survey (core/) |
| 13 | CORE-13 | Set `POSITION_INDEPENDENT_CODE ON` on `l2a_core` and add Sanitizer flags in CMake | M1 | Survey (core/) |
| 14 | LINUX-01 | Prevent Remote Command Execution by sanitizing MIME type and using direct exec in `wayland_clipboard.cpp` | M2 | Survey (linux/) |
| 15 | LINUX-02 | Prevent Remote Command Execution by avoiding shell concatenation in `notify-send` | M2 | Survey (linux/) |
| 16 | LINUX-03 | Ignore `SIGPIPE` in `linux/src/main.cpp` to prevent process death on broken pipes/sockets | M2 | Survey (linux/) |
| 17 | LINUX-04 | Add `wl-paste --watch` background monitoring support when window is minimized to system tray | M2 | Survey (linux/) |
| 18 | LINUX-05 | Store and remove `g_timeout_add` Source ID in `AppWindow` to prevent Use-After-Free | M2 | Survey (linux/) |
| 19 | LINUX-06 | Offload synchronous drag-and-drop file scanning & SHA-256 hashing from GTK main thread to background worker | M2 | Survey (linux/) |
| 20 | LINUX-07 | Offload synchronous ADB execution & TCP connection from GTK main thread to background worker | M2 | Survey (linux/) |
| 21 | LINUX-08 | Non-blocking subprocess execution with timeout in `AdbHelper::run_cmd_capture` | M2 | Survey (linux/) |
| 22 | LINUX-09 | Inspect real subprocess exit codes using `WEXITSTATUS` macro rather than raw `pclose` status | M2 | Survey (linux/) |
| 23 | LINUX-10 | Correct ADB port forwarding logic (ensure Android port 53321 target, enforce both forward and reverse succeed) | M2 | Survey (linux/) |
| 24 | LINUX-11 | Suppress local clipboard echo loops in `WaylandClipboardBridge` | M2 | Survey (linux/) |
| 25 | LINUX-12 | Dynamic I2C adapter resolution and exponential backoff retry on bus errors in NFC controller | M2 | Survey (linux/) |
| 26 | LINUX-13 | Validate NDEF URI length (< 255 bytes) to prevent payload length byte integer overflow | M2 | Survey (linux/) |
| 27 | LINUX-14 | Bound GTK log text buffer length to 1000 lines to prevent unbounded memory growth | M2 | Survey (linux/) |
| 28 | LINUX-15 | Replace hardcoded Ubuntu `/usr/lib/x86_64-linux-gnu` paths in `linux/CMakeLists.txt` with `pkg-config` | M2 | Survey (linux/) |
| 29 | ANDROID-01 | Automatically invoke `DetachCurrentThread` on native worker thread termination to prevent fatal ART abort | M3 | Survey (android/) |
| 30 | ANDROID-02 | Synchronize `g_callback_obj` lifecycle with mutex to eliminate Use-After-Free race on service stop | M3 | Survey (android/) |
| 31 | ANDROID-03 | Release `g_engine_mtx` during blocking `connect_to_peer` to prevent UI thread ANR freezes | M3 | Survey (android/) |
| 32 | ANDROID-04 | Check and clear pending JNI exceptions after calling Java callbacks | M3 | Survey (android/) |
| 33 | ANDROID-05 | Cache `jmethodID` and class references during `JNI_OnLoad` | M3 | Survey (android/) |
| 34 | ANDROID-06 | Support zero-copy Scoped Storage streaming via `ParcelFileDescriptor` (`nativeSendFileFd`) without disk buffering | M3 | Survey (android/) |
| 35 | ANDROID-07 | Index received media files into `MediaStore` via `MediaScannerConnection.scanFile` | M3 | Survey (android/) |
| 36 | ANDROID-08 | Add runtime permission request for `POST_NOTIFICATIONS` in `MainActivity.kt` on Android 13/14 | M3 | Survey (android/) |
| 37 | ANDROID-09 | Request battery optimization exemption and guide HyperOS Autostart configuration | M3 | Survey (android/) |
| 38 | ANDROID-10 | Add `CHANGE_WIFI_MULTICAST_STATE` permission and acquire `WifiManager.MulticastLock` in `L2AService` | M3 | Survey (android/) |
| 39 | ANDROID-11 | Filter out `LinuxClipboard` label in Android `clipListener` to prevent ping-pong loops | M3 | Survey (android/) |
| 40 | ANDROID-12 | Handle large text (>512KB) from temp file path and image clipboard items | M3 | Survey (android/) |
| 41 | ANDROID-13 | Register `ACTION_SEND_MULTIPLE` in `ShareReceiverActivity` for multi-file sharing | M3 | Survey (android/) |
| 42 | TEST-01 | Add bidirectional transfer concurrency regression test without deadlock | M4 | Survey (tests/) |
| 43 | TEST-02 | Add large folder (>750 / 2,000 files) metadata transfer test | M4 | Survey (tests/) |
| 44 | TEST-03 | Add re-entrant callback deadlock verification test | M4 | Survey (tests/) |
| 45 | TEST-04 | Add simulated abrupt disconnect and reconnect resume test | M4 | Survey (tests/) |
| 46 | TEST-05 | Add corrupted partial file (.l2apart) recovery test | M4 | Survey (tests/) |
| 47 | TEST-06 | Add malformed packet fuzzing and DoS boundary defense test | M4 | Survey (tests/) |
| 48 | TEST-07 | Add temporary file cleanup and leak verification test | M4 | Survey (tests/) |
| 49 | TEST-08 | Add Linux subsystem mocks & NDEF encoding test suite | M4 | Survey (tests/) |

---

## Milestones

| # | Name | Scope | Dependencies | Status |
|---|---|---|---|---|
| M1 | Core C++ Remediation | `core/` (CORE-01 to CORE-13) | none | PLANNED |
| M2 | Linux Desktop Remediation | `linux/` (LINUX-01 to LINUX-15) | M1 | PLANNED |
| M3 | Android Subsystem Remediation | `android/` (ANDROID-01 to ANDROID-13) | M1 | PLANNED |
| M4 | Test Suite Enhancement & Full Regression | `tests/`, `CMakeLists.txt` (TEST-01 to TEST-08) | M1, M2 | PLANNED |
| M5 | Final Verification & Forensic Audit | Full project build & CTest verification, Forensic Integrity Audit | M1, M2, M3, M4 | PLANNED |

---

## Interface Contracts

### `core/` ↔ `linux/`
- `Engine::send_files(const std::vector<std::string>& file_paths)`: Non-blocking or offloaded to worker thread.
- `Engine::push_clipboard_text(const std::string& text)`: Guarded against local echo.
- `ICoreListener::on_file_completed(const std::string& path, bool success, const std::string& err)`: Listener MUST NOT hold internal engine locks.
- `ICoreListener::on_connection_state_changed(bool connected, const std::string& peer_id, const std::string& peer_name)`: Reconnection attempts MUST NOT self-join `recv_thread_`.

### `core/` ↔ `android/` (JNI)
- `Java_com_reid_l2a_CoreBridge_nativeSendFileFd(JNIEnv* env, jobject thiz, jint fd, jstring display_name, jlong file_size)`: Direct streaming from Android `ParcelFileDescriptor`.
- `Java_com_reid_l2a_CoreBridge_nativeConnect(JNIEnv* env, jobject thiz, jstring ip, jint port)`: Connects outside of `g_engine_mtx` to prevent main thread ANR.
- Thread Attachment: Any native thread invoking Java callbacks MUST be registered with a thread-local RAII cleaner that calls `DetachCurrentThread` on thread exit.

---

## Code Layout
```
/home/reid-pu/Programming/Linux_to_Android/
├── CMakeLists.txt
├── core/
│   ├── CMakeLists.txt
│   ├── include/l2a/
│   │   ├── clipboard_stream.hpp
│   │   ├── discovery.hpp
│   │   ├── engine.hpp
│   │   ├── file_transfer.hpp
│   │   ├── listener.hpp
│   │   ├── protocol.hpp
│   │   └── tls_channel.hpp
│   └── src/
│       ├── clipboard_stream.cpp
│       ├── discovery.cpp
│       ├── engine.cpp
│       ├── file_transfer.cpp
│       ├── protocol.cpp
│       └── tls_channel.cpp
├── linux/
│   ├── CMakeLists.txt
│   └── src/
│       ├── adb_helper.{hpp,cpp}
│       ├── app_window.{hpp,cpp}
│       ├── gtk_abi.hpp
│       ├── main.cpp
│       ├── nfc_ntag5332.{hpp,cpp}
│       ├── tray_indicator.{hpp,cpp}
│       └── wayland_clipboard.{hpp,cpp}
├── android/
│   ├── app/src/main/
│   │   ├── AndroidManifest.xml
│   │   ├── cpp/
│   │   │   ├── CMakeLists.txt
│   │   │   └── jni_bridge.cpp
│   │   └── java/com/reid/l2a/
│   │       ├── CoreBridge.kt
│   │       ├── L2AService.kt
│   │       ├── MainActivity.kt
│   │       ├── NfcTriggerActivity.kt
│   │       └── ShareReceiverActivity.kt
└── tests/
    ├── CMakeLists.txt
    ├── test_protocol.cpp
    ├── test_loopback_transfer.cpp
    └── [new regression test suites]
```
