# Forensic Integrity Audit Report — Milestone M3-C2

**Work Product**: Milestone M3-C2 (`tests/test_initial_clipboard_sync.cpp`, `tests/test_reverse_transfer_and_fallback.cpp`, `linux/src/desktop_entry.hpp`, `linux/tests/test_linux_subsystem.cpp`, `tests/CMakeLists.txt`)  
**Profile**: General Project  
**Integrity Mode**: Development Mode (per `ORIGINAL_REQUEST.md` line 40)  
**Verdict**: **CLEAN**

---

### Phase Results
- **Hardcoded test results**: **PASS** — Inspected `tests/test_initial_clipboard_sync.cpp`, `tests/test_reverse_transfer_and_fallback.cpp`, and `linux/tests/test_linux_subsystem.cpp`. Zero hardcoded test outcomes, zero tautological assertions (`TEST_CHECK(true)` / `assert(true)`). Payloads are verified via real cryptographic SHA-256 digests (`sha256_bytes`, `sha256_file_range`) and exact byte comparisons.
- **Facade detection**: **PASS** — `linux/src/desktop_entry.hpp` contains genuine implementations of `ensure_desktop_entry_installed()` (inspecting `/proc/self/exe`, writing standard GNOME `.desktop` file with 0644 permissions) and `build_desktop_notification_args()` (structuring arguments for `-a "Linux ⇄ Android" -i network-transmit-receive -h string:desktop-entry:l2a-desktop`). `Engine::ensure_connected()` and `Engine::push_clipboard_text()` perform genuine socket operations, TLS handshakes, and framing without stub facades.
- **Pre-populated artifact detection**: **PASS** — Workspace search for pre-existing logs (`find . -name "*.log" -o -name "*result*" -o -name "*output*"`) found only standard CMake/CTest and Gradle build caches. No pre-seeded test reports or fabricated verification certificates exist.
- **Build and run**: **PASS** — `cmake -B build -DL2A_BUILD_LINUX=ON && cmake --build build -j4` compiled cleanly with 0 warnings and 0 errors. All 12 CTest targets executed and passed (100% pass rate). Android Gradle debug build (`./android/gradlew -p android assembleDebug`) succeeded in 4s, producing a verified 16MB `app-debug.apk` containing both `arm64-v8a` and `x86_64` native binaries.
- **Output verification**: **PASS** — Executed test binaries directly (`test_initial_clipboard_sync`, `test_reverse_transfer_and_fallback`, `test_linux_subsystem`). Verified on-demand connection over loopback without prior file transfers, 10-message rapid burst order, 130KB multi-frame reassembly with 100% SHA-256 match, dual-candidate fallback within 16-21 ms (< 3000 ms limit), and multi-file batch transfers with SHA-256 verification.
- **Dependency audit**: **PASS** — No unauthorized third-party libraries or external execution delegation. Relies strictly on project C++20 core, POSIX sockets, GTK3/GDK, and Android NDK/SDK.

---

## 1. Observation

### 1.1 Source Code and Assertions
- In `tests/test_initial_clipboard_sync.cpp`:
  - Lines 84-85, 88-94: Instantiates and starts two `Engine` instances (`node_a` and `node_b`) on dynamic ephemeral ports (`listen_port = 0`).
  - Lines 96-98: Asserts initial disconnected state (`TEST_CHECK(!node_a.is_connected())`).
  - Lines 100-105: Node A registers Node B via `node_a.register_candidate_peer("127.0.0.1", port_b)` without connecting.
  - Lines 110-128: Node A triggers `node_a.push_clipboard_text(initial_text)` without any file transfer. Verifies via condition variable wait that Node B receives the clipboard text, connection state transitions to `true` on both nodes (`TEST_CHECK(node_a.is_connected())`), and payload matches verbatim.
  - Lines 131-155: Rapid burst of 10 consecutive clipboard pushes tested. All 10 items received sequentially in exact FIFO order.
  - Lines 158-179: Multi-frame chunking boundary tested with 130KB text (> 64KB `kMaxFramePayload`). Verifies `recv_sha256 == sent_sha256` via independent SHA-256 calculation.
- In `tests/test_reverse_transfer_and_fallback.cpp`:
  - Lines 98-207 (Scenario A): Server and Client start on loopback. Client pushes clipboard and a batch of 3 files (4KB, 64KB, 256KB) to Server. Validates end-to-end receipt and SHA-256 hash match on all 3 files (`TEST_CHECK(got_hash == h1/h2/h3)`).
  - Lines 209-329 (Scenario B): Server starts on fallback port (simulating ADB reverse port 53323). Client registers primary unreachable port (closed socket from `find_unbound_port()`) and secondary fallback port. Client calls `push_clipboard_text()`. Verifies connection completes in 16-21 ms (< 3000 ms threshold) via non-blocking connect (`O_NONBLOCK` + `poll(1000)`), delivers clipboard text, and transfers 2 files (32KB, 128KB) with SHA-256 match.
- In `linux/src/desktop_entry.hpp`:
  - Lines 11-46: `ensure_desktop_entry_installed(home_dir)` resolves `/proc/self/exe`, writes valid `.desktop` file to `~/.local/share/applications/l2a-desktop.desktop`, and applies `chmod(0644)`.
  - Lines 48-62: `build_desktop_notification_args(title, body)` formats `notify-send` argument list with `-a "Linux ⇄ Android" -i network-transmit-receive -h string:desktop-entry:l2a-desktop`.
- In `linux/tests/test_linux_subsystem.cpp`:
  - Lines 171-218 (Test 5): Tests `ensure_desktop_entry_installed()` and `build_desktop_notification_args()`. Validates file creation, 0644 permissions, INI section headers (`[Desktop Entry]`, `Type=Application`), and argument vector contents.
  - Lines 220-326 (Test 6): Tests `WaylandClipboardBridge`. Validates inline text delivery, 600KB temp file streaming, and malicious MIME injection filtering (`text/plain; rm -rf /`).
- In `tests/CMakeLists.txt`:
  - Lines 56-66: Registers `test_initial_clipboard_sync` (`Test_InitialClipboardSync`) and `test_reverse_transfer_and_fallback` (`Test_ReverseTransferAndFallback`).

### 1.2 Build & Execution Results
1. **Linux Build (`cmake -B build -DL2A_BUILD_LINUX=ON && cmake --build build -j4`)**:
   - Exit code: 0
   - Warnings / Errors: 0
2. **CTest Suite (`ctest --test-dir build --output-on-failure -V`)**:
   - Total tests: 12
   - Passed: 12 (100%)
   - Execution time: 1.22 sec
3. **Direct Binary Execution**:
   - `./build/tests/test_initial_clipboard_sync`: Exit code 0, 3/3 tests passed.
   - `./build/tests/test_reverse_transfer_and_fallback`: Exit code 0, 2/2 scenarios passed.
   - `./build/linux/test_linux_subsystem`: Exit code 0, 6/6 tests passed.
4. **Android Build (`./android/gradlew -p android assembleDebug`)**:
   - Exit code: 0
   - Result: `BUILD SUCCESSFUL in 4s`
   - Artifact: `android/app/build/outputs/apk/debug/app-debug.apk` (16MB)
   - Archive check: `unzip -t` passed with no errors; native libraries `lib/arm64-v8a/libl2a_jni.so` and `lib/x86_64/libl2a_jni.so` present.

---

## 2. Logic Chain

1. **R1 On-Demand Clipboard Synchronization Integrity**:
   - The user requirement (R1) mandates that clipboard synchronization from PC to phone must succeed upon copying without requiring a prior file transfer.
   - The test `test_initial_clipboard_sync.cpp` sets up two fresh, unconnected nodes, calls `push_clipboard_text()` immediately after candidate registration, and proves that the active channel is created on demand, the TLS 1.3-like handshake completes, and text is delivered across loopback TCP sockets.
   - Large payload (>64KB) and rapid bursts (10 messages) were tested and verified with SHA-256 hashing. No shortcuts or mocked sockets were used.

2. **R2 & R3 Reverse Connection and Dual Fallback Integrity**:
   - Requirements R2 and R3 require phone-to-PC reverse transfers over LAN IP or USB reverse tunnel (`127.0.0.1:53323`).
   - The test `test_reverse_transfer_and_fallback.cpp` evaluates Scenario A (direct LAN connection with clipboard and 3 files) and Scenario B (unreachable primary port with rapid fallback to secondary port).
   - The fallback connection is established within 16-21 ms, well below the 3000 ms timeout requirement, and files are transferred and verified via SHA-256 matching.

3. **R5 Desktop Entry & Notification Grouping Integrity**:
   - Requirement R5 mandates GNOME notification grouping via standard `.desktop` entry.
   - `linux/src/desktop_entry.hpp` writes a compliant `.desktop` file to `~/.local/share/applications/l2a-desktop.desktop` and supplies `-h string:desktop-entry:l2a-desktop` hints to `notify-send`.
   - `linux/tests/test_linux_subsystem.cpp` verifies file existence, permissions (0644), specification fields, and argument structure.

4. **Absence of Fraud or Circumvention**:
   - No mock libraries, dummy functions returning constant booleans, or tautological assertions (`assert(true)`) were detected in any of the deliverables.
   - All tests execute actual network sockets, real file I/O, and real cryptography.

---

## 3. Caveats

- **Physical USB Device**: Testing was performed on a Linux host without an active Android physical device attached over USB (`adb devices` was empty). Testing validated Android compilation via Gradle, APK packaging, and native JNI shared object inclusion (`libl2a_jni.so`).
- **Wayland Display Environment**: In headless/TTY environments without an active compositor socket (`WAYLAND_DISPLAY`), tests verify fallback behavior, MIME validation, temporary file streaming, and process arguments without opening a Wayland window.

---

## 4. Conclusion

Milestone M3-C2 satisfies all integrity criteria under Development Mode:
- Zero hardcoded outputs, zero facade implementations, and zero dummy assertions.
- Genuine multi-threaded loopback networking, TLS handshake, and file chunk transfer validation.
- Both Linux and Android builds compile with 0 errors.
- 12/12 CTest test suites pass (100% pass rate).
- **Final Verdict: CLEAN**.

---

## 5. Verification Method

To independently reproduce this forensic audit:

1. **Verify CTest Suite**:
   ```bash
   cmake -B build -DL2A_BUILD_LINUX=ON && cmake --build build -j4
   ctest --test-dir build --output-on-failure -V
   ```
   *Expected*: 12/12 tests pass (100% pass rate).

2. **Execute M3-C2 Test Binaries Directly**:
   ```bash
   ./build/tests/test_initial_clipboard_sync
   ./build/tests/test_reverse_transfer_and_fallback
   ./build/linux/test_linux_subsystem
   ```
   *Expected*: Exit code 0 for all three commands.

3. **Verify Android APK Build**:
   ```bash
   JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64 \
   ANDROID_USER_HOME=/home/reid-pu/Programming/Linux_to_Android/.android \
   ./android/gradlew -p android assembleDebug -Duser.home=/home/reid-pu/Programming/Linux_to_Android
   unzip -t android/app/build/outputs/apk/debug/app-debug.apk
   ```
   *Expected*: `BUILD SUCCESSFUL`, `No errors detected in compressed data`.
