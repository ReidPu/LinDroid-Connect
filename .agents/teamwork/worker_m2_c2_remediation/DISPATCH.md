# DISPATCH — worker_m2_c2_remediation

## Task Assignment
**Role**: Android Subsystem Remediation Worker (Iteration 3)
**Working Directory**: `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/worker_m2_c2_remediation`
**Project Plan**: `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/PROJECT.md`
**Source of Truth**: `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/ORIGINAL_REQUEST.md` (section `## 2026-10-23T12:34:22Z`)
**Reviewer & Challenger Reports**:
- `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/reviewer_m2_c2_2/handoff.md`
- `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/challenger_m2_c2_2/handoff.md`

## File Ownership
You exclusively own and may modify:
- `android/app/src/main/java/com/reid/l2a/MainActivity.kt`
- `android/app/src/main/java/com/reid/l2a/ShareReceiverActivity.kt`
- `android/app/src/main/java/com/reid/l2a/L2AService.kt`
- `android/app/src/main/cpp/jni_bridge.cpp`

## Remediation Tasks
1. **Fix Scoped Storage Fallback Bypass (`ShareReceiverActivity.kt:103-125`)**:
   - Currently, if `pfd != null` but `CoreBridge.nativeSendFileFd` returns `0L` (e.g. virtual pipe, non-seekable cloud descriptor), the `else` block containing the staging copy fallback is bypassed.
   - Fix:
     ```kotlin
     var batchId = 0L
     if (pfd != null) {
         val fileSize = pfd.statSize
         val fd = pfd.detachFd()
         batchId = CoreBridge.nativeSendFileFd(fd, displayName, fileSize)
     }
     if (batchId == 0L) {
         // Fallback to stream copy into cacheDir staging file
         val stagingDir = File(cacheDir, "share_staging").apply { mkdirs() }
         val tempFile = File(stagingDir, "${System.currentTimeMillis()}_$displayName")
         contentResolver.openInputStream(uri)?.use { input ->
             tempFile.outputStream().use { output ->
                 input.copyTo(output)
             }
         }
         if (tempFile.exists() && tempFile.length() > 0) {
             batchId = CoreBridge.nativeSendFile(tempFile.absolutePath)
         }
     }
     if (batchId != 0L) {
         sentCount++
     }
     ```
2. **Fix NFC Leaked Connections on I/O Exception (`MainActivity.kt:243-285`)**:
   - Wrap `ndef.connect()` and `formatable.connect()` in `try ... finally` blocks to ensure `ndef.close()` and `formatable.close()` are ALWAYS called even if `writeNdefMessage` or `format` throws an exception (e.g. `TagLostException`, `IOException`).
   - Also ensure `ndef.close()` is called in `finally` inside `handleTapTrigger`.
3. **Fix Background Binder Thread UI View Access (`MainActivity.kt:215-225`)**:
   - In `MainActivity.kt`: `hostIdInput.text` was accessed on the Binder thread from `readerCallback.onTagDiscovered`.
   - Store `currentHostId` or read `hostIdInput.text.toString()` on the UI thread when starting or during user interaction, and wrap `handleTagDiscovered` in a top-level `try/catch` so no Binder exception can crash the application.
4. **Fix Staging Directory Leak & Relative Path Dots (`ShareReceiverActivity.kt` & `L2AService.kt`)**:
   - In `ShareReceiverActivity.kt:resolveCleanDisplayName`: if `cleanName.isBlank() || cleanName == "." || cleanName == ".."`, fall back to `"shared_${System.currentTimeMillis()}"`.
   - In `L2AService.kt:onCreate`: clean up any existing temporary staging files in `File(cacheDir, "share_staging")`. In `ShareReceiverActivity.kt`: delete staging files after sending or schedule cleanup.

## Verification
Rebuild and verify:
```bash
JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64 \
ANDROID_USER_HOME=/home/reid-pu/Programming/Linux_to_Android/.android \
./gradlew assembleDebug -Duser.home=/home/reid-pu/Programming/Linux_to_Android
```
Ensure 0 errors and APK generated.

## Mandatory Integrity Warning
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A teamwork_preview_auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.

Produce `handoff.md` with build logs upon completion.

## 2026-10-23T14:09:38Z
You are worker_m2_c2_remediation.
Working Directory: `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/worker_m2_c2_remediation`
You MUST read:
- `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/ORIGINAL_REQUEST.md` (section `## 2026-10-23T12:34:22Z`)
- `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/PROJECT.md`
- `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/worker_m2_c2_remediation/DISPATCH.md`
- Reviewer & Challenger reports:
  - `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/reviewer_m2_c2_2/handoff.md`
  - `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/challenger_m2_c2_2/handoff.md`

Your Task:
Implement the 4 targeted remediation items in `android/app/src/main/java/com/reid/l2a/`:
1. `ShareReceiverActivity.kt`: allow staging copy fallback when `batchId == 0L` even if `pfd != null`; clean up `cacheDir/share_staging` files; guard against `"."` and `".."` in `resolveCleanDisplayName`.
2. `MainActivity.kt`: ensure `ndef.close()` and `formatable.close()` are ALWAYS called in `finally` blocks; do not touch `hostIdInput.text` from the Binder thread in `handleTagDiscovered` (read it on UI thread); wrap `handleTagDiscovered` in top-level `try/catch`.
3. `L2AService.kt`: prune stale staging files in `onCreate()`.

Rebuild and verify:
```bash
JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64 \
ANDROID_USER_HOME=/home/reid-pu/Programming/Linux_to_Android/.android \
./gradlew assembleDebug -Duser.home=/home/reid-pu/Programming/Linux_to_Android
```

