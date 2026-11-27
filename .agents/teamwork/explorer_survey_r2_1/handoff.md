# Handoff Report: Investigation of Requirements R1 and R2 (Core & Linux Desktop)

## 1. Observation

### 1.1 Core Engine Channel Lifecycle and Clipboard Transmission
- **File**: `core/src/engine.cpp`, Lines 375–385:
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
  `Engine::send_frame_active` checks `active_channel_`. If `active_channel_` is `nullptr` (unconnected or disconnected), it immediately returns `false`. There is no on-demand connection attempt, no candidate queue, and no fallback to known peers or ADB ports.

- **File**: `core/include/l2a/engine.hpp`, Lines 116–123 & `core/src/engine.cpp`, Lines 85–96:
  ```cpp
  if (config_.discovery_port != 0) {
      discovery_mgr_ = std::make_unique<DiscoveryManager>(
          identity_.device_id,
          identity_.device_name,
          config_.platform,
          bound_port_.load(),
          identity_.fingerprint,
          config_.discovery_port);
      discovery_mgr_->start([this](const DiscoveredPeer& peer) {
          if (listener_) listener_->on_peer_discovered(peer);
      });
  }
  ```
  `Engine` does NOT store discovered peer IP addresses or ports. When `discovery_mgr_` discovers a peer via UDP, it merely forwards it to `listener_->on_peer_discovered(peer)`. `Engine` has no member variable tracking `last_discovered_peer_` or candidate endpoints.

- **File**: `core/src/clipboard_stream.cpp`, Lines 45–97:
  ```cpp
  bool ClipboardStreamManager::push_clipboard_bytes(
      std::string_view mime_type,
      std::span<const uint8_t> content,
      bool force_lazy_pull) {
      ...
      Frame offer_frame{};
      offer_frame.header.type = FrameType::kClipOffer;
      offer_frame.header.stream_id = stream_id;
      offer_frame.header.flags = is_lazy ? FrameFlags::kFin : FrameFlags::kNone;
      offer_frame.payload = offer.encode();

      if (!send_frame_fn_(offer_frame)) {
          return false;
      }
      ...
  }
  ```
  `ClipboardStreamManager` calls `send_frame_fn_` (which is `Engine::send_frame_active`). When `active_channel_` is null, `send_frame_fn_` returns `false`, and the clipboard push is immediately aborted.

### 1.2 Linux Desktop Wayland Clipboard Gatekeeping
- **File**: `linux/src/wayland_clipboard.cpp`, Lines 263–272:
  ```cpp
  if (self->engine_.is_connected()) {
      if (self->engine_.push_clipboard_text(utf8_str)) {
          if (self->log_cb_) {
              std::string preview = utf8_str.substr(0, std::min<size_t>(40, utf8_str.size()));
              self->log_cb_("[剪贴板 -> 手机] 已同步文本 (" +
                            std::to_string(utf8_str.size()) + " B): " + preview);
          }
      }
  }
  ```
- **File**: `linux/src/wayland_clipboard.cpp`, Lines 461–470:
  ```cpp
  if (engine_.is_connected()) {
      if (engine_.push_clipboard_text(text)) {
          if (log_cb_) {
              std::string preview = text.substr(0, std::min<size_t>(40, text.size()));
              log_cb_("[Wayland后台 -> 手机] 已捕获并同步剪贴板 (" +
                      std::to_string(text.size()) + " B): " + preview);
          }
      }
  }
  ```
  Both the GTK owner-change listener (`on_gtk_text_received`) and the background Wayland watcher (`wayland_watcher_loop`) evaluate `if (engine_.is_connected())`. If false, the captured clipboard event is **silently discarded** without even calling `push_clipboard_text()`.

### 1.3 Linux Desktop Clipboard Application Under Wayland
- **File**: `linux/src/wayland_clipboard.cpp`, Lines 297–306:
  ```cpp
  is_applying_remote_.store(true);

  if (gtk_clip_) {
      gtk_clipboard_set_text(gtk_clip_, text.c_str(), static_cast<gint>(text.size()));
  } else if (has_wl_clipboard_tools()) {
      write_to_wl_copy("text/plain", item.data.data(), item.data.size());
  }

  is_applying_remote_.store(false);
  ```
- **File**: `linux/src/wayland_clipboard.cpp`, Lines 212–216:
  ```cpp
  void WaylandClipboardBridge::start_gtk_monitoring() {
      GdkAtom clip_atom = gdk_atom_intern_static_string("CLIPBOARD");
      gtk_clip_ = gtk_clipboard_get(clip_atom);
      if (gtk_clip_) {
          g_signal_connect(gtk_clip_, "owner-change", on_gtk_owner_change, this);
      }
  ```
  `gtk_clip_` is initialized in `start_gtk_monitoring()` via `gtk_clipboard_get()` and is always non-null. Therefore, `else if (has_wl_clipboard_tools())` is **dead code** for text MIME types!
  Under Wayland compositors (GNOME Mutter), a client window cannot set the global system clipboard selection via `gtk_clipboard_set_text()` unless the window currently has active keyboard/input focus. Because `l2a-desktop` is running in the background or minimized, `gtk_clipboard_set_text()` is ignored by the compositor. As a result, when the user presses `Ctrl+V` in an external application (browser, editor, terminal), the received text is NOT pasted!

- **File**: `linux/src/wayland_clipboard.cpp`, Line 287:
  ```cpp
  if (item.mime_type.rfind("text/", 0) == 0 && !item.data.empty()) {
  ```
  When text payload exceeds `kMaxInlineMemoryBytes` (512 KB), `ClipboardStreamManager` writes it to `item.temp_file_path`, leaving `item.data` empty. In `apply_remote_clipboard`, because `!item.data.empty()` evaluates to false, large text items bypass text handling entirely and fall through to binary `wl-copy` without computing `last_applied_hash_`.

### 1.4 Android Side Connection & Notification Lifecycle
- **File**: `android/app/src/main/java/com/reid/l2a/L2AService.kt`, Lines 89–93:
  ```kotlin
  // Attempt USB ADB Reverse Tunnel (127.0.0.1:53321) immediately if plugged in via USB
  Thread {
      CoreBridge.nativeConnect("127.0.0.1", 53321)
  }.start()
  ```
  `L2AService` attempts to connect to `127.0.0.1:53321`. But port `53321` is the Android service's own listening port! The reverse tunnel set up by Linux desktop (`adb reverse tcp:53323 tcp:<transfer_port>`) maps to Android local port `53323`. Connecting to `53321` is attempting to connect to itself, which immediately fails. Moreover, this is only attempted once in `onCreate()`.

- **File**: `android/app/src/main/java/com/reid/l2a/L2AService.kt`, Lines 148–154:
  ```kotlin
  if (!text.isNullOrEmpty()) {
      lastReceivedText = text
      mainHandler.post {
          clipboardManager.setPrimaryClip(ClipData.newPlainText("LinuxClipboard", text))
      }
  }
  ```
  When clipboard text is received on Android, `onClipboardReceived` updates the clip data but does NOT update the foreground notification or show any user-facing feedback (such as a Toast or notification). On Xiaomi HyperOS (Android 14), background clipboard writes without user notification provide zero confirmation.

- **File**: `android/app/src/main/java/com/reid/l2a/NfcTriggerActivity.kt`, Lines 33–42:
  ```kotlin
  val text = clip.getItemAt(0).coerceToText(this)?.toString()
  if (!text.isNullOrEmpty()) {
      Thread {
          CoreBridge.nativePushClipboardText(text)
      }.start()
      Toast.makeText(this, "已通过一碰传同步剪贴板至 Linux", Toast.LENGTH_SHORT).show()
  }
  finish()
  ```
  `NfcTriggerActivity` directly invokes `CoreBridge.nativePushClipboardText(text)`. If `active_channel_` is not established, the frame is silently dropped without attempting to connect to the discovered PC IP or `127.0.0.1:53323`.

---

## 2. Logic Chain

1. **Why Initial Clipboard Sync Failed (R1)**:
   - When the user starts both apps and connects via Wi-Fi or USB:
     - On USB: UDP multicast/broadcast does not cross ADB. No UDP discovery packet is ever received by either device.
     - On Wi-Fi: If router AP isolation is enabled, UDP discovery beacons on port 53320 are dropped. Even if a UDP beacon is received by `AppWindow::on_peer_discovered()`, `Engine` does not persist the peer's endpoint information.
     - In both scenarios, `active_channel_` on PC remains `nullptr`, and `is_connected()` is `false`.
   - The user presses `Ctrl+C` on Linux:
     - `WaylandClipboardBridge` checks `if (self->engine_.is_connected())` (Observation 1.2). Since `is_connected()` is `false`, the event is dropped on the floor.
     - Even if `push_clipboard_text()` were called, `Engine::send_frame_active()` checks `if (!ch) return false;` (Observation 1.1) and immediately fails because it has no on-demand connection logic and does not store the target peer address or ADB fallback port `127.0.0.1:53322`.
   - Why did sending a file "unblock" clipboard sync in user testing?
     - In user testing, when the user attempted file transfer or clicked "一键 ADB 后台剪贴板授权 & USB 通道" (`AppWindow::run_adb_setup()`), it executed `AdbHelper::setup_android_permissions_and_usb_tunnel()` followed by `engine_.connect_to_peer("127.0.0.1", 53322)` (`linux/src/app_window.cpp:309`).
     - Once `connect_to_peer` succeeded, `attach_channel()` stored the connection in `active_channel_` and set `connected_ = true`.
     - After that, any subsequent `Ctrl+C` found `engine_.is_connected() == true` and `active_channel_ != nullptr`, so clipboard sync began working. Thus, clipboard sync was artificially dependent on an external action having already connected `active_channel_`.

2. **Why Linux Desktop Fails to Paste Received Clipboard (R2 - Linux side)**:
   - When Android sends clipboard text to Linux over an established channel, `Engine::channel_recv_loop` delivers the frame to `ClipboardStreamManager` -> `AppWindow::on_clipboard_received` -> `flush_ui_events` -> `WaylandClipboardBridge::apply_remote_clipboard`.
   - Inside `apply_remote_clipboard`, lines 299–303:
     ```cpp
     if (gtk_clip_) {
         gtk_clipboard_set_text(gtk_clip_, text.c_str(), static_cast<gint>(text.size()));
     } else if (has_wl_clipboard_tools()) {
         write_to_wl_copy("text/plain", item.data.data(), item.data.size());
     }
     ```
     `gtk_clip_` is always non-null. The `write_to_wl_copy` branch is never entered (Observation 1.3).
   - Under Wayland (GNOME Mutter on Ubuntu), the compositor protocol enforces that a client window cannot claim clipboard ownership (`wl_data_device.set_selection`) unless it currently has active keyboard/seat focus.
   - Because `l2a-desktop` is running in the background or minimized to tray while the user is working in another app (browser, IDE, terminal), the compositor rejects or ignores `gtk_clipboard_set_text()`.
   - When the user presses `Ctrl+V` in their active application, the compositor queries its current data source, which was NEVER updated. The user pastes stale text or nothing at all.
   - When `wl-copy` is executed, it communicates with the compositor data-control interface to set the selection system-wide without requiring active window focus. Therefore, skipping `wl-copy` under Wayland is the exact root cause of `Ctrl+V` failing.

---

## 3. Caveats

1. **Wayland Tools Availability**:
   - `wl-copy` requires the `wl-clipboard` system package. If `wl-copy` is not installed on Ubuntu GNOME, clipboard injection falls back to `gtk_clipboard_set_text()`, which can only update the clipboard when `l2a-desktop` has window focus.
2. **Android 10+ / HyperOS Background Read Restrictions**:
   - Android OS does not permit background services to read `ClipboardManager.getPrimaryClip()` unless granted `READ_CLIPBOARD_IN_BACKGROUND` via AppOps (`adb shell cmd appops set com.reid.l2a READ_CLIPBOARD allow`). `NfcTriggerActivity` circumvents this by momentarily acquiring foreground focus.
3. **USB Tunnel Port Conventions**:
   - PC forwards local port 53322 to Android service port 53321: `adb forward tcp:53322 tcp:53321`.
   - PC reverses Android local port 53323 to PC transfer port: `adb reverse tcp:53323 tcp:<transfer_port>`.
   - Android must connect to `127.0.0.1:53323` (NOT `53321`) to reach Linux.

---

## 4. Conclusion & Concrete Recommended Fixes

### 4.1 Fix for Requirement R1 (On-demand Channel Establishment & Fallback)

#### A. C++ Core: Candidate Peer Endpoints & On-demand Connection in `Engine`
1. In `core/include/l2a/engine.hpp`:
   - Add candidate endpoint data structure and registration methods:
     ```cpp
     struct PeerEndpoint {
         std::string ip;
         uint16_t port{0};
     };
     void register_candidate_peer(const std::string& ip, uint16_t port);
     bool ensure_connected();
     ```
   - In `core/src/engine.cpp`:
     - In `Engine::start()`, when `discovery_mgr_` discovers a peer:
       ```cpp
       discovery_mgr_->start([this](const DiscoveredPeer& peer) {
           register_candidate_peer(peer.ip_address, peer.tcp_port);
           if (listener_) listener_->on_peer_discovered(peer);
       });
       ```
     - Add default fallback endpoint on Linux: `register_candidate_peer("127.0.0.1", 53322)`.
     - Implement `ensure_connected()`:
       ```cpp
       bool Engine::ensure_connected() {
           if (connected_.load()) return true;
           std::vector<PeerEndpoint> candidates;
           {
               std::lock_guard<std::mutex> lock(candidates_mutex_);
               candidates = candidate_endpoints_;
           }
           for (const auto& ep : candidates) {
               if (connect_to_peer(ep.ip, ep.port)) {
                   return true;
               }
           }
           return false;
       }
       ```
     - In `Engine::send_frame_active()` (`core/src/engine.cpp:375`):
       ```cpp
       bool Engine::send_frame_active(const Frame& frame) {
           std::shared_ptr<TlsChannel> ch;
           {
               std::lock_guard<std::mutex> lock(channel_mutex_);
               ch = active_channel_;
           }
           if (!ch) {
               if (ensure_connected()) {
                   std::lock_guard<std::mutex> lock(channel_mutex_);
                   ch = active_channel_;
               }
           }
           if (!ch) {
               return false;
           }
           return ch->send_frame(frame);
       }
       ```

2. Non-blocking timeout in `Engine::connect_to_peer` (`core/src/engine.cpp:213`):
   - Replace blocking `::connect()` with non-blocking connect + `poll(..., 1000)` (1 second timeout) so candidate endpoint iteration is fast and never blocks threads for 75+ seconds.

#### B. Linux Desktop: Unblock Clipboard Gate in `WaylandClipboardBridge`
1. In `linux/src/wayland_clipboard.cpp:263` and line 461:
   - Remove the `if (self->engine_.is_connected())` gate!
   - Replace with background offloaded push:
     ```cpp
     // In on_gtk_text_received:
     std::thread([self, utf8_str = std::move(utf8_str)]() {
         if (self->engine_.push_clipboard_text(utf8_str)) {
             if (self->log_cb_) {
                 std::string preview = utf8_str.substr(0, std::min<size_t>(40, utf8_str.size()));
                 self->log_cb_("[剪贴板 -> 手机] 已同步文本 (" +
                               std::to_string(utf8_str.size()) + " B): " + preview);
             }
         }
     }).detach();
     ```
     By invoking `engine_.push_clipboard_text()` in a background thread, if `active_channel_` is not yet established, `Engine::send_frame_active()` automatically triggers `ensure_connected()`, connects to the phone (via Wi-Fi IP or ADB forward `127.0.0.1:53322`), completes TLS handshake, and sends the clipboard frame—all without stalling the GTK main loop.

#### C. Android Service: Fix USB Reverse Port & Feedback
1. In `android/app/src/main/java/com/reid/l2a/L2AService.kt:91`:
   - Change `CoreBridge.nativeConnect("127.0.0.1", 53321)` to connect to reverse port `53323`.
   - Add periodic reconnection check / candidate fallback to `127.0.0.1:53323` when disconnected.
2. In `L2AService.kt:148`:
   - When clipboard text is received, display a foreground notification and Toast:
     `startForegroundWithNotification("已同步电脑剪贴板: ${text.take(30)}...")`
     `mainHandler.post { Toast.makeText(this, "已同步电脑剪贴板", Toast.LENGTH_SHORT).show() }`

---

### 4.2 Fix for Requirement R2 (Linux Side Wayland Clipboard Application)

#### A. Dual Application (`wl-copy` + `gtk_clipboard_set_text`)
1. In `linux/src/wayland_clipboard.cpp`, Lines 287–312:
   ```cpp
   // BEFORE:
   if (gtk_clip_) {
       gtk_clipboard_set_text(gtk_clip_, text.c_str(), static_cast<gint>(text.size()));
   } else if (has_wl_clipboard_tools()) {
       write_to_wl_copy("text/plain", item.data.data(), item.data.size());
   }

   // AFTER:
   // 1. If running under Wayland, wl-copy is MANDATORY to bypass window focus restrictions
   if (has_wl_clipboard_tools()) {
       write_to_wl_copy("text/plain", item.data.data(), item.data.size());
   }
   // 2. Also update GTK clipboard for internal GTK widgets
   if (gtk_clip_) {
       gtk_clipboard_set_text(gtk_clip_, text.c_str(), static_cast<gint>(text.size()));
   }
   ```
2. Handle large text > 512 KB in `apply_remote_clipboard`:
   ```cpp
   if (item.mime_type.rfind("text/", 0) == 0) {
       std::string text;
       if (!item.data.empty()) {
           text.assign(reinterpret_cast<const char*>(item.data.data()), item.data.size());
       } else if (!item.temp_file_path.empty()) {
           std::ifstream in(item.temp_file_path, std::ios::binary);
           if (in.is_open()) {
               text.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
           }
       }
       if (!text.empty()) {
           auto hash = l2a::sha256_string(text);
           {
               std::lock_guard<std::mutex> lock(hash_mutex_);
               last_applied_hash_ = hash;
               has_applied_hash_ = true;
           }
           is_applying_remote_.store(true);
           if (has_wl_clipboard_tools()) {
               write_to_wl_copy("text/plain", reinterpret_cast<const uint8_t*>(text.data()), text.size());
           }
           if (gtk_clip_) {
               gtk_clipboard_set_text(gtk_clip_, text.c_str(), static_cast<gint>(text.size()));
           }
           is_applying_remote_.store(false);
           ...
       }
   }
   ```

#### B. Android Quick-Send & Focus Timing
1. In `android/app/src/main/java/com/reid/l2a/NfcTriggerActivity.kt`:
   - In `onWindowFocusChanged(hasFocus == true)`, read `primaryClip`. If not currently connected, trigger `CoreBridge.nativeConnect(targetIp, targetPort)` to discovered PC IP or `127.0.0.1:53323` before pushing clipboard.

---

## 5. Verification Method

### 5.1 Unit & Integration Test Strategy
1. **New Automated Test**: `tests/test_initial_clipboard_sync.cpp`
   - **Goal**: Satisfy Acceptance Criterion R1 (Line 82):
     "验证双端在仅完成 UDP 发现或仅配置 ADB 端口转发（未发送任何文件）的初始状态下，调用 `send_clipboard` 能主动建立加密会话并将剪贴板数据 100% 投递至对端。"
   - **Test Setup**:
     - Instantiate `Node A` (Linux Engine, listening on port $P_A$, not connected).
     - Instantiate `Node B` (Android Engine, listening on port $P_B$, not connected).
     - Scenario 1 (Discovered peer candidate): Register `Node B`'s endpoint (`127.0.0.1`, $P_B$) in `Node A`'s candidate list (simulating UDP discovery beacon). Do NOT call `connect_to_peer()` or `send_files()`.
     - Call `Node A.push_clipboard_text("Initial sync without file transfer")`.
     - **Assertion**: Verify that `Node A.push_clipboard_text` triggers on-demand session establishment, completes TLS 1.3 handshake, and `Node B` receives `on_clipboard_received` within 2 seconds with exact matching text.
     - Scenario 2 (Fallback / ADB tunnel candidate): Register fallback port `127.0.0.1:53322` when initial address is unreachable. Verify automatic fallback and delivery.

2. **Linux Subsystem Regression Test**:
   - Extend `linux/tests/test_linux_subsystem.cpp` to verify:
     - `apply_remote_clipboard` executes `wl-copy` even when `gtk_clip_` is active.
     - Anti-echo hash suppression prevents `wl-paste` loops.

### 5.2 Build & Execution Commands
```bash
# 1. Compile C++ core and Linux desktop
cmake -B build -DL2A_BUILD_LINUX=ON
cmake --build build -j4

# 2. Run all unit and integration tests
ctest --test-dir build --output-on-failure -V
```
