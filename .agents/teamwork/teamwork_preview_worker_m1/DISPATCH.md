## 2026-10-21T01:46:30Z
You are teamwork_preview_worker_m1.
Your working directory is: /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/teamwork_preview_worker_m1/
Project root is: /home/reid-pu/Programming/Linux_to_Android
Parent Orchestrator ID: 046339d7-ff3b-4ec5-91cc-957d813227d5

MANDATORY FIRST STEP: Read /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/ORIGINAL_REQUEST.md.
Also read:
- Master Scope: /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/PROJECT.md
- Defect Diagnostic Report: /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/DEFECT_REVIEW_REPORT.md
- Explorer 1 Detailed Report: /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/teamwork_preview_explorer_survey_1/handoff.md

MANDATORY INTEGRITY WARNING:
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A teamwork_preview_auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.

EXCLUSIVE FILE WRITE OWNERSHIP:
You own exclusively the files in `core/`:
- `core/src/file_transfer.cpp`, `core/include/l2a/file_transfer.hpp`
- `core/src/engine.cpp`, `core/include/l2a/engine.hpp`
- `core/src/clipboard_stream.cpp`, `core/include/l2a/clipboard_stream.hpp`
- `core/src/tls_channel.cpp`, `core/include/l2a/tls_channel.hpp`
- `core/src/protocol.cpp`, `core/include/l2a/protocol.hpp`
- `core/CMakeLists.txt`
Do NOT modify files in `linux/` or `android/` in this milestone.

YOUR TASK (Milestone 1: C++ Core Library Remediation):
Implement genuine, high-quality, robust C++20 fixes for CORE-01 through CORE-13:
1. CORE-01: Decouple `stream_outgoing_files` from `recv_thread_` into an async/detached worker thread in `FileTransferManager` so `Engine::channel_recv_loop` is never blocked sending file chunks, eliminating the bidirectional transfer deadlock.
2. CORE-02: Release `FileTransferManager::mutex_` before computing SHA-256 on incoming files, sending ACK frames, and invoking `on_file_completed_` to prevent re-entrant callback deadlocks and lock contention.
3. CORE-03: Prevent self-join deadlock in `Engine::attach_channel`: check `std::this_thread::get_id() != recv_thread_.get_id()` or detach/clean without blocking self-join when reconnecting from `on_connection_state_changed`.
4. CORE-04: Batch metadata single-frame payload overflow for >750 files: chunk or paginate `FileBatchMetaPayload` across frames or support multi-part metadata, ensuring `recv_frame` payload ceiling is never exceeded.
5. CORE-05: RAII temporary file cleanup for large clipboard streaming (>512KB) and startup sweep of orphaned `.tmp` files in `temp_dir_`.
6. CORE-06: Prune completed batches in `outgoing_batches_`, `incoming_batches_`, and `lazy_sources_` in `FileTransferManager` and `ClipboardStreamManager` to eliminate unbounded memory growth.
7. CORE-07: Implement periodic keep-alive Ping transmission (e.g., in a dedicated heartbeat thread or timer) and configure socket timeouts (`SO_RCVTIMEO`, `SO_SNDTIMEO`, `SO_KEEPALIVE`).
8. CORE-08: Sanitize/escape device names and robustly parse `trusted_peers.tsv` and `identity.cfg` handling spaces and special characters.
9. CORE-09: Use `std::atomic<int>` for `sock_fd_` and synchronize socket close lifecycle in `TlsChannel`.
10. CORE-10: Bounds check (`count <= remaining_bytes / min_entry_size`) before calling `reserve()` in protocol decoders to prevent DoS.
11. CORE-11: Delete or truncate corrupt `.l2apart` on SHA-256 verification failure to avoid infinite resume loops.
12. CORE-12: Constant-time comparison in TLS Finished HMAC verification.
13. CORE-13: Add `POSITION_INDEPENDENT_CODE ON` in `core/CMakeLists.txt`.

VERIFICATION REQUIREMENTS:
Run:
`cmake -B build -DL2A_BUILD_LINUX=ON && cmake --build build -j4`
`ctest --test-dir build --output-on-failure -V`
Both MUST compile cleanly with 0 errors and pass 100%.

OUTPUT:
Deliver a comprehensive handoff report to:
`/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/teamwork_preview_worker_m1/handoff.md`
Detailing all files modified, diff summary, verification commands, and test output.
When finished, send a completion message to the parent orchestrator via send_message.
