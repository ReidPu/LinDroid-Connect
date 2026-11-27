# Milestone 1 Adversarial Review Report (Reviewer & Critic)

**Review Target**: Milestone 1 Core C++ Library Remediation (`core/`)  
**Reviewer Role**: `teamwork_preview_reviewer_m1_2` (Reviewer & Adversarial Critic)  
**Target Verdict**: `REQUEST_CHANGES`  
**Integrity Status**: CLEAN (No hardcoded facades, bypasses, or fabricated tests detected)

---

## 1. Review Summary

**Verdict**: **`REQUEST_CHANGES`**  
While the M1 implementation provides real cryptographic primitives, eliminates previous deadlocks, passes ThreadSanitizer and AddressSanitizer with zero runtime warnings on the baseline test suite, and exhibits no integrity violations, in-depth adversarial stress analysis identified **two Major defects** in `core/src/file_transfer.cpp` that cause protocol packet drops and indefinite transfer hangs in real-world synchronization scenarios.

---

## 2. Findings & Adversarial Challenges

### [Major] Finding 1: Unbounded Frame Payload Risk in Metadata Slicing (CORE-04 Edge Case)
- **What**: `FileTransferManager::send_paths` chunks batch metadata using a fixed count of 400 files without checking the cumulative encoded byte length.
- **Where**: `core/src/file_transfer.cpp:165-188`, `core/src/protocol.cpp:179-181`, `core/src/tls_channel.cpp:863-865`.
- **Why**: 
  - Each `FileEntryMeta` has a minimum fixed wire size of 54 bytes plus `relative_path.size()` (2-byte length prefix + string bytes).
  - Formula: $\text{Payload Size} = 24 + \sum_{i=1}^{count} (54 + \text{path\_len}_i)$.
  - For 400 files, the fixed overhead is $24 + 400 \times 54 = 21,624$ bytes.
  - The maximum allowable payload is `kMaxFramePayload = 66,560` bytes.
  - If the average relative path length exceeds $\frac{66,560 - 21,624}{400} = 112.34$ bytes (common in deep Android storage structures, e.g. `Android/data/com.example.app/...`, or nested Git repositories), the serialized payload exceeds 66,560 bytes.
  - When `FrameHeader::decode` or `TlsChannel::recv_frame` inspects `payload_length > kMaxFramePayload`, it returns `std::nullopt` and treats the packet as malformed, immediately dropping the connection.
- **Suggestion**: Slice by byte budget rather than item count alone. Dynamically accumulate entries in `slice_files` until the estimated or encoded size approaches a safe threshold (e.g., 48,000 bytes or `kMaxChunkSize`), or reduce the count ceiling to a conservative number (e.g. 150-200) coupled with dynamic byte checking.

---

### [Major] Finding 2: Pre-existing / 100% Completed Files Never Receive ACK, Causing Batch Completion Hang
- **What**: When a file is already 100% transferred on the receiver before the batch begins, the receiver never triggers `finalize_incoming_file`, and the sender never receives `kFileVerifiedAck`, causing `outgoing_batches_` to never complete and callbacks to never fire.
- **Where**: `core/src/file_transfer.cpp:263-271`, `core/src/file_transfer.cpp:294-297`, `core/src/file_transfer.cpp:497-499`.
- **Why**:
  1. On receiving `kFileBatchMeta`, the receiver checks if `final_path` exists with matching size and matching SHA-256 (`core/src/file_transfer.cpp:263-271`). If so, it sets `st.completed = true` and `r_entry.resume_offset = fmeta.file_size`.
  2. Notice that `zero_len_files.push_back(...)` (`line 296`) is guarded by `fmeta.file_size == 0 && !st.completed`. Thus, existing non-zero files are NOT placed in `zero_len_files`.
  3. The receiver sends `kFileResumeOffset` indicating `resume_offset = file_size`.
  4. On the sender (`core/src/file_transfer.cpp:497-499`):
     ```cpp
     if (rentry.resume_offset >= ofile.meta.file_size && ofile.meta.file_size > 0) {
         continue;  // Receiver already has verified full file
     }
     ```
     The sender sends ZERO chunks for this file.
  5. Because no chunks are sent, the receiver's `handle_frame(kFileDataChunk)` is NEVER called for this file, so `finalize_incoming_file` is NEVER called.
  6. Because `finalize_incoming_file` is never called, the receiver NEVER sends `kFileVerifiedAck` to the sender.
  7. On the sender (`core/src/file_transfer.cpp:465-468`):
     ```cpp
     bit->second.acked_files.insert(ack->file_index);
     if (bit->second.acked_files.size() >= bit->second.files.size()) {
         outgoing_batches_.erase(bit);
     }
     ```
     The sender never receives the ACK for this file. As a result, `acked_files.size()` never reaches `files.size()`, the batch remains permanently in `outgoing_batches_`, and `on_file_completed` is never called for that file on either the sender or receiver!
- **Suggestion**: In `core/src/file_transfer.cpp`, when a file is detected as already complete (`existing_hash && *existing_hash == fmeta.file_sha256`), push it to an `immediate_finalize_files` list (along with 0-byte files), so that `finalize_incoming_file` is called to dispatch `kFileVerifiedAck` and notify both peers immediately.

---

### [Minor] Finding 3: Engine Shutdown Ordering Latency Risk
- **What**: In `Engine::stop()`, `heartbeat_thread_.join()` is called before `disconnect_active_peer()`.
- **Where**: `core/src/engine.cpp:115-130`.
- **Why**: If `heartbeat_thread_` happens to be inside `send_frame_active(ping)` when `stop()` is invoked, and the underlying TCP connection is unresponsive, `::send()` may block up to 15 seconds (the socket timeout `SO_SNDTIMEO`). Calling `disconnect_active_peer()` first triggers `::shutdown(fd, SHUT_RDWR)` on the socket, which immediately aborts any blocking socket calls, allowing `heartbeat_thread_` to exit in 0 ms.
- **Suggestion**: Reorder `Engine::stop()` to call `disconnect_active_peer()` before joining `heartbeat_thread_`.

---

### [Minor] Finding 4: CRLF Line Endings in Trusted Peer TSV Loading
- **What**: `TrustedPeerStore::load()` does not strip carriage return characters (`\r`) when reading lines.
- **Where**: `core/src/tls_channel.cpp:504-513`.
- **Why**: If `trusted_peers.tsv` or `identity.cfg` is transferred from Windows or edited with tools that output CRLF (`\r\n`), `std::getline(in, line)` splits at `\n`, leaving a trailing `\r` on `line`. In tab-separated parsing, `dev_name = unescape_tsv_field(line.substr(sep2 + 1))` preserves this trailing `\r`, which is not an escaped `\\r`.
- **Suggestion**: Add `if (!line.empty() && line.back() == '\r') line.pop_back();` right after `std::getline(in, line)`.

---

## 3. Scrutiny Checklist & Verified Claims

| # | Check Item | Verification Method | Status | Notes |
|:---|:---|:---|:---:|:---|
| 1 | **TlsChannel socket teardown** (`shutdown` + `scoped_lock`) | Source review (`core/src/tls_channel.cpp:587-599`) & TSan test execution | **PASS** | `::shutdown(fd, SHUT_RDWR)` safely unblocks blocking `recv()`/`send()`. `scoped_lock(recv_mutex_, send_mutex_)` protects `exchange(-1)` and `::close(fd)`. No data races observed under TSan. |
| 2 | **Heartbeat ping thread lifecycle** | Source review (`core/src/engine.cpp:81-83, 115-122, 148-165`) | **PASS** | Uses `heartbeat_cv_.wait_for` with `!running_.load()` predicate. Cleanly wakes on `stop()` and terminates. Re-entrant on restarts. (Minor shutdown ordering suggestion noted in Finding 3). |
| 3 | **Constant-time HMAC comparison** (CORE-12) | Source review (`core/src/tls_channel.cpp:812-818`) | **PASS** | 32-byte XOR accumulator (`fin_diff \|= got ^ exp`) without early-exit branch. Constant-time AEAD Poly1305 verification (`core/src/tls_channel.cpp:396-402`) also verified. |
| 4 | **TSV escaping & unescaping** (CORE-08) | Source review (`core/src/tls_channel.cpp:413-446, 503-530`) | **PASS** | Properly escapes `\\`, `\n`, `\r`, `\t`. Correctly handles trailing backslashes and special characters without buffer overruns. Legacy space format fallback preserved. |
| 5 | **Slicing logic for multi-frame metadata & resume** | Source review (`core/src/file_transfer.cpp:165-201, 318-376`) | **FAIL** | Fixed count of 400 entries in `send_paths` exceeds `kMaxFramePayload` when relative paths > 112 bytes (Finding 1). Resuming 100% pre-existing files drops ACKs (Finding 2). |
| 6 | **Integrity Violation Check** | Automated & manual forensic inspection of tests and source code | **PASS** | Zero hardcoded test vector cheats, zero fake mocks, real cryptographic implementations (X25519, ChaCha20, Poly1305, SHA-256). |
| 7 | **ThreadSanitizer (TSan) Cleanliness** | `cmake -B build-tsan -DL2A_ENABLE_TSAN=ON && ctest` | **PASS** | 2/2 tests passed (1.22s real), 0 data races, 0 deadlocks. |
| 8 | **AddressSanitizer (ASan) Cleanliness** | `cmake -B build-asan -DL2A_ENABLE_ASAN=ON && ctest` | **PASS** | 2/2 tests passed (0.38s real), 0 memory leaks, 0 heap buffer overflows. |

---

## 4. Adversarial Attack Surface & Stress Test Results

- **Attack Scenario 1: Abrupt Socket Teardown During Frame Exchange**
  - *Method*: Simultaneous socket close from peer while receive loop is blocked.
  - *Result*: POSIX `shutdown` causes `recv()` to return 0/EOF. Mutexes prevent double-close. TSan reports zero data race. (PASSED)
- **Attack Scenario 2: Deep Directory Metadata Overflow DoS**
  - *Method*: Transfer a project containing 400 nested files with relative paths of 120 characters each.
  - *Result*: Encoded metadata frame payload size = $24 + 400 \times (54 + 120) = 69,624$ bytes. This exceeds `kMaxFramePayload` (66,560 bytes). Peer's `recv_frame()` rejects the frame as malformed and terminates the connection. (FAILED - see Finding 1)
- **Attack Scenario 3: Pre-existing File Synchronization / Re-transfer**
  - *Method*: Re-transfer an already synchronized folder where files exist on the receiver.
  - *Result*: Receiver validates full file and skips chunk requests. Sender sends 0 chunks. Receiver never sends `kFileVerifiedAck`. Sender hangs waiting for ACK, keeping batch alive indefinitely. (FAILED - see Finding 2)

---

## 5. Logic Chain

1. **Observation 1.1**: Lines 165-176 of `core/src/file_transfer.cpp` define `kMaxMetaFilesPerFrame = 400` and slice `slice_files` up to 400 items. Lines 179-181 of `core/src/protocol.cpp` and lines 863-865 of `core/src/tls_channel.cpp` reject any frame with payload greater than `kMaxFramePayload` (66,560 bytes).
2. **Logic 1.2**: If relative path lengths exceed 112 bytes, $400 \times (54 + 113) + 24 = 66,824 > 66,560$. Any realistic directory structure with long file names will break transmission.
3. **Observation 2.1**: Lines 263-271 of `core/src/file_transfer.cpp` mark `st.completed = true` when an existing file on the receiver matches the SHA-256 hash. Lines 294-297 only push zero-length files to `zero_len_files`. Lines 497-499 skip chunk transmission when `rentry.resume_offset >= ofile.meta.file_size`.
4. **Logic 2.2**: Because `zero_len_files` excludes non-zero pre-existing files, and because sender transmits no chunks for them, no code path on the receiver ever calls `finalize_incoming_file(fin_bid, fin_fidx, ...)`. Thus, `kFileVerifiedAck` is never transmitted to the sender. Lines 465-468 on the sender will never receive the ACK, preventing batch pruning and `on_file_completed` invocation.
5. **Conclusion**: While all baseline requirements and sanitizer validations are met without integrity violations, Findings 1 and 2 represent major regression and stability risks in the newly introduced metadata pagination and resumption logic. A verdict of `REQUEST_CHANGES` is necessary to ensure these edge cases are rectified before proceeding to subsequent milestones.

---

## 6. Caveats

- Tests in `tests/test_loopback_transfer.cpp` only test partial resumption (128 KB of a 320 KB file), which masked Finding 2. Adding a test case for 100% pre-existing file transfer is strongly advised in Milestone 4.
- Out of scope for this review: `linux/` and `android/` modules, which are scheduled for Milestones 2 and 3.

---

## 7. Verification Method

To independently verify the test runs and findings:
1. **ThreadSanitizer execution**:
   ```bash
   cmake -B build-tsan -DL2A_BUILD_LINUX=ON -DL2A_ENABLE_TSAN=ON
   cmake --build build-tsan -j4
   ctest --test-dir build-tsan --output-on-failure -V
   ```
2. **AddressSanitizer execution**:
   ```bash
   cmake -B build-asan -DL2A_BUILD_LINUX=ON -DL2A_ENABLE_ASAN=ON
   cmake --build build-asan -j4
   ctest --test-dir build-asan --output-on-failure -V
   ```
3. **Inspect Finding 1**: Calculate payload size in `core/src/file_transfer.cpp:165-201` for 400 entries with 120-byte paths against `kMaxFramePayload` in `core/include/l2a/protocol.hpp:20`.
4. **Inspect Finding 2**: Trace `core/src/file_transfer.cpp:263-271` through `lines 294-297`, `lines 446`, and `lines 497-499` to observe that non-zero completed files never reach `finalize_incoming_file`.
