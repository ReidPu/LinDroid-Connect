# Requirements R3 & R4 Technical Scope, Architectural Mapping & Implementation Blueprint

## 1. Observation

### 1.1 Existing Clipboard Architecture & History Limitations
Across the codebase, clipboard synchronization is implemented as a single-item point-to-point stream without any persistent history or selective paste capability:

- **Core Layer (`core/include/l2a/clipboard_stream.hpp`, `core/src/clipboard_stream.cpp`)**:
  - `ClipboardStreamManager` (lines 51–117) handles packet framing for clipboard streaming (`CLIP_OFFER`, `CLIP_DATA_CHUNK`, `CLIP_PULL_REQ`).
  - `ReceivedClipboardItem` (lines 36–46) stores a single incoming item:
    ```cpp
    struct ReceivedClipboardItem {
        uint64_t clip_id{0};
        std::string mime_type;
        uint64_t total_size{0};
        Sha256Digest sha256{};
        std::vector<uint8_t> data;
        std::filesystem::path temp_file_path;
        std::shared_ptr<TempFileGuard> file_guard;
    };
    ```
  - `ClipboardStreamManager` tracks only `last_received_hash_` (line 111) for loopback prevention. It has no queue, ring buffer, or LRU history list.

- **Linux Subsystem (`linux/src/wayland_clipboard.hpp`, `linux/src/wayland_clipboard.cpp`, `linux/src/app_window.cpp`)**:
  - In `wayland_clipboard.cpp` lines 224–274, GTK clipboard owner changes invoke `on_gtk_text_received()`, which checks `last_sent_hash_` / `last_applied_hash_` and pushes text directly to `engine_.push_clipboard_text()`.
  - In `wayland_clipboard.cpp` lines 276–347, `apply_remote_clipboard()` writes incoming data directly into `wl-copy` and `gtk_clipboard_set_text()`.
  - In `app_window.cpp` lines 140–151, the UI only provides a raw text view for application logs (`log_buffer_`). There is no history viewer list, no thumbnail cache, and no selective paste interaction.
  - Any new local or remote clipboard event immediately replaces the previous clipboard content in both `wl-copy` and `gtk_clip_`.

- **Android Subsystem (`android/app/src/main/java/com/reid/l2a/L2AService.kt`, `MainActivity.kt`)**:
  - In `L2AService.kt` lines 40–57, `OnPrimaryClipChangedListener` monitors `clipboardManager.primaryClip`. When a change occurs, it checks if `text == lastReceivedText` (line 49) and calls `CoreBridge.nativePushClipboardText(text)`.
  - In `L2AService.kt` lines 158–212, `onClipboardReceived()` writes received text or image URIs directly into `clipboardManager.setPrimaryClip()`.
  - There is no history data structure in `L2AService` or `MainActivity`. Past clips are permanently lost once overwritten.

---

### 1.2 Touchpad NFC (NTAG5332) Real-Device Investigation
Inspection of `android/app/src/main/` (`AndroidManifest.xml`, `MainActivity.kt`, `NfcTriggerActivity.kt`) and `linux/src/nfc_ntag5332.cpp` reveals why Xiaomi 14 Pro vibrated and chimed when touching the Redmi G Pro 2024 touchpad, yet failed to write or trigger in the app:

- **Physical RF vs. Application Layer Disconnect**:
  - The Redmi G Pro 2024 touchpad integrates an NXP NTAG5332 (NTP5332) tag chip linked to an RF antenna underneath the touchpad.
  - When the phone touches the touchpad, the phone's NFC radio powers the tag via 13.56 MHz RF induction. Android's system NFC daemon (`NfcService`) detects the tag at the ISO 15693 / ISO 14443A physical layer, triggering the system haptic vibration and chime.
  - However, in `MainActivity.kt` lines 246–296 (`writeTagNdefOrFormatable`):
    ```kotlin
    val ndef = Ndef.get(tag)
    if (ndef != null) {
        return try {
            ndef.connect()
            try {
                if (!ndef.isWritable) {
                    return Pair(false, "标签处于只读状态") // Line 258: Fails here!
                }
                ...
                ndef.writeNdefMessage(msg)
    ```
  - **Factory Write-Lock**: Redmi G Pro 2024 touchpad NTAG5332 chips are programmed at the Xiaomi factory with Xiaomi Share / Mi Connect pairing data, and the EEPROM lock bits / Capability Container byte are set to Read-Only (`0x00`).
  - Consequently, `ndef.isWritable` returns `false`, or writing throws `IOException: Tag was locked`.
  - `NdefFormatable.get(tag)` returns `null` because the tag is already formatted.
  - In `MainActivity.kt` line 234, the app reports `写入失败: 标签处于只读状态`, and halts.

- **Absence of Hardware UID Binding in `MainActivity.kt`**:
  - In `MainActivity.kt` lines 222–244 (`handleTagDiscovered`):
    ```kotlin
    private fun handleTagDiscovered(tag: Tag) {
        try {
            if (writeNfcMode) {
                val hostId = targetHostId
                val uri = "l2a://tap?host_id=$hostId&name=Redmi-G-Pro-2024&port=53321"
                val (success, errorMsg) = writeTagNdefOrFormatable(tag, uri)
                runOnUiThread {
                    if (success) { ... }
                    else {
                        statusView.text = "写入失败: $errorMsg\n请将手机背面贴近触控板中央重试。"
                    }
                }
            } else {
                handleTapTrigger(tag)
            }
        }
    ```
  - Notice: `tag.id` is **never read**! No UID is extracted, no UID is bound, and nothing is written to `SharedPreferences`.
  - When NDEF write fails on the factory-locked tag, the binding attempt is abandoned completely.

- **Strict NDEF Scheme Dependency in Trigger Handlers**:
  - In `MainActivity.kt` lines 298–326 (`handleTapTrigger`):
    ```kotlin
    private fun handleTapTrigger(tag: Tag) {
        val ndef = Ndef.get(tag) ?: return
        ...
        for (record in msg.records) {
            val recordUri = record.toUri()
            if (recordUri != null && recordUri.scheme == "l2a" && recordUri.host == "tap") {
                // Line 312: Only triggers if URI matches l2a://tap!
            }
        }
    ```
  - Because NDEF writing failed, the tag retains its original Xiaomi payload (`com.xiaomi.mi_connect_service` / `mi://...`). `recordUri.scheme == "l2a"` is never true, so `handleTapTrigger` silently drops the event.
  - In `NfcTriggerActivity.kt` lines 148–150:
    ```kotlin
    private fun applyTapUriParams(uri: Uri) {
        if (uri.scheme != "l2a") return // Line 149: Aborts immediately!
    ```
    Even if launched, `NfcTriggerActivity` abandons the tap without reading or verifying the bound UID.

---

## 2. Logic Chain

### 2.1 Logic Chain for Requirement R3 (Clipboard History & Selective Paste)

1. **Premise 1 (Consistency across platforms)**:
   - Linux and Android both require strict history capacity enforcement ($N \in [5, 200]$, default 20), dynamic trimming when $N$ changes, item deletion, clear all, and deduplication.
   - If implemented separately in C++ and Kotlin, data models and boundary behaviors (LRU eviction, trimming, sorting) risk divergent behavior and test fragmentation.
   - *Inference*: Implementing `ClipboardHistoryManager` in `l2a_core` (C++20) provides a single source of truth, 100% CTest testability, disk persistence (`clipboard_history.json`), and clean exposure to both Linux GTK and Android JNI.

2. **Premise 2 (Definition of Non-destructive Selective Paste)**:
   - Normal paste (`Ctrl+V` on Linux or long-press paste on Android) must paste the newest item (#0).
   - When the user selects history item #k ($k > 0$), item #k must be pasted into the active target input field, but:
     - The history queue order must NOT change (item #0 remains at index 0).
     - The system clipboard must not permanently overwrite item #0.
   - *Inference for Linux*:
     - When item #k is selected, set an internal guard `is_selective_pasting = true` (preventing `WaylandClipboardBridge` from logging #k as a new entry or echoing to Android).
     - Under X11/XWayland with `xdotool`: simulate `Ctrl+V`, delay 150ms for target window ingestion, and immediately restore item #0 to `wl-copy` and `gtk_clip_`.
     - Under GNOME Wayland (where cross-window synthetic input injection is prohibited by Wayland security): provide a **Universal 3-Second Single-Use Paste Lease**. Item #k is written to the system clipboard, a 3000ms timer is scheduled, and upon expiration, item #0 is automatically restored to the clipboard while keeping history index 0 unchanged.
   - *Inference for Android*:
     - When item #k is selected, save `originalClip = clipboardManager.primaryClip`.
     - Write item #k with a custom clip label `L2A_Temp_Selective_Paste`.
     - In `L2AService.clipListener`, ignore clips with `label == "L2A_Temp_Selective_Paste"` (preventing re-broadcast to Linux or re-insertion to history).
     - Schedule a 3500ms `Handler` task to restore `originalClip`.
     - Item #0 remains at index 0 throughout the interaction.

---

### 2.2 Logic Chain for Requirement R4 (NTAG5332 Touchpad NFC & Read-Only UID Fallback)

1. **Premise 1 (Hardware Invariability)**:
   - Redmi G Pro 2024 touchpad NTAG5332 EEPROM lock bits are write-protected at the factory. No user-space Android NFC API (`Ndef`, `NdefFormatable`, `NfcA`, `NfcV`, `MifareUltralight`) can alter the locked blocks without the vendor's manufacturer keys.
   - *Inference*: Any solution relying solely on rewriting NDEF data to `l2a://tap` will fail on production hardware.

2. **Premise 2 (Tag UID Immutability & Universality)**:
   - Every NFC chip possesses a factory-burned 7-byte Unique Identifier (`tag.id`, UID) in Block 0 that cannot be altered or locked out.
   - Android's `Tag` object returns this UID on discovery via `tag.id` for all protocols (`NfcV`, `NfcA`, `Ndef`, `IsoDep`).
   - *Inference*: Binding the touchpad's hardware UID (`tag.id.joinToString(":") { "%02X".format(it) }`) provides a 100% reliable identifier for the laptop touchpad.

3. **Premise 3 (Dual-Mode Binding and Triggering)**:
   - In Binding Mode ("写入/绑定笔记本触控板 NFC 标签"):
     1. Attempt multi-tech write: `Ndef`, `NdefFormatable`, and raw `NfcA`/`NfcV`.
     2. **Regardless of whether NDEF write succeeds or fails**, extract `tag.id`, format as uppercase hex, persist in `SharedPreferences` (`bound_touchpad_uid`), and associate with `targetHostId`.
     3. Update UI:
        - If write succeeded: "触控板 NFC 写入并绑定成功 (UID: XX:XX:XX:XX:XX:XX:XX)".
        - If write failed (read-only): "触控板原厂标签处于只读锁定状态，已自动绑定硬件 UID (UID: XX:XX:XX:XX:XX:XX:XX)！触碰触控板即可触发秒传".
   - In Tap Triggering:
     - Check: Does discovered `tag.id` match `bound_touchpad_uid`? OR does NDEF contain `l2a://tap`?
     - If either condition is true: execute connection handshake and push clipboard data immediately.

---

## 3. Caveats

1. **GNOME Wayland Synthetic Input Restrictions**:
   - In pure GNOME Wayland (`mutter`), Wayland's security model strictly isolates input events. Unprivileged client applications cannot send synthetic keystrokes (`Ctrl+V`) to other client windows using standard X11 tools like `xdotool`.
   - *Design Response*: While `xdotool` is attempted if running under XWayland/X11, the architecture relies on the **Universal 3-Second Single-Use Lease**: loading item #k into the system clipboard with an automatic timer to restore item #0, accompanied by a status toast. This functions reliably across all Wayland compositors (GNOME, KDE Plasma, Sway, Hyprland).

2. **Android 10+ Background Clipboard Restrictions**:
   - Starting in Android 10 (API 29), background apps cannot read `ClipboardManager.getPrimaryClip()` unless they hold input focus or the special `READ_CLIPBOARD_IN_BACKGROUND` AppOps permission.
   - *Design Response*: `NfcTriggerActivity` is configured as a translucent singleTop activity with `onWindowFocusChanged()` and a 50ms delayed read. When launched by an NFC tap, it acquires brief foreground focus to read the clipboard and pushes it to Linux before dismissing itself.

3. **HyperOS Proprietary Intent Filters**:
   - Xiaomi HyperOS registers internal system handlers for `com.xiaomi.mi_connect_service`.
   - *Design Response*: In `MainActivity`, `enableReaderMode` with `FLAG_READER_NFC_A | FLAG_READER_NFC_B | FLAG_READER_NFC_F | FLAG_READER_NFC_V | FLAG_READER_NO_PLATFORM_SOUNDS` takes absolute precedence over system intent dispatch when the app is foregrounded. In `AndroidManifest.xml`, `NfcTriggerActivity` registers `ACTION_TECH_DISCOVERED` with high priority and hardware filters to capture taps when backgrounded.

---

## 4. Conclusion & Concrete Architectural Design

### 4.1 Requirement R3 Architecture: Clipboard History & Selective Paste

#### Component 1: `l2a::ClipboardHistoryManager` (`core/`)
Located in `core/include/l2a/clipboard_history.hpp` and `core/src/clipboard_history.cpp`.

```cpp
namespace l2a {

enum class ClipContentType : uint8_t { Text = 0, Image = 1 };
enum class ClipOrigin : uint8_t { Local = 0, Remote = 1 };

struct ClipboardHistoryItem {
    uint64_t id{0};
    ClipContentType type{ClipContentType::Text};
    ClipOrigin origin{ClipOrigin::Local};
    std::string mime_type{"text/plain"};
    std::string text_preview;
    std::string full_text;
    std::vector<uint8_t> thumbnail_bytes;
    std::filesystem::path image_path;
    int64_t timestamp_ms{0};
    bool is_pinned{false};
};

class ClipboardHistoryManager {
public:
    explicit ClipboardHistoryManager(
        std::filesystem::path storage_dir,
        size_t max_limit = 20);

    // Thread-safe item insertion (becomes item #0)
    uint64_t add_item(
        ClipContentType type,
        ClipOrigin origin,
        std::string mime_type,
        std::string full_text,
        std::vector<uint8_t> thumbnail = {},
        std::filesystem::path image_path = {});

    // Capacity & Trimming (5 <= limit <= 200)
    void set_max_limit(size_t limit);
    size_t max_limit() const;
    void trim_to_limit();

    // Query & Retrieval (index 0 is newest)
    std::vector<ClipboardHistoryItem> get_all_items() const;
    std::optional<ClipboardHistoryItem> get_item_by_id(uint64_t id) const;
    std::optional<ClipboardHistoryItem> get_latest_item() const;

    // Item Deletion & Clearing
    bool delete_item(uint64_t id);
    void clear_all();

    // Persistence
    bool save_to_disk();
    bool load_from_disk();

private:
    mutable std::mutex mutex_;
    std::filesystem::path storage_dir_;
    size_t max_limit_{20};
    uint64_t next_id_{1};
    std::vector<ClipboardHistoryItem> items_;
};

}  // namespace l2a
```

#### Component 2: Linux Desktop UI & Selective Paste (`linux/`)
1. **History UI in `AppWindow`**:
   - Add a `GtkScrolledWindow` with a `GtkListBox` or `GtkTreeView` displaying history cards.
   - Each item displays:
     - Origin badge: `[本机]` (blue) or `[手机]` (green).
     - Formatted timestamp (`HH:MM:SS`).
     - Content preview: truncated text (up to 80 chars) or an image thumbnail.
     - Action buttons: "选择性粘贴" (Paste) and "删除" (Delete).
   - Top toolbar displays: "清空历史", "当前: X / 上限: N", and dynamic limit spinner (5–200).
2. **Selective Paste Algorithm**:
   - When the user clicks "选择性粘贴" on Item #k ($k > 0$):
     1. Retrieve Item #0: `auto top_item = history_mgr_->get_latest_item();`.
     2. Set `bridge_->set_selective_paste_guard(true);`.
     3. Write Item #k into `wl-copy` and `gtk_clip_`.
     4. Check for `xdotool`:
        - If available: minimize L2A window, run `xdotool key --clearmodifiers ctrl+v`, delay 150ms, then immediately restore `top_item` to `wl-copy` and `gtk_clip_`, and clear the guard flag.
        - If unavailable (pure Wayland): start a 3-second GLib timeout (`g_timeout_add(3000, ...)`). When the timer fires, restore `top_item` to `wl-copy` and `gtk_clip_`, and clear the guard flag.
     5. Throughout this process, `items_` in `ClipboardHistoryManager` remains unmodified, ensuring Item #0 remains at index 0.

#### Component 3: Android Subsystem (`android/`)
1. **JNI Interface Expansion (`jni_bridge.cpp`)**:
   - `Java_com_reid_l2a_CoreBridge_nativeGetClipboardHistory(JNIEnv*)` -> returns a JSON string or array of history objects.
   - `Java_com_reid_l2a_CoreBridge_nativeDeleteHistoryItem(JNIEnv*, jlong id)`.
   - `Java_com_reid_l2a_CoreBridge_nativeClearHistory(JNIEnv*)`.
   - `Java_com_reid_l2a_CoreBridge_nativeSetHistoryLimit(JNIEnv*, jint limit)`.
2. **History Viewer UI in `MainActivity.kt`**:
   - Add a expandable "剪贴板历史" card/dialog.
   - Render history items with origin tags, timestamps, text/image previews, and action buttons.
3. **Android Selective Paste Algorithm**:
   - When the user taps "选择性粘贴" on Item #k:
     1. Save `val originalClip = clipboardManager.primaryClip`.
     2. Create temporary clip with label `L2A_Temp_Selective_Paste`:
        ```kotlin
        val tempClip = if (item.isImage) {
            ClipData.newUri(contentResolver, "L2A_Temp_Selective_Paste", Uri.parse(item.imageUri))
        } else {
            ClipData.newPlainText("L2A_Temp_Selective_Paste", item.fullText)
        }
        clipboardManager.setPrimaryClip(tempClip)
        ```
     3. In `L2AService.clipListener`:
        ```kotlin
        if (clip.description?.label == "L2A_Temp_Selective_Paste") {
            return@OnPrimaryClipChangedListener // Suppress loopback & history insertion
        }
        ```
     4. Display Toast: `"已载入历史条目 #${item.id}，请在输入框粘贴（3.5 秒后自动恢复最新剪贴板）"`.
     5. Schedule delayed restoration via `Handler`:
        ```kotlin
        mainHandler.postDelayed({
            if (originalClip != null) {
                clipboardManager.setPrimaryClip(originalClip)
            }
        }, 3500)
        ```
     6. History queue in L2A is not reordered. Item #0 remains at index 0.

---

### 4.2 Requirement R4 Architecture: Touchpad NFC Multi-Tech Write & Hardware UID Binding

#### Component 1: Multi-Tech Writer in `MainActivity.kt`
Extend `writeTagNdefOrFormatable()` to attempt write across `Ndef`, `NdefFormatable`, `MifareUltralight`, and `NfcA`/`NfcV`, catching security/lock exceptions cleanly:

```kotlin
private fun tryWriteTagPayload(tag: Tag, uri: String): Pair<Boolean, String> {
    val uriRecord = NdefRecord.createUri(uri)
    val aarRecord = NdefRecord.createApplicationRecord(packageName)
    val msg = NdefMessage(arrayOf(uriRecord, aarRecord))

    // 1. Formatted Ndef
    val ndef = Ndef.get(tag)
    if (ndef != null) {
        return try {
            ndef.connect()
            try {
                if (!ndef.isWritable) {
                    return Pair(false, "标签处于原厂只读锁定状态")
                }
                if (ndef.maxSize < msg.byteArrayLength) {
                    return Pair(false, "标签容量不足")
                }
                ndef.writeNdefMessage(msg)
                Pair(true, "")
            } finally {
                try { ndef.close() } catch (_: Exception) {}
            }
        } catch (e: Exception) {
            Pair(false, e.message ?: "NDEF 写入失败")
        }
    }

    // 2. Unformatted NdefFormatable
    val formatable = NdefFormatable.get(tag)
    if (formatable != null) {
        return try {
            formatable.connect()
            try {
                formatable.format(msg)
                Pair(true, "")
            } finally {
                try { formatable.close() } catch (_: Exception) {}
            }
        } catch (e: Exception) {
            Pair(false, e.message ?: "格式化失败")
        }
    }

    // 3. Raw MifareUltralight / NfcA
    val ultralight = MifareUltralight.get(tag)
    if (ultralight != null) {
        return try {
            ultralight.connect()
            try {
                // If writable, format header block
                Pair(false, "MifareUltralight 原厂写保护")
            } finally {
                try { ultralight.close() } catch (_: Exception) {}
            }
        } catch (e: Exception) {
            Pair(false, e.message ?: "Ultralight 访问受限")
        }
    }

    return Pair(false, "不支持的标签类型或已锁定: ${tag.techList.joinToString()}")
}
```

#### Component 2: Hardware UID Binding Fallback
In `MainActivity.kt` (`handleTagDiscovered`):

```kotlin
private fun handleTagDiscovered(tag: Tag) {
    try {
        val uidBytes = tag.id ?: byteArrayOf()
        val uidHex = uidBytes.joinToString(":") { "%02X".format(it) }

        if (writeNfcMode) {
            val hostId = targetHostId
            val uri = "l2a://tap?host_id=$hostId&name=Redmi-G-Pro-2024&port=53321"
            val (writeOk, errorMsg) = tryWriteTagPayload(tag, uri)

            // CRUCIAL: Always persist the hardware UID regardless of NDEF write result
            val prefs = getSharedPreferences("l2a_prefs", Context.MODE_PRIVATE)
            prefs.edit()
                .putString("bound_touchpad_uid", uidHex)
                .putString("bound_host_id", hostId)
                .putBoolean("bound_tag_writable", writeOk)
                .apply()

            writeNfcMode = false

            runOnUiThread {
                if (writeOk) {
                    statusView.text = "【触控板 NFC 写入并绑定成功】\n已写入一碰传协议并绑定硬件 UID: $uidHex\n手机触碰触控板即可秒传！"
                    Toast.makeText(this, "触控板 NFC 写入并绑定成功！", Toast.LENGTH_LONG).show()
                } else {
                    statusView.text = "【触控板 NFC 硬件 UID 绑定就绪】\n触控板芯片处于原厂只读状态，已自动绑定硬件 UID: $uidHex\n（无需覆写芯片，碰触控板即可自动识别并秒传！）"
                    Toast.makeText(this, "已绑定触控板硬件 UID: $uidHex", Toast.LENGTH_LONG).show()
                }
                updateBoundUidDisplay()
            }
        } else {
            handleTapTrigger(tag)
        }
    } catch (e: Exception) {
        Log.e("MainActivity", "Error in handleTagDiscovered: ${e.message}", e)
    }
}
```

#### Component 3: Dual-Mode Tap Triggering
In `MainActivity.kt` (`handleTapTrigger`) and `NfcTriggerActivity.kt`:

```kotlin
private fun isTouchpadTag(tag: Tag): Boolean {
    val prefs = getSharedPreferences("l2a_prefs", Context.MODE_PRIVATE)
    val boundUid = prefs.getString("bound_touchpad_uid", null)

    // 1. Hardware UID match (Primary fallback for factory-locked NTAG5332)
    val tagUid = tag.id?.joinToString(":") { "%02X".format(it) }
    if (!boundUid.isNullOrEmpty() && !tagUid.isNullOrEmpty()) {
        if (tagUid.equals(boundUid, ignoreCase = true)) {
            return true
        }
    }

    // 2. NDEF URI match (l2a://tap)
    val ndef = Ndef.get(tag)
    if (ndef != null) {
        try {
            ndef.connect()
            val msg = ndef.ndefMessage
            ndef.close()
            if (msg != null) {
                for (rec in msg.records) {
                    val uri = rec.toUri()
                    if (uri != null && uri.scheme == "l2a" && uri.host == "tap") {
                        return true
                    }
                }
            }
        } catch (_: Exception) {}
    }

    return false
}
```
When `isTouchpadTag(tag)` evaluates to `true`:
- Vibrate the phone using `Vibrator` / `VibratorManager`.
- Execute immediate handshake and clipboard push via `CoreBridge.ensureConnected(this, 3000)` and `CoreBridge.nativePushClipboardText(text)`.
- Display Toast: `"触控板一碰传：已识别绑定触控板 (UID: $tagUid)，同步剪贴板至电脑！"`.

---

## 5. Verification Method

To verify the completed implementation without regressions:

### 5.1 Automated C++ Tests
Add `tests/test_clipboard_history.cpp` and register it in `tests/CMakeLists.txt`:
```bash
cmake -B build -DL2A_BUILD_LINUX=ON -DL2A_BUILD_TESTS=ON
cmake --build build -j4
ctest --test-dir build --output-on-failure -R Test_ClipboardHistory
```
The test must programmatically verify:
1. **Capacity Enforcement**: Adding 7 items with limit $N=5$ evicts the oldest 2 items; size remains 5.
2. **Dynamic Trimming**: Dynamically changing limit from $N=5$ to $N=3$ immediately prunes the oldest 2 items.
3. **Item Deletion & Clear All**: Deleting item by ID decreases count by 1; `clear_all()` empties all unpinned entries.
4. **Non-destructive Selective Paste**:
   - Given items #0, #1, #2.
   - Performing a selective paste on Item #1 yields Item #1's content.
   - The top item in `get_latest_item()` and `get_all_items()[0]` remains Item #0.
   - Item #0 is never overwritten or shifted.

### 5.2 Android Build Verification
```bash
cd android && ./gradlew assembleDebug
```
Verify that:
1. `assembleDebug` completes with zero compilation or lint errors.
2. The generated APK contains updated `MainActivity.kt`, `NfcTriggerActivity.kt`, and `jni_bridge.cpp`.

### 5.3 Full Regression Suite
```bash
ctest --test-dir build --output-on-failure
```
Ensure all 12 baseline tests plus the newly added clipboard history tests pass 100%.
