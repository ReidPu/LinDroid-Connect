# Milestone 1 Challenger Evaluation Report: Adversarial Verification

**Evaluator**: teamwork_preview_challenger_m1_1 (Empirical Challenger)  
**Target Milestone**: Milestone 1 (Core C++ Remediation)  
**Final Verdict**: `REQUEST_CHANGES`

---

## 1. Observation

### 1.1 Baseline Build and Test Suite
We executed the project build and test suite across standard, AddressSanitizer (ASan), and ThreadSanitizer (TSan) configurations:
1. **Standard Build**:
   ```bash
   cmake -B build -DL2A_BUILD_LINUX=ON && cmake --build build -j4 && ctest --test-dir build --output-on-failure -V
   ```
   Result: `100% tests passed, 0 tests failed out of 2` (0.13 sec).
2. **ASan Build**:
   ```bash
   cmake -B build-asan -DL2A_BUILD_LINUX=ON -DL2A_ENABLE_ASAN=ON && cmake --build build-asan -j4 && ctest --test-dir build-asan --output-on-failure -V
   ```
   Result: `100% tests passed, 0 tests failed out of 2` (0.30 sec).
3. **TSan Build**:
   ```bash
   cmake -B build-tsan -DL2A_BUILD_LINUX=ON -DL2A_ENABLE_TSAN=ON && cmake --build build-tsan -j4 && ctest --test-dir build-tsan --output-on-failure -V
   ```
   Result: `100% tests passed, 0 tests failed out of 2` (0.47 sec).

While the baseline suite (`test_protocol` and `test_loopback_transfer`) passes, it executes linear, single-direction transfer operations without stress, concurrency, or re-connection edge cases.

---

### 1.2 Adversarial Stress Verification Results

We constructed and executed an empirical stress harness (`build/challenger_stress.cpp`) targeting the three core focus areas:

#### Focus Area 1: Concurrency and Thread Safety of `stream_outgoing_files`
- **Bidirectional Transfers**: Simultaneous bidirectional file streaming succeeded cleanly. Asynchronous worker thread decoupling (`stream_queue_`) successfully avoided deadlocking `recv_thread_`.
- **Abrupt Disconnection Mid-Stream**: When a channel is closed mid-stream, `stream_outgoing_files` detects `send_frame_fn_` failure and returns cleanly without deadlocking.

#### Focus Area 2: Releasing `mutex_` in `handle_file_data_chunk`
- **Re-entrant Callback Deadlock Defense**: Calling `send_files()` or `push_clipboard_text()` from inside `ICoreListener::on_file_completed` executed without deadlock because `mutex_` is released before callback invocation.
- **Corrupted Block Handling**: When `.l2apart` is corrupted and tail block hash mismatches, the sender falls back to offset 0, and the receiver truncates the `.l2apart` file upon receiving offset 0.
- **Defect Discovered — Existing File Transfer Hang**: When sending a file that already exists on the receiver with matching SHA-256 (`core/src/file_transfer.cpp:261-271`), the receiver sets `resume_offset = fmeta.file_size` and the sender skips chunk transmission (`core/src/file_transfer.cpp:497-499`). However, `finalize_incoming_file` is never invoked, `kFileVerifiedAck` is never transmitted, `on_file_completed` is never triggered on either side, and the batch is never purged from `incoming_batches_` or `outgoing_batches_`.

#### Focus Area 3: `Engine::attach_channel` and Thread Cleanup
- **Defect Discovered — TSan Data Race on `recv_thread_`**:
  Under ThreadSanitizer (`./build-tsan/challenger_stress`), a data race was confirmed on `std::thread recv_thread_` when `attach_channel` is invoked during reconnection while another thread calls `Engine::stop()`:
  ```
  WARNING: ThreadSanitizer: data race (pid=35)
    Read of size 8 at 0x7fffffffcd68 by main thread:
      #0 std::thread::get_id() const in l2a::Engine::stop() (core/src/engine.cpp:140)
      #1 l2a::Engine::stop() (core/src/engine.cpp:139)
    Previous write of size 8 at 0x7fffffffcd68 by thread T24:
      #0 std::thread::operator=(std::thread&&)
      #1 l2a::Engine::attach_channel(std::shared_ptr<l2a::TlsChannel>) (core/src/engine.cpp:285)
      #2 l2a::Engine::connect_with_fd(int, bool) (core/src/engine.cpp:258)
  SUMMARY: ThreadSanitizer: data race in std::thread::get_id() const
  ThreadSanitizer: reported 3 warnings (exit code 66)
  ```
- **Defect Discovered — Teardown Inversion in `Engine::stop()`**:
  In `core/src/engine.cpp:111-130`, `file_mgr_->stop()` is invoked before `disconnect_active_peer()`. If the worker thread is blocked on a full TCP send buffer to a non-responsive peer, it waits up to the 15-second `SO_SNDTIMEO` before exiting.

---

## 2. Logic Chain

1. **Root Cause Analysis of Existing File Completion Hang**:
   - `core/src/file_transfer.cpp:263-271`:
     ```cpp
     if (std::filesystem::exists(st.final_path, ec) &&
         std::filesystem::file_size(st.final_path, ec) == fmeta.file_size) {
         auto existing_hash = sha256_file_range(st.final_path, 0, 0);
         if (existing_hash && *existing_hash == fmeta.file_sha256) {
             st.current_offset = fmeta.file_size;
             st.completed = true;
             r_entry.resume_offset = fmeta.file_size;
         }
     }
     ```
   - When `st.completed` is true from the start, the receiver sends `r_entry.resume_offset = fmeta.file_size`.
   - `core/src/file_transfer.cpp:497-499`:
     The sender sees `rentry.resume_offset >= ofile.meta.file_size` and continues to the next file without sending any `kFileDataChunk` frames.
   - `core/src/file_transfer.cpp:430-438`:
     `finalize_incoming_file` is only triggered upon receiving a data chunk where `st.current_offset >= st.meta.file_size`.
   - Because no chunk is ever received, `finalize_incoming_file` is never executed.
   - Consequence:
     1. Receiver never sends `kFileVerifiedAck`.
     2. Receiver never invokes `on_file_completed`.
     3. Sender never receives `kFileVerifiedAck`, leaving the batch in `outgoing_batches_` indefinitely.
     4. Sender never invokes `on_file_completed`.
     5. Both ends hang indefinitely waiting for completion.

2. **Root Cause Analysis of `recv_thread_` Data Race**:
   - `Engine::attach_channel` (`core/src/engine.cpp:264-285`) executes:
     ```cpp
     if (recv_thread_.joinable()) {
         if (std::this_thread::get_id() == recv_thread_.get_id()) {
             recv_thread_.detach();
         } else {
             recv_thread_.join();
         }
     }
     ...
     recv_thread_ = std::thread(&Engine::channel_recv_loop, this, std::move(channel));
     ```
   - `Engine::stop` (`core/src/engine.cpp:139-145`) executes:
     ```cpp
     if (recv_thread_.joinable()) {
         if (std::this_thread::get_id() != recv_thread_.get_id()) {
             recv_thread_.join();
         } else {
             recv_thread_.detach();
         }
     }
     ```
   - Neither method synchronizes access to `recv_thread_` with a mutex. While `channel_mutex_` exists, it only guards `active_channel_`.
   - When a connection drops and `on_connection_state_changed` triggers reconnection in `recv_thread_`, `recv_thread_ = std::thread(...)` is written by the reconnecting thread while `Engine::stop()` reads `recv_thread_.joinable()` / `recv_thread_.get_id()`.
   - This causes concurrent, unsynchronized read/write of `std::thread`, which is undefined behavior and caught by TSan.

3. **Root Cause Analysis of Shutdown Order Inversion**:
   - `Engine::stop()` calls `file_mgr_->stop()` at line 112 before calling `disconnect_active_peer()` at line 130.
   - `file_mgr_->stop()` performs `worker_thread_.join()`.
   - If `worker_thread_` is blocked in `::send()` (e.g., peer hung or socket unresponsive), `disconnect_active_peer()` has not yet executed `::shutdown(fd, SHUT_RDWR)`.
   - As a result, `worker_thread_` does not unblock immediately, blocking `Engine::stop()` for the 15-second socket timeout (`SO_SNDTIMEO`).

---

## 3. Caveats
- No modifications were made to implementation code in `core/`, `linux/`, or `android/`. All testing code was written as an isolated empirical harness in `build/`.
- The data race on `recv_thread_` requires asynchronous connection/reconnection overlapping with `Engine::stop()`; it does not manifest in serial single-threaded tests, explaining why `test_loopback_transfer` did not catch it.
- Linux GUI (`linux/`) and Android JNI (`android/`) integration remain outside the scope of M1 and were not evaluated.

---

## 4. Conclusion & Required Changes

**Verdict**: `REQUEST_CHANGES`

The Milestone 1 implementation introduces significant and valuable improvements (bounded deserialization, constant-time HMAC check, async file streaming queue, RAII temp file cleanup). However, empirical adversarial review identified three defects that must be remediated:

### Required Changes:
1. **[CRITICAL] Synchronize `recv_thread_` Lifecycle in `Engine`**:
   - Protect `recv_thread_` access across `attach_channel()` and `stop()` with a dedicated mutex (e.g. `thread_lifecycle_mutex_`), ensuring thread reassignment and joining cannot race.
2. **[HIGH] Handle Pre-existing Verified Files in `FileTransferManager`**:
   - In `core/src/file_transfer.cpp:261-271`, when a file is detected to be already completely transferred (`st.completed = true`), dispatch a `kFileVerifiedAck` immediately (or collect in an `already_completed_files` vector and call `finalize_incoming_file`) so both peers receive completion callbacks and batches are erased from `incoming_batches_` and `outgoing_batches_`.
3. **[MEDIUM] Order Socket Shutdown Before Worker Join in `Engine::stop()`**:
   - In `core/src/engine.cpp:101-131`, move `disconnect_active_peer()` before `file_mgr_->stop()`. Calling `disconnect_active_peer()` first will shut down the socket and immediately abort any blocked `::send()`, allowing `file_mgr_->stop()` to join instantly without waiting for 15s timeouts.

---

## 5. Verification Method

To reproduce the findings independently:

1. **ThreadSanitizer Data Race Reproduction**:
   Compile and run `challenger_stress` with TSan:
   ```bash
   g++ -std=c++20 -O2 -fsanitize=thread -Icore/include build/challenger_stress.cpp build-tsan/core/libl2a_core.a -lpthread -o build-tsan/challenger_stress
   ./build-tsan/challenger_stress
   ```
   Observe the TSan data race warning on `std::thread::get_id()` and exit code 66.

2. **Existing File Transfer Completion Hang Reproduction**:
   In `build/challenger_stress.cpp`, initialize a batch where the destination file already exists and matches SHA-256. Observe that `cli_l.completed_count` never increments and `outgoing_batches_` is never pruned.

3. **Standard Regression Verification**:
   ```bash
   cmake -B build -DL2A_BUILD_LINUX=ON && cmake --build build -j4 && ctest --test-dir build --output-on-failure -V
   ```
