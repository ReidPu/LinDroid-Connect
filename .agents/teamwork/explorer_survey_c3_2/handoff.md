# Handoff Report: Technical Survey & Architecture for Requirement R2 (Binary Image Clipboard Sync & File Manager Resolution)

**Explorer**: `explorer_survey_c3_2`  
**Milestone**: `survey_c3_r2`  
**Date**: 2026-10-24  
**Working Directory**: `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/explorer_survey_c3_2`

---

## 1. Observation

### 1.1 Linux Desktop Clipboard Capture Pipeline
- **File**: `linux/src/wayland_clipboard.cpp`, Lines 224–274:
  ```cpp
  void WaylandClipboardBridge::on_gtk_owner_change(
      GtkClipboard* clipboard,
      GdkEvent* /*event*/,
      gpointer user_data) {
      auto* self = static_cast<WaylandClipboardBridge*>(user_data);
      if (!self->sync_enabled_.load()) {
          return;
      }
      // LINUX-11: Suppress callback when actively applying remote clipboard
      if (self->is_applying_remote_.load()) {
          return;
      }
      gtk_clipboard_request_text(clipboard, on_gtk_text_received, self);
  }

  void WaylandClipboardBridge::on_gtk_text_received(
      GtkClipboard* /*clipboard*/,
      const gchar* text,
      gpointer user_data) {
      auto* self = static_cast<WaylandClipboardBridge*>(user_data);
      if (!text || text[0] == '\0' || !self->sync_enabled_.load()) {
          return;
      }
      std::string utf8_str(text);
      ...
      std::thread([self, utf8_str = std::move(utf8_str)]() {
          if (self->engine_.push_clipboard_text(utf8_str)) {
              if (self->log_cb_) {
                  std::string preview = utf8_str.substr(0, std::min<size_t>(40, utf8_str.size()));
                  self->log_cb_("[剪贴板 -> 手机] 已同步文本 (" +
                                std::to_string(utf8_str.size()) + " B): " + preview);
              }
          }
      }).detach();
  }
  ```
  `on_gtk_owner_change` unconditionally invokes `gtk_clipboard_request_text(clipboard, on_gtk_text_received, self)`. It never queries image availability or image targets (`image/png`, `image/jpeg`, `GdkPixbuf`).

- **File**: `linux/src/wayland_clipboard.cpp`, Lines 385–399:
  ```cpp
  const char* args[] = {
      "wl-paste",
      "--type",
      "text/plain",
      "--watch",
      "/bin/sh",
      "-c",
      "cat > \"$1\"",
      "sh",
      fifo_path_.c_str(),
      nullptr
  };
  ::execvp("wl-paste", const_cast<char* const*>(args));
  ```
  The background Wayland watcher explicitly hardcodes `--type text/plain`. Any clipboard event containing only image pixel data (`image/png`, `image/jpeg`) is silently ignored by `wl-paste` and never forwarded to the FIFO pipe.

- **File**: `core/src/engine.cpp`, Lines 473–494:
  ```cpp
  bool Engine::push_clipboard_text(std::string_view utf8_text) {
      return clipboard_mgr_->push_clipboard_bytes(
          "text/plain",
          std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(utf8_text.data()),
                                   utf8_text.size()),
          false);
  }

  bool Engine::push_clipboard_bytes(
      std::string_view mime_type,
      std::span<const uint8_t> data,
      bool force_lazy_pull) {
      return clipboard_mgr_->push_clipboard_bytes(mime_type, data, force_lazy_pull);
  }

  bool Engine::push_clipboard_file(
      std::string_view mime_type,
      const std::filesystem::path& file_path,
      bool lazy_pull) {
      return clipboard_mgr_->push_clipboard_file(mime_type, file_path, lazy_pull);
  }
  ```
  `Engine` already exposes `push_clipboard_bytes` and `push_clipboard_file`. However, `WaylandClipboardBridge` exclusively calls `push_clipboard_text`, hardcoding MIME type `"text/plain"`.

### 1.2 Linux File Manager (Nautilus) Image File Copying
- When a user copies an image file (e.g. `screenshot.png` or `photo.jpg`) in GNOME Nautilus, the clipboard selection contains targets:
  - `text/uri-list`: `file:///home/user/Pictures/screenshot.png\r\n`
  - `x-special/nautilus-clipboard`: `copy\nfile:///home/user/Pictures/screenshot.png`
  - `text/plain` / `UTF8_STRING`: `file:///home/user/Pictures/screenshot.png`
- In `on_gtk_text_received` and `wayland_watcher_loop`, `text` is received as `"file:///home/user/Pictures/screenshot.png"`.
- Because there is no check or parser to inspect whether the string represents a local image file path, `push_clipboard_text` transmits the literal string `"file:///home/user/Pictures/screenshot.png"` with MIME `"text/plain"`.
- The Android phone receives the text string of the PC path instead of the actual binary image.

### 1.3 Android Reception and Insertion into `ClipboardManager`
- **File**: `android/app/src/main/java/com/reid/l2a/L2AService.kt`, Lines 160–211:
  ```kotlin
  override fun onClipboardReceived(mimeType: String, data: ByteArray, tempFilePath: String) {
      when {
          mimeType.startsWith("text/") -> {
              val text = if (data.isNotEmpty()) {
                  String(data, Charsets.UTF_8)
              } else if (tempFilePath.isNotEmpty()) {
                  val tempFile = File(tempFilePath)
                  if (tempFile.exists()) tempFile.readText(Charsets.UTF_8) else null
              } else {
                  null
              }
              if (!text.isNullOrEmpty()) {
                  lastReceivedText = text
                  mainHandler.post {
                      clipboardManager.setPrimaryClip(ClipData.newPlainText("LinuxClipboard", text))
                      Toast.makeText(this@L2AService, "已同步电脑剪贴板", Toast.LENGTH_SHORT).show()
                  }
              }
          }
          mimeType.startsWith("image/") -> {
              val imageBytes = if (data.isNotEmpty()) {
                  data
              } else if (tempFilePath.isNotEmpty()) {
                  val tempFile = File(tempFilePath)
                  if (tempFile.exists()) tempFile.readBytes() else null
              } else {
                  null
              }
              if (imageBytes != null && imageBytes.isNotEmpty()) {
                  try {
                      val ext = if (mimeType.contains("jpeg") || mimeType.contains("jpg")) "jpg" else "png"
                      val values = ContentValues().apply {
                          put(MediaStore.Images.Media.DISPLAY_NAME, "clip_${System.currentTimeMillis()}.$ext")
                          put(MediaStore.Images.Media.MIME_TYPE, mimeType)
                          put(MediaStore.Images.Media.RELATIVE_PATH, "${Environment.DIRECTORY_PICTURES}/LinuxToAndroid")
                      }
                      val uri = contentResolver.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values)
                      if (uri != null) {
                          contentResolver.openOutputStream(uri)?.use { os ->
                              os.write(imageBytes)
                          }
                          mainHandler.post {
                              val clipData = ClipData.newUri(contentResolver, "LinuxClipboard", uri)
                              clipboardManager.setPrimaryClip(clipData)
                              Toast.makeText(this@L2AService, "已同步电脑剪贴板 (图片)", Toast.LENGTH_SHORT).show()
                          }
                      }
                  } catch (e: Exception) {
                      Log.e("L2AService", "Failed to save image clipboard: ${e.message}")
                  }
              }
          }
      }
  }
  ```
- **File**: `android/app/src/main/AndroidManifest.xml`, Lines 16–118:
  No `<provider>` tag exists anywhere in the manifest.
- **File**: `android/app/src/main/res/xml/`:
  Only `nfc_tech_filter.xml` exists; there is no `file_paths.xml` or `provider_paths.xml`.
- When `L2AService` sets `ClipData.newUri(contentResolver, "LinuxClipboard", uri)` using a `MediaStore` URI (`content://media/external/images/media/...`), third-party apps (WeChat, QQ, System Notes, Browser, Telegram) fail to open the stream upon pasting. In Android 10+ / 13+ / 14 (Scoped Storage), target apps do not receive grantable URI permissions from `MediaStore` without holding broad runtime storage permissions (`READ_MEDIA_IMAGES`).
- In contrast, Android's framework explicitly requires a `ContentProvider` / `FileProvider` with `android:grantUriPermissions="true"`. Android's `ClipboardService` automatically attaches transient `FLAG_GRANT_READ_URI_PERMISSION` to pasting apps for URIs hosted by providers declaring `grantUriPermissions="true"`.

### 1.4 Phone-to-PC Image Capture and Transmission
- **File**: `android/app/src/main/java/com/reid/l2a/L2AService.kt`, Lines 40–57:
  ```kotlin
  private val clipListener = ClipboardManager.OnPrimaryClipChangedListener {
      val clip = clipboardManager.primaryClip ?: return@OnPrimaryClipChangedListener
      if (clip.description?.label == "LinuxClipboard") {
          return@OnPrimaryClipChangedListener
      }
      if (clip.itemCount > 0) {
          val text = clip.getItemAt(0).coerceToText(this)?.toString()
          if (!text.isNullOrEmpty()) {
              if (text == lastReceivedText) {
                  return@OnPrimaryClipChangedListener
              }
              Thread {
                  CoreBridge.nativePushClipboardText(text)
              }.start()
          }
      }
  }
  ```
  `clip.getItemAt(0).coerceToText(this)` is called directly. When an image is copied on the phone, `item.uri` contains a content URI, and `coerceToText()` returns the string `"content://..."` or `null`. The image binary is never read.
- **File**: `android/app/src/main/java/com/reid/l2a/MainActivity.kt`, Lines 98–104 & `NfcTriggerActivity.kt`, Lines 174–183:
  Both classes also only call `clip.getItemAt(0).coerceToText()?.toString()` and `CoreBridge.nativePushClipboardText(text)`.
- **File**: `android/app/src/main/java/com/reid/l2a/CoreBridge.kt`, Line 31 & `android/app/src/main/cpp/jni_bridge.cpp`, Lines 510–522:
  `CoreBridge` only defines `nativePushClipboardText(text: String): Boolean`. There is no JNI function to push binary data (`nativePushClipboardBytes`).

### 1.5 Linux Reception of Image Clipboard & Anti-Echo Guard
- **File**: `linux/src/wayland_clipboard.cpp`, Lines 330–346:
  ```cpp
  // For images or binary MIME types: stream to wl-copy via direct exec
  if (has_wl_clipboard_tools()) {
      bool ok = false;
      if (!item.temp_file_path.empty()) {
          ok = write_file_to_wl_copy(item.mime_type, item.temp_file_path);
      } else if (!item.data.empty()) {
          ok = write_to_wl_copy(item.mime_type, item.data.data(), item.data.size());
      }

      if (ok && log_cb_) {
          log_cb_("[手机 -> 剪贴板] 已通过 wl-copy 写入二进制剪贴板 (" +
                  item.mime_type + ", " + std::to_string(item.total_size) + " B)");
      }
  }
  ```
  Notice: Unlike the text branch (lines 302–308), the binary/image branch:
  1. Does NOT record `last_applied_hash_ = item.sha256;`.
  2. Does NOT set `is_applying_remote_.store(true);`.
  When `wl-copy` updates the system clipboard, the local watcher can detect the change and re-transmit the received image back to the phone, creating an echo loop.
  3. Does NOT set the image into `gtk_clip_` (via `gtk_clipboard_set_image`), meaning non-Wayland GTK applications cannot access the image if `wl-copy` is unavailable or when operating under X11/XWayland.

---

## 2. Logic Chain

```
[User copies image file in Linux Nautilus]
               │
               ▼
Nautilus sets clipboard: `text/uri-list` with `file:///home/user/Pictures/sample.png`
               │
               ▼
WaylandClipboardBridge::on_gtk_owner_change calls gtk_clipboard_request_text()
(wl-paste --watch --type text/plain feeds FIFO)
               │
               ▼
on_gtk_text_received gets string "file:///home/user/Pictures/sample.png"
               │
               ▼
Engine::push_clipboard_text("file:///home/user/Pictures/sample.png")
transmits frame with MIME "text/plain"
               │
               ▼
Android L2AService receives MIME "text/plain"
Executes String(data, UTF_8) -> sets ClipData.newPlainText("LinuxClipboard", text)
               │
               ▼
[FAILURE] Phone pastes string "file:///home/user/Pictures/sample.png", NOT the image!
```

```
[User copies raw image pixels (Flameshot / browser "Copy Image")]
               │
               ▼
Clipboard owner offers targets: image/png, image/jpeg, (optional text/html or alt-text)
               │
               ├─────────────────────────────────────────────┐
               ▼                                             ▼
GTK on_gtk_owner_change                           Wayland watcher (wl-paste)
Calls gtk_clipboard_request_text()                Runs `wl-paste --type text/plain`
Returns alt-text OR null                          Drops image/png completely!
               │                                             │
               ▼                                             ▼
Transmits text description or drops               Nothing sent
               │
               ▼
[FAILURE] Image pixel data never transmitted to phone
```

```
[Android receives image/* and attempts insertion into MediaStore]
               │
               ▼
L2AService inserts into MediaStore.Images.Media.EXTERNAL_CONTENT_URI
Creates ClipData.newUri("LinuxClipboard", content://media/external/images/media/123)
               │
               ▼
User pastes into WeChat / QQ / Notes
Target app attempts contentResolver.openInputStream(uri)
               │
               ▼
[FAILURE] SecurityException / null stream: MediaStore does not grant transient URI permissions
to third-party apps via ClipboardManager without READ_MEDIA_IMAGES permission!
Target app requires FileProvider with `grantUriPermissions="true"`.
```

```
[User copies image on Android phone]
               │
               ▼
Clip contains item.uri (`content://...`)
L2AService / MainActivity / NfcTriggerActivity calls:
`clip.getItemAt(0).coerceToText(this)?.toString()`
               │
               ▼
Returns "content://..." URI string or null
CoreBridge only has `nativePushClipboardText(text)`
               │
               ▼
Transmits text string to PC; PC pastes "content://..." instead of image
```

---

## 3. Caveats

1. **Wayland Background Image Watcher Protocol Constraints**:
   - In GNOME Wayland (`mutter`), `wlr-data-control` is not supported. `wl-paste --watch` can monitor clipboard changes. However, running `wl-paste --type image/png --watch` only triggers on PNG updates, while `wl-paste --watch <command>` without `--type` triggers on every clipboard selection change. Inside `<command>`, one can query available types via `wl-paste -l` to check whether `image/png`, `image/jpeg`, or text is present.
2. **File Size Limits on Clipboard Images**:
   - Copying a massive RAW photo (e.g. 100 MB TIFF/DNG) should not exhaust device heap. `l2a_core`'s `ClipboardStreamManager` already supports streaming payloads >512 KB directly via temp files on disk (`push_clipboard_file`). A sensible upper threshold (e.g., 50 MB) should be enforced to protect network bandwidth and mobile cache.
3. **Xiaomi HyperOS Clipboard Background Read Permissions**:
   - On Android 10+ (and Xiaomi HyperOS), background services cannot read `primaryClip` without `READ_CLIPBOARD_IN_BACKGROUND` AppOps permission (`adb shell cmd appops set com.reid.l2a READ_CLIPBOARD allow`). When copying an image while outside the app, if this permission is not granted, the service cannot read the clip in the background. The app already features `NfcTriggerActivity` (touchpad NFC tap) and foreground notification quick-send buttons that momentarily acquire window focus, guaranteeing clip access.

---

## 4. Conclusion & Concrete Recommended Architecture

### 4.1 Linux PC Image Clipboard Capture: 3-Tier Priority Architecture

In `linux/src/wayland_clipboard.cpp`, replace unconditional text requests with a structured 3-tier priority extraction pipeline:

```
                  ┌────────────────────────────────────────┐
                  │       Clipboard Changed Event          │
                  │ (GTK owner-change / Wayland watcher)   │
                  └──────────────────┬─────────────────────┘
                                     │
                                     ▼
        ┌────────────────────────────────────────────────────────┐
        │ Priority 1: Check for Raw Image Pixel Data?            │
        │ - GTK: gtk_clipboard_wait_is_image_available(clip)    │
        │ - Wayland: `wl-paste -l` has image/png or image/jpeg   │
        └────────────────────────────┬───────────────────────────┘
                                     │
                      YES ───────────┴─────────── NO
                       │                           │
                       ▼                           ▼
        ┌────────────────────────────┐ ┌──────────────────────────────────────┐
        │ Extract raw image bytes    │ │ Request Clipboard Text (UTF8/URI)   │
        │ (e.g. encode GdkPixbuf to  │ └──────────────────┬───────────────────┘
        │ PNG or read wl-paste png)  │                    │
        │                            │                    ▼
        │ Push: push_clipboard_bytes │ ┌──────────────────────────────────────┐
        │ ("image/png", bytes)       │ │ Priority 2: Resolves to Local Image? │
        └────────────────────────────┘ │ (file:///.../x.png or /.../x.jpg)    │
                                       │ Ext: png, jpg, jpeg, webp, gif, bmp  │
                                       └──────────────────┬───────────────────┘
                                                          │
                                           YES ───────────┴─────────── NO
                                            │                           │
                                            ▼                           ▼
                             ┌────────────────────────────┐ ┌─────────────────┐
                             │ Read image file binary     │ │ Priority 3:     │
                             │ Push: push_clipboard_file  │ │ Normal Text     │
                             │ (mime, path) or bytes      │ │ push_clipboard_ │
                             └────────────────────────────┘ │ text(utf8_str)  │
                                                            └─────────────────┘
```

#### A. File Manager URI & Local Path Resolution Algorithm
Create a dedicated helper function in `wayland_clipboard.cpp` (and expose for unit testing):
```cpp
struct ResolvedImageFile {
    bool is_image{false};
    std::filesystem::path path;
    std::string mime_type;
};

inline ResolvedImageFile try_resolve_local_image_file(std::string_view text) {
    // 1. Split lines (handle text/uri-list and x-special/nautilus-clipboard)
    std::string candidate;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find_first_of("\r\n", start);
        if (end == std::string::npos) end = text.size();
        std::string_view line = text.substr(start, end - start);
        start = end + 1;

        // Skip Nautilus prefixes and comments
        if (line == "copy" || line == "cut" || line.starts_with("#")) continue;
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
        if (line.empty()) continue;

        candidate = std::string(line);
        break; // Inspect primary target file
    }

    if (candidate.empty()) return {};

    std::filesystem::path file_path;
    if (candidate.starts_with("file://")) {
        // Percent-decode URI
        std::string raw = candidate.substr(7);
        // Handle file://localhost/... vs file:///...
        if (raw.starts_with("localhost/")) raw = raw.substr(9);
        std::string decoded;
        decoded.reserve(raw.size());
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == '%' && i + 2 < raw.size()) {
                int hex_val = 0;
                if (std::sscanf(raw.substr(i + 1, 2).c_str(), "%x", &hex_val) == 1) {
                    decoded.push_back(static_cast<char>(hex_val));
                    i += 2;
                    continue;
                }
            }
            decoded.push_back(raw[i]);
        }
        file_path = decoded;
    } else if (candidate.starts_with("/")) {
        file_path = candidate;
    } else {
        return {};
    }

    std::error_code ec;
    if (!std::filesystem::exists(file_path, ec) || !std::filesystem::is_regular_file(file_path, ec)) {
        return {};
    }

    // Check extension
    std::string ext = file_path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    std::string mime;
    if (ext == ".png") mime = "image/png";
    else if (ext == ".jpg" || ext == ".jpeg") mime = "image/jpeg";
    else if (ext == ".webp") mime = "image/webp";
    else if (ext == ".gif") mime = "image/gif";
    else if (ext == ".bmp") mime = "image/bmp";
    else return {};

    uintmax_t sz = std::filesystem::file_size(file_path, ec);
    if (ec || sz == 0 || sz > 50 * 1024 * 1024) return {}; // Enforce 50MB safety limit

    return {true, file_path, mime};
}
```

#### B. GTK & Wayland Priority Dispatch
1. In `on_gtk_owner_change`:
   - First check if raw image data is present via `gtk_clipboard_wait_is_image_available(clipboard)`.
   - If true: call `gtk_clipboard_request_image` (or `gtk_clipboard_wait_for_image`), convert `GdkPixbuf` to PNG bytes using `gdk_pixbuf_save_to_buffer`, compute SHA-256 for `last_sent_hash_`, and invoke `engine_.push_clipboard_bytes("image/png", bytes)`.
   - If false: request text via `gtk_clipboard_request_text`. In `on_gtk_text_received`, pass text through `try_resolve_local_image_file`. If it resolves to an image file, push via `engine_.push_clipboard_file(resolved.mime_type, resolved.path)`; otherwise, push as regular text via `engine_.push_clipboard_text(utf8_str)`.
2. In `wayland_watcher_loop`:
   - Trigger watcher on any clipboard change. Inspect MIME types using `wl-paste -l`.
   - If `image/png` or `image/jpeg` is listed: extract binary via `wl-paste --type image/png` and push as binary clipboard.
   - If only text is available: read via `wl-paste --type text/plain`, pass through `try_resolve_local_image_file`, and dispatch image or text accordingly.

---

### 4.2 Android Image Clipboard Reception & Insertion Architecture

#### A. Configure `androidx.core.content.FileProvider` in `AndroidManifest.xml`
In `android/app/src/main/AndroidManifest.xml`, inside `<application>`:
```xml
<provider
    android:name="androidx.core.content.FileProvider"
    android:authorities="${applicationId}.fileprovider"
    android:exported="false"
    android:grantUriPermissions="true">
    <meta-data
        android:name="android.support.FILE_PROVIDER_PATHS"
        android:resource="@xml/file_paths" />
</provider>
```

#### B. Create `android/app/src/main/res/xml/file_paths.xml`
```xml
<?xml version="1.0" encoding="utf-8"?>
<paths xmlns:android="http://schemas.android.com/apk/res/android">
    <cache-path name="clipboard_images" path="clipboard_images/" />
    <files-path name="clipboard_files" path="clipboard_images/" />
</paths>
```

#### C. Implement FileProvider Image Staging & Insertion in `L2AService.kt`
Replace lines 178–210 in `L2AService.kt`:
```kotlin
mimeType.startsWith("image/") -> {
    val imageBytes = if (data.isNotEmpty()) {
        data
    } else if (tempFilePath.isNotEmpty()) {
        val tempFile = File(tempFilePath)
        if (tempFile.exists()) tempFile.readBytes() else null
    } else {
        null
    }
    if (imageBytes != null && imageBytes.isNotEmpty()) {
        try {
            val clipDir = File(cacheDir, "clipboard_images").apply { if (!exists()) mkdirs() }
            val ext = when {
                mimeType.contains("jpeg") || mimeType.contains("jpg") -> "jpg"
                mimeType.contains("webp") -> "webp"
                mimeType.contains("gif") -> "gif"
                mimeType.contains("bmp") -> "bmp"
                else -> "png"
            }
            val imageFile = File(clipDir, "clip_${System.currentTimeMillis()}.$ext")
            imageFile.writeBytes(imageBytes)

            val contentUri = androidx.core.content.FileProvider.getUriForFile(
                this@L2AService,
                "${packageName}.fileprovider",
                imageFile
            )

            mainHandler.post {
                val clipData = ClipData.newUri(contentResolver, "L2A Image", contentUri)
                clipboardManager.setPrimaryClip(clipData)
                Toast.makeText(this@L2AService, "已同步电脑剪贴板 (图片)", Toast.LENGTH_SHORT).show()
            }
        } catch (e: Exception) {
            Log.e("L2AService", "Failed to save and set image clipboard: ${e.message}")
        }
    }
}
```
*Why this guarantees pasting in WeChat / QQ / Notes*:
Because `FileProvider` has `grantUriPermissions="true"`, when `ClipData.newUri` is set into `ClipboardManager`, Android's system `ClipboardService` automatically grants temporary read permissions to the recipient application when the user pastes into any input field. The target app reads the image via `contentResolver.openInputStream(contentUri)` without security exceptions.

---

### 4.3 Phone -> PC Image Clipboard Capture & Linux Writing

#### A. Android Native JNI Bridge Extension
1. In `android/app/src/main/cpp/jni_bridge.cpp`:
   ```cpp
   JNIEXPORT jboolean JNICALL
   Java_com_reid_l2a_CoreBridge_nativePushClipboardBytes(
       JNIEnv* env,
       jobject /*thiz*/,
       jstring jmime,
       jbyteArray jdata) {
       std::shared_ptr<l2a::Engine> engine;
       {
           std::lock_guard<std::mutex> lock(g_engine_mtx);
           engine = g_engine;
       }
       if (!engine || !jdata) return JNI_FALSE;
       std::string mime = jstring_to_utf8(env, jmime);
       jsize len = env->GetArrayLength(jdata);
       std::vector<uint8_t> buf(static_cast<size_t>(len));
       env->GetByteArrayRegion(jdata, 0, len, reinterpret_cast<jbyte*>(buf.data()));
       return engine->push_clipboard_bytes(mime, buf) ? JNI_TRUE : JNI_FALSE;
   }
   ```
2. In `android/app/src/main/java/com/reid/l2a/CoreBridge.kt`:
   ```kotlin
   external fun nativePushClipboardBytes(mimeType: String, data: ByteArray): Boolean
   ```

#### B. Android Clipboard Capture (`L2AService.kt`, `MainActivity.kt`, `NfcTriggerActivity.kt`)
In `L2AService.kt` `clipListener`:
```kotlin
val clip = clipboardManager.primaryClip ?: return@OnPrimaryClipChangedListener
val label = clip.description?.label?.toString()
if (label == "LinuxClipboard" || label == "L2A Image") {
    return@OnPrimaryClipChangedListener // Suppress echo loops
}
if (clip.itemCount == 0) return@OnPrimaryClipChangedListener
val item = clip.getItemAt(0)
val uri = item.uri

if (uri != null) {
    val type = contentResolver.getType(uri) ?: clip.description.getMimeType(0)
    if (type != null && type.startsWith("image/")) {
        Thread {
            try {
                contentResolver.openInputStream(uri)?.use { input ->
                    val bytes = input.readBytes()
                    if (bytes.isNotEmpty() && CoreBridge.ensureConnected(this@L2AService, 3000)) {
                        CoreBridge.nativePushClipboardBytes(type, bytes)
                    }
                }
            } catch (e: Exception) {
                Log.e("L2AService", "Failed to push image to Linux: ${e.message}")
            }
        }.start()
        return@OnPrimaryClipChangedListener
    }
}

// Fallback to text
val text = item.coerceToText(this)?.toString()
if (!text.isNullOrEmpty() && text != lastReceivedText) {
    Thread {
        if (CoreBridge.ensureConnected(this@L2AService, 3000)) {
            CoreBridge.nativePushClipboardText(text)
        }
    }.start()
}
```

#### C. Linux Clipboard Application & Anti-Echo Guard
In `linux/src/wayland_clipboard.cpp`, lines 330–346:
```cpp
// Guard against echo loops
{
    std::lock_guard<std::mutex> lock(hash_mutex_);
    last_applied_hash_ = item.sha256;
    has_applied_hash_ = true;
}
is_applying_remote_.store(true);

if (has_wl_clipboard_tools()) {
    bool ok = false;
    if (!item.temp_file_path.empty()) {
        ok = write_file_to_wl_copy(item.mime_type, item.temp_file_path);
    } else if (!item.data.empty()) {
        ok = write_to_wl_copy(item.mime_type, item.data.data(), item.data.size());
    }
    if (ok && log_cb_) {
        log_cb_("[手机 -> 剪贴板] 已通过 wl-copy 写入二进制图片 (" +
                item.mime_type + ", " + std::to_string(item.total_size) + " B)");
    }
}

is_applying_remote_.store(false);
```

---

## 5. Verification Method

### 5.1 Independent Test Verification Strategy
1. **Automated Unit & Regression Tests**:
   - Create `tests/test_image_clipboard_sync.cpp` validating:
     - PC Node sends binary image frame (`push_clipboard_bytes("image/png", png_bytes)`).
     - Android Node receives `on_clipboard_received`, verifying identical MIME type (`image/png`) and SHA-256 digest.
     - PC Node sends local image file path / `file://` URI via `try_resolve_local_image_file()`, verifying it automatically transmits as binary `image/png` instead of string path.
     - Anti-loopback suppression: Verify that after receiving an image, a subsequent push with the same SHA-256 hash is ignored.
2. **Linux Subsystem Test Suite**:
   - Add unit tests to `linux/tests/test_linux_subsystem.cpp` for `try_resolve_local_image_file`:
     - Test valid `file:///path/to/test.png` -> resolves to `image/png` and local path.
     - Test valid `/path/to/test.jpg` -> resolves to `image/jpeg` and local path.
     - Test percent-encoded `file:///path/to/My%20Image%202026.webp` -> resolves with space decoded.
     - Test non-image file `file:///path/to/doc.pdf` -> rejected (`is_image == false`).
     - Test non-existent file `file:///path/to/fake_12345.png` -> rejected (`is_image == false`).
3. **Android Build Verification**:
   - Verify `assembleDebug` builds cleanly with `androidx.core.content.FileProvider` and JNI `nativePushClipboardBytes`.

### 5.2 Verification Commands
```bash
# 1. Compile C++ Core, Linux Desktop, and Tests
cmake -B build -DL2A_BUILD_LINUX=ON
cmake --build build -j4

# 2. Run all CTest test suites
ctest --test-dir build --output-on-failure -V

# 3. Build Android APK
mkdir -p .android_home
cd android && ANDROID_USER_HOME=/home/reid-pu/Programming/Linux_to_Android/.android_home ./gradlew assembleDebug -Dorg.gradle.java.home=/usr/lib/jvm/java-21-openjdk-amd64
```
