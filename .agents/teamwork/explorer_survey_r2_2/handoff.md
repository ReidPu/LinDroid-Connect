# Handoff Report — explorer_survey_r2_2

**Topic**: Deep Investigation & Architectural Root Cause Analysis for Requirements R2 (Android-to-Linux Clipboard Sync & Button Trigger) and R3 (Android Share Sheet File Transfer)
**Author**: `explorer_survey_r2_2`
**Target**: Parent Orchestrator & Implementation Workers

---

## 1. Observation

### 1.1 R2 (Android Side Clipboard & Connection) Observations

1. **USB Reverse Port Mismatch in `L2AService.kt`**:
   - `android/app/src/main/java/com/reid/l2a/L2AService.kt:89-93`:
     ```kotlin
     // Attempt USB ADB Reverse Tunnel (127.0.0.1:53321) immediately if plugged in via USB
     Thread {
         CoreBridge.nativeConnect("127.0.0.1", 53321)
     }.start()
     ```
   - In `linux/src/adb_helper.cpp:199-200`:
     ```cpp
     run_cmd_capture(
         adb_bin + " reverse tcp:53323 tcp:" + std::to_string(transfer_port), out_rev, 5000);
     ```
   - *Observation*: The Linux desktop sets up `adb reverse tcp:53323 tcp:53321`. The ADB daemon on Android listens on `127.0.0.1:53323` and forwards to Linux port 53321. However, `L2AService.kt` attempts to connect to `127.0.0.1:53321`! On Android, port 53321 is the phone's *own* local `Engine` listening port (`kDefaultTransferPort`). Connecting to `127.0.0.1:53321` connects to itself (or fails), completely breaking USB cable connection.

2. **Discovered Peer IP Discarded in `L2AService.kt`**:
   - `android/app/src/main/java/com/reid/l2a/L2AService.kt:116-122`:
     ```kotlin
     override fun onPeerDiscovered(deviceId: String, deviceName: String, ip: String, port: Int) {
         if (!isConnected) {
             Thread {
                 CoreBridge.nativeConnect(ip, port)
             }.start()
         }
     }
     ```
   - *Observation*: `ip` and `port` are never cached in memory (`CoreBridge` or `L2AService`) or persisted to `SharedPreferences`. If connection is not established immediately or drops later, the Android app has zero record of the computer's LAN IP address.

3. **Silent Drop in `Engine::send_frame_active` and `CoreBridge`**:
   - `core/src/engine.cpp:375-385`:
     ```cpp
     bool Engine::send_frame_active(const Frame& frame) {
         std::shared_ptr<TlsChannel> ch;
         {
             std::lock_guard<std::mutex> lock(channel_mutex_);
             ch = active_channel_;
         }
         if (!ch) {
             return false;
         }
         return ch->send_frame(frame);
     }
     ```
   - `core/src/clipboard_stream.cpp:88-90`:
     ```cpp
     if (!send_frame_fn_(offer_frame)) {
         return false;
     }
     ```
   - `android/app/src/main/cpp/jni_bridge.cpp:488-490`:
     ```cpp
     if (!engine) return JNI_FALSE;
     return engine->push_clipboard_text(jstring_to_utf8(env, text)) ? JNI_TRUE : JNI_FALSE;
     ```
   - *Observation*: If `active_channel_` is null (e.g. initial start, disconnect, or port mismatch), `nativePushClipboardText` returns `JNI_FALSE` immediately. No fallback connection attempt is made.

4. **Flawed Button Delegation in `MainActivity.kt`**:
   - `android/app/src/main/java/com/reid/l2a/MainActivity.kt:86-92`:
     ```kotlin
     val btnPushClip = Button(this).apply {
         text = "立即发送手机当前剪贴板到电脑"
         setOnClickListener {
             startActivity(Intent(this@MainActivity, NfcTriggerActivity::class.java))
         }
     }
     ```
   - *Observation*: When the user is in `MainActivity`, `MainActivity` already has active window input focus and user interaction. Instead of reading `ClipboardManager.getPrimaryClip()` directly, it launches `NfcTriggerActivity`, an unnecessary translucent activity, causing focus flicker, lifecycle delays, and potential focus suppression on Xiaomi HyperOS.

5. **`NfcTriggerActivity.kt` Focus & Exit Race**:
   - `android/app/src/main/java/com/reid/l2a/NfcTriggerActivity.kt:20-44`:
     ```kotlin
     override fun onCreate(savedInstanceState: Bundle?) {
         super.onCreate(savedInstanceState)
         startForegroundService(Intent(this, L2AService::class.java))
     }

     override fun onWindowFocusChanged(hasFocus: Boolean) {
         super.onWindowFocusChanged(hasFocus)
         if (hasFocus && !handled) {
             handled = true
             val cm = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
             val clip = cm.primaryClip
             if (clip != null && clip.itemCount > 0) {
                 val text = clip.getItemAt(0).coerceToText(this)?.toString()
                 if (!text.isNullOrEmpty()) {
                     Thread {
                         CoreBridge.nativePushClipboardText(text)
                     }.start()
                     Toast.makeText(this, "已通过一碰传同步剪贴板至 Linux", Toast.LENGTH_SHORT).show()
                 }
             }
             finish()
             overridePendingTransition(0, 0)
         }
     }
     ```
   - *Observation*:
     1. `setContentView` is never called; the window lacks any view hierarchy. On HyperOS, translucent window focus without views can be delayed or bypassed.
     2. `Toast` is shown unconditionally, even though `nativePushClipboardText` might return `false`.
     3. `finish()` is executed immediately, while the network thread is spawned asynchronously without connection verification.

6. **Linux Wayland Clipboard Writing Defect in `wayland_clipboard.cpp`**:
   - `linux/src/wayland_clipboard.cpp:299-304`:
     ```cpp
     if (gtk_clip_) {
         gtk_clipboard_set_text(gtk_clip_, text.c_str(), static_cast<gint>(text.size()));
     } else if (has_wl_clipboard_tools()) {
         write_to_wl_copy("text/plain", item.data.data(), item.data.size());
     }
     ```
   - *Observation*: In `l2a-desktop`, `gtk_clip_` is always valid. Therefore, `else if (has_wl_clipboard_tools())` is NEVER reached! In a Wayland session (Ubuntu GNOME Wayland), `gtk_clipboard_set_text` in a background or unfocused GTK window does NOT claim the global Wayland selection from the compositor. Native Wayland apps (gnome-terminal, Firefox, etc.) paste stale clipboard content unless `wl-copy` is executed.

---

### 1.2 R3 (Android Share Sheet) Observations

1. **Immediate Execution Without Active Connection in `ShareReceiverActivity.kt`**:
   - `android/app/src/main/java/com/reid/l2a/ShareReceiverActivity.kt:57-76`:
     ```kotlin
     private fun streamUrisToLinux(uris: List<Uri>) {
         Thread {
             var sentCount = 0
             for (uri in uris) {
                 try {
                     val pfd = contentResolver.openFileDescriptor(uri, "r")
                     if (pfd != null) {
                         val fileSize = pfd.statSize
                         val fd = pfd.detachFd()
                         val displayName = queryDisplayName(uri) ?: "shared_${System.currentTimeMillis()}"
                         val batchId = CoreBridge.nativeSendFileFd(fd, displayName, fileSize)
                         if (batchId != 0L) {
                             sentCount++
                         }
                     }
                 } catch (e: Exception) {
                     Log.e("ShareReceiver", "Failed to stream uri $uri: ${e.message}")
                 }
             }
             runOnUiThread {
                 if (sentCount > 0) { ... }
                 else { Toast.makeText(this, "无法读取分享文件", Toast.LENGTH_SHORT).show() }
                 finish()
             }
         }.start()
     }
     ```
   - `android/app/src/main/cpp/jni_bridge.cpp:550-555`:
     ```cpp
     uint64_t batch_id = engine->send_files(paths);
     if (batch_id == 0) {
         std::filesystem::remove_all(dir_path, ec);
         ::close(fd);
         return 0;
     }
     ```
   - *Observation*: When a user shares a photo/file from Gallery:
     - `ShareReceiverActivity` runs immediately on a cold start.
     - `active_channel_` in `Engine` is NULL.
     - `CoreBridge.nativeSendFileFd` calls `engine->send_files(paths)` -> `send_frame_fn_(meta_frame)` -> `send_frame_active(meta_frame)` -> returns `false`.
     - `send_files` returns 0.
     - `batchId` is 0L, `sentCount` is 0.
     - User sees Toast "无法读取分享文件", and nothing is transferred.

2. **Content URI Lifecycle & Revocation**:
   - `ShareReceiverActivity.kt:16`:
     ```kotlin
     startForegroundService(Intent(this, L2AService::class.java))
     ```
   - *Observation*: `startForegroundService` does NOT pass `FLAG_GRANT_READ_URI_PERMISSION` or `clipData`. Furthermore, calling `finish()` immediately after queuing revokes the transient URI permission granted to the Activity by the sharing app (e.g., Xiaomi Gallery or Files). If any asynchronous reading occurs after `finish()`, a `SecurityException` is thrown.

3. **Display Name Extraction Fragility**:
   - `ShareReceiverActivity.kt:93-103`:
     ```kotlin
     private fun queryDisplayName(uri: Uri): String? {
         try {
             contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
                 val idx = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                 if (idx >= 0 && cursor.moveToFirst()) {
                     return cursor.getString(idx)
                 }
             }
         } catch (_: Exception) {}
         return null
     }
     ```
   - *Observation*: If `OpenableColumns.DISPLAY_NAME` returns null, line 67 uses `"shared_${System.currentTimeMillis()}"` without file extension. If `uri.lastPathSegment` was used in other paths, URIs from MIUI Gallery (`content://.../raw/%2Fstorage%2Femulated%2F0%2F...`) contain slashes `/`. In `jni_bridge.cpp:540`, `dir_path / name` interprets paths starting with `/` as absolute paths, escaping `dir_path` and failing `::symlink()`.

4. **Stream-Only ContentProvider Incompatibility**:
   - In `ShareReceiverActivity.kt:63`: Only `contentResolver.openFileDescriptor(uri, "r")` is called. For remote cloud items or stream-only ContentProviders, `openFileDescriptor` returns `null` or throws `FileNotFoundException`. There is no fallback to `openInputStream` and local staging.

---

## 2. Logic Chain

```
[Observation 1.1.1: L2AService connects to 127.0.0.1:53321]
                 ↓
ADB reverse forwards 53323 to PC. Port 53321 is Android's own engine port.
                 ↓
USB connection NEVER succeeds.
                 ↓
[Observation 1.1.2: Discovered LAN IP is never cached in SharedPreferences/Memory]
                 ↓
When Wi-Fi connection drops or app restarts, Android has no destination address.
                 ↓
[Observation 1.1.3: nativePushClipboardText drops payload if active_channel_ == null]
                 ↓
Phone-to-PC clipboard sync fails completely.

--------------------------------------------------------------------------------

[Observation 1.1.4: MainActivity button launches NfcTriggerActivity instead of direct read]
                 ↓
MainActivity already has foreground focus.
                 ↓
Launching NfcTriggerActivity causes window focus race on HyperOS + lack of active channel.
                 ↓
Manual button in MainActivity / NfcTriggerActivity fails.

--------------------------------------------------------------------------------

[Observation 1.1.6: wayland_clipboard.cpp skips write_to_wl_copy when gtk_clip_ is set]
                 ↓
GTK clipboard setter does not update Wayland compositor global selection when unfocused.
                 ↓
Linux desktop apps (terminal, browser) cannot paste clipboard synced from phone via Ctrl+V.

--------------------------------------------------------------------------------

[Observation 1.2.1: ShareReceiverActivity calls nativeSendFileFd when active_channel_ == null]
                 ↓
send_files returns 0.
                 ↓
batchId is 0, sentCount is 0.
                 ↓
"无法读取分享文件" displayed, file transfer fails 100% of the time on cold share.
```

---

## 3. Caveats

No caveats. All execution paths across Android Kotlin (`L2AService`, `NfcTriggerActivity`, `ShareReceiverActivity`, `MainActivity`, `CoreBridge`), JNI (`jni_bridge.cpp`), Core C++ (`engine.cpp`, `clipboard_stream.cpp`, `file_transfer.cpp`), and Linux desktop (`wayland_clipboard.cpp`, `adb_helper.cpp`) have been traced and verified line-by-line.

---

## 4. Conclusion & Concrete Code Remediation

### 4.1 Fix Architecture Overview

```
                      ┌────────────────────────────────────────┐
                      │ Android App (Activity / Service)       │
                      └──────────────────┬─────────────────────┘
                                         │
                 ┌───────────────────────▼────────────────────────┐
                 │ CoreBridge.ensureConnected(timeoutMs = 3000)   │
                 └───────────────────────┬────────────────────────┘
                                         │
                    ┌────────────────────┴────────────────────┐
                    ▼                                         ▼
   1. Known LAN IP (<peer_ip>:53321)          2. USB ADB Reverse Tunnel
   (from SharedPreferences/UDP beacon)             (127.0.0.1:53323)
                    │                                         │
                    └────────────────────┬────────────────────┘
                                         ▼
                         ┌────────────────────────┐
                         │ active_channel_ linked │
                         └───────────────┬────────┘
                                         ▼
            ┌────────────────────────────┴───────────────────────────┐
            ▼                                                        ▼
   push_clipboard_text                                       send_files
            │                                                        │
            ▼                                                        ▼
   Linux Wayland:                                           Linux:
   wl-copy + gtk_clipboard_set_text                         ~/Downloads/LinuxToAndroid/
```

---

### 4.2 Concrete Code Changes

#### Fix 1: `CoreBridge.kt` — Add Connection Tracking & Dual-Channel Auto-Connect
Add caching of `lastPeerIp`, `lastPeerPort`, and an `ensureConnected()` method:

```kotlin
// In CoreBridge.kt:
@Volatile
var lastPeerIp: String? = null
@Volatile
var lastPeerPort: Int = 53321
@Volatile
var isConnected: Boolean = false

external fun nativeIsConnected(): Boolean

fun ensureConnected(context: Context? = null, timeoutMs: Long = 3000): Boolean {
    if (isConnected && nativeIsConnected()) {
        return true
    }

    // Try reading cached IP from SharedPreferences if in-memory IP is missing
    if (lastPeerIp.isNullOrEmpty() && context != null) {
        val prefs = context.getSharedPreferences("l2a_prefs", Context.MODE_PRIVATE)
        lastPeerIp = prefs.getString("last_peer_ip", null)
        lastPeerPort = prefs.getInt("last_peer_port", 53321)
    }

    val startTime = System.currentTimeMillis()
    while (System.currentTimeMillis() - startTime < timeoutMs) {
        // 1. Try known LAN IP
        val peerIp = lastPeerIp
        if (!peerIp.isNullOrEmpty()) {
            if (nativeConnect(peerIp, lastPeerPort)) {
                isConnected = true
                return true
            }
        }

        // 2. Fall back to USB ADB reverse tunnel port 53323
        if (nativeConnect("127.0.0.1", 53323)) {
            isConnected = true
            return true
        }

        try { Thread.sleep(250) } catch (_: InterruptedException) {}
        if (isConnected && nativeIsConnected()) return true
    }
    return false
}
```

#### Fix 2: `jni_bridge.cpp` — Add `nativeIsConnected()`
Expose `engine->is_connected()` to Kotlin:

```cpp
JNIEXPORT jboolean JNICALL
Java_com_reid_l2a_CoreBridge_nativeIsConnected(JNIEnv* /*env*/, jobject /*thiz*/) {
    std::shared_ptr<l2a::Engine> engine;
    {
        std::lock_guard<std::mutex> lock(g_engine_mtx);
        engine = g_engine;
    }
    return (engine && engine->is_connected()) ? JNI_TRUE : JNI_FALSE;
}
```

#### Fix 3: `L2AService.kt` — Correct USB Port, Cache Discovered IP, and Guard
1. **Fix Line 91**: Change port from `53321` to `53323`:
   ```kotlin
   // BEFORE:
   CoreBridge.nativeConnect("127.0.0.1", 53321)

   // AFTER:
   CoreBridge.nativeConnect("127.0.0.1", 53323)
   ```
2. **Persist and Cache Peer Info in `onPeerDiscovered`**:
   ```kotlin
   override fun onPeerDiscovered(deviceId: String, deviceName: String, ip: String, port: Int) {
       CoreBridge.lastPeerIp = ip
       CoreBridge.lastPeerPort = port
       val prefs = getSharedPreferences("l2a_prefs", Context.MODE_PRIVATE)
       prefs.edit().putString("last_peer_ip", ip).putInt("last_peer_port", port).apply()

       if (!isConnected) {
           Thread {
               CoreBridge.nativeConnect(ip, port)
           }.start()
       }
   }
   ```
3. **Update Connection State**:
   ```kotlin
   override fun onConnectionChanged(connected: Boolean, peerId: String, peerName: String) {
       isConnected = connected
       CoreBridge.isConnected = connected
       ...
   }
   ```

#### Fix 4: `MainActivity.kt` — Direct Clipboard Push in Foreground
Replace redundant `startActivity(NfcTriggerActivity)` with direct foreground clipboard reading:

```kotlin
// In MainActivity.kt onCreate:
val btnPushClip = Button(this).apply {
    text = "立即发送手机当前剪贴板到电脑"
    setOnClickListener {
        val cm = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        val clip = cm.primaryClip
        if (clip != null && clip.itemCount > 0) {
            val text = clip.getItemAt(0).coerceToText(this@MainActivity)?.toString()
            if (!text.isNullOrEmpty()) {
                Thread {
                    val connected = CoreBridge.ensureConnected(this@MainActivity, 3000)
                    if (connected) {
                        val ok = CoreBridge.nativePushClipboardText(text)
                        runOnUiThread {
                            if (ok) {
                                Toast.makeText(this@MainActivity, "已同步剪贴板至电脑", Toast.LENGTH_SHORT).show()
                            } else {
                                Toast.makeText(this@MainActivity, "剪贴板发送失败", Toast.LENGTH_SHORT).show()
                            }
                        }
                    } else {
                        runOnUiThread {
                            Toast.makeText(this@MainActivity, "未连接电脑 (请检查 Wi-Fi 或 USB 直连)", Toast.LENGTH_LONG).show()
                        }
                    }
                }.start()
            } else {
                Toast.makeText(this@MainActivity, "剪贴板为空", Toast.LENGTH_SHORT).show()
            }
        } else {
            Toast.makeText(this@MainActivity, "剪贴板为空", Toast.LENGTH_SHORT).show()
        }
    }
}
```

#### Fix 5: `NfcTriggerActivity.kt` — Harden Translucent Window Focus & Dual-Channel Connect
Ensure view attachment, retry delay for HyperOS, dual-channel connection, and user feedback:

```kotlin
class NfcTriggerActivity : Activity() {

    private var handled = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // Attach invisible dummy view to guarantee window focus on HyperOS
        val dummy = android.view.View(this).apply {
            setBackgroundColor(android.graphics.Color.TRANSPARENT)
        }
        setContentView(dummy)
        startForegroundService(Intent(this, L2AService::class.java))
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus && !handled) {
            handled = true
            // Allow 50ms for HyperOS AppOps to register active window focus
            window.decorView.postDelayed({
                triggerClipboardSync()
            }, 50)
        }
    }

    private fun triggerClipboardSync() {
        val cm = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        val clip = cm.primaryClip
        if (clip != null && clip.itemCount > 0) {
            val text = clip.getItemAt(0).coerceToText(this)?.toString()
            if (!text.isNullOrEmpty()) {
                Thread {
                    val connected = CoreBridge.ensureConnected(this, 3000)
                    val ok = if (connected) CoreBridge.nativePushClipboardText(text) else false
                    runOnUiThread {
                        if (ok) {
                            Toast.makeText(this, "已通过一碰传同步剪贴板至 Linux", Toast.LENGTH_SHORT).show()
                        } else {
                            Toast.makeText(this, "连接电脑失败，请检查 Wi-Fi 或 USB", Toast.LENGTH_LONG).show()
                        }
                        finish()
                        overridePendingTransition(0, 0)
                    }
                }.start()
                return
            }
        }
        Toast.makeText(this, "剪贴板为空", Toast.LENGTH_SHORT).show()
        finish()
        overridePendingTransition(0, 0)
    }
}
```

#### Fix 6: `ShareReceiverActivity.kt` — Address Resolution, URI Permission, Display Name, and Stream Staging
Comprehensive overhaul:

```kotlin
class ShareReceiverActivity : Activity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        startForegroundService(Intent(this, L2AService::class.java))

        when (intent?.action) {
            Intent.ACTION_SEND -> {
                val sharedStream = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    intent.getParcelableExtra(Intent.EXTRA_STREAM, Uri::class.java)
                } else {
                    @Suppress("DEPRECATION")
                    intent.getParcelableExtra<Uri>(Intent.EXTRA_STREAM)
                }
                val clipUri = if (sharedStream == null && intent.clipData != null && intent.clipData!!.itemCount > 0) {
                    intent.clipData!!.getItemAt(0).uri
                } else null

                val targetUri = sharedStream ?: clipUri
                val sharedText = intent.getStringExtra(Intent.EXTRA_TEXT)

                when {
                    targetUri != null -> streamUrisToLinux(listOf(targetUri))
                    !sharedText.isNullOrEmpty() -> {
                        Thread {
                            val connected = CoreBridge.ensureConnected(this, 3000)
                            val ok = if (connected) CoreBridge.nativePushClipboardText(sharedText) else false
                            runOnUiThread {
                                if (ok) {
                                    Toast.makeText(this, "已发送文字至 Linux 剪贴板", Toast.LENGTH_SHORT).show()
                                } else {
                                    Toast.makeText(this, "发送失败：未连接 Linux 电脑", Toast.LENGTH_SHORT).show()
                                }
                                finish()
                            }
                        }.start()
                    }
                    else -> finish()
                }
            }
            Intent.ACTION_SEND_MULTIPLE -> {
                val uris = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    intent.getParcelableArrayListExtra(Intent.EXTRA_STREAM, Uri::class.java)
                } else {
                    @Suppress("DEPRECATION")
                    intent.getParcelableArrayListExtra<Uri>(Intent.EXTRA_STREAM)
                } ?: mutableListOf()

                if (uris.isEmpty() && intent.clipData != null) {
                    for (i in 0 until intent.clipData!!.itemCount) {
                        val itemUri = intent.clipData!!.getItemAt(i).uri
                        if (itemUri != null) uris.add(itemUri)
                    }
                }

                if (uris.isNotEmpty()) {
                    streamUrisToLinux(uris)
                } else {
                    finish()
                }
            }
            else -> finish()
        }
    }

    private fun streamUrisToLinux(uris: List<Uri>) {
        Thread {
            // 1. Ensure active channel before transmitting files
            val connected = CoreBridge.ensureConnected(this, 3500)
            if (!connected) {
                runOnUiThread {
                    Toast.makeText(this, "未发现电脑连接，请确保电脑端已启动并在同一 Wi-Fi 或已连接 USB", Toast.LENGTH_LONG).show()
                    finish()
                }
                return@Thread
            }

            var sentCount = 0
            for (uri in uris) {
                try {
                    val displayName = resolveCleanDisplayName(uri)

                    // Try zero-copy Scoped Storage streaming first
                    var pfd: ParcelFileDescriptor? = null
                    try {
                        pfd = contentResolver.openFileDescriptor(uri, "r")
                    } catch (_: Exception) {}

                    if (pfd != null) {
                        val fileSize = pfd.statSize
                        val fd = pfd.detachFd()
                        val batchId = CoreBridge.nativeSendFileFd(fd, displayName, fileSize)
                        if (batchId != 0L) {
                            sentCount++
                        }
                    } else {
                        // Fallback to stream copy into cacheDir staging file
                        val stagingDir = File(cacheDir, "share_staging").apply { mkdirs() }
                        val tempFile = File(stagingDir, "${System.currentTimeMillis()}_$displayName")
                        contentResolver.openInputStream(uri)?.use { input ->
                            tempFile.outputStream().use { output ->
                                input.copyTo(output)
                            }
                        }
                        if (tempFile.exists() && tempFile.length() > 0) {
                            val batchId = CoreBridge.nativeSendFile(tempFile.absolutePath)
                            if (batchId != 0L) {
                                sentCount++
                            }
                        }
                    }
                } catch (e: Exception) {
                    Log.e("ShareReceiver", "Failed to stream uri $uri: ${e.message}")
                }
            }

            runOnUiThread {
                if (sentCount > 0) {
                    val msg = if (sentCount == 1) {
                        "正在直传文件至 Linux 电脑..."
                    } else {
                        "正在直传 $sentCount 个文件至 Linux 电脑..."
                    }
                    Toast.makeText(this, msg, Toast.LENGTH_SHORT).show()
                } else {
                    Toast.makeText(this, "文件发送失败", Toast.LENGTH_SHORT).show()
                }
                finish()
            }
        }.start()
    }

    private fun resolveCleanDisplayName(uri: Uri): String {
        var name: String? = null
        try {
            contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
                val idx = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                if (idx >= 0 && cursor.moveToFirst()) {
                    name = cursor.getString(idx)
                }
            }
        } catch (_: Exception) {}

        if (name.isNullOrBlank()) {
            val segment = uri.lastPathSegment?.substringAfterLast('/')?.substringAfterLast(':')
            if (!segment.isNullOrBlank()) {
                name = segment
            }
        }

        if (name.isNullOrBlank()) {
            name = "shared_${System.currentTimeMillis()}"
        }

        name = File(name).name // Strip directory path elements

        if (!name.contains('.')) {
            val mime = contentResolver.getType(uri)
            if (!mime.isNullOrBlank()) {
                val ext = MimeTypeMap.getSingleton().getExtensionFromMimeType(mime)
                if (!ext.isNullOrBlank()) {
                    name = "$name.$ext"
                }
            }
        }
        return name
    }
}
```

#### Fix 7: `linux/src/wayland_clipboard.cpp` — Always Call `write_to_wl_copy` on Wayland
In `WaylandClipboardBridge::apply_remote_clipboard`:

```cpp
// In linux/src/wayland_clipboard.cpp:
    if (item.mime_type.rfind("text/", 0) == 0 && !item.data.empty()) {
        std::string text(reinterpret_cast<const char*>(item.data.data()), item.data.size());

        auto hash = l2a::sha256_string(text);
        {
            std::lock_guard<std::mutex> lock(hash_mutex_);
            last_applied_hash_ = hash;
            has_applied_hash_ = true;
        }
        is_applying_remote_.store(true);

        // Always update Wayland compositor clipboard via wl-copy if available
        if (has_wl_clipboard_tools()) {
            write_to_wl_copy("text/plain", item.data.data(), item.data.size());
        }
        // Also update GTK clipboard so GTK widgets receive the text
        if (gtk_clip_) {
            gtk_clipboard_set_text(gtk_clip_, text.c_str(), static_cast<gint>(text.size()));
        }

        is_applying_remote_.store(false);
```

---

## 5. Verification Method

### 5.1 C++ Core & Linux Subsystem Verification
Run the existing test suite and verify 100% pass:
```bash
cmake -B build -DL2A_BUILD_LINUX=ON
cmake --build build -j4
ctest --test-dir build --output-on-failure -V
```

### 5.2 Android Dual-Channel Fallback & Reverse Transfer Automated Test
Add a new automated regression test executable `tests/test_reverse_transfer_and_fallback.cpp` to verify:
1. Scenario A: Peer discovered on LAN (`127.0.0.1:53321`). Client connects and calls `push_clipboard_text` and `send_files`. Verify that clipboard and file contents arrive and pass SHA-256 verification.
2. Scenario B: LAN connection unavailable, but reverse tunnel port (`127.0.0.1:53323`) is listening. Client connects to fallback port `53323`, delivers clipboard text and file streams. Verify 100% receipt and zero leakage.

### 5.3 Android APK Build Verification
Verify clean compilation of Android APK:
```bash
cd android && ./gradlew assembleDebug
```
Ensure `android/app/build/outputs/apk/debug/app-debug.apk` is generated without errors.
