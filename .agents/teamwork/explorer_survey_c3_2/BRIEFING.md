# BRIEFING — 2026-10-24T03:22:45Z

## Mission
Survey technical scope, existing architecture, and concrete implementation strategy for Requirement R2: Binary Image Clipboard Sync & File Manager Resolution (Linux PC -> Android & Android -> Linux PC).

## 🔒 My Identity
- Archetype: explorer
- Roles: survey, analyze, report
- Working directory: /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/explorer_survey_c3_2
- Original parent: 8eeb0c45-a89c-4e54-a47f-ce415553534a
- Milestone: survey_c3_r2

## 🔒 Key Constraints
- Read-only investigation — do NOT implement
- Base findings on verifiable evidence (file paths, line numbers, code quotes)
- Write handoff.md following 5-component handoff protocol
- Communicate via send_message to parent agent

## Current Parent
- Conversation ID: 8eeb0c45-a89c-4e54-a47f-ce415553534a
- Updated: not yet

## Investigation State
- **Explored paths**:
  - `linux/src/wayland_clipboard.{hpp,cpp}`
  - `linux/src/gtk_abi.hpp`
  - `core/include/l2a/clipboard_stream.hpp`, `core/src/clipboard_stream.cpp`
  - `core/include/l2a/engine.hpp`, `core/src/engine.cpp`
  - `android/app/src/main/java/com/reid/l2a/L2AService.kt`
  - `android/app/src/main/java/com/reid/l2a/CoreBridge.kt`
  - `android/app/src/main/java/com/reid/l2a/MainActivity.kt`
  - `android/app/src/main/java/com/reid/l2a/NfcTriggerActivity.kt`
  - `android/app/src/main/cpp/jni_bridge.cpp`
  - `android/app/src/main/AndroidManifest.xml`
  - `android/app/src/main/res/xml/nfc_tech_filter.xml`
  - `android/app/build.gradle.kts`
  - `tests/test_initial_clipboard_sync.cpp`
  - `linux/tests/test_linux_subsystem.cpp`
- **Key findings**:
  1. Linux capture root cause: `on_gtk_owner_change` unconditionally requests text via `gtk_clipboard_request_text`, ignoring raw images and converting Nautilus file copies (`text/uri-list`) to text paths (`file:///.../xxx.png`). `wl-paste` is hardcoded to `--type text/plain`.
  2. Local image file resolution: Missing in Linux. Needs URI percent-decoding, existence/regular-file checks, extension mapping (`.png`, `.jpg`, `.jpeg`, `.webp`, `.gif`, `.bmp`), and binary transmission via `push_clipboard_file` / `push_clipboard_bytes`.
  3. Android reception: Currently saves to `MediaStore`, whose URIs cannot be read by third-party apps (WeChat, QQ, Notes) without storage permissions. Android requires `androidx.core.content.FileProvider` (`grantUriPermissions="true"`), cache directory storage, and `ClipData.newUri(contentResolver, "L2A Image", contentUri)`.
  4. Phone -> PC: Missing `nativePushClipboardBytes` in `jni_bridge.cpp` and `CoreBridge.kt`. `clipListener`, `MainActivity`, and `NfcTriggerActivity` only call `coerceToText()`, converting image URIs to text strings or dropping them. On Linux reception, image items miss anti-echo hash recording in `apply_remote_clipboard`.
- **Unexplored areas**: None for R2 scope.

## Key Decisions Made
- Formulated complete 3-tier priority capture architecture for Linux.
- Formulated FileProvider configuration and implementation for Android clipboard reception.
- Formulated Phone -> PC binary capture, JNI extension, and anti-echo mechanisms.

## Artifact Index
- DISPATCH.md — Dispatch log
- BRIEFING.md — Persistent context & state
- progress.md — Liveness heartbeat
- handoff.md — Final investigation report
