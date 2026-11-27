## 2026-10-21T03:56:14Z
From: 046339d7-ff3b-4ec5-91cc-957d813227d5
Task: Milestone 3: Android Subsystem Remediation (ANDROID-01 through ANDROID-13)

MANDATORY FIRST STEP: Read /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/ORIGINAL_REQUEST.md.
Also read:
- Master Scope: /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/PROJECT.md
- Defect Diagnostic Report: /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/DEFECT_REVIEW_REPORT.md
- Explorer 3 Android Subsystem Report: /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/teamwork_preview_explorer_survey_3/handoff.md

EXCLUSIVE FILE WRITE OWNERSHIP:
You own exclusively files under `android/`:
- `android/app/src/main/cpp/jni_bridge.cpp`
- `android/app/src/main/cpp/CMakeLists.txt`
- `android/app/src/main/java/com/reid/l2a/CoreBridge.kt`
- `android/app/src/main/java/com/reid/l2a/L2AService.kt`
- `android/app/src/main/java/com/reid/l2a/MainActivity.kt`
- `android/app/src/main/java/com/reid/l2a/ShareReceiverActivity.kt`
- `android/app/src/main/AndroidManifest.xml`
- `android/app/build.gradle.kts`
Do NOT modify files in `core/` or `linux/`.

YOUR TASK (Milestone 3: Android Subsystem Remediation):
Implement genuine, robust, production-grade Kotlin and C++ JNI fixes for ANDROID-01 through ANDROID-13:
1. ANDROID-01: Auto `DetachCurrentThread` on native worker thread termination in `jni_bridge.cpp`. Implement an automatic thread cleanup mechanism using `thread_local` RAII guard or `pthread_key_create` destructor so that any background C++ thread attached via `AttachCurrentThread` detaches when exiting, eliminating fatal ART crashes (`Native thread exited without detaching from Java VM`).
2. ANDROID-02: Protect `g_callback_obj` lifecycle against Use-After-Free race on service stop in `jni_bridge.cpp`. Synchronize callback invocations with a dedicated mutex/lock or snapshot pointer, ensuring `nativeStop` cannot delete the global ref while a callback is executing.
3. ANDROID-03: Release `g_engine_mtx` during blocking `nativeConnect` in `jni_bridge.cpp`. Obtain `engine` reference under lock, but execute `connect_to_peer` outside the lock with a 3-second timeout so the Android UI thread never suffers an Application Not Responding (ANR) freeze when calling `nativeStop` or sending clipboard items.
4. ANDROID-04: Check and clear pending JNI exceptions in `jni_bridge.cpp`. After every `env->CallVoidMethod(...)`, invoke `env->ExceptionCheck()` and `env->ExceptionClear()` with error logging to prevent fatal ART crashes (`JNI call with pending exception`).
5. ANDROID-05: Direct Scoped Storage zero-copy streaming via `ParcelFileDescriptor` (`nativeSendFileFd`). In `CoreBridge.kt` and `jni_bridge.cpp`, implement `nativeSendFileFd(fd, displayName, fileSize)` so `ShareReceiverActivity.kt` streams content directly from `contentResolver.openFileDescriptor(uri, "r")` without copying multi-GB files into `cacheDir`. Retain file descriptor lifetime safely until transmission completes.
6. ANDROID-06: MediaStore indexing of received media files in `L2AService.kt`. In `onFileCompleted`, call `MediaScannerConnection.scanFile` on `savedPath` so photos, videos, and music are immediately indexed into Xiaomi Gallery and Android system media collections.
7. ANDROID-07: Add runtime permission request for `POST_NOTIFICATIONS` in `MainActivity.kt` on Android 13/14 (API 33/34) so the foreground service notification is visible.
8. ANDROID-08: Add `ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS` prompt and guide HyperOS Autostart in `MainActivity.kt` so Xiaomi HyperOS does not freeze or terminate the background service.
9. ANDROID-09: Wi-Fi Multicast Lock in `L2AService.kt` and `AndroidManifest.xml`. Add `<uses-permission android:name="android.permission.CHANGE_WIFI_MULTICAST_STATE" />` and acquire `WifiManager.MulticastLock` in `L2AService.onCreate()` so UDP peer discovery beacons on port 53322 are not dropped by the Wi-Fi chipset.
10. ANDROID-10: Suppress clipboard echo loop in `L2AService.kt`. In `clipListener`, check `clip.description?.label == "LinuxClipboard"` or compare text with `lastReceivedText` to prevent infinite reflection back across the network.
11. ANDROID-11: Handle large text (>512KB) from temp file path and image clipboard items in `L2AService.kt`. When `item.data` is empty and `tempFilePath` is non-empty, read content from `tempFilePath`. Support image clipboard items (`image/png`, `image/jpeg`).
12. ANDROID-12: Register `ACTION_SEND_MULTIPLE` in `AndroidManifest.xml` and support multi-file streaming in `ShareReceiverActivity.kt`.
13. ANDROID-13: Implement `on_file_progress` in `JniCoreListener` and cache `jmethodID`s during `JNI_OnLoad` for efficient callback dispatch.

VERIFICATION REQUIREMENTS:
Run:
`cmake -B build -DL2A_BUILD_LINUX=ON && cmake --build build -j4`
`ctest --test-dir build --output-on-failure -V`
Both MUST continue to compile cleanly with 0 errors and pass 100%.
Verify syntax and integrity of all Kotlin and JNI C++ changes. Write unit/integration tests for JNI parsing and Scoped Storage streaming logic where appropriate.
