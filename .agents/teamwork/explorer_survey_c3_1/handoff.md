# Requirement R1 Technical Scope, Root-Cause Analysis, and Architecture Specification

## 1. Observation

### 1.1 Linux Desktop Subsystem (GTK3 & Wayland)
1. **Wayland Window Focus Requirement Root Cause**:
   - In `linux/src/wayland_clipboard.cpp:211-222`:
     ```cpp
     void WaylandClipboardBridge::start_gtk_monitoring() {
         GdkAtom clip_atom = gdk_atom_intern_static_string("CLIPBOARD");
         gtk_clip_ = gtk_clipboard_get(clip_atom);
         if (gtk_clip_) {
             g_signal_connect(gtk_clip_, "owner-change", on_gtk_owner_change, this);
         }
         if (is_wayland_session()) {
             start_wayland_watcher();
         }
     }
     ```
   - In `linux/src/wayland_clipboard.cpp:224-237`:
     `on_gtk_owner_change` triggers `gtk_clipboard_request_text(clipboard, on_gtk_text_received, self)`.
     Under Ubuntu GNOME Wayland (`mutter`), GTK3's GDK Wayland backend (`gdkselection-wayland.c`) maps clipboard operations to `wl_data_device.selection`. GNOME Mutter strictly adheres to the Wayland security specification: `wl_data_device.selection` events are delivered **only** to client surfaces currently holding keyboard focus. When `l2a-desktop` is minimized to the system tray or behind another window (e.g. browser, terminal), Mutter silences selection events to `l2a-desktop`. Thus `on_gtk_owner_change` never fires until the window is explicitly clicked.
   - In `linux/src/wayland_clipboard.cpp:385-399`:
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
     Investigation via shell probes (`which wl-paste wl-copy`) verified that neither `wl-paste` nor `wl-copy` is installed by default on the system (exit code 127). Furthermore, even if installed, `wl-paste --watch` relies on the wlroots data control protocol (`wlr-data-control-unstable-v1`), which GNOME Mutter does NOT implement. In GNOME Wayland sessions, `wl-paste --watch` immediately exits with an unsupported protocol error.

2. **XWayland Availability & X11 Library Probing**:
   - Probe results: GNOME Shell version 50 is running. XWayland is the default X11 translation server on GNOME Wayland.
   - Libraries present: `/usr/lib/x86_64-linux-gnu/libX11.so`, `/usr/lib/x86_64-linux-gnu/libXfixes.so.3` exist on the host filesystem.
   - X11 semantics: In X11/XWayland, selection event delivery is NOT constrained by window focus. Any X11 client can register on the root window for `XFixesSetSelectionOwnerNotifyMask` via `XFixesSelectSelectionInput(dpy, root, XA_CLIPBOARD, ...)` and receive immediate owner change notifications in the background, even when completely headless or unmapped.
   - Mutter bidirectional bridge: GNOME Mutter contains built-in bidirectional clipboard mirroring between native Wayland surfaces and XWayland (`meta-x11-selection.c`). When a native Wayland app copies, Mutter claims the X11 `CLIPBOARD` selection in XWayland. When an X11 client claims the `CLIPBOARD` selection, Mutter mirrors it to Wayland clients.

3. **Existing Settings and Clipboard Toggles in Linux**:
   - `linux/src/wayland_clipboard.hpp:26-27` and `58`:
     Only a single atomic boolean `sync_enabled_` exists.
   - `linux/src/tray_indicator.cpp:32-36`:
     Only a single menu toggle exists: "启用双向剪贴板无感同步" (Enable bidirectional clipboard auto-sync).
   - In `linux/src/main.cpp:35-42`:
     `l2a::EngineConfig` is initialized with `config.storage_dir = home_dir / ".config" / "linux_to_android"`, but there is currently no persistent settings file (such as `settings.json`) or UI settings dialog.

### 1.2 Android Subsystem (Kotlin & JNI)
1. **Background Clipboard Limitation on Android 10+ / HyperOS**:
   - In `android/app/src/main/java/com/reid/l2a/L2AService.kt:40-57`:
     ```kotlin
     private val clipListener = ClipboardManager.OnPrimaryClipChangedListener {
         val clip = clipboardManager.primaryClip ?: return@OnPrimaryClipChangedListener
         ...
         Thread {
             CoreBridge.nativePushClipboardText(text)
         }.start()
     }
     ```
   - On Android 10+ (API 29+) through Android 14 (HyperOS on Xiaomi 14 Pro), AOSP `ClipboardService` strictly enforces that `getPrimaryClip()` returns `null` or throws a security exception if the calling application does not possess current window focus (`hasWindowFocus() == false`) and is not the default IME.
   - In addition, Xiaomi HyperOS enforces an OEM permission `READ_CLIPBOARD` (AppOps 29). Even with AppOps allowed, AOSP focus checks block background background services from calling `getPrimaryClip()`.
   - In `android/app/src/main/java/com/reid/l2a/NfcTriggerActivity.kt:20-25` and `99-109`:
     The app currently uses `NfcTriggerActivity` (a translucent zero-UI Activity) as a manual focus trampoline:
     ```kotlin
     override fun onWindowFocusChanged(hasFocus: Boolean) {
         super.onWindowFocusChanged(hasFocus)
         if (hasFocus && !handled) {
             handled = true
             window.decorView.postDelayed({ triggerClipboardSync() }, 50)
         }
     }
     ```
     This proves that acquiring window focus for 50ms completely bypasses the Android 10+ restriction and allows reading `primaryClip`.
   - In `android/app/src/main/AndroidManifest.xml`:
     `<uses-permission android:name="android.permission.SYSTEM_ALERT_WINDOW" />` is currently **missing** from `AndroidManifest.xml`, although `AdbHelper::setup_android_permissions_and_usb_tunnel` in `linux/src/adb_helper.cpp:188` already attempts to grant it via:
     `adb shell cmd appops set com.reid.l2a SYSTEM_ALERT_WINDOW allow`.
   - With `SYSTEM_ALERT_WINDOW` granted, Android allows creating a 1x1 transparent overlay or launching trampoline activities from background services without hitting Background Activity Launch (BAL) blocks.

2. **Existing Android Settings & Preferences**:
   - In `android/app/src/main/java/com/reid/l2a/L2AService.kt:123-129` and `MainActivity.kt`:
     `SharedPreferences` named `"l2a_prefs"` stores `last_peer_ip`, `last_peer_port`, `last_peer_id`, and `last_peer_name`.
     There are currently no persistent settings for directional auto-sync toggles or history retention limits.

### 1.3 Core Protocol & Clipboard Capabilities
1. **Clipboard Streaming Protocol (`core/include/l2a/protocol.hpp:29-31`, `104-129`)**:
   - `FrameType::kClipOffer` (0x10): advertises `clip_id`, `mime_type`, `total_size`, `sha256`, `lazy_pull`.
   - `FrameType::kClipDataChunk` (0x11): streams chunks up to 64KB.
   - `FrameType::kClipPullReq` (0x12): carries `clip_id`.
2. **Current Handling in `core/src/clipboard_stream.cpp:335-358`**:
   - `kClipPullReq` only looks up `lazy_sources_.find(req->clip_id)`.
   - If `clip_id == 0`, it is unhandled. By reserving `clip_id == 0` as a generic "Pull Remote Latest Clipboard" command, both Linux and Android can implement the manual "Pull remote clipboard" feature without altering wire protocol framing!

---

## 2. Logic Chain

```
               [Requirement R1 Core Pillars]
                /             |             \
   [Dual-end Settings]  [True Background]   [Manual Control Mode]
           |                  |                      |
     +-----+-----+      +-----+-----+          +-----+-----+
     |           |      |           |          |           |
 [Linux GTK] [Android] [Linux]  [Android]   [Linux GTK] [Android]
     |           |      |           |          |           |
SettingsDialog  Prefs  XWayland  1x1 Overlay  Send/Pull   Notification
     +           +     XFixes   & Trampoline   Buttons    Action Buttons
settings.json  SharedPreferences
```

### 2.1 Settings Architecture Logic Chain
1. *Observation 1.1.3 & 1.2.2*: Neither Linux nor Android currently has a unified settings model for directional auto-share or history limits.
2. *Deduction*:
   - Define a shared data schema:
     - `auto_share_pc_to_phone` (bool, default: `true`)
     - `auto_share_phone_to_pc` (bool, default: `true`)
     - `history_retention_limit` (int, default: `20`, clamped to `[5, 200]`)
   - **Linux Storage**: Store in `${config_.storage_dir}/settings.json` (`~/.config/linux_to_android/settings.json`). Provide a thread-safe C++ `AppSettings` class with zero-dependency JSON serialization/deserialization.
   - **Linux UI**: Add a "偏好设置" (Preferences / Settings) dialog button to `AppWindow` and a menu item in `TrayIndicator`. The dialog provides GTK switch toggles for each direction and a `GtkSpinButton` (5 to 200, step 1, default 20).
   - **Android Storage**: Store in `SharedPreferences` (`"l2a_settings"`).
   - **Android UI**: Add a dedicated Settings Card/Dialog in `MainActivity.kt` with Material `Switch` toggles for "手机 → 电脑 实时自动共享" and "电脑 → 手机 实时自动同步", and a `NumberPicker` or `SeekBar` for history retention limit (5-200).

### 2.2 Linux Background Seamless Sync Logic Chain
1. *Observation 1.1.1*: GNOME Mutter silences Wayland selection events for unfocused windows, and `wl-paste --watch` fails due to lack of `wlr-data-control` and missing binary.
2. *Observation 1.1.2*: XWayland is active in GNOME Wayland sessions, and `/usr/lib/x86_64-linux-gnu/libX11.so` / `libXfixes.so.3` are present on the host. X11 delivers `XFixesSetSelectionOwnerNotify` to registered background clients regardless of window focus. Mutter bidirectionally bridges X11 and Wayland clipboards.
3. *Deduction*:
   - Implement an in-process X11 background clipboard monitor (`X11ClipboardMonitor`) dynamically linking or dlopening `libX11.so.6` and `libXfixes.so.3`.
   - **Background Capture**:
     1. Connect to `$DISPLAY` (or fallback to `:0`, `:1`).
     2. Create a 1x1 unmapped input-only helper window.
     3. Call `XFixesSelectSelectionInput(dpy, root, clipboard_atom, XFixesSetSelectionOwnerNotifyMask)`.
     4. On `XFixesSetSelectionOwnerNotify`: if the owner is NOT our helper window, request conversion for `TARGETS`, `UTF8_STRING`, or `image/png`. Read the property data in `SelectionNotify`.
     5. If `auto_share_pc_to_phone` is true, check SHA-256 against loopback hashes and push to engine!
   - **Background Write (Ctrl+V in external apps)**:
     1. When remote clipboard arrives from Android and `auto_share_phone_to_pc` is true:
     2. If `wl-copy` is available, invoke `write_to_wl_copy`.
     3. Concurrently, set our X11 helper window as the selection owner: `XSetSelectionOwner(dpy, clipboard_atom, helper_win, CurrentTime)`.
     4. When external Wayland/X11 apps request paste, Mutter converts from our helper window. Thus `Ctrl+V` works in any external app (browser, terminal, editor) with ZERO clicks on `l2a-desktop`!

### 2.3 Android 14 / HyperOS Background Capture Logic Chain
1. *Observation 1.2.1*: Android 10+ / HyperOS returns `null` from `ClipboardManager.getPrimaryClip()` in background service, but succeeds when holding foreground window focus.
2. *Deduction*:
   - Add `<uses-permission android:name="android.permission.SYSTEM_ALERT_WINDOW" />` to `AndroidManifest.xml`.
   - When `SYSTEM_ALERT_WINDOW` is granted:
     `L2AService` maintains a 1x1 transparent `TYPE_APPLICATION_OVERLAY` window via `WindowManager`.
     When `clipListener.onPrimaryClipChanged()` triggers:
     Attempt direct `primaryClip` read. If null:
     Briefly request focus on the 1x1 overlay View (`view.requestFocus()`), read `primaryClip`, and release focus.
   - Fallback when overlay permission is not yet granted:
     Launch `NfcTriggerActivity` with a translucent trampoline intent (`ACTION_READ_CLIP_BACKGROUND`) for 50ms to read `primaryClip` and finish.
   - Directional gating:
     If `auto_share_phone_to_pc` is false, `onPrimaryClipChanged` ignores the event.
     If `auto_share_pc_to_phone` is false, `onClipboardReceived` does not write to system `ClipboardManager`.

### 2.4 Manual Control Mode Logic Chain
1. *Observation 1.3.1 & 1.3.2*: `ClipPullReqPayload` exists in the core protocol.
2. *Deduction*:
   - Reserve `clip_id == 0` in `ClipPullReqPayload` as a special `PULL_LATEST_CLIPBOARD` request.
   - When `engine_.request_remote_clipboard()` is called: it sends `FrameType::kClipPullReq` with `clip_id = 0`.
   - When a peer receives `kClipPullReq{clip_id: 0}`:
     - Linux calls `push_current_clipboard()`.
     - Android calls `CoreBridge.nativePushClipboardText(...)`.
   - UI controls:
     - Linux: Add "立即发送当前剪贴板" and "拉取手机最新剪贴板" buttons in `AppWindow` and menu items in `TrayIndicator`.
     - Android: Add "立即发送手机当前剪贴板到电脑" and "拉取电脑最新剪贴板" buttons in `MainActivity`, and add Notification Action Buttons in `L2AService` persistent notification (`ACTION_PUSH_CLIPBOARD_NOW` and `ACTION_PULL_CLIPBOARD_NOW`).

---

## 3. Caveats

1. **Headless / Non-XWayland Environments**:
   - If a pure Wayland compositor (like Sway or Hyprland) is run WITHOUT XWayland, the X11 bridge will fail to open `DISPLAY`. In that specific environment, `wl-clipboard` (`wl-paste --watch`) is required. However, for the target environment explicitly requested (Ubuntu GNOME Wayland / Mutter), XWayland is always active by default.
2. **Android Background Overlay Permissions**:
   - On Xiaomi HyperOS, `SYSTEM_ALERT_WINDOW` ("显示悬浮窗") can be granted either by the user in Settings -> Apps -> Other permissions -> Display pop-up window, OR automatically via USB debugging with `adb shell cmd appops set com.reid.l2a SYSTEM_ALERT_WINDOW allow`. The Linux desktop UI already has a one-click button "一键 ADB 后台剪贴板授权 & USB 通道" that grants this.
3. **Clipboard Large Object Latency**:
   - For image clipboard items (>1MB), reading and transmitting takes ~50-200ms depending on Wi-Fi bandwidth. The UI must display non-blocking progress without freezing the main event loop.

---

## 4. Conclusion & Concrete Architectural Design

### 4.1 Linux Architecture & Code Changes

```
Linux Desktop App Architecture:
┌─────────────────────────────────────────────────────────────┐
│                       AppWindow (GTK3)                      │
│   ┌──────────────────┐  ┌──────────────────┐  ┌───────────┐ │
│   │ Settings Dialog  │  │ Manual Push/Pull │  │ Drag&Drop │ │
│   └────────┬─────────┘  └────────┬─────────┘  └─────┬─────┘ │
└────────────┼─────────────────────┼──────────────────┼───────┘
             ▼                     ▼                  ▼
┌──────────────────────┐  ┌─────────────────────────────────┐
│ AppSettingsManager   │  │   WaylandClipboardBridge        │
│ (~/.config/.../      │  │   ┌───────────────────────────┐ │
│   settings.json)     │  │   │ X11ClipboardMonitor       │ │
│                      │  │   │ (XFixesSelectionNotify)   │ │
│ - pc_to_phone_auto   │  │   │ Background read & write   │ │
│ - phone_to_pc_auto   │  │   └───────────────────────────┘ │
│ - history_limit: 20  │  │   ┌───────────────────────────┐ │
└──────────────────────┘  │   │ GtkClipboard / wl-copy    │ │
                          │   └───────────────────────────┘ │
                          └─────────────────┬───────────────┘
                                            ▼
                          ┌─────────────────────────────────┐
                          │     Engine (l2a_core C++20)     │
                          └─────────────────────────────────┘
```

#### Proposed Component 1: `linux/src/settings_manager.hpp` & `settings_manager.cpp`
- **Purpose**: Manage persistent application settings in `~/.config/linux_to_android/settings.json`.
- **Interface**:
  ```cpp
  struct AppSettings {
      bool auto_share_pc_to_phone{true};
      bool auto_share_phone_to_pc{true};
      int history_retention_limit{20};  // clamped to 5..200
  };

  class SettingsManager {
  public:
      explicit SettingsManager(std::filesystem::path config_dir);
      AppSettings get() const;
      void update(const AppSettings& settings);
      bool save();
      bool load();
  private:
      std::filesystem::path file_path_;
      mutable std::mutex mutex_;
      AppSettings settings_;
  };
  ```

#### Proposed Component 2: `linux/src/x11_clipboard_monitor.hpp` & `x11_clipboard_monitor.cpp`
- **Purpose**: Background XWayland/X11 clipboard capture & write via `XFixes`.
- **Mechanism**:
  - Dynamically links against `libX11.so` and `libXfixes.so.3` (or declared minimal ABI).
  - Background listener thread runs an X event loop:
    ```cpp
    XFixesSelectSelectionInput(dpy, root, clipboard_atom, XFixesSetSelectionOwnerNotifyMask);
    ```
  - When notified, requests `UTF8_STRING` / `image/png`, reads data via `XGetWindowProperty`, hashes with SHA-256 to prevent echo loops, and calls callback.
  - On write, calls `XSetSelectionOwner` so external apps pressing `Ctrl+V` receive content from Mutter XWayland bridge without `l2a-desktop` window focus.

#### Proposed Modifications to Existing Files:
1. `linux/src/wayland_clipboard.hpp` & `wayland_clipboard.cpp`:
   - Replace or augment `start_wayland_watcher()` with `X11ClipboardMonitor`.
   - Separate directional flags:
     `void set_auto_share_pc_to_phone(bool enabled)`
     `void set_auto_share_phone_to_pc(bool enabled)`
   - In `apply_remote_clipboard`: check `auto_share_phone_to_pc` before applying.
   - In local change callback: check `auto_share_pc_to_phone` before sending.
   - Add `bool push_current_local_clipboard()` for manual send.
2. `linux/src/app_window.hpp` & `app_window.cpp`:
   - Instantiate `SettingsManager`.
   - Add "设置" (Settings) button in UI: opens modal dialog to configure directional toggles and history retention limit (5-200).
   - Add "发送当前剪贴板" (Send Clipboard) and "拉取手机剪贴板" (Pull Clipboard) buttons in the main button row.
   - On pull button clicked: call `engine_.request_remote_clipboard()`.
3. `linux/src/tray_indicator.hpp` & `tray_indicator.cpp`:
   - Add menu items:
     - "立即发送当前剪贴板至手机"
     - "拉取手机最新剪贴板"
     - "电脑 → 手机 实时自动同步" (CheckMenuItem)
     - "手机 → 电脑 实时自动同步" (CheckMenuItem)
     - "偏好设置..." (Opens settings dialog)

---

### 4.2 Android Architecture & Code Changes

```
Android App Architecture:
┌─────────────────────────────────────────────────────────────┐
│                    MainActivity (UI)                        │
│   ┌───────────────────┐  ┌────────────────────────────────┐ │
│   │  Settings Section │  │ Manual Control Buttons         │ │
│   │  - Auto-Share Sw. │  │ - "立即发送手机当前剪贴板"     │ │
│   │  - Limit (5..200) │  │ - "拉取电脑最新剪贴板"         │ │
│   └─────────┬─────────┘  └───────────────┬────────────────┘ │
└─────────────┼────────────────────────────┼──────────────────┘
              ▼                            ▼
┌─────────────────────────────────────────────────────────────┐
│                    L2AService (Foreground)                  │
│   ┌───────────────────────────────────────────────────────┐ │
│   │ 1x1 Overlay Focus View (SYSTEM_ALERT_WINDOW)          │ │
│   │ Background OnPrimaryClipChangedListener Focus Pulse   │ │
│   └───────────────────────────────────────────────────────┘ │
│   ┌───────────────────────────────────────────────────────┐ │
│   │ Persistent Notification Action Buttons                │ │
│   │ [发送剪贴板 (Send)]  [拉取剪贴板 (Pull)]              │ │
│   └───────────────────────────────────────────────────────┘ │
└─────────────────────────────┬───────────────────────────────┘
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                   CoreBridge (JNI to C++)                   │
│   - nativePushClipboardText()                               │
│   - nativeRequestRemoteClipboard()                          │
└─────────────────────────────────────────────────────────────┘
```

#### Proposed Modifications to Existing Android Files:
1. `android/app/src/main/AndroidManifest.xml`:
   - Add `<uses-permission android:name="android.permission.SYSTEM_ALERT_WINDOW" />`.
2. `android/app/src/main/java/com/reid/l2a/L2AService.kt`:
   - Read directional settings from `"l2a_settings"`:
     - `auto_share_to_pc`: if false, ignore clipboard changes in background.
     - `auto_share_from_pc`: if false, do not write received clips to system clipboard.
   - Overlay pulse: When `clipListener` triggers, if `clipboardManager.primaryClip` is null and `Settings.canDrawOverlays(this)` is true, attach/focus 1x1 overlay window for 50ms, retrieve clip, and push to PC.
   - Add `ACTION_PULL_CLIPBOARD_NOW`: calls `CoreBridge.nativeRequestRemoteClipboard()`.
   - Update `startForegroundWithNotification`:
     Add TWO Action buttons to notification:
     1. "发送剪贴板" -> triggers `ACTION_PUSH_CLIPBOARD_NOW`.
     2. "拉取剪贴板" -> triggers `ACTION_PULL_CLIPBOARD_NOW`.
3. `android/app/src/main/java/com/reid/l2a/MainActivity.kt`:
   - Add Settings UI card:
     - Switch: "手机 → 电脑 实时自动共享" (default true)
     - Switch: "电脑 → 手机 实时自动同步" (default true)
     - Retention Limit input/slider: "历史记录上限 (5-200 条，默认 20)"
   - Add "拉取电脑最新剪贴板" button:
     Calls `CoreBridge.nativeRequestRemoteClipboard()`.
4. `android/app/src/main/java/com/reid/l2a/CoreBridge.kt` & `jni_bridge.cpp`:
   - Add `external fun nativeRequestRemoteClipboard(): Boolean`.
   - Implement `Java_com_reid_l2a_CoreBridge_nativeRequestRemoteClipboard`: calls `g_engine->request_clipboard_lazy_pull(0)` or `g_engine->request_remote_clipboard()`.

---

### 4.3 Core C++ Engine Changes (`core/`)

1. `core/include/l2a/engine.hpp` & `core/src/engine.cpp`:
   - Add API: `bool request_remote_clipboard();`
     Implementation: sends `FrameType::kClipPullReq` with `clip_id = 0`.
   - In `ICoreListener`:
     Add optional `virtual void on_clipboard_pull_requested() {}`.
   - When `kClipPullReq` arrives with `clip_id == 0`:
     Listener is notified so it can push its current local clipboard to the requester!

---

## 5. Verification Method

### 5.1 Automated Unit & Integration Tests to Add
1. **Settings Persistence Test (`tests/test_settings.cpp`)**:
   - Verify serialization and deserialization of `settings.json`.
   - Verify defaults: `auto_share_pc_to_phone=true`, `auto_share_phone_to_pc=true`, `history_retention_limit=20`.
   - Verify boundary clamping: setting limit < 5 clamps to 5, > 200 clamps to 200.
2. **Directional Sync Suppression Test (`tests/test_directional_clipboard_sync.cpp`)**:
   - Verify when `auto_share_pc_to_phone` is false, local clipboard updates do not emit `kClipOffer` frames over TLS channel.
   - Verify manual push (`push_clipboard_text`) still succeeds even when auto-share is false.
   - Verify manual pull: sending `kClipPullReq{clip_id: 0}` triggers remote clipboard response and delivery.
3. **Linux Subsystem Test Expansion (`linux/tests/test_linux_subsystem.cpp`)**:
   - Add test verifying `X11ClipboardMonitor` initialization, atom resolution, and echo hash suppression.

### 5.2 Verification Commands
1. **Linux Build & CTest**:
   ```bash
   cmake -B build -DL2A_BUILD_LINUX=ON
   cmake --build build -j4
   ctest --test-dir build --output-on-failure -V
   ```
2. **Android APK Build**:
   ```bash
   export JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64
   export ANDROID_USER_HOME=$(pwd)/.android_home
   cd android && ./gradlew assembleDebug -Duser.home=$(pwd)/.gradle_home
   ```
3. **Real-Device Live Validation**:
   - Launch `build/linux/l2a-desktop`.
   - Minimize window to tray.
   - Copy text in Firefox/Terminal on Linux. Verify phone receives notification and clipboard without focusing `l2a-desktop`.
   - In Android app, toggle "手机 → 电脑 实时自动共享" OFF. Copy text on phone. Verify PC does not automatically receive it.
   - Click "发送当前剪贴板" in Android notification. Verify PC receives it immediately.
   - Click "拉取手机最新剪贴板" in Linux GTK window. Verify PC pulls phone's latest clipboard.
