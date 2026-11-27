# BRIEFING — 2026-10-23T12:48:45Z

## Mission
Investigate R1 (Computer -> Phone initial clipboard sync failure) and R2 (Linux desktop clipboard reception & application) to identify root causes and formulate concrete fixes and verification methods.

## 🔒 My Identity
- Archetype: explorer
- Roles: Codebase Researcher & Protocol Explorer (Core & Linux Clipboard / Channel Architecture)
- Working directory: /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/explorer_survey_r2_1
- Original parent: af5814b6-fe43-4f2b-b498-cb11fbccdb34
- Milestone: Investigation R1 & R2

## 🔒 Key Constraints
- Read-only investigation — do NOT implement / modify source code.
- Write only to working directory `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/explorer_survey_r2_1/`.
- Produce 5-component handoff report (`handoff.md`).

## Current Parent
- Conversation ID: af5814b6-fe43-4f2b-b498-cb11fbccdb34
- Updated: not yet

## Investigation State
- **Explored paths**:
  - `core/include/l2a/engine.hpp`
  - `core/src/engine.cpp`
  - `core/src/clipboard_stream.cpp`
  - `core/src/file_transfer.cpp`
  - `core/src/discovery.cpp`
  - `core/src/tls_channel.cpp`
  - `linux/src/wayland_clipboard.hpp`
  - `linux/src/wayland_clipboard.cpp`
  - `linux/src/app_window.hpp`
  - `linux/src/app_window.cpp`
  - `linux/src/main.cpp`
  - `android/app/src/main/java/com/reid/l2a/L2AService.kt`
  - `android/app/src/main/java/com/reid/l2a/NfcTriggerActivity.kt`
  - `android/app/src/main/cpp/jni_bridge.cpp`
  - `tests/test_loopback_transfer.cpp`
  - `linux/tests/test_linux_subsystem.cpp`
- **Key findings**:
  - **R1 Core Cause**: `Engine::send_frame_active()` returns false immediately if `active_channel_` is null/disconnected without on-demand connection. `Engine` lacks candidate endpoint caching (discovered peer IP or ADB `127.0.0.1:53322`).
  - **R1 Linux Desktop Gate**: `WaylandClipboardBridge` checks `if (self->engine_.is_connected())` before calling `push_clipboard_text()`, silently dropping initial `Ctrl+C` copy events when unconnected.
  - **R1 Android USB Tunnel Bug**: `L2AService.kt:91` attempts `CoreBridge.nativeConnect("127.0.0.1", 53321)` (its own listening port) instead of reverse port `53323`, and only tries once.
  - **R2 Wayland Desktop Root Cause**: `apply_remote_clipboard()` in `wayland_clipboard.cpp:299-303` uses `if (gtk_clip_) { gtk_clipboard_set_text(...); } else if (has_wl_clipboard_tools()) { write_to_wl_copy(...); }`. Because `gtk_clip_` is always non-null, `wl-copy` is DEAD CODE for text! Wayland compositors (GNOME Mutter) reject `gtk_clipboard_set_text()` from unfocused background apps, preventing `Ctrl+V` in external apps from pasting.
  - **R2 Large Text Bug**: Text items >512 KB use temp files (`item.data` empty), falling through to binary handler without setting `last_applied_hash_`.
- **Unexplored areas**: None, full end-to-end evidence chains established.

## Key Decisions Made
- [Initial] Investigated R1 and R2 across core, linux, and android codebases.
- [Synthesis] Identified 5 root causes for R1 and 4 root causes for R2; designed on-demand candidate connection mechanism and dual `wl-copy` + GTK clipboard application.

## Artifact Index
- DISPATCH.md — Task dispatch and instructions
- BRIEFING.md — Persistent context & identity
- progress.md — Heartbeat and activity log
- handoff.md — Final 5-component handoff deliverable
