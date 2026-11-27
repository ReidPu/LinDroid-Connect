# Architectural Survey & Defect Diagnostic Report: Android Client Subsystem & JNI Bridge

## 1. Observation

### 1.1 Android Module Architecture & Key Files

The Android client subsystem is located under `android/` and integrates directly with the root C++20 core engine via CMake and JNI without code duplication.

| Component / Layer | File Path | Role & Technology |
|---|---|---|
| **Build Configuration** | `android/app/build.gradle.kts`<br>`android/build.gradle.kts` | AGP + Kotlin DSL, `compileSdk=34`, `minSdk=29`, `targetSdk=34`, NDK ABIs: `arm64-v8a`, `x86_64`, C++20 standard |
| **Manifest & Permissions** | `android/app/src/main/AndroidManifest.xml` | App components, permissions (`INTERNET`, `ACCESS_NETWORK_STATE`, `ACCESS_WIFI_STATE`, `NFC`, `FOREGROUND_SERVICE`, `FOREGROUND_SERVICE_DATA_SYNC`, `POST_NOTIFICATIONS`) |
| **Native CMake Build** | `android/app/src/main/cpp/CMakeLists.txt` | CMake 3.20+, links `l2a_core` directly via `add_subdirectory(../../../../../core l2a_core_build)` and produces `libl2a_jni.so` |
| **JNI Bridge (C++)** | `android/app/src/main/cpp/jni_bridge.cpp` | Implements `Java_com_reid_l2a_CoreBridge_*` and `JniCoreListener` (`l2a::ICoreListener`) |
| **Core Bridge (Kotlin)** | `android/app/src/main/java/com/reid/l2a/CoreBridge.kt` | Kotlin singleton `object CoreBridge`, declares native methods, dispatches native callbacks to `Listener` |
| **Foreground Service** | `android/app/src/main/java/com/reid/l2a/L2AService.kt` | `Service` with `foregroundServiceType="dataSync"`, manages lifecycle, clipboard listener, incoming/outgoing state |
| **Main UI Activity** | `android/app/src/main/java/com/reid/l2a/MainActivity.kt` | Main interface, NFC NDEF writer for laptop touchpad (NTAG5332), service starter |
| **NFC Trigger Activity** | `android/app/src/main/java/com/reid/l2a/NfcTriggerActivity.kt` | Translucent window-focus acquiring Activity for one-touch NFC `l2a://tap` clipboard sync and notification action |
| **Share Sheet Receiver** | `android/app/src/main/java/com/reid/l2a/ShareReceiverActivity.kt` | System Share Sheet target (`Intent.ACTION_SEND`), copies `content://` URI to internal cache and invokes `nativeSendFile` |

---

### 1.2 Categorized Defect Findings

#### [DEFECT-01] JNI Thread Detachment Omission Causing Runtime Abort / Resource Leak (Severity: HIGH)
- **File & Line**: `android/app/src/main/cpp/jni_bridge.cpp:108-115`
- **Observed Code**:
  ```cpp
  static JNIEnv* attach_env() {
      if (!g_jvm) return nullptr;
      JNIEnv* env = nullptr;
      if (g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
          g_jvm->AttachCurrentThread(&env, nullptr);
      }
      return env;
  }
  ```
- **Root Cause**:
  Native background threads created in C++ core (`recv_thread_` in `Engine::channel_recv_loop`, `recv_thread_` in `DiscoveryManager::recv_loop`, etc.) invoke `attach_env()` whenever a callback (`on_peer_discovered`, `on_connection_state_changed`, `on_clipboard_received`, `on_file_completed`) is fired. If the thread is detached, `g_jvm->AttachCurrentThread(&env, nullptr)` is executed.
  However, `g_jvm->DetachCurrentThread()` is **never called anywhere** in the codebase.
- **Trigger Scenario**:
  When a peer disconnects, `Engine::channel_recv_loop` calls `on_connection_state_changed(false, ...)` (attaching the thread to the JVM) and then immediately terminates (lines 275-278 of `core/src/engine.cpp`). On Android ART, exiting an attached POSIX pthread without calling `DetachCurrentThread` causes:
  `art/runtime/thread.cc: "Native thread exited without detaching from Java VM"` leading to fatal `SIGABRT` / crash in debug/CheckJNI mode or permanent leaks of ART `Thread` instances, JNI local reference tables, and pthread TLS slots.
- **Concrete Fix Strategy**:
  Implement an automatic thread-exit cleanup mechanism using a `pthread_key_create` destructor or a `thread_local` RAII guard:
  ```cpp
  struct JniThreadAttacher {
      static JNIEnv* get_env() {
          if (!g_jvm) return nullptr;
          JNIEnv* env = nullptr;
          if (g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) {
              return env;
          }
          if (g_jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
              thread_local struct ThreadExitCleaner {
                  ~ThreadExitCleaner() {
                      if (g_jvm) g_jvm->DetachCurrentThread();
                  }
              } cleaner;
              return env;
          }
          return nullptr;
      }
  };
  ```

---

#### [DEFECT-02] Data Race on `g_callback_obj` & Use-After-Free during Engine Shutdown (Severity: HIGH)
- **File & Line**: `android/app/src/main/cpp/jni_bridge.cpp:19-105, 145-148, 169-172`
- **Observed Code**:
  ```cpp
  // Callbacks read g_callback_obj without lock:
  void on_peer_discovered(const l2a::DiscoveredPeer& peer) override {
      JNIEnv* env = attach_env();
      if (!env || !g_callback_obj) return;
      jclass cls = env->GetObjectClass(g_callback_obj);
      ...
  }
  // While nativeStop mutates g_callback_obj under g_engine_mtx:
  Java_com_reid_l2a_CoreBridge_nativeStop(JNIEnv* env, jobject /*thiz*/) {
      std::lock_guard<std::mutex> lock(g_engine_mtx);
      if (g_engine) { g_engine->stop(); g_engine.reset(); }
      if (g_callback_obj) { env->DeleteGlobalRef(g_callback_obj); g_callback_obj = nullptr; }
  }
  ```
- **Root Cause**:
  `g_callback_obj` is accessed concurrently by worker threads in `JniCoreListener` without acquiring `g_engine_mtx` or an atomic pointer check. If `nativeStop` runs on the UI thread while a background callback thread is in `GetObjectClass(g_callback_obj)` or `CallVoidMethod(g_callback_obj, ...)`, `g_callback_obj` is freed via `DeleteGlobalRef` and set to `nullptr`.
- **Trigger Scenario**:
  Stopping the service (e.g. `onDestroy()`) while discovery packets are received or while file transfer/clipboard is finalizing. ART detects invalid reference dereference (`JNI ERROR (app bug): use of deleted global reference`) and aborts the app.
- **Concrete Fix Strategy**:
  1. Protect `g_callback_obj` dispatch with a dedicated reader-writer lock or snapshot under `g_engine_mtx`.
  2. Alternatively, cache `CoreBridge` `jclass` as a global reference at `JNI_OnLoad` and call methods statically on `CoreBridge.INSTANCE` or use static JNI callbacks.

---

#### [DEFECT-03] Blocking Network Connect Holding Global Engine Mutex Causing ANR & Deadlock (Severity: HIGH)
- **File & Line**: `android/app/src/main/cpp/jni_bridge.cpp:176-187`
- **Observed Code**:
  ```cpp
  JNIEXPORT jboolean JNICALL
  Java_com_reid_l2a_CoreBridge_nativeConnect(
      JNIEnv* env, jobject /*thiz*/, jstring ip, jint port) {
      std::lock_guard<std::mutex> lock(g_engine_mtx);
      if (!g_engine) return JNI_FALSE;
      return g_engine->connect_to_peer(jstring_to_utf8(env, ip), static_cast<uint16_t>(port))
                 ? JNI_TRUE : JNI_FALSE;
  }
  ```
- **Root Cause**:
  `nativeConnect` acquires `g_engine_mtx` for the entire duration of `g_engine->connect_to_peer()`. Inside `connect_to_peer()`, `::connect` is called on a blocking socket with no timeout (default TCP timeout is 75-120 seconds), followed by TLS 1.3 handshake and `attach_channel()`.
  While `g_engine_mtx` is held, any call to `nativePushClipboardText()`, `nativeSendFile()`, or `nativeStop()` from any thread is blocked.
  Crucially, `L2AService.onDestroy()` calls `nativeStop()`, which runs on the Android main thread.
- **Trigger Scenario**:
  1. `L2AService.onCreate()` spawns `Thread { CoreBridge.nativeConnect("127.0.0.1", 53321) }`. If USB ADB reverse tunnel is not active, `connect` blocks.
  2. If the user closes or restarts the service while this background connection attempt is in progress, the main thread calls `nativeStop()`, blocks on `g_engine_mtx`, and Android triggers a 5-second Application Not Responding (ANR) crash dialog.
- **Concrete Fix Strategy**:
  1. Do not hold `g_engine_mtx` across network I/O; obtain a `std::shared_ptr<l2a::Engine>` snapshot under lock and invoke `connect_to_peer()` outside the lock.
  2. Set a 3-second connection timeout (`SO_SNDTIMEO` or non-blocking socket connect) in `connect_to_peer`.

---

#### [DEFECT-04] Unhandled JNI Exceptions Leaving Dirty Pending State (Severity: HIGH)
- **File & Line**: `android/app/src/main/cpp/jni_bridge.cpp:30, 52, 76, 99`
- **Observed Code**:
  ```cpp
  env->CallVoidMethod(g_callback_obj, mid, ...);
  // No env->ExceptionCheck()
  env->DeleteLocalRef(...);
  ```
- **Root Cause**:
  If any Kotlin callback method throws an exception (e.g. `SecurityException` reading clipboard, `OutOfMemoryError` allocating byte arrays, or NPE), ART enters a pending exception state. JNI rules strictly forbid invoking most JNI functions while an exception is pending. If C++ code ignores it and continues or returns, ART issues a fatal abort:
  `JNI DETECTED ERROR IN APPLICATION: JNI call with pending exception`.
- **Trigger Scenario**:
  Any uncaught runtime exception thrown by `L2AService` or `CoreBridge` listener implementations.
- **Concrete Fix Strategy**:
  After every `env->CallVoidMethod`, check and clear exceptions:
  ```cpp
  if (env->ExceptionCheck()) {
      env->ExceptionDescribe();
      env->ExceptionClear();
  }
  ```

---

#### [DEFECT-05] Scoped Storage Full-File Cache Duplication & Disk Exhaustion (Severity: HIGH)
- **File & Line**: `android/app/src/main/java/com/reid/l2a/ShareReceiverActivity.kt:35-55`
- **Observed Code**:
  ```kotlin
  val fileName = queryDisplayName(uri) ?: "shared_${System.currentTimeMillis()}"
  val tempFile = File(cacheDir, fileName)
  contentResolver.openInputStream(uri)?.use { input ->
      FileOutputStream(tempFile).use { output ->
          input.copyTo(output, bufferSize = 65536)
      }
  }
  CoreBridge.nativeSendFile(tempFile.absolutePath)
  ```
- **Root Cause**:
  1. **Full Disk Duplication**: When sharing large files (e.g., 4GB 4K videos), the entire content is copied to `cacheDir` before transmission begins. If free space is below the file size, it fails with `IOException: No space left on device`.
  2. **Severe Latency**: Copying 4GB to internal flash takes 15–30 seconds before a single packet is sent over the network.
  3. **Permanent Disk Leak**: `tempFile` in `cacheDir` is never deleted after transfer finishes.
  4. **Premature URI Revocation**: `ShareReceiverActivity.finish()` is called at line 32 immediately after launching the thread. In Android 11–14, closing the Activity can revoke temporary `content://` URI read grants before `input.copyTo` completes, causing `SecurityException`.
- **Trigger Scenario**:
  Sharing large videos, photos, or documents from Android Gallery or Files app to Linux.
- **Concrete Fix Strategy**:
  1. Implement direct streaming via `ParcelFileDescriptor`:
     `val pfd = contentResolver.openFileDescriptor(uri, "r")`
     `val fd = pfd.detachFd()`
     Pass `fd` to C++ via `CoreBridge.nativeSendFileFd(fd, fileName, fileSize)`. C++ core reads 64KB chunks directly from the descriptor, achieving true zero-copy streaming without disk buffering.
  2. If fallback temporary files are used, register a cleanup callback in `onFileCompleted` and retain URI permissions until stream opening completes.

---

#### [DEFECT-06] MediaStore Non-Indexing of Received Files on Android 10–14+ (Severity: HIGH)
- **File & Line**: `android/app/src/main/java/com/reid/l2a/L2AService.kt:39-43, 104-110`
- **Observed Code**:
  ```kotlin
  val recvDir = File(
      Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS),
      "LinuxToAndroid"
  ).apply { mkdirs() }
  ...
  override fun onFileCompleted(savedPath: String, success: Boolean, errorMsg: String) {
      if (success) {
          mainHandler.post {
              startForegroundWithNotification("已收到文件: ${File(savedPath).name}")
          }
      }
  }
  ```
- **Root Cause**:
  Under Scoped Storage (API 29–34), C++ saves files directly to `/storage/emulated/0/Download/LinuxToAndroid/` using POSIX `open`/`rename`.
  Because `MediaScannerConnection.scanFile(...)` is not invoked and no `MediaStore` record is created:
  - Received photos, videos, and music do not show up in the Xiaomi Gallery, Album, or Video player.
  - On app reinstall, because the new UID does not own the old install's files, subsequent transfers of identically named files or resume `.l2apart` files fail with `EACCES` (Permission Denied).
- **Trigger Scenario**:
  Sending images or videos from Linux to Android; user checks Xiaomi Gallery and files are invisible.
- **Concrete Fix Strategy**:
  In `L2AService.onFileCompleted`, trigger media indexing:
  ```kotlin
  android.media.MediaScannerConnection.scanFile(
      this,
      arrayOf(savedPath),
      null
  ) { path, uri ->
      Log.i("L2A", "Scanned $path into MediaStore: $uri")
  }
  ```

---

#### [DEFECT-07] Missing `POST_NOTIFICATIONS` Runtime Permission & HyperOS Autostart Killing (Severity: HIGH)
- **File & Line**: `android/app/src/main/java/com/reid/l2a/MainActivity.kt:28`, `android/app/src/main/AndroidManifest.xml:10`
- **Root Cause**:
  1. On Android 13+ (API 33) and Android 14 (API 34), `POST_NOTIFICATIONS` is a runtime permission. `MainActivity.kt` declares it in manifest but never requests it from the user. Consequently, the foreground service notification is suppressed by the OS.
  2. Xiaomi HyperOS aggressive battery killer: HyperOS's MIUI Battery Saver ("神隐模式") freezes network sockets and terminates background services after screen-off unless "无限制" (No restrictions) and "允许自启动" (Autostart) are granted. No intent or guidance exists in `MainActivity` to request battery optimization exemption (`Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS`).
- **Trigger Scenario**:
  First app launch on Xiaomi HyperOS / Android 13+. Service notification is invisible; service is killed within 10 minutes of screen locking.
- **Concrete Fix Strategy**:
  1. Add runtime permission request for `Manifest.permission.POST_NOTIFICATIONS` in `MainActivity.kt`.
  2. Add `Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS` intent prompt and guide user to enable HyperOS Autostart.

---

#### [DEFECT-08] UDP Wi-Fi Discovery Broken by Missing `MulticastLock` & Permission (Severity: MEDIUM)
- **File & Line**: `android/app/src/main/java/com/reid/l2a/L2AService.kt:30-49`, `android/app/src/main/AndroidManifest.xml:4-11`
- **Root Cause**:
  Android Wi-Fi chipsets filter out broadcast and multicast UDP packets by default to preserve battery.
  To receive discovery beacons on UDP 53322, Android requires `<uses-permission android:name="android.permission.CHANGE_WIFI_MULTICAST_STATE" />` and an active `WifiManager.MulticastLock`.
  Neither the permission nor the lock exists in the Android codebase.
- **Trigger Scenario**:
  Automatic peer discovery over Wi-Fi fails or works only when screen is on and connected via USB.
- **Concrete Fix Strategy**:
  Add `CHANGE_WIFI_MULTICAST_STATE` to manifest and acquire `MulticastLock` in `L2AService.onCreate()`.

---

#### [DEFECT-09] Clipboard Ping-Pong Echo Loop & Redundant JNI Overhead (Severity: MEDIUM)
- **File & Line**: `android/app/src/main/java/com/reid/l2a/L2AService.kt:25-28, 63-73, 95-102`
- **Root Cause**:
  When Linux sends clipboard text, `onClipboardReceived` calls `clipboardManager.setPrimaryClip(...)`.
  Setting the clip immediately triggers Android's `clipListener`, which invokes `pushCurrentClipboardToLinux()`, spawning a thread, crossing JNI, and calling `nativePushClipboardText()`.
  Although C++ has a single-entry `last_received_hash_` check, newline normalization (LF vs CRLF) or encoding differences can cause hash mismatches, producing an infinite ping-pong loop.
- **Trigger Scenario**:
  Copying text on Linux triggers redundant thread creation and JNI execution on Android.
- **Concrete Fix Strategy**:
  Filter out self-originated clipboard events in `L2AService.kt`:
  ```kotlin
  if (clip.description?.label == "LinuxClipboard" || text == lastReceivedText) return
  ```

---

#### [DEFECT-10] Large Text (>512 KB) and Image Clipboard Data Silently Ignored (Severity: MEDIUM)
- **File & Line**: `android/app/src/main/java/com/reid/l2a/L2AService.kt:95-102`, `android/app/src/main/cpp/jni_bridge.cpp:60-82`
- **Root Cause**:
  In C++ `clipboard_stream.cpp:232`, payloads > 512 KB (`kMaxInlineMemoryBytes`) are streamed to `temp_file_path`, and `item.data` is left empty.
  In `L2AService.kt:96`, `onClipboardReceived` checks `if (mimeType.startsWith("text/") && data.isNotEmpty())`.
  If someone copies text > 512 KB on Linux, `data` is empty, so `L2AService` silently drops it.
  Additionally, images (`image/png`, `image/jpeg`) are ignored entirely.
- **Trigger Scenario**:
  Copying large documents or screenshots on Linux; Android does not update clipboard.
- **Concrete Fix Strategy**:
  In `onClipboardReceived`, if `data.isEmpty()` and `tempFilePath.isNotEmpty()`, read from `File(tempFilePath)`. For images, save to cache and create `ClipData.newUri(...)`.

---

#### [DEFECT-11] Share Sheet Missing Multiple File Sharing (`ACTION_SEND_MULTIPLE`) (Severity: MEDIUM)
- **File & Line**: `android/app/src/main/java/com/reid/l2a/ShareReceiverActivity.kt:18-33`, `android/app/src/main/AndroidManifest.xml:46-51`
- **Root Cause**:
  Only `Intent.ACTION_SEND` is registered. Selecting multiple photos or documents in Gallery emits `Intent.ACTION_SEND_MULTIPLE` with `ArrayList<Uri>` in `EXTRA_STREAM`.
- **Trigger Scenario**:
  Sharing multiple files to Linux; L2A does not appear in the system share menu.
- **Concrete Fix Strategy**:
  Add `<action android:name="android.intent.action.SEND_MULTIPLE" />` and parse `getParcelableArrayListExtra<Uri>(Intent.EXTRA_STREAM)`.

---

#### [DEFECT-12] Missing Transfer Progress Callback in JNI & UI (Severity: LOW)
- **File & Line**: `android/app/src/main/cpp/jni_bridge.cpp:17-116`, `android/app/src/main/java/com/reid/l2a/CoreBridge.kt:8-13`
- **Root Cause**:
  `JniCoreListener` does not implement `on_file_progress`, leaving the Android notification without a progress bar during large multi-gigabyte transfers.
- **Concrete Fix Strategy**:
  Expose `onFileProgress(bytesTransferred, totalBytes, speed)` through JNI to update `Notification.Builder.setProgress`.

---

#### [DEFECT-13] Inefficient JNI Method ID Lookup on Every Callback (Severity: LOW)
- **File & Line**: `android/app/src/main/cpp/jni_bridge.cpp:23-25, 46-48, 64-66, 93-95`
- **Root Cause**:
  `GetObjectClass` and `GetMethodID` are executed on every single callback invocation, incurring repeated string matching and local reference churn.
- **Concrete Fix Strategy**:
  Cache `jmethodID` and a global reference to `CoreBridge` class during `JNI_OnLoad`.

---

## 2. Logic Chain

1. **JNI Thread Safety**:
   - `core/src/engine.cpp` creates pthreads for socket reception and discovery (`recv_thread_`).
   - When calling callbacks into Java, `attach_env()` attaches the pthread via `AttachCurrentThread`.
   - Upon connection loss, `channel_recv_loop` terminates immediately after invoking `on_connection_state_changed`.
   - Because `DetachCurrentThread` is never invoked, the termination of this attached POSIX pthread violates ART's lifecycle contract, leading directly to fatal abort or leaked ART Thread allocations.

2. **Scoped Storage Bottlenecks**:
   - Android 10+ deprecated external storage path access and enforced `content://` URIs.
   - `ShareReceiverActivity` currently bridges this by copying the entire file from the input stream to internal cache.
   - For multi-gigabyte media, this creates quadratic storage requirements and 15–30s of write overhead before network transfer.
   - Files received by C++ core are written directly to filesystem paths without notifying Android's `MediaScannerConnection`, preventing them from appearing in system media apps.

3. **HyperOS / Android 14 Restrictions**:
   - Android 10+ restricts background clipboard reading to the default IME or the app with active window focus.
   - HyperOS adds MIUI Battery Saver and proprietary clipboard permission controls.
   - The current architecture attempts to bypass this using `NfcTriggerActivity` (which gains focus), but omits `POST_NOTIFICATIONS` runtime permission and battery optimization exemption, leading to service termination and suppressed notifications.

---

## 3. Scoped Storage & HyperOS Compatibility Assessment

| Dimension | Modern Android (13/14) Compatibility | Xiaomi HyperOS Compatibility | Status & Gaps |
|---|---|---|---|
| **Background Clipboard Reading** | Blocked by AOSP for background services (`clipboardManager.primaryClip` returns null/empty). Requires foreground window focus. | Blocked by MIUI `SecurityServer`. Third-party apps restricted to "智能保护" or "仅使用中允许". | `NfcTriggerActivity` successfully acquires focus on tap, but background passive monitoring without ADB requires explicit user setup. Needs UI guidance for HyperOS clipboard permission. |
| **Foreground Service Persistence** | Requires `FOREGROUND_SERVICE_DATA_SYNC` and `POST_NOTIFICATIONS` runtime grant. | MIUI "神隐模式" aggressively kills background services after 10-15 mins with screen off. | Missing `POST_NOTIFICATIONS` runtime prompt. Missing `REQUEST_IGNORE_BATTERY_OPTIMIZATIONS` and HyperOS autostart navigation. |
| **Large File Sharing (`content://`)** | Requires Scoped Storage URI handling via `ParcelFileDescriptor`. | Same as AOSP. | Current implementation copies entire file to `cacheDir`, risking disk exhaustion and storage leaks. Requires `ParcelFileDescriptor` streaming. |
| **Received File Visibility** | POSIX files written to `Download/LinuxToAndroid` are not indexed into MediaStore. | Gallery/Albums rely strictly on MediaStore database. | Files are invisible in Gallery until indexed via `MediaScannerConnection.scanFile(...)`. |
| **Wi-Fi Peer Discovery** | Wi-Fi chipset drops UDP broadcast packets in powersave mode. | Aggressive Wi-Fi broadcast packet filtering. | Missing `CHANGE_WIFI_MULTICAST_STATE` permission and `WifiManager.MulticastLock`. |

---

## 4. Caveats

- **Device-Specific Testing**: Investigation was performed via static source code and architectural analysis. Dynamic runtime behaviors on specific HyperOS firmware versions (e.g. HyperOS 1.0 vs 2.0) may exhibit slight differences in permission prompt dialogs.
- **ADB AppOps Dependency**: Passive background clipboard listening on Android 10+ inherently relies on system-level privileges (`cmd appops set com.reid.l2a READ_CLIPBOARD_IN_BACKGROUND allow`). Without ADB or Root, modern Android fundamentally prevents background clipboard monitoring; `NfcTriggerActivity` serves as the primary unprivileged fallback.
- **NDK Toolchain**: The Android build requires Android SDK (API 34) and NDK (r25+). Build verification on Linux host requires `ANDROID_HOME` / `ANDROID_NDK_ROOT` environment configuration.

---

## 5. Conclusion

The Android Client Subsystem shares the high-performance C++20 core cleanly through CMake, ensuring byte-level protocol and cryptographic parity with Linux. However, several critical architectural defects exist across the JNI bridge and Android OS platform integrations:
1. **Thread Detachment & JNI Concurrency**: Exiting attached threads without `DetachCurrentThread` and holding `g_engine_mtx` during blocking `::connect` create fatal abort and ANR hazards.
2. **Scoped Storage Inefficiencies**: Buffering large files into `cacheDir` creates double-storage consumption and storage leaks, while received files are not indexed into MediaStore.
3. **HyperOS & Android 14 Lifecycle**: Missing runtime `POST_NOTIFICATIONS` and lack of battery saver exemption lead to suppressed notifications and service termination.
4. **Wi-Fi Discovery**: Omission of `MulticastLock` prevents UDP discovery packets from reaching the native core on battery/powersave.

---

## 6. Suggested Milestones & Dependencies

```
[Milestone M1: JNI Bridge Hardening & Thread Safety]
  ├── M1.1: Thread-local RAII DetachCurrentThread handler (DEFECT-01)
  ├── M1.2: Eliminate g_engine_mtx contention & non-blocking connect (DEFECT-03)
  ├── M1.3: Thread-safe g_callback_obj lifecycle & method ID caching (DEFECT-02, DEFECT-13)
  └── M1.4: JNI exception check & clear guard (DEFECT-04)

[Milestone M2: Scoped Storage & Large File Streaming]
  ├── M2.1: ParcelFileDescriptor nativeSendFileFd zero-copy streaming (DEFECT-05)
  ├── M2.2: MediaScannerConnection indexing on file completion (DEFECT-06)
  └── M2.3: ACTION_SEND_MULTIPLE support in ShareReceiverActivity (DEFECT-11)

[Milestone M3: HyperOS, Permissions & Protocol Parity]
  ├── M3.1: POST_NOTIFICATIONS runtime permission & battery exemption prompt (DEFECT-07)
  ├── M3.2: WifiManager.MulticastLock & CHANGE_WIFI_MULTICAST_STATE (DEFECT-08)
  ├── M3.3: Anti-loopback clipboard label filter & large text/image handling (DEFECT-09, DEFECT-10)
  └── M3.4: File transfer progress notification integration (DEFECT-12)
```

---

## 7. Verification Method

1. **JNI Thread Detachment & Leak Verification**:
   - Inspect `android/app/src/main/cpp/jni_bridge.cpp` lines 108-115 to confirm whether `DetachCurrentThread` is present.
   - Run Android app with `CheckJNI` enabled (`adb shell setprop debug.checkjni 1`), trigger connection drop, and monitor logcat for `art/runtime/thread.cc` warnings.
2. **Core Baseline Build and Test**:
   - Command: `cmake -B build -DL2A_BUILD_LINUX=ON && cmake --build build -j4`
   - Command: `ctest --test-dir build --output-on-failure -V`
   - Both commands currently pass 100% on Linux host.
3. **Scoped Storage Cache Inspection**:
   - Inspect `android/app/src/main/java/com/reid/l2a/ShareReceiverActivity.kt` lines 35-55 to verify copy to `cacheDir` and missing deletion.
4. **Permission & Multicast Audit**:
   - Inspect `android/app/src/main/AndroidManifest.xml` to verify missing `CHANGE_WIFI_MULTICAST_STATE` and missing runtime request in `MainActivity.kt`.
