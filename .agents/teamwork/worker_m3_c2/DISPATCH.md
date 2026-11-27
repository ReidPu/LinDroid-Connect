# DISPATCH — worker_m3_c2

## 2026-10-23T14:33:53Z

## Task Assignment
**Role**: Test Suite Implementation Worker
**Working Directory**: `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/worker_m3_c2`
**Project Workspace Root**: `/home/reid-pu/Programming/Linux_to_Android`
**Source of Truth**: `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/ORIGINAL_REQUEST.md` (read section `## 2026-10-23T12:34:22Z`)
**Master Scope**: `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/PROJECT.md`

## Milestone M3-C2 Objectives: Automated Regression Testing & Dual Builds
You are responsible for implementing the automated regression test suites and verifying both the Linux desktop and Android builds.

### 1. Implement `tests/test_initial_clipboard_sync.cpp` (R1)
- Objective: Verify that calling `push_clipboard_text()` (or `send_frame_active()`) on an unconnected node with registered candidate endpoints (discovered peer LAN IP or fallback port) automatically triggers on-demand connection (`ensure_connected()`), completes the handshake, and successfully delivers clipboard text to the receiving node without any prior file transfer.
- Structure:
  - Setup two `Engine` instances (Node A and Node B) on localhost loopback with distinct ports.
  - Node B listens. Node A registers Node B as candidate peer (`register_candidate_peer("127.0.0.1", node_b_port)`).
  - Verify `node_a.is_connected()` is initially `false`.
  - Node A calls `node_a.push_clipboard_text("Hello from Initial Clipboard Sync!")` WITHOUT initiating any file transfer.
  - Node B's clipboard callback captures the received item.
  - Assert that `node_a.is_connected()` becomes `true`, and Node B receives the exact text within timeout.
  - Test boundary cases: rapid consecutive clipboard pushes, large clipboard payload (>64KB).

### 2. Implement `tests/test_reverse_transfer_and_fallback.cpp` (R2, R3)
- Objective: Verify reverse connection from client to server under two realistic topologies:
  - Scenario A (Direct LAN IP): Client connects to Server IP:Port, sends clipboard and a batch of files. Verify file receipt, chunk hashing, and clipboard integrity.
  - Scenario B (Dual Candidate Fallback): Primary candidate endpoint is unreachable (e.g. port closed or connection refused), Client's `ensure_connected()` gracefully falls back to secondary candidate endpoint (simulating USB reverse tunnel `127.0.0.1:53323`), establishes TLS session, and successfully transmits clipboard and files.
- Assert non-blocking timeout (does not hang indefinitely when primary is down) and clean completion.

### 3. Expand `linux/tests/test_linux_subsystem.cpp` (R2, R5)
- Verify `ensure_desktop_entry_installed`: creates valid `~/.local/share/applications/l2a-desktop.desktop` with proper `[Desktop Entry]`, `Type=Application`, `Name=Linux ⇄ Android`, `Exec`, `Icon`.
- Verify `AppWindow::send_desktop_notification` formatting: validates `-h string:desktop-entry:l2a-desktop` and `-i network-transmit-receive`.
- Verify Wayland clipboard bridge logic: `apply_remote_clipboard` text handling and temporary file usage for large payloads.

### 4. Register in `tests/CMakeLists.txt`
- Register `test_initial_clipboard_sync` and `test_reverse_transfer_and_fallback` as CTest executables linking `l2a_core`.

### 5. Dual Build & Verification
- Compile and test Linux:
  ```bash
  cmake -B build -DL2A_BUILD_LINUX=ON && cmake --build build -j4
  ctest --test-dir build --output-on-failure -V
  ```
  Ensure all 12+ tests pass (100% pass rate).
- Compile Android:
  ```bash
  JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64 \
  ANDROID_USER_HOME=/home/reid-pu/Programming/Linux_to_Android/.android \
  ./gradlew assembleDebug -Duser.home=/home/reid-pu/Programming/Linux_to_Android
  ```
  Ensure `android/app/build/outputs/apk/debug/app-debug.apk` is cleanly produced.
- Check ADB device availability: `adb devices`. If an online device is detected, run `./gradlew installDebug`.

## Mandatory Integrity Warning
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A teamwork_preview_auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.

Produce `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/worker_m3_c2/handoff.md` with build and test logs, update your `progress.md` regularly, and send a message when complete.
