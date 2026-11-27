# Handoff Report — reviewer_m2_c2_2

**Milestone**: M2-C2 (Android Subsystem Remediation Adversarial Review)  
**Author**: `reviewer_m2_c2_2`  
**Target Audience**: Orchestrator (`af5814b6-fe43-4f2b-b498-cb11fbccdb34`), `worker_m2_c2`  

---

## Review Summary

**Verdict**: **REQUEST_CHANGES**

No integrity violations were detected (implementations are genuine, free of hardcoded mock bypasses or facade logic). The project successfully compiles with `./gradlew assembleDebug` (7s) generating the APK artifact (`17,758,701 bytes`), and Linux CTest regression tests pass 10/10.

However, adversarial stress testing identified **3 Major** and **3 Minor** reliability, exception safety, and resource lifecycle defects that must be remediated:
1. **[Major] Scoped Storage Fallback Bypass**: In `ShareReceiverActivity.kt`, when `openFileDescriptor` returns a `ParcelFileDescriptor` but `nativeSendFileFd` returns `0L` (e.g. pipe / cloud file / non-regular file descriptor), the fallback staging copy branch is completely skipped, causing silent transmission drop.
2. **[Major] Leaked NFC Tag Connection on I/O Exception**: In `MainActivity.kt`, `ndef.connect()` and `formatable.connect()` are called without a `finally` block or safe closure; if an RF disconnection or formatting exception occurs during `writeNdefMessage` or `format`, the tag handle remains open in the Android NFC stack.
3. **[Major] Background Thread UI View Access in NFC Callback**: In `MainActivity.kt:217`, `hostIdInput.text` is accessed directly on the Binder thread from `readerCallback.onTagDiscovered` before entering `runOnUiThread`. In Android, touching view hierarchies from a non-Looper thread violates thread affinity and can throw uncaught exceptions that crash the app process.
4. **[Minor] FD & Symlink Leak on Transfer Drop**: In `jni_bridge.cpp`, active FD streams are only purged when `on_file_completed` arrives; dropped connections never send `kFileVerifiedAck`, leaking the FD until process termination.
5. **[Minor] Missing Disk Staging Cleanup**: Files copied into `cacheDir/share_staging` during stream fallback are never pruned from device cache.
6. **[Minor] Dot/Dot-Dot Handling in Display Name Resolution**: `File("..").name` evaluates to `".."` and passes unescaped to `jni_bridge.cpp`, colliding with the parent `fd_streams` directory.

---

## 1. Observation

### 1.1 Scoped Storage Streaming & Fallback in `ShareReceiverActivity.kt`
- Location: `android/app/src/main/java/com/reid/l2a/ShareReceiverActivity.kt:98-125`
```kotlin
98:                     var pfd: ParcelFileDescriptor? = null
99:                     try {
100:                         pfd = contentResolver.openFileDescriptor(uri, "r")
101:                     } catch (_: Exception) {}
102: 
103:                     if (pfd != null) {
104:                         val fileSize = pfd.statSize
105:                         val fd = pfd.detachFd()
106:                         val batchId = CoreBridge.nativeSendFileFd(fd, displayName, fileSize)
107:                         if (batchId != 0L) {
108:                             sentCount++
109:                         }
110:                     } else {
111:                         // Fallback to stream copy into cacheDir staging file
112:                         val stagingDir = File(cacheDir, "share_staging").apply { mkdirs() }
113:                         val tempFile = File(stagingDir, "${System.currentTimeMillis()}_$displayName")
114:                         contentResolver.openInputStream(uri)?.use { input ->
115:                             tempFile.outputStream().use { output ->
116:                                 input.copyTo(output)
117:                             }
118:                         }
119:                         if (tempFile.exists() && tempFile.length() > 0) {
120:                             val batchId = CoreBridge.nativeSendFile(tempFile.absolutePath)
121:                             if (batchId != 0L) {
122:                                 sentCount++
123:                             }
124:                         }
125:                     }
```
- In `jni_bridge.cpp:554-567`:
```cpp
    std::string proc_fd = "/proc/self/fd/" + std::to_string(fd);
    if (::symlink(proc_fd.c_str(), link_path.c_str()) != 0) {
        LOGE("Failed to create symlink for fd %d: %s", fd, link_path.c_str());
        std::filesystem::remove_all(dir_path, ec);
        ::close(fd);
        return 0;
    }

    std::vector<std::filesystem::path> paths = {link_path};
    uint64_t batch_id = engine->send_files(paths);
    if (batch_id == 0) {
        std::filesystem::remove_all(dir_path, ec);
        ::close(fd);
        return 0;
    }
```
- In `core/src/file_transfer.cpp:113-115`:
```cpp
    if (!std::filesystem::is_regular_file(abs_path, ec)) {
        return;
    }
```
If a ContentProvider provides a pipe or FIFO (e.g. cloud stream), `is_regular_file` is `false`, `engine->send_files` returns 0, and `nativeSendFileFd` returns `0L`. Because `pfd != null`, lines 110-125 are skipped.

### 1.2 Unclosed NFC Connection in `MainActivity.kt`
- Location: `android/app/src/main/java/com/reid/l2a/MainActivity.kt:243-274` and `280-285`
```kotlin
243:             return try {
244:                 ndef.connect()
245:                 if (!ndef.isWritable) {
246:                     ndef.close()
247:                     return Pair(false, "标签处于只读状态")
248:                 }
249:                 if (ndef.maxSize < msg.byteArrayLength) {
250:                     val maxSize = ndef.maxSize
251:                     val reqSize = msg.byteArrayLength
252:                     ndef.close()
253:                     return Pair(false, "容量不足 ($maxSize < $reqSize)")
254:                 }
255:                 ndef.writeNdefMessage(msg)
256:                 ndef.close()
257:                 Pair(true, "")
258:             } catch (e: Exception) {
259:                 Pair(false, e.message ?: "NDEF 写入失败")
260:             }
```
And:
```kotlin
266:             return try {
267:                 formatable.connect()
268:                 formatable.format(msg)
269:                 formatable.close()
270:                 Pair(true, "")
271:             } catch (e: Exception) {
272:                 Pair(false, e.message ?: "标签格式化失败")
273:             }
```
And:
```kotlin
281:         try {
282:             ndef.connect()
283:             val msg = ndef.ndefMessage
284:             ndef.close()
```
If `ndef.writeNdefMessage(msg)` or `formatable.format(msg)` or `ndef.ndefMessage` throws an exception, `close()` is never invoked.

### 1.3 Thread Safety in NFC Reader Callback
- Location: `android/app/src/main/java/com/reid/l2a/MainActivity.kt:179-181` & `215-220`:
```kotlin
179:     private val readerCallback = NfcAdapter.ReaderCallback { tag ->
180:         handleTagDiscovered(tag)
181:     }
...
215:     private fun handleTagDiscovered(tag: Tag) {
216:         if (writeNfcMode) {
217:             val hostId = hostIdInput.text.toString().trim()
218:             val uri = "l2a://tap?host_id=$hostId&name=Redmi-G-Pro-2024&port=53321"
219:             val (success, errorMsg) = writeTagNdefOrFormatable(tag, uri)
220:             runOnUiThread {
```
`readerCallback.onTagDiscovered(tag)` is called by the Android NFC service on an arbitrary Binder thread. `hostIdInput` is an `EditText` initialized on the main looper. Line 217 executes on the Binder thread before `runOnUiThread` on line 220.

### 1.4 Path Traversal Defense in `resolveCleanDisplayName`
- Location: `android/app/src/main/java/com/reid/l2a/ShareReceiverActivity.kt:170-172`:
```kotlin
170:         var cleanName = File(baseName).name
171: 
172:         if (!cleanName.contains('.')) {
```
Evaluated in Java 21 runtime:
`new java.io.File("..").getName()` returns `".."`.
`cleanName.contains('.')` is true for `".."`. Line 172 condition is false, leaving `cleanName` as `".."`.
In `jni_bridge.cpp:548-552`:
`link_path = dir_path / name;` -> `base / "fd_streams" / "<fd>_<seq>" / ".."` resolves to `base / "fd_streams"`.
`::symlink(proc_fd.c_str(), link_path.c_str())` attempts to create a symlink over the directory `fd_streams`.

### 1.5 Build Verification
Command:
```bash
JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64 ANDROID_USER_HOME=/home/reid-pu/Programming/Linux_to_Android/.android ./gradlew assembleDebug -Duser.home=/home/reid-pu/Programming/Linux_to_Android
```
Result:
```
BUILD SUCCESSFUL in 7s
41 actionable tasks: 5 executed, 36 up-to-date
```
APK size: 17,758,701 bytes.

---

## 2. Logic Chain

1. **Scoped Storage Fallback Logic**:
   - Upstream requirement R3 and `PROJECT.md` REAL-08 specify supporting Scoped Storage `ParcelFileDescriptor` streaming with fallback to `cacheDir` staging copy.
   - Observation 1.1 shows that when `openFileDescriptor` returns a valid PFD, but `nativeSendFileFd` cannot stream it (e.g., pipes or FIFO created by virtual/cloud ContentProviders), `batchId == 0L`.
   - Because the code uses `if (pfd != null) { ... } else { ... }`, the staging fallback is structured to execute only when `pfd == null`.
   - Consequence: Transmission fails silently and completely for any URI where `pfd != null` but zero-copy symlink streaming returns 0.

2. **NFC Connection Resource Leaks**:
   - Observation 1.2 demonstrates that `connect()` is executed, but `close()` is positioned at the bottom of the `try` block.
   - Any hardware I/O exception (`TagLostException`, `IOException`, `FormatException`) jumps directly to `catch (e: Exception)` without executing `close()`.
   - Consequence: Tag communication channel remains open in Android's NFC service, frequently causing subsequent tag taps to fail until the device is removed from the RF field for several seconds.

3. **Android View Threading Violation**:
   - Observation 1.3 shows that `hostIdInput.text` is evaluated on the Binder thread.
   - Android Views are not thread-safe and enforce thread checking in many operations.
   - Consequence: Potential `CalledFromWrongThreadException` or corrupted string reads, which would bubble out of `onTagDiscovered` and kill the process.

4. **Dot-Dot Path Normalization**:
   - Observation 1.4 confirms that `File("..").name` does not remove `".."` or `"."`.
   - Consequence: When a URI resolves to `".."`, the symlink path collides with the `fd_streams` directory.

---

## 3. Findings

### [Major] Finding 1: Scoped Storage Stream Copy Fallback Bypassed on Native Send Failure
- **What**: Staging stream copy is skipped when `openFileDescriptor` succeeds but `nativeSendFileFd` returns 0.
- **Where**: `android/app/src/main/java/com/reid/l2a/ShareReceiverActivity.kt:103-125`
- **Why**: Many ContentProviders (e.g. cloud drives, document providers) return a pipe PFD or non-seekable descriptor. `nativeSendFileFd` fails with 0, and because `pfd != null`, the stream copy fallback is never reached.
- **Suggestion**:
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

### [Major] Finding 2: NFC Tag Connections Leaked on I/O Exception
- **What**: `ndef.close()` and `formatable.close()` are not protected by `finally` blocks.
- **Where**: `android/app/src/main/java/com/reid/l2a/MainActivity.kt:243-274` and `280-285`
- **Why**: When a write or format fails due to RF detachment, the tag is left connected, blocking retries.
- **Suggestion**:
  Use `finally { try { ndef.close() } catch (_: Exception) {} }` or Kotlin's `.use { ... }` block for both `Ndef` and `NdefFormatable`.

### [Major] Finding 3: Non-UI Thread Accessing `EditText.text` in NFC Reader Callback
- **What**: `hostIdInput.text` is accessed on a Binder background thread in `handleTagDiscovered`.
- **Where**: `android/app/src/main/java/com/reid/l2a/MainActivity.kt:217`
- **Why**: `hostIdInput` is a UI widget. Touching UI widgets from non-main threads violates Android's UI threading model.
- **Suggestion**: Cache `targetHostId` in a member variable on the main thread when `btnWriteNfc` is clicked, or read `hostIdInput.text.toString()` on the main thread before starting the write operation. Wrap the entire body of `handleTagDiscovered` in a top-level `try/catch` to ensure exceptions cannot crash the app.

### [Minor] Finding 4: File Descriptor and Symlink Leak on Transfer Interruption
- **What**: Active FD stream entries in `g_active_fd_streams` are only closed upon receiving `kFileVerifiedAck`.
- **Where**: `android/app/src/main/cpp/jni_bridge.cpp:230-260`
- **Why**: If a network connection drops mid-transfer, `on_file_completed` is never invoked, leaving the FD open and the symlink on disk until service shutdown.
- **Suggestion**: Close `s.fd` and clean up `dir_path` when `on_connection_state_changed(false)` is reported, or once `engine->send_files()` finishes reading the local file.

### [Minor] Finding 5: `cacheDir/share_staging` Temporary Files Never Cleaned
- **What**: Staging files written during stream copy fallback accumulate permanently in `cacheDir/share_staging`.
- **Where**: `android/app/src/main/java/com/reid/l2a/ShareReceiverActivity.kt:113`
- **Why**: No deletion logic is scheduled or executed after transfer completion.
- **Suggestion**: Add a cleanup routine in `L2AService.onCreate()` to delete stale files in `File(cacheDir, "share_staging")`.

### [Minor] Finding 6: Relative Directory Component Escape in `resolveCleanDisplayName`
- **What**: `cleanName` can be `".."` or `"."` or empty string if `rawName` is composed of relative path dots.
- **Where**: `android/app/src/main/java/com/reid/l2a/ShareReceiverActivity.kt:170`
- **Why**: `File("..").name` evaluates to `".."`, which bypasses extension appending and causes `dir_path / name` to collide with `dir_path`'s parent.
- **Suggestion**:
  ```kotlin
  if (cleanName.isBlank() || cleanName == "." || cleanName == "..") {
      cleanName = "shared_${System.currentTimeMillis()}"
  }
  ```

---

## 4. Caveats

- Physical NFC RF hardware testing with the touchpad NTAG5332 requires tactile physical interaction with the Redmi G Pro laptop touchpad; verification was conducted via static control-flow analysis and mock simulation.
- `assembleDebug` was compiled with JDK 21.0.12 and CMake 3.22.1 on Linux.

---

## 5. Conclusion

Milestone M2-C2 contains genuine, high-quality implementations for USB port alignment (53323), reader mode upgrade, and dual-fallback connection logic. However, due to the **3 Major findings** (Scoped Storage fallback bypass, NFC unclosed tag leak on exception, and Binder thread UI view access), changes are requested before milestone sign-off.

**Verdict**: **REQUEST_CHANGES**

---

## 6. Verification Method

1. **Build Verification**:
   ```bash
   JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64 \
   ANDROID_USER_HOME=/home/reid-pu/Programming/Linux_to_Android/.android \
   ./gradlew assembleDebug -Duser.home=/home/reid-pu/Programming/Linux_to_Android
   ```
2. **CTest Regression**:
   ```bash
   ctest --test-dir build --output-on-failure
   ```
3. **Inspect Modified Files**:
   - `android/app/src/main/java/com/reid/l2a/ShareReceiverActivity.kt`
   - `android/app/src/main/java/com/reid/l2a/MainActivity.kt`
   - `android/app/src/main/cpp/jni_bridge.cpp`
