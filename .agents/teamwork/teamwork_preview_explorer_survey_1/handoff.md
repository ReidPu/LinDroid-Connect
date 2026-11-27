# Architectural Survey & Defect Investigation Report: C++ Core Library (`core/`), CMake & Tests

**Author**: `teamwork_preview_explorer_survey_1`  
**Date**: 2026-10-21  
**Project**: Linux to Android (`Linux_to_Android`)  
**Scope**: Read-only Architectural Survey & Defect Investigation across `core/`, `tests/`, and build configurations.

---

## 1. Observation

Direct code inspections, line references, and build baseline observations:

### 1.1 Baseline Build and Test Status
- Command executed: `cmake -B build -DL2A_BUILD_LINUX=ON && cmake --build build -j4 && ctest --test-dir build --output-on-failure -V`
- Result: 100% pass (2 tests: `test_protocol`, `test_loopback_transfer`).
- Note: Existing tests cover only happy-path sequential transfers in single threads.

### 1.2 Direct Code Observations by Dimension

#### A. Multithreading Concurrency & Deadlocks
1. **`core/src/file_transfer.cpp:235` and `core/src/engine.cpp:257`**:
   `Engine::channel_recv_loop` executes on `recv_thread_`. When a frame of type `FrameType::kFileResumeOffset` arrives, `file_mgr_->handle_frame(frame)` calls `stream_outgoing_files(batch_copy, *resume);` synchronously. Inside `stream_outgoing_files` (lines 358-398), the loop streams all file data chunks over the socket synchronously. During this time, `channel_recv_loop` is blocked from calling `recv_frame()`.
2. **`core/src/file_transfer.cpp:202, 287, 402-436`**:
   `FileTransferManager::handle_frame` acquires `std::lock_guard<std::mutex> lock(mutex_);`. Under this lock, line 287 invokes `finalize_incoming_file(chunk->batch_id, st);`. Inside `finalize_incoming_file`, it computes `sha256_file_range(st.part_path, 0, 0)` synchronously, calls `send_frame_fn_(ack_frame)` (which acquires `channel_mutex_` and `TlsChannel::send_mutex_`), and invokes user callback `on_file_completed_` while `mutex_` is still held.
3. **`core/src/engine.cpp:194-214, 275-277`**:
   In `Engine::channel_recv_loop`, on disconnect or recv failure, `listener_->on_connection_state_changed(false, pid, pname)` is invoked on `recv_thread_`. If the listener calls `engine.connect_to_peer()` or `connect_with_fd()`, it enters `attach_channel()`, which executes `if (recv_thread_.joinable()) recv_thread_.join();`. Joining `recv_thread_` from within itself causes `std::system_error` (Resource deadlock avoided). In addition, `attach_channel()` is not synchronized by any mutex.
4. **`core/src/tls_channel.cpp:535-541`**:
   `TlsChannel::close()` modifies `sock_fd_ = -1` without acquiring `send_mutex_` or `recv_mutex_`, and `sock_fd_` is not atomic (`int sock_fd_{-1};`). `write_all()` (line 546) and `read_exact()` (line 558) read `sock_fd_` concurrently without synchronization.
5. **`core/src/clipboard_stream.cpp:307-330`**:
   When `FrameType::kClipPullReq` arrives in `handle_frame`, `stream_file_now` or `stream_bytes_now` is called directly on `recv_thread_`.

#### B. Memory Lifecycle & Resource Leaks
1. **`core/src/clipboard_stream.cpp:234-235, 266, 297`**:
   For incoming clipboard items > 512KB, `stream.temp_file` is created on disk (`l2a_clip_<id>.tmp`). When chunk offset mismatches (line 266), when hash fails (line 297), or when transfer completes and is passed to the listener, the `.tmp` file is never removed from disk.
2. **`core/src/file_transfer.cpp:119, 209` and `core/src/clipboard_stream.cpp:54, 107`**:
   `outgoing_batches_[bid] = std::move(batch);` and `incoming_batches_[meta->batch_id] = std::move(ib);` insert entries that are never erased upon completion. `lazy_sources_` in `ClipboardStreamManager` also grows monotonically with every lazy clip offer.
3. **`core/src/file_transfer.cpp:402-419`**:
   When full-file SHA-256 verification fails in `finalize_incoming_file`, the corrupted `.l2apart` file is not deleted or truncated.
4. **`android/app/src/main/cpp/jni_bridge.cpp:108-115`**:
   `attach_env()` calls `g_jvm->AttachCurrentThread(&env, nullptr)` on native threads, but never invokes `DetachCurrentThread` when threads terminate.

#### C. Network Reconnection & Malformed Packet Tolerance
1. **`core/include/l2a/protocol.hpp:19-20`, `core/src/protocol.cpp:178`, `core/src/tls_channel.cpp:793`**:
   `kMaxFramePayload` is 66,560 bytes (`kMaxChunkSize + 1024`). `FileBatchMetaPayload::encode()` packs the entire folder tree metadata into a single frame. With 84 bytes per entry, any directory containing >750 files generates a payload > 66,560 bytes. `TlsChannel::recv_frame` checks `if (ct_len > kMaxFramePayload + kFrameHeaderSize + 16) return std::nullopt;` and immediately drops the connection.
2. **`core/src/engine.cpp:239-245`**:
   `Engine::channel_recv_loop` responds to `kPing` with `kPong`, but nowhere in `Engine`, `TlsChannel`, or discovery is `kPing` ever periodically transmitted. Connected sockets do not configure `SO_RCVTIMEO`, `SO_SNDTIMEO`, or `SO_KEEPALIVE`.
3. **`core/src/protocol.cpp:383, 422`**:
   In `FileBatchMetaPayload::decode` and `FileResumeOffsetPayload::decode`, `count` is read as a 32-bit integer and bounded only by `count > 100000`. `p.files.reserve(count)` is called before verifying if the input buffer has enough bytes to supply `count` items, allowing a tiny crafted packet to trigger an ~8.8MB memory allocation.
4. **`core/src/file_transfer.cpp:38-51`**:
   `sanitize_relative_path` only filters `.` and `..` components based on Linux forward slash separators. Windows backslashes (`\`) or symlinks within `receive_dir_` are not sanitized.

#### D. TLS 1.3 State Machine & Chunk Resume Boundaries
1. **`core/src/tls_channel.cpp:443-445, 492-494, 467-468`**:
   `DeviceIdentity` and `TrustedPeerStore` serialize using raw space-separated text lines (`out << peer.device_id << " " << to_hex(peer.public_key) << " " << peer.device_name << "\n";`). If `device_id` or `device_name` contains whitespace or newlines, `iss >> dev_id >> pub_hex` corrupts the parse, causing peer verification failure or persistent identity regeneration on restart.
2. **`core/src/tls_channel.cpp:746`**:
   `got_peer_fin != expected_peer_fin` uses `std::array::operator!=` which early-exits on the first mismatched byte, introducing a timing side-channel on TLS Finished MAC verification.
3. **`core/src/file_transfer.cpp:184-191, 342-348`**:
   Resume validation only tests `tail_block_sha256` (the final <=64KB block of partial file). If earlier blocks were damaged or truncated, resume assumes the prefix is valid and appends, leading to wasted bandwidth and late failure at final verification.

#### E. Build Setup & Compiler Flags
1. **`linux/CMakeLists.txt:21-31`**:
   Hardcodes `set(LINUX_LIB_DIR "/usr/lib/x86_64-linux-gnu")` and links directly to concrete `.so.0` files. This breaks ARM64 Linux, Fedora, Arch, and NixOS builds.
2. **`core/CMakeLists.txt:5-28`**:
   `l2a_core` lacks `set_target_properties(l2a_core PROPERTIES POSITION_INDEPENDENT_CODE ON)`. When linked into Android's shared library `libl2a_jni.so`, non-PIC code causes link errors on 64-bit architectures.
3. **Sanitizers**:
   No CMake configuration exists to enable `-fsanitize=address,undefined` or `-fsanitize=thread`.

---

## 2. Logic Chain

1. **Premise**: In network applications, read loops must never be blocked by write loops, because TCP sockets have finite kernel buffer space (typically 64KB-4MB).
   - *From Observation 1.2.A.1*: `stream_outgoing_files` runs synchronously inside `channel_recv_loop` on `recv_thread_`.
   - *Deduction*: When two connected devices both initiate a file batch transfer, both `recv_thread_`s become blocked writing file chunks. Because neither thread reads from the TCP socket, the TCP receive window shrinks to zero. Both ends block inside `::send()`.
   - *Conclusion*: A bidirectional file transfer will deadlock and freeze both clients permanently.

2. **Premise**: User callbacks and long I/O operations must never be executed while holding internal class mutexes.
   - *From Observation 1.2.A.2*: `finalize_incoming_file` holds `FileTransferManager::mutex_` while calculating full-file SHA-256 hashes, sending ACK frames over TLS, and executing `on_file_completed_`.
   - *Deduction*: Any call into `Engine` from within `on_file_completed_` deadlocks on `mutex_`. Furthermore, while a large file is being hashed, all other file transfer operations are completely blocked.
   - *Conclusion*: Invoking callbacks and hashing under `mutex_` creates critical deadlock and latency vulnerabilities.

3. **Premise**: Protocols with fixed maximum payload limits must ensure that control frames cannot exceed the limit under valid application inputs.
   - *From Observation 1.2.C.1*: `kMaxFramePayload` is 66,560 bytes. `FileBatchMetaPayload` places all directory items in a single message (~84 bytes per item).
   - *Deduction*: A folder with 800 files yields ~67KB, which exceeds `kMaxFramePayload`. The receiver discards the packet as malformed and terminates the connection.
   - *Conclusion*: Realistic folder transfers with >750 files reliably crash the network connection.

4. **Premise**: Temporary files created during streaming must have a defined deletion lifecycle.
   - *From Observation 1.2.B.1*: `ClipboardStreamManager` creates `.tmp` files in `temp_dir_` for payloads > 512KB, but has no deletion calls in failure paths or consumption paths.
   - *Conclusion*: Clipboard operations leak unbounded disk space over time.

---

## 3. Categorized Defect Findings

| ID | Module / File:Line | Severity | Category | Root Cause & Trigger Scenario | Concrete Fix Strategy |
|:---|:---|:---:|:---:|:---|:---|
| **DEF-01** | `core/src/file_transfer.cpp:235`<br>`core/src/engine.cpp:257` | **High** | Concurrency / Deadlock | `stream_outgoing_files` is called synchronously on `recv_thread_`. Simultaneous bidirectional file sending blocks both read loops, deadlocking in `::send()`. | Execute `stream_outgoing_files` on a detached worker thread or thread pool queue; keep `recv_thread_` solely reading packets. |
| **DEF-02** | `core/src/file_transfer.cpp:202, 287, 402` | **High** | Concurrency / Deadlock | `finalize_incoming_file` runs under `mutex_`, hashes multi-GB files, sends network ACKs, and invokes `on_file_completed_`. Re-entrant calls deadlock `mutex_`. | Extract file state, release `mutex_`, then perform hashing, ACK transmission, and callback invocation. |
| **DEF-03** | `core/src/engine.cpp:194, 276` | **High** | Concurrency / Lifecycle | Listener `on_connection_state_changed(false)` runs on `recv_thread_`. If listener tries to reconnect, `attach_channel()` calls `recv_thread_.join()` on itself. | Check `std::this_thread::get_id() != recv_thread_.get_id()` before joining, or join via an asynchronous lifecycle dispatcher. |
| **DEF-04** | `core/include/l2a/protocol.hpp:20`<br>`core/src/tls_channel.cpp:793` | **High** | Protocol / Network | Single `kFileBatchMeta` frame exceeds `kMaxFramePayload` (64KB+1KB) when folder has >750 files, causing immediate connection drop. | Paginate `FileBatchMetaPayload` into chunks or increase/decouple metadata frame size ceiling. |
| **DEF-05** | `core/src/clipboard_stream.cpp:234, 266, 297` | **High** | Resource Leak | Temporary clipboard files (`l2a_clip_*.tmp`) are never deleted on failure, completion, or startup. | Implement RAII cleanup in `ReceivedClipboardItem`, delete temp files on error, and sweep orphaned files on startup. |
| **DEF-06** | `core/src/file_transfer.cpp:119, 209`<br>`core/src/clipboard_stream.cpp:54` | **Medium** | Resource Leak | `outgoing_batches_`, `incoming_batches_`, and `lazy_sources_` maps are never erased or purged after batch completion. | Erase completed batches from maps once all files are verified; add LRU/TTL eviction to `lazy_sources_`. |
| **DEF-07** | `core/src/engine.cpp:133, 157, 239` | **Medium** | Network / Keepalive | No `kPing` frames are sent periodically, and TCP sockets have no `SO_RCVTIMEO` / `SO_KEEPALIVE`. Silent drops freeze connections. | Introduce a keepalive timer (send `kPing` every 5s, timeout after 15s) and enable socket keep-alive / timeouts. |
| **DEF-08** | `core/src/tls_channel.cpp:443, 492` | **Medium** | State / Serialization | Whitespace or newlines in device names/IDs break line-based parsing in `trusted_peers.tsv` and `identity.cfg`. | Escape or encode identifiers (e.g. URI encoding or length prefixing) and sanitize device names on receipt. |
| **DEF-09** | `core/src/tls_channel.cpp:535-541` | **Medium** | Concurrency / Data Race | `sock_fd_` is non-atomic and mutated in `close()` without mutex protection while read in `write_all`/`read_exact`. | Use `std::atomic<int>` for `sock_fd_` and synchronize socket shutdown/close lifecycle. |
| **DEF-10** | `core/src/protocol.cpp:383, 422` | **Medium** | Security / DoS | `count` up to 100,000 blindly calls `reserve()` without verifying input buffer length, risking memory exhaustion. | Validate `count <= remaining_bytes / min_entry_size` before memory reservation. |
| **DEF-11** | `core/src/file_transfer.cpp:402-419` | **Medium** | File Integrity | Corrupted `.l2apart` files that fail SHA-256 are retained, causing infinite failed resume loops on retry. | Delete or truncate the `.l2apart` file upon verification failure. |
| **DEF-12** | `android/app/src/main/cpp/jni_bridge.cpp:108` | **Medium** | Android JNI | Native threads attached via `AttachCurrentThread` are never detached before exit, causing ART thread leak/crash. | Use `pthread_key_create` with a destructor calling `DetachCurrentThread` on thread termination. |
| **DEF-13** | `linux/CMakeLists.txt:21-31` | **Low** | Build System | Hardcodes x86_64 Ubuntu paths (`/usr/lib/x86_64-linux-gnu`), breaking ARM64/Arch/Fedora builds. | Use `find_package(PkgConfig REQUIRED)` and `pkg_check_modules()`. |
| **DEF-14** | `core/CMakeLists.txt:5-28` | **Low** | Build System | Missing `POSITION_INDEPENDENT_CODE ON` on `l2a_core` and lacks ASan/TSan build flags. | Set `POSITION_INDEPENDENT_CODE ON` and add `-DL2A_ENABLE_ASAN` / `-DL2A_ENABLE_TSAN` options. |
| **DEF-15** | `core/src/tls_channel.cpp:746` | **Low** | Cryptography | Non-constant-time comparison for TLS Finished HMAC verification. | Replace with constant-time buffer comparison. |

---

## 4. Test Suite Analysis & Recommended Regression Test Cases

### 4.1 Current Test Suite Status
- `tests/test_protocol.cpp`: Verifies SHA-256, X25519 ECDH, ChaCha20-Poly1305, Frame serialization/deserialization, and 1-file Batch Meta payload.
- `tests/test_loopback_transfer.cpp`: Sequential test of TLS handshake, text clipboard + anti-loopback, 200KB image streaming, lazy-pull URI list, and 2-file folder transfer with pre-existing partial file.

### 4.2 Recommended Regression Test Suite
1. **`test_bidirectional_concurrency`**:
   Simultaneously initiate `send_files()` on both endpoints of an active loopback connection. Verify that both file transfers complete without deadlock or thread starvation.
2. **`test_large_folder_scaling`**:
   Transfer a directory containing 2,000 files (totaling > 150KB of metadata). Verify that `FileBatchMeta` and `FileResumeOffset` successfully transmit and do not drop the connection.
3. **`test_reentrant_callback_deadlock`**:
   Register an `ICoreListener` whose `on_file_completed` immediately invokes `engine.send_files()` or `engine.receive_dir()`. Verify that no recursive mutex deadlock occurs.
4. **`test_abrupt_disconnect_reconnect`**:
   Close socket mid-transfer (after 50% of chunks sent). Call `connect_to_peer` inside `on_connection_state_changed(false)` to ensure `recv_thread_` does not deadlock on self-join, and verify file resume succeeds on reconnection.
5. **`test_corrupted_partial_file_recovery`**:
   Create a corrupt `.l2apart` file (wrong prefix or corrupt tail). Verify that the system detects the mismatch, truncates/cleans the part file, and completes a clean transfer without getting stuck.
6. **`test_malformed_packet_fuzzing`**:
   Inject truncated payloads, frames with `payload_length` mismatch, invalid CRC16, and huge `count` values. Verify that decoders return `std::nullopt` gracefully without memory exhaustion or crash.
7. **`test_temp_file_cleanup`**:
   Stream a 1MB clipboard item and simulate an abort mid-stream. Check that no orphaned `.tmp` files remain in `temp_dir_`.
8. **`test_device_name_special_characters`**:
   Pair devices with names like `"Pixel 7 Pro (5G)\nAdmin"` and `"Test\tDevice"`. Verify `identity.cfg` and `trusted_peers.tsv` save and restore cleanly.

---

## 5. Suggested Milestones & Dependencies

```
[Milestone 1: Concurrency & Deadlock Remediation]
  ├── Decouple stream_outgoing_files & lazy pull from recv_thread_ (Worker Thread / Async)
  ├── Release FileTransferManager::mutex_ before hashing, sending ACKs, & calling listener
  └── Fix Engine::attach_channel self-join and serialize channel state transitions
        │
        ▼
[Milestone 2: Protocol Framing, Resource Leaks & Keepalive]
  ├── Support large batch metadata (>750 files) in protocol framing
  ├── Implement RAII temp file cleanup & batch map pruning
  ├── Add periodic kPing/kPong heartbeat timer & socket timeouts
  └── Fix .l2apart corruption recovery
        │
        ▼
[Milestone 3: Security, Platform & Build Hardening]
  ├── Sanitize device names & use robust TSV/JSON formatting
  ├── Harden Android JNI thread detachment (pthread_key)
  ├── Modernize CMake (pkg-config for GTK3/AppIndicator, -fPIC on core, ASan/TSan options)
  └── Constant-time Finished MAC comparison & input validation
        │
        ▼
[Milestone 4: Automated Regression Test Expansion]
  ├── Expand tests/ with bidirectional concurrent transfer test
  ├── Add large directory scaling test (2,000 files)
  ├── Add network drop & reconnect test
  └── Validate under ThreadSanitizer (TSan) & AddressSanitizer (ASan)
```

---

## 6. Caveats

- Android UI / Kotlin layer (`L2AService.kt`, `MainActivity.kt`) and Linux Wayland windowing specifics were inspected for API integration contracts, but dynamic GUI tests were not executed in this read-only C++ survey.
- Hardware-specific NFC NTAG5332 I2C communication relies on `/dev/i2c-*` which is mocked or simulated in software builds.

---

## 7. Conclusion

The C++ Core Library (`l2a_core`) provides an elegant and clean zero-dependency cryptographic implementation (RFC 7748 X25519, RFC 8439 ChaCha20-Poly1305, SHA-256) and binary protocol design. However, the architectural survey identified 5 High-severity defects and 7 Medium-severity defects centered around:
1. **Thread coupling**: File streaming directly inside the single network receive loop, causing bidirectional deadlocks.
2. **Lock inversion & re-entrancy**: Invoking file hashing, network I/O, and listener callbacks while holding internal mutexes.
3. **Framing limits**: Fixed 64KB frame boundaries dropping connections on large directories (>750 files).
4. **Resource leaks**: Lack of cleanup for temporary clipboard files and transfer batch maps.
5. **Connection liveness**: Inactive ping/pong heartbeats and missing socket timeouts.

Remediating these architectural defects according to the milestones above will establish robust stability, zero deadlocks, and leak-free cross-platform operations.

---

## 8. Verification Method

To independently verify the survey observations:
1. **Compile and execute baseline**:
   ```bash
   cmake -B build -DL2A_BUILD_LINUX=ON
   cmake --build build -j4
   ctest --test-dir build --output-on-failure -V
   ```
2. **Inspect code references**:
   - Trace `core/src/file_transfer.cpp:235` into `stream_outgoing_files` to verify synchronous execution on `channel_recv_loop`.
   - Trace `core/src/file_transfer.cpp:287` into `finalize_incoming_file` to observe lock contention on `mutex_`.
   - Inspect `core/include/l2a/protocol.hpp:20` and `core/src/tls_channel.cpp:793` to observe the `kMaxFramePayload` check versus large `FileBatchMetaPayload`.
   - Inspect `core/src/clipboard_stream.cpp:234` for absence of `std::filesystem::remove` on failure/completion paths.
