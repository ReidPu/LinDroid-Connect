# Milestone 1 Remediation Handoff Report (Iteration 2)

**Worker**: `teamwork_preview_worker_m1_gen2`  
**Milestone**: Milestone 1 — Core Networking & Transfer Engine Remediation  
**Target Verdict**: PASS / READY FOR VERIFICATION  
**Sanitizer Status**: Clean under Standard, ThreadSanitizer (TSan), and AddressSanitizer (ASan/UBSan)  
**Exclusive File Write Scope**: `core/` only (`engine.hpp`, `engine.cpp`, `file_transfer.cpp`, `tls_channel.cpp`)  

---

## 1. Observation

### 1.1 Defect 1: ThreadSanitizer Data Race on `recv_thread_` in `Engine::attach_channel` vs `Engine::stop()`
- **Reported in**: `teamwork_preview_challenger_m1_1/handoff.md:47-60`.
- **Verbatim Error**:
  ```
  WARNING: ThreadSanitizer: data race (pid=35)
    Read of size 8 at 0x7fffffffcd68 by main thread:
      #0 std::thread::get_id() const in l2a::Engine::stop() (core/src/engine.cpp:140)
      #1 l2a::Engine::stop() (core/src/engine.cpp:139)
    Previous write of size 8 at 0x7fffffffcd68 by thread T24:
      #0 std::thread::operator=(std::thread&&)
      #1 l2a::Engine::attach_channel(std::shared_ptr<l2a::TlsChannel>) (core/src/engine.cpp:285)
      #2 l2a::Engine::connect_with_fd(int, bool) (core/src/engine.cpp:258)
  ```
- **Code Locations**: `core/include/l2a/engine.hpp:125`, `core/src/engine.cpp:139-145`, `core/src/engine.cpp:264-285`.
- **Additional Data Race Found During TSan Execution**:
  ```
  WARNING: ThreadSanitizer: data race (pid=137)
    Write of size 4 at 0x7fffffffcc20 by main thread:
      #0 l2a::Engine::stop() in core/src/engine.cpp
    Previous read of size 4 at 0x7fffffffcc20 by thread T3:
      #0 l2a::Engine::accept_loop() in core/src/engine.cpp
  ```
  `listen_sock_` was declared as plain `int` (`core/include/l2a/engine.hpp:116`) and modified concurrently with `::accept(listen_sock_, ...)`.

### 1.2 Defect 2: Pre-existing 100% Verified Files Never Dispatched ACK
- **Reported in**: `teamwork_preview_reviewer_m1_2/handoff.md:33-58` and `teamwork_preview_challenger_m1_1/handoff.md:44-45`.
- **Observed Behavior**:
  - In `core/src/file_transfer.cpp:263-271`, when destination file already existed on disk with matching SHA-256, receiver marked `st.completed = true` and `r_entry.resume_offset = fmeta.file_size`.
  - Lines 294-297 only appended zero-byte files (`fmeta.file_size == 0 && !st.completed`) to `zero_len_files`.
  - Sender saw `rentry.resume_offset >= ofile.meta.file_size` (`core/src/file_transfer.cpp:497-499`) and skipped sending data chunks.
  - Receiver never executed `finalize_incoming_file()`, never sent `kFileVerifiedAck`, receiver and sender never triggered `on_file_completed()`, and the batch remained permanently in memory.

### 1.3 Defect 3: Unbounded Frame Payload Risk in Metadata Slicing
- **Reported in**: `teamwork_preview_reviewer_m1_2/handoff.md:19-30`.
- **Observed Behavior**:
  - `core/src/file_transfer.cpp:165-176` used a static count ceiling `kMaxMetaFilesPerFrame = 400`.
  - Each metadata entry encodes to $54 + \text{path\_len}$ bytes with 24-byte frame header overhead.
  - If average relative path length exceeded 112 bytes, serialized frame size exceeded `kMaxFramePayload` (66,560 bytes), causing `TlsChannel::recv_frame` or `FrameHeader::decode` to reject the packet as malformed and drop the connection.

### 1.4 Defect 4: Teardown Order Inversion in `Engine::stop()`
- **Reported in**: `teamwork_preview_reviewer_m1_2/handoff.md:61-66` and `teamwork_preview_challenger_m1_1/handoff.md:61-63`.
- **Observed Behavior**:
  - In `core/src/engine.cpp:111-130`, `file_mgr_->stop()` and `heartbeat_thread_.join()` were invoked before `disconnect_active_peer()`.
  - If a worker or heartbeat thread was blocked on socket I/O with an unresponsive peer, it could block up to 15 seconds (`SO_SNDTIMEO`) before terminating.

### 1.5 Defect 5: CRLF Line Endings in TSV and Identity Config Parsing
- **Reported in**: `teamwork_preview_reviewer_m1_2/handoff.md:69-74`.
- **Observed Behavior**:
  - In `core/src/tls_channel.cpp:504-520`, `std::getline(in, line)` split lines by `\n`. On CRLF-formatted inputs, lines retained a trailing `\r`, which was preserved in `dev_name`, and in `identity.cfg` caused `from_hex(priv_hex)` to fail.

---

## 2. Logic Chain

### 2.1 Resolution of Thread Races & Teardown Order (Defects 1 & 4)
1. **Synchronization of `recv_thread_`**:
   - Introduced dedicated `std::mutex recv_thread_mutex_` in `core/include/l2a/engine.hpp`.
   - In `Engine::attach_channel`: acquired `std::unique_lock<std::mutex> lock(recv_thread_mutex_)` around joining or detaching the prior `recv_thread_` and assigning the new `recv_thread_ = std::thread(&Engine::channel_recv_loop, this, channel)`.
   - In `Engine::stop`: wrapped joining/detaching of `recv_thread_` in `std::lock_guard<std::mutex> lock(recv_thread_mutex_)`.
   - Preserved self-join detection (`std::this_thread::get_id() == recv_thread_.get_id() -> detach()`).
2. **Synchronization of `listen_sock_`**:
   - Changed `listen_sock_` from `int` to `std::atomic<int> listen_sock_{-1}`.
   - In `Engine::accept_loop()`: loaded `listen_sock_.load()` into local `lsock` and checked `lsock < 0` before calling `::accept()`.
   - In `Engine::stop()`: performed `::shutdown(lsock, SHUT_RDWR)` first to unblock `::accept()`, joined `accept_thread_`, and then performed `listen_sock_.exchange(-1)` and `::close()`.
3. **Shutdown Ordering in `Engine::stop()`**:
   - Placed `disconnect_active_peer()` immediately after `if (!running_.exchange(false)) return;`.
   - `disconnect_active_peer()` calls `ch->close()` which issues `::shutdown(fd, SHUT_RDWR)`, instantaneously unblocking any thread blocked on socket `send()` or `recv()`.
   - Consequently, `file_mgr_->stop()`, `heartbeat_thread_.join()`, `accept_thread_.join()`, and `recv_thread_.join()` all terminate with 0 ms blocking latency.

### 2.2 Resolution of Pre-existing File Completion (Defect 2)
1. Replaced `zero_len_files` with `immediate_finalize_files` vector in `FileTransferManager::handle_frame(kFileBatchMeta)`.
2. When receiver verifies that `final_path` exists with matching size and matching SHA-256 (`existing_hash && *existing_hash == fmeta.file_sha256`), receiver marks `st.completed = true`, sets `r_entry.resume_offset = fmeta.file_size`, and appends the file to `immediate_finalize_files`. Zero-byte files (`fmeta.file_size == 0`) are likewise appended to `immediate_finalize_files`.
3. After releasing `mutex_`, if `all_meta_received` is true, receiver transmits `kFileResumeOffset` frames first, followed by invoking `finalize_incoming_file()` for all entries in `immediate_finalize_files`.
4. In `FileTransferManager::finalize_incoming_file()`:
   - Added check: if `final_path` already exists and its SHA-256 matches `expected_hash`, mark `ok = true` and remove any leftover `.l2apart` file.
   - If not already complete, verify `part_path` and rename to `final_path`.
   - Transmit `kFileVerifiedAck` to sender.
   - Update `incoming_batches_` state: if all files are complete and all metadata has arrived, prune batch from `incoming_batches_`.
   - Fire `on_file_completed` listener callback on receiver.
5. On the sender side, when `kFileVerifiedAck` arrives, sender records ack in `acked_files`, fires `on_file_completed` on sender, and when all files in the batch are acked, cleanly prunes the batch from `outgoing_batches_`.

### 2.3 Resolution of Metadata Slicing Byte Budget (Defect 3)
1. In `FileTransferManager::send_paths`:
   - Configured `kMaxPayloadBytesPerSlice = 48000`, `kMaxFilesPerSlice = 128`, `kBaseHeaderBytes = 24`, and `kEntryFixedBytes = 54`.
   - Iterated over files, dynamically tracking `current_payload_bytes += (54 + meta.relative_path.size())`.
   - Sliced into a new frame whenever `current_payload_bytes + entry_bytes > 48000` or file count reached 128 (while ensuring at least 1 file is added per slice).
   - Set `is_last = (offset + count >= all_files.size())` and applied `FrameFlags::kFin` on the final frame.
2. Guaranteed frame payload length mathematically cannot exceed $48000 + 54 + \text{single\_path\_len} \approx 52,150 \ll 66,560$ bytes (`kMaxFramePayload`).

### 2.4 Resolution of CRLF Parsing (Defect 5)
1. In `TrustedPeerStore::load()` (`core/src/tls_channel.cpp`):
   - Added `if (!line.empty() && line.back() == '\r') line.pop_back();` right after `std::getline(in, line)`.
   - Added trailing `\r` removal when parsing space-separated fallback lines.
2. In `DeviceIdentity::load_or_create()` (`core/src/tls_channel.cpp`):
   - Stripped trailing `\r` from `raw_id`, `raw_name`, and `priv_hex` before unescaping and hex decoding.

---

## 3. Caveats

- **Scope Boundary**: Files in `linux/` (Desktop UI/Wayland/ADB/NFC) and `android/` (JNI/Kotlin/Services) were not modified, in strict accordance with the exclusive file write ownership rules for Milestone 1.
- **Test File Location**: Empirical verification harnesses were built and executed in `build/` (`challenger_stress.cpp` and `remediation_verify.cpp`). No source or test code was placed in `.agents/teamwork/`.

---

## 4. Conclusion

All 5 defects identified by Reviewer 2 and Challenger 1 have been completely remediated in `core/`:
- **Defect 1 & 4**: ThreadSanitizer data races eliminated via `recv_thread_mutex_`, `std::atomic<int> listen_sock_`, and immediate `disconnect_active_peer()` socket shutdown.
- **Defect 2**: Pre-existing 100% verified files and 0-byte files reliably dispatch `kFileVerifiedAck`, invoke `on_file_completed` on both peers, and prune batch tables without hanging.
- **Defect 3**: Dynamic metadata byte budgeting guarantees frame payloads never exceed `kMaxFramePayload` even under deeply nested file directory structures.
- **Defect 5**: TSV and identity configuration parsers cleanly strip CRLF line endings.

All automated unit, integration, and stress tests pass 100% with zero warnings, zero data races, and zero memory leaks under Standard, ThreadSanitizer, and AddressSanitizer builds.

---

## 5. Verification Method

To independently reproduce and verify all builds and test suites:

### 5.1 Standard Build & CTest
```bash
cmake -B build -DL2A_BUILD_LINUX=ON
cmake --build build -j4
ctest --test-dir build --output-on-failure -V
```
**Observed Result**: 2/2 tests passed (0.11s real time), 0 failures.

### 5.2 ThreadSanitizer (TSan) Build & CTest
```bash
cmake -B build-tsan -DL2A_BUILD_LINUX=ON -DL2A_ENABLE_TSAN=ON
cmake --build build-tsan -j4
ctest --test-dir build-tsan --output-on-failure -V
```
**Observed Result**: 2/2 tests passed (0.52s real time), 0 data races, 0 warnings.

### 5.3 AddressSanitizer (ASan / UBSan) Build & CTest
```bash
cmake -B build-asan -DL2A_BUILD_LINUX=ON -DL2A_ENABLE_ASAN=ON
cmake --build build-asan -j4
ctest --test-dir build-asan --output-on-failure -V
```
**Observed Result**: 2/2 tests passed (0.30s real time), 0 memory leaks, 0 heap errors.

### 5.4 Challenger Stress Harness (TSan & ASan)
```bash
# TSan execution
g++ -std=c++20 -O2 -fsanitize=thread -Icore/include build/challenger_stress.cpp build-tsan/core/libl2a_core.a -lpthread -o build-tsan/challenger_stress
./build-tsan/challenger_stress

# ASan execution
g++ -std=c++20 -O2 -fsanitize=address,undefined -Icore/include build/challenger_stress.cpp build-asan/core/libl2a_core.a -lpthread -o build-asan/challenger_stress
./build-asan/challenger_stress
```
**Observed Result**: `ALL ADVERSARIAL CHALLENGER SUITES PASSED!` (Exit code 0, 0 warnings).

### 5.5 Remediation Verification Harness (TSan & ASan)
```bash
# TSan execution
g++ -std=c++20 -O2 -fsanitize=thread -Icore/include build/remediation_verify.cpp build-tsan/core/libl2a_core.a -lpthread -o build-tsan/remediation_verify
./build-tsan/remediation_verify

# ASan execution
g++ -std=c++20 -O2 -fsanitize=address,undefined -Icore/include build/remediation_verify.cpp build-asan/core/libl2a_core.a -lpthread -o build-asan/remediation_verify
./build-asan/remediation_verify
```
**Observed Result**: `ALL REMEDIATION EMPIRICAL TESTS PASSED!` (Validates 300 deep files with >120-byte paths, 100% pre-existing file transfers, and CRLF config parsing).
