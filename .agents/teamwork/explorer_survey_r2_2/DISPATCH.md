# DISPATCH — explorer_survey_r2_2

## Task Assignment
**Role**: Codebase Researcher & Android Subsystem Explorer (Clipboard & Share Sheet)
**Working Directory**: `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/explorer_survey_r2_2`
**Source of Truth**: `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/ORIGINAL_REQUEST.md` (read section `## 2026-10-23T12:34:22Z`)

## Scope & Objective
Investigate Requirements R2 (Android side) and R3 (Android Share Sheet):
1. **R2 (Android side)**:
   - Why does Phone -> Computer clipboard sync fail and why does the manual button in `NfcTriggerActivity` fail?
   - Inspect `android/app/src/main/java/com/reid/l2a/NfcTriggerActivity.kt`, `L2AService.kt`, `CoreBridge.kt`, and `jni_bridge.cpp`.
   - Android 10+ / HyperOS clipboard restrictions: `ClipboardManager.getPrimaryClip()` requires window input focus (`onWindowFocusChanged(true)` or active foreground interaction). How is it currently invoked?
   - Peer address resolution: When sending clipboard to computer, how does the Android app locate the computer's address? Does it resolve known peer LAN IP (`<peer_ip>:53321`) or fall back to USB ADB reverse port (`127.0.0.1:53323`)?
2. **R3**:
   - Why does `ShareReceiverActivity` fail when sharing files/photos from gallery/file manager to Linux PC?
   - Inspect `android/app/src/main/java/com/reid/l2a/ShareReceiverActivity.kt` and `L2AService.kt`.
   - Content URI permission lifecycle: Does `ShareReceiverActivity` call `finish()` immediately, revoking URI read permissions before the background thread or service can open the stream? Check `FLAG_GRANT_READ_URI_PERMISSION` or temporary streaming/copying.
   - Display name extraction: Check `OpenableColumns.DISPLAY_NAME` vs `uri.lastPathSegment` (avoiding invalid slashes/encoded characters).
   - Target address resolution: Automatic LAN address (`<peer_ip>:53321`) and fallback to USB ADB reverse tunnel (`127.0.0.1:53323`).

## Deliverables
Produce `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/explorer_survey_r2_2/handoff.md` detailing:
- Exact root causes with file names and line numbers.
- Proposed concrete code changes and architectural fixes.
- Verification method for Android clipboard and file sharing.

## 2026-10-23T12:38:35Z
You are explorer_survey_r2_2.
Your working directory is: `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/explorer_survey_r2_2`
Please read `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/ORIGINAL_REQUEST.md` (section `## 2026-10-23T12:34:22Z`) and your dispatch file `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/explorer_survey_r2_2/DISPATCH.md`.

Investigate Requirements R2 (Android side) and R3 (Android Share Sheet):
1. R2 (Android side):
   - Why does Phone -> Computer clipboard sync fail and why does the manual button in `NfcTriggerActivity` fail?
   - Inspect `android/app/src/main/java/com/reid/l2a/NfcTriggerActivity.kt`, `L2AService.kt`, `CoreBridge.kt`, and `jni_bridge.cpp`.
   - Android 10+ / HyperOS clipboard restrictions: `ClipboardManager.getPrimaryClip()` requires window input focus (`onWindowFocusChanged(true)` or active foreground interaction). How is it currently invoked?
   - Peer address resolution: When sending clipboard to computer, how does the Android app locate the computer's address? Does it resolve known peer LAN IP (`<peer_ip>:53321`) or fall back to USB ADB reverse port (`127.0.0.1:53323`)?
2. R3:
   - Why does `ShareReceiverActivity` fail when sharing files/photos from gallery/file manager to Linux PC?
   - Inspect `android/app/src/main/java/com/reid/l2a/ShareReceiverActivity.kt` and `L2AService.kt`.
   - Content URI permission lifecycle: Does `ShareReceiverActivity` call `finish()` immediately, revoking URI read permissions before the background thread or service can open the stream? Check `FLAG_GRANT_READ_URI_PERMISSION` or temporary streaming/copying.
   - Display name extraction: Check `OpenableColumns.DISPLAY_NAME` vs `uri.lastPathSegment` (avoiding invalid slashes/encoded characters).
   - Target address resolution: Automatic LAN address (`<peer_ip>:53321`) and fallback to USB ADB reverse tunnel (`127.0.0.1:53323`).

You are read-only. Do NOT modify source code. Produce `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/explorer_survey_r2_2/handoff.md` with detailed evidence, file lines, root cause analysis, and concrete recommended fixes. Update your `progress.md` regularly and send a completion message when done.
