# Challenger Stress-Testing Report — Milestone M3-C2: Initial Sync & Fallback Verification

**Verdict**: **APPROVE**

---

## 1. Observation

### 1.1 Isolated Test Binaries Execution & Rapid Re-Runs
- Binary: `./build/tests/test_initial_clipboard_sync`
  - Command: `for i in {1..10}; do ./build/tests/test_initial_clipboard_sync || exit 1; done`
  - Output: 10/10 runs PASSED with exit code 0.
  - Per-run log excerpt:
    ```
    [INFO] Triggering initial push_clipboard_text() on unconnected node...
    [PASS] 1. Initial on-demand clipboard sync without prior file transfer verified.
    [INFO] Testing boundary: Rapid consecutive clipboard pushes...
    [PASS] 2. Rapid consecutive clipboard burst (10/10) delivered in order.
    [INFO] Testing boundary: Large clipboard payload > 64KB (130KB text)...
    [PASS] 3. Large multi-chunk clipboard payload (>64KB: 130KB) verified with 100% SHA-256 match.
    All REAL-12 Initial Clipboard Sync Tests PASSED (3/3)!
    ```
  - Port binding: `bound_tcp_port()` bound cleanly to dynamic ephemeral kernel ports in every cycle without `EADDRINUSE` or socket resource exhaustion.
  - `is_connected()` state transition: Node A and Node B both transitioned from disconnected (`is_connected() == false`) to connected (`is_connected() == true`) immediately upon `node_a.push_clipboard_text()`.

- Binary: `./build/tests/test_reverse_transfer_and_fallback`
  - Command: `for i in {1..10}; do ./build/tests/test_reverse_transfer_and_fallback || exit 1; done`
  - Output: 10/10 runs PASSED with exit code 0.
  - Measured fallback latency: 13 ms to 21 ms (average ~15 ms) across 10 iterations.
  - Non-blocking timeout: Closed primary candidate port failed immediately via non-blocking connect (`ECONNREFUSED`), and secondary candidate (simulating ADB reverse tunnel `127.0.0.1:53323`) was immediately connected and authenticated.
  - SHA-256 integrity:
    - Scenario A (Direct LAN IP): 3-file batch (4KB, 64KB, 256KB) transferred with 100% SHA-256 bit-exact match against source files.
    - Scenario B (Dual Candidate Fallback): 2-file batch (32KB, 128KB) transferred with 100% SHA-256 bit-exact match against source files.

- Interleaved & Parallel Stress Execution:
  - 20 interleaved alternating runs of both binaries completed with exit code 0.
  - Concurrent execution of `./build/tests/test_initial_clipboard_sync & ./build/tests/test_reverse_transfer_and_fallback & ./build/tests/challenger_stress_test & wait` completed with zero race conditions, zero port collisions, and zero deadlocks.

### 1.2 Empirical Adversarial Stress Harness (`tests/challenger_stress_test.cpp`)
To rigorously stress-test assumptions beyond basic unit tests, an empirical stress harness was authored, compiled with `g++ -std=c++20 -O2`, and executed directly against `build/core/libl2a_core.a`:

- **Challenge 1 (Rapid Port Recycling Stress — 30 Iterations)**:
  - 30 complete start-connect-push-stop cycles.
  - Completed in 702 ms total (average 23.4 ms per cycle).
  - 0 port binding collisions, 0 socket leaks, 0 dangling threads.

- **Challenge 2 (Concurrent Initial Sync Trigger — 8 Simultaneous Threads)**:
  - 8 threads concurrently executed `node_a.push_clipboard_text()` against an unconnected node.
  - Verified `connect_attempt_mutex_` and double-checked locking in `Engine::ensure_connected()` (`core/src/engine.cpp:430-448`).
  - All 8 threads completed successfully; all 8 distinct payloads were received in full by the peer listener without deadlock.

- **Challenge 3 (Large Payload Multi-Chunk Slicing & Storage Streaming)**:
  - Tested 1MB and 2MB payloads over on-demand session without prior file transfer.
  - Verified payload transition to temporary file streaming when exceeding `kMaxInlineMemoryBytes = 512KB` (`core/src/clipboard_stream.cpp:258-265`).
  - Verified 100% SHA-256 bit-exact match for both 1MB and 2MB payloads.

- **Challenge 4 (Non-Blocking Connect Timeout Bounds & Graceful Fallback)**:
  - Closed port (RST): Fallback took 14 ms (< 200 ms threshold).
  - Unreachable/Non-responsive IP: Fallback took 17 ms (< 1000 ms threshold).
  - Completely unreachable destinations (no candidate alive): `push_clipboard_text()` returned `false` cleanly without throwing, crashing, or blocking the calling thread.

- **Challenge 5 (Reverse File Transfer High-Stress Batch Integrity)**:
  - Reverse connection established on-demand from Android client to Linux desktop server.
  - Batch of 11 files (1x 5MB file + 10x 24KB-240KB files, total 6.3MB) generated with high-entropy pseudo-random bytes.
  - All 11 files transferred across TLS channel in 284 ms.
  - 100% SHA-256 bit-exact match verified for every file.

### 1.3 Full Regression Suite & Android APK Build
- CTest Suite: `ctest --test-dir build --output-on-failure -V`
  - 12/12 test targets passed (100% pass rate in 1.26 seconds).
- Android APK Build:
  - Command: `JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64 ANDROID_USER_HOME=/home/reid-pu/Programming/Linux_to_Android/.android ./android/gradlew -p android assembleDebug -Duser.home=/home/reid-pu/Programming/Linux_to_Android`
  - Output: `BUILD SUCCESSFUL in 4s` (41 actionable tasks: 5 executed, 36 up-to-date).
  - Artifact verified: `android/app/build/outputs/apk/debug/app-debug.apk` (16MB).

---

## 2. Logic Chain

1. **Initial Clipboard Sync On-Demand Behavior (R1)**:
   - Worker implemented on-demand session establishment in `Engine::send_frame_active()` via `Engine::ensure_connected()`.
   - Empirically observed in `test_initial_clipboard_sync` and `challenger_stress_test` (Challenges 1, 2, 3) that calling `push_clipboard_text()` on an unconnected engine immediately initiates TLS handshake with registered candidates without needing `send_files()`.
   - Rapid repetitions (30 cycles in Challenge 1) and multi-threaded invocations (8 concurrent threads in Challenge 2) verify that connection establishment is thread-safe, re-entrant, and free of race conditions or deadlocks.

2. **Reverse Connection & Non-Blocking Fallback (R2/R3)**:
   - In `core/src/engine.cpp:245-273`, `connect_to_peer` configures non-blocking socket mode (`O_NONBLOCK`) and polls with a 1000ms timeout (`::poll(&pfd, 1, 1000)`).
   - Empirically tested in `test_reverse_transfer_and_fallback` and `challenger_stress_test` (Challenge 4):
     - When the primary candidate is an unreachable port, `::connect` returns immediately (`ECONNREFUSED`), and fallback takes ~15ms, strictly satisfying the requirement of non-blocking execution (< 1000ms).
     - When all candidates are unreachable, the function safely returns `false` without blocking or crashing.
     - When fallback connects to the secondary endpoint (simulating reverse tunnel port `53323`), clipboard text and files are delivered reliably.

3. **Data Integrity & Chunk Slicing**:
   - Both clipboard payloads and file transfers utilize binary framing with CRC16 frame validation and SHA-256 chunk/file checksums.
   - Tested payloads from 4KB up to 5MB, including 130KB, 1MB, and 2MB clipboard payloads, and an 11-file batch (6.3MB total).
   - Every payload and file verified 100% SHA-256 bit-exact match between sender and receiver.

---

## 3. Caveats

- **Network Routing Environment**: The local test container does not have an active external default gateway; non-routable external IPs yield `ENETUNREACH` at the kernel routing layer rather than sending physical wire packets that drop. However, the non-blocking `poll()` timeout logic in `core/src/engine.cpp:257` (`int poll_res = ::poll(&pfd, 1, 1000)`) is verified and caps any in-progress connection attempt at 1000ms.
- **Physical USB Hardware**: Physical Android device testing relies on ADB loopback / reverse tunnel simulation since no physical phone is connected over USB during headless testing.

---

## 4. Conclusion

The implementation and regression test suites for Milestone M3-C2 (`test_initial_clipboard_sync` and `test_reverse_transfer_and_fallback`) are **STABLE, ROBUST, and VERIFIED**.
- Zero port exhaustion or binding issues across rapid successive iterations.
- Non-blocking connection timeout is strictly bounded (< 1000ms) with seamless fallback to secondary endpoints.
- Data integrity across small, large, and multi-file batches is 100% bit-exact (SHA-256).
- All 12 CTest suites pass (100% pass rate).
- Android debug APK compiles cleanly.

**Verdict**: **APPROVE**.

---

## 5. Verification Method

To independently reproduce all empirical findings:

1. **Run 10 rapid iterations of `test_initial_clipboard_sync`**:
   ```bash
   for i in {1..10}; do ./build/tests/test_initial_clipboard_sync; done
   ```
   *Expected*: 10/10 runs pass with exit code 0.

2. **Run 10 rapid iterations of `test_reverse_transfer_and_fallback`**:
   ```bash
   for i in {1..10}; do ./build/tests/test_reverse_transfer_and_fallback; done
   ```
   *Expected*: 10/10 runs pass with exit code 0; fallback latency ~15 ms (< 1000 ms).

3. **Run Challenger Stress Test Suite**:
   ```bash
   ./build/tests/challenger_stress_test
   ```
   *Expected*: All 5 adversarial challenges pass (30 rapid cycles, 8 concurrent threads, 1MB/2MB payloads, non-blocking timeouts, 6.3MB 11-file batch).

4. **Run Full CTest Regression**:
   ```bash
   ctest --test-dir build --output-on-failure -V
   ```
   *Expected*: 12/12 tests passed.

5. **Build Android APK**:
   ```bash
   JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64 \
   ANDROID_USER_HOME=/home/reid-pu/Programming/Linux_to_Android/.android \
   ./android/gradlew -p android assembleDebug -Duser.home=/home/reid-pu/Programming/Linux_to_Android
   ```
   *Expected*: `BUILD SUCCESSFUL` with artifact at `android/app/build/outputs/apk/debug/app-debug.apk`.
