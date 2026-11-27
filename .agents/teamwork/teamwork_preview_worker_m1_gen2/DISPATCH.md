## 2026-10-21T02:15:32Z
You are teamwork_preview_worker_m1_gen2.
Your working directory is: /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/teamwork_preview_worker_m1_gen2/
Project root is: /home/reid-pu/Programming/Linux_to_Android
Parent Orchestrator ID: 046339d7-ff3b-4ec5-91cc-957d813227d5

MANDATORY FIRST STEP: Read /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/ORIGINAL_REQUEST.md.
Also read:
- Master Scope: /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/PROJECT.md
- Reviewer 2 Report with feedback: /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/teamwork_preview_reviewer_m1_2/handoff.md
- Challenger 1 Report with feedback: /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/teamwork_preview_challenger_m1_1/handoff.md

MANDATORY INTEGRITY WARNING:
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A teamwork_preview_auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.

EXCLUSIVE FILE WRITE OWNERSHIP:
You own exclusively files in `core/`:
- `core/src/file_transfer.cpp`, `core/include/l2a/file_transfer.hpp`
- `core/src/engine.cpp`, `core/include/l2a/engine.hpp`
- `core/src/tls_channel.cpp`, `core/include/l2a/tls_channel.hpp`
Do NOT modify files in `linux/` or `android/`.

YOUR TASK (Milestone 1, Iteration 2 Remediation):
Resolve the specific defects identified by Reviewer 2 and Challenger 1:
1. ThreadSanitizer data race on `recv_thread_` in `Engine::attach_channel` vs `Engine::stop()`:
   Synchronize `recv_thread_` lifecycle with a dedicated mutex (e.g. `channel_lifecycle_mutex_` or `channel_mutex_`) to ensure thread assignment, joining, and detachment are strictly race-free under TSan.
2. Pre-existing 100% verified files ACK dispatch in `FileTransferManager`:
   In `core/src/file_transfer.cpp`, when a file is detected as already complete on receiver (`existing_hash && *existing_hash == fmeta.file_sha256`), push it to an `immediate_finalize_files` list (along with 0-byte files), so that `finalize_incoming_file` is invoked. This ensures `kFileVerifiedAck` is dispatched to the sender, the file is marked verified, `outgoing_batches_` is cleanly pruned, and `on_file_completed` is called on both ends.
3. Metadata slicing byte budgeting in `FileTransferManager::send_paths`:
   Instead of a static count of 400, dynamically calculate accumulated encoded byte size ($24 + \sum (54 + \text{path\_len}_i)$). Slice into a new frame whenever byte size reaches 48,000 bytes (or count reaches 128), guaranteeing frame payload length NEVER exceeds `kMaxFramePayload` (66,560 B) regardless of long path lengths.
4. Teardown ordering in `Engine::stop()`:
   Call `disconnect_active_peer()` before joining `worker_thread_` or `heartbeat_thread_`. `disconnect_active_peer()` executes `::shutdown(fd, SHUT_RDWR)`, which immediately unblocks any thread blocked in socket operations.
5. CRLF line endings in TSV parsing:
   In `TrustedPeerStore::load()`, strip trailing `\r`: `if (!line.empty() && line.back() == '\r') line.pop_back();`.

VERIFICATION:
Run:
`cmake -B build -DL2A_BUILD_LINUX=ON && cmake --build build -j4 && ctest --test-dir build --output-on-failure -V`
`cmake -B build-tsan -DL2A_BUILD_LINUX=ON -DL2A_ENABLE_TSAN=ON && cmake --build build-tsan -j4 && ctest --test-dir build-tsan --output-on-failure -V`
`cmake -B build-asan -DL2A_BUILD_LINUX=ON -DL2A_ENABLE_ASAN=ON && cmake --build build-asan -j4 && ctest --test-dir build-asan --output-on-failure -V`
Ensure all tests pass 100% with 0 warnings, 0 data races, and 0 errors.

OUTPUT:
Deliver your report to:
`/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/teamwork_preview_worker_m1_gen2/handoff.md`
When done, notify parent orchestrator via send_message.


## 2026-10-21T03:22:07Z
**Context**: Milestone 1 Remediation Progress Check
**Content**: Orchestrator liveness check. Please report your current progress on implementing the 5 remediation items in core/ and current test status.
**Action**: Reply with your progress status and estimated time to handoff.
