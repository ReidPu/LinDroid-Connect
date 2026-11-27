# BRIEFING — 2026-10-23T12:52:15Z

## Mission
Investigate R2 (Android-to-Linux Clipboard Sync & NfcTriggerActivity button failure) and R3 (Android Share Sheet / ShareReceiverActivity file transfer failure) to identify exact root causes and design concrete code fixes.

## 🔒 My Identity
- Archetype: explorer
- Roles: Codebase Researcher & Android Subsystem Explorer (Clipboard & Share Sheet)
- Working directory: /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/explorer_survey_r2_2
- Original parent: af5814b6-fe43-4f2b-b498-cb11fbccdb34
- Milestone: Investigation & Survey R2/R3

## 🔒 Key Constraints
- Read-only investigation — do NOT implement in source code
- Produce handoff.md in own folder with 5 sections: Observation, Logic Chain, Caveats, Conclusion, Verification Method
- Communicate with parent agent via send_message
- Regular updates to progress.md

## Current Parent
- Conversation ID: af5814b6-fe43-4f2b-b498-cb11fbccdb34
- Updated: not yet

## Investigation State
- **Explored paths**:
  - `android/app/src/main/java/com/reid/l2a/NfcTriggerActivity.kt`
  - `android/app/src/main/java/com/reid/l2a/MainActivity.kt`
  - `android/app/src/main/java/com/reid/l2a/L2AService.kt`
  - `android/app/src/main/java/com/reid/l2a/ShareReceiverActivity.kt`
  - `android/app/src/main/java/com/reid/l2a/CoreBridge.kt`
  - `android/app/src/main/cpp/jni_bridge.cpp`
  - `core/src/engine.cpp`, `core/src/clipboard_stream.cpp`, `core/src/file_transfer.cpp`, `core/src/discovery.cpp`
  - `linux/src/wayland_clipboard.cpp`, `linux/src/adb_helper.cpp`, `linux/src/app_window.cpp`
- **Key findings**:
  1. `L2AService.kt:91` attempts connection to `127.0.0.1:53321` instead of `127.0.0.1:53323` (ADB reverse tunnel port). Port 53321 is the phone's own engine listen port! USB connection is completely non-functional.
  2. Android app fails to cache discovered peer LAN IP (`<peer_ip>:53321`) in memory or SharedPreferences.
  3. `ShareReceiverActivity` and `NfcTriggerActivity` call `nativeSendFileFd` / `nativePushClipboardText` immediately without verifying or ensuring an active channel. When `active_channel_` is null, native methods return 0/false and silently drop the payload.
  4. `MainActivity` unnecessarily starts `NfcTriggerActivity` when clicking "立即发送手机当前剪贴板到电脑" instead of directly reading clipboard in `MainActivity` which already has foreground window focus.
  5. `ShareReceiverActivity` immediate `finish()` can revoke Content URI read permission before deferred stream reads complete; filenames can have illegal paths if slashes are not stripped.
  6. Linux `wayland_clipboard.cpp:300` bypasses `write_to_wl_copy` when `gtk_clip_` is valid, preventing global Wayland clipboard sync when l2a-desktop window is unfocused.
- **Unexplored areas**: None.

## Key Decisions Made
- Recommend dual-channel auto-connect helper (`ensureConnected`) with LAN IP (`<peer_ip>:53321`) and USB ADB reverse (`127.0.0.1:53323`).
- Recommend direct clipboard read in `MainActivity`, and harden `NfcTriggerActivity` with explicit view attachment, retry delay, and `ensureConnected`.
- Recommend robust display name extraction with MIME extension fallback and staging fallback for stream-only ContentProviders.
- Recommend calling both `gtk_clipboard_set_text` and `write_to_wl_copy` on Linux Wayland.

## Artifact Index
- `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/explorer_survey_r2_2/DISPATCH.md` — Task assignment
- `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/explorer_survey_r2_2/BRIEFING.md` — Situational awareness
- `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/explorer_survey_r2_2/progress.md` — Liveness & progress tracking
- `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/explorer_survey_r2_2/handoff.md` — Final survey & root cause report
