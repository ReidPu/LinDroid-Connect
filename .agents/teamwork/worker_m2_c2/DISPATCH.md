# DISPATCH — worker_m2_c2

## Task Assignment
**Role**: Android Subsystem Implementation Worker
**Working Directory**: `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/worker_m2_c2`
**Project Plan**: `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/PROJECT.md`
**Source of Truth**: `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/ORIGINAL_REQUEST.md` (read section `## 2026-10-23T12:34:22Z`)
**Explorer Reports to Read**:
- `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/explorer_survey_r2_2/handoff.md`
- `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/explorer_survey_r2_3/handoff.md`

## File Ownership
You exclusively own and may modify:
- `android/app/src/main/cpp/jni_bridge.cpp`
- `android/app/src/main/java/com/reid/l2a/CoreBridge.kt`
- `android/app/src/main/java/com/reid/l2a/L2AService.kt`
- `android/app/src/main/java/com/reid/l2a/MainActivity.kt`
- `android/app/src/main/java/com/reid/l2a/NfcTriggerActivity.kt`
- `android/app/src/main/java/com/reid/l2a/ShareReceiverActivity.kt`

## Scope & Implementation Tasks (REAL-05 to REAL-11)
1. **REAL-05 (Fix USB reverse port in `L2AService.kt`)**:
   - Change `CoreBridge.nativeConnect("127.0.0.1", 53321)` at line 91 to `CoreBridge.nativeConnect("127.0.0.1", 53323)` (matching Linux `adb reverse tcp:53323 tcp:<transfer_port>`).
2. **REAL-06 (Peer IP caching & `ensureConnected()` in `CoreBridge.kt` & JNI)**:
   - In `android/app/src/main/cpp/jni_bridge.cpp`, implement `Java_com_reid_l2a_CoreBridge_nativeIsConnected(JNIEnv*, jobject)` returning `(engine && engine->is_connected()) ? JNI_TRUE : JNI_FALSE`.
   - In `CoreBridge.kt`, add variables `lastPeerIp`, `lastPeerPort`, `isConnected`, `external fun nativeIsConnected(): Boolean`.
   - Implement `fun ensureConnected(context: Context? = null, timeoutMs: Long = 3000): Boolean` which checks `nativeIsConnected()`, or attempts `lastPeerIp` (falling back to SharedPreferences `"l2a_prefs"`), and falls back to USB ADB reverse port `"127.0.0.1:53323"`.
   - In `L2AService.kt`, in `onPeerDiscovered(deviceId, deviceName, ip, port)`, cache `lastPeerIp` and `lastPeerPort` in `CoreBridge` and SharedPreferences (`"l2a_prefs"`). In `onConnectionChanged`, update `CoreBridge.isConnected`.
3. **REAL-07 (MainActivity Button Direct Push & User Notification)**:
   - In `MainActivity.kt`: Button `"立即发送手机当前剪贴板到电脑"` reads `primaryClip` directly in `MainActivity` (since it has active input focus), calls `CoreBridge.ensureConnected()`, and sends via `CoreBridge.nativePushClipboardText(text)` with Toast confirmation.
   - In `L2AService.kt:onClipboardReceived`: show foreground notification / Toast: `"已同步电脑剪贴板"` so HyperOS users have immediate feedback.
4. **REAL-08 & REAL-09 (ShareReceiverActivity File Sharing & Display Name Extraction)**:
   - In `ShareReceiverActivity.kt`:
     - Before sending files, call `CoreBridge.ensureConnected(this, 3500)`. If connection fails, show Toast `"未发现电脑连接，请确保电脑端已启动并在同一 Wi-Fi 或已连接 USB"` and finish.
     - Extract clean display name: query `OpenableColumns.DISPLAY_NAME`, fall back to MIME extension (`MimeTypeMap`), and sanitize via `File(name).name` to eliminate any path traversal / slashes.
     - Stream files: try zero-copy Scoped Storage `contentResolver.openFileDescriptor(uri, "r")` with `CoreBridge.nativeSendFileFd`. If null, fall back to streaming copy into staging file in `File(cacheDir, "share_staging")` and call `CoreBridge.nativeSendFile`.
     - Show Toast with count of transferred files upon completion.
5. **REAL-10 & REAL-11 (NFC enableReaderMode & NdefFormatable & Tap Trigger)**:
   - In `MainActivity.kt`:
     - Replace `enableForegroundDispatch` with `NfcAdapter.enableReaderMode` using flags `FLAG_READER_NFC_A | FLAG_READER_NFC_B | FLAG_READER_NFC_F | FLAG_READER_NFC_V | FLAG_READER_NO_PLATFORM_SOUNDS` to prevent HyperOS system interception and mute OS sounds.
     - In `onPause()`, call `disableReaderMode(this)`.
     - In `handleTagDiscovered(tag)`:
       - If `writeNfcMode`: support both `Ndef.get(tag)` and `NdefFormatable.get(tag)` (for unformatted NTAG5332).
       - Marshal all UI / status / Toast updates to main thread via `runOnUiThread`.
       - If not `writeNfcMode`: handle tap trigger by reading `NdefMessage` for URI `l2a://tap` and launch `NfcTriggerActivity`.
   - In `NfcTriggerActivity.kt`:
     - Attach a dummy view so the window has views on HyperOS.
     - Post delayed (50ms) to ensure window focus before reading `primaryClip`.
     - Call `CoreBridge.ensureConnected()` before sending, and show Toast feedback.

## Verification Commands
Verify Android compilation:
```bash
cd android && ./gradlew assembleDebug
```
And check if any device is connected via ADB:
```bash
adb devices
```
If an online device is present, attempt `./gradlew installDebug`.

## Mandatory Integrity Warning
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A teamwork_preview_auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.

Produce `handoff.md` in your working directory with build logs.


## 2026-10-23T13:25:26Z
You are worker_m2_c2.
Your working directory is: `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/worker_m2_c2`
Implement Milestone M2-C2 (Android Subsystem Remediation: REAL-05 to REAL-11):
1. `jni_bridge.cpp`: expose `nativeIsConnected()` returning `g_engine->is_connected()`.
2. `CoreBridge.kt`: add `ensureConnected(context, timeoutMs)` with dual fallback (LAN IP + ADB `127.0.0.1:53323`), `lastPeerIp`, `lastPeerPort`, and `isConnected`.
3. `L2AService.kt`: correct USB reverse port from 53321 to 53323; cache discovered IP in `SharedPreferences`; add Toast/notification on clipboard received.
4. `MainActivity.kt`:
   - Direct foreground clipboard push on button click.
   - Upgrade NFC to `NfcAdapter.enableReaderMode` with `FLAG_READER_NFC_A/B/F/V/NO_PLATFORM_SOUNDS` to silence OS sounds and prevent HyperOS preemption.
   - Support `Ndef` + `NdefFormatable` (unformatted NTAG5332 chips) with main-thread UI dispatch (`runOnUiThread`).
   - Non-write mode `l2a://tap` trigger.
5. `NfcTriggerActivity.kt`: dummy view for focus on HyperOS, delayed focus check, ensure connected before send.
6. `ShareReceiverActivity.kt`: ensure connection before send, Scoped Storage `ParcelFileDescriptor` with fallback to `cacheDir` staging copy, clean display name extraction with MIME extension fallback and path sanitization.

Build and verify:
```bash
cd android && ./gradlew assembleDebug
```
Check if device is connected via ADB (`adb devices`), and if present, attempt `./gradlew installDebug`.


## 2026-10-23T13:40:41Z
**Context**: Milestone M2-C2 (Android Subsystem Remediation)
**Content**: Checking in on progress. How is the implementation and `assembleDebug` compilation proceeding?
**Action**: Please update progress.md and provide a brief status update.
