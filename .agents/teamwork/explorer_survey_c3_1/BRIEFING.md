# BRIEFING — 2026-10-24T03:22:30Z

## Mission
Investigate and architect Requirement R1: Dual-end Settings, Background Seamless Clipboard Sync (Wayland & Android 14 HyperOS), and Manual Control Mode.

## 🔒 My Identity
- Archetype: explorer
- Roles: investigation, synthesis
- Working directory: /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/explorer_survey_c3_1
- Original parent: 8eeb0c45-a89c-4e54-a47f-ce415553534a
- Milestone: survey_c3_r1

## 🔒 Key Constraints
- Read-only investigation — do NOT implement
- Scope strictly on Requirement R1: Dual-end Settings Module, True Background Seamless Sync without Window Focus, Manual Control Mode.

## Current Parent
- Conversation ID: 8eeb0c45-a89c-4e54-a47f-ce415553534a
- Updated: not yet

## Investigation State
- **Explored paths**:
  - `linux/src/wayland_clipboard.{hpp,cpp}`, `app_window.{hpp,cpp}`, `main.cpp`, `tray_indicator.{hpp,cpp}`, `adb_helper.{hpp,cpp}`, `gtk_abi.hpp`
  - `core/include/l2a/engine.hpp`, `core/src/engine.cpp`, `clipboard_stream.{hpp,cpp}`, `protocol.{hpp,cpp}`
  - `android/app/src/main/AndroidManifest.xml`, `L2AService.kt`, `MainActivity.kt`, `CoreBridge.kt`, `NfcTriggerActivity.kt`, `jni_bridge.cpp`
  - Tests: `linux/tests/test_linux_subsystem.cpp`, `tests/CMakeLists.txt`, `tests/*`
- **Key findings**:
  1. Wayland Focus Root Cause: GNOME Mutter delivers `wl_data_device.selection` only to focused surfaces; `wl-paste --watch` fails due to lack of `wlr-data-control` and missing binary.
  2. Wayland Solution: In-process XWayland/X11 bridge using `XFixesSelectSelectionInput` on `:0`/`$DISPLAY` which receives events in background without focus; writes to X11 selection so Mutter mirrors to Wayland for Ctrl+V.
  3. Android Background Limitation: Android 10+ / HyperOS restricts `primaryClip` without window focus.
  4. Android Solution: Declare `SYSTEM_ALERT_WINDOW`, pulse focus via 1x1 transparent overlay or trampoline activity, and provide Notification Action Buttons.
  5. Settings & Manual Control: Add `SettingsManager` (`settings.json`) on Linux, `SharedPreferences` on Android; reserve `kClipPullReq{clip_id: 0}` for remote clipboard pulling.
- **Unexplored areas**: None for Requirement R1. Fully mapped.

## Key Decisions Made
- Architecture specified in `handoff.md` with 5 required sections.

## Artifact Index
- DISPATCH.md — Incoming dispatch
- progress.md — Liveness heartbeat
- BRIEFING.md — Working memory
- handoff.md — Final handoff report
