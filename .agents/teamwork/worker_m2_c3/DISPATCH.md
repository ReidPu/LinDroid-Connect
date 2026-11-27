## 2026-10-24T04:24:58Z
You are worker_m2_c3.
Your working directory is: /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/worker_m2_c3
The authoritative user request is in: /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/ORIGINAL_REQUEST.md under header `## 2026-10-24T03:09:08Z`.
Read ORIGINAL_REQUEST.md before starting work.
Also read /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/PROJECT.md.

Read the detailed survey reports:
1. /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/explorer_survey_c3_1/handoff.md (Android settings, SYSTEM_ALERT_WINDOW, notification actions)
2. /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/explorer_survey_c3_2/handoff.md (Android FileProvider, UTF-8 fix, image ClipData.newUri, phone->PC image push)
3. /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/explorer_survey_c3_3/handoff.md (NFC NTAG5332 UID binding fallback, dual-mode trigger, Android history viewer & non-destructive selective paste)

FILE WRITE OWNERSHIP:
You exclusively own and will implement code in:
- `android/app/src/main/AndroidManifest.xml`
- `android/app/src/main/res/xml/file_paths.xml` (NEW)
- `android/app/src/main/res/layout/activity_main.xml` (or activity layouts)
- `android/app/src/main/java/com/reid/l2a/L2AService.kt`
- `android/app/src/main/java/com/reid/l2a/CoreBridge.kt`
- `android/app/src/main/java/com/reid/l2a/MainActivity.kt`
- `android/app/src/main/java/com/reid/l2a/NfcTriggerActivity.kt`

MANDATORY INTEGRITY WARNING:
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A teamwork_preview_auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.

DETAILED IMPLEMENTATION REQUIREMENTS:
1. AndroidManifest.xml & file_paths.xml:
   - Add `<uses-permission android:name="android.permission.SYSTEM_ALERT_WINDOW" />`
   - Register `androidx.core.content.FileProvider` with authority `${applicationId}.fileprovider`, `grantUriPermissions="true"`, referencing `@xml/file_paths`.
   - Create `res/xml/file_paths.xml` with `<cache-path name="clipboard_images" path="clipboard_images/" />`.
   - Update NFC intent-filters in `MainActivity` and `NfcTriggerActivity` to support `ACTION_TECH_DISCOVERED`, `ACTION_TAG_DISCOVERED`, and `ACTION_NDEF_DISCOVERED`.
2. L2AService.kt:
   - Fix Image Reception Defect:
     In `onClipboardReceived(item)`: Do NOT convert `image/*` to UTF-8 String! If MIME starts with `image/`, write binary bytes to `cacheDir / "clipboard_images" / "clip_${System.currentTimeMillis()}.png"`, obtain content URI via `FileProvider.getUriForFile()`, create `ClipData.newUri(contentResolver, "L2A Image", contentUri)`, and set to `clipboardManager.setPrimaryClip()`.
   - Directional auto-share check: Check `phone_to_pc_auto_share` in `clipListener`.
   - Foreground notification action buttons: "立即发送剪贴板" and "拉取电脑剪贴板".
   - Phone -> PC Image Capture: If `primaryClip.getItemAt(0).uri` is an image URI, read stream bytes and call `CoreBridge.nativePushClipboardBytes(mimeType, bytes)`.
   - Non-destructive Selective Paste: Ignore clips labeled `"L2A_Temp_Selective_Paste"` in `clipListener`.
3. MainActivity.kt:
   - Settings UI & Persistence: Directional switches (`pc_to_phone_auto_share`, `phone_to_pc_auto_share`), history limit (5-200, default 20) in `SharedPreferences("l2a_prefs")`, syncing limit to `CoreBridge.nativeSetMaxHistoryLimit`.
   - Manual Buttons: "立即发送当前剪贴板" and "拉取电脑剪贴板" (`CoreBridge.nativePullRemoteClipboard()`).
   - Clipboard History Viewer UI: Display list of history items, with text/thumbnail preview, origin badge, delete, clear, and "选择性粘贴" (sets clip with label `"L2A_Temp_Selective_Paste"` and schedules 3.5s delayed restore of item #0).
   - Touchpad NFC R4 (Multi-tech Write & Hardware UID Binding Fallback):
     - In binding mode: When tag is discovered, regardless of whether NDEF overwrite succeeds or fails on factory read-only NTAG5332, read hardware UID via `val uid = tag.id.joinToString(":") { "%02X".format(it) }`, save to `SharedPreferences` as `bound_touchpad_uid`, and display "已绑定触控板标签 UID: $uid (出厂只读硬件绑定成功，触碰即可触发双向同步)".
     - Dual-mode trigger matching: In `handleTapTrigger`, check if `tag.id` matches `bound_touchpad_uid` OR NDEF matches `l2a://tap`.
4. NfcTriggerActivity.kt:
   - Check `bound_touchpad_uid` fallback when tag is tapped.
5. Compilation & Verification:
   - Run `cd android && ./gradlew assembleDebug`
   - Ensure 0 errors and APK is generated.
   - Write comprehensive handoff report to `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/worker_m2_c3/handoff.md`.
