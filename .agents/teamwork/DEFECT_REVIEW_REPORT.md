# Linux_to_Android 全模块代码缺陷审查与诊断分析报告 (R1)

**审查日期**: 2026-10-21  
**项目路径**: `/home/reid-pu/Programming/Linux_to_Android`  
**审查范围**: C++ 共享核心库 (`core/`)、Linux 原生桌面端 (`linux/`)、Android 移动端 (`android/`)、构建与测试套件 (`tests/`)

---

## 1. 缺陷综述 (Executive Summary)

本报告基于三位专项探索者（Explorer）对工程全模块进行的深度静态分析与跨模块数据流、控制流排查，共发现 **41 项** 具体缺陷，涵盖：
- **严重等级分布**: 
  - **Critical / High (高危/严重)**: 12 项（含 2 项分布式/并发死锁、2 项远程命令注入、2 项 Use-After-Free/内存越界、3 项 Android JNI 崩溃与磁盘写穿、1 项协议超限断连、1 项持久磁盘泄漏）
  - **Medium (中度)**: 20 项（含网络无心跳冻结、非原子 FD 竞态、未捕获 SIGPIPE 崩溃、GTK UI 主线程卡死、ADB 端口映射错误、HyperOS 进程保活与权限遗漏、Scoped Storage 媒体索引缺失等）
  - **Low (轻度/优化)**: 9 项（含构建配置平台硬编码、常量时间 HMAC 校验、JNI 方法查找缓存等）

---

## 2. 缺陷清单与详细诊断分析 (Detailed Defect Inventory)

### 2.1 C++ 共享核心库 (`core/`)

| 编号 | 模块/文件:行号 | 严重级别 | 类别 | 触发场景 | 根因分析 | 修复方案 |
|:---|:---|:---:|:---:|:---|:---|:---|
| **CORE-01** | `core/src/file_transfer.cpp:235`<br>`core/src/engine.cpp:257` | **CRITICAL** | 并发死锁 | 双方同时发起文件传输 | `Engine::channel_recv_loop` 运行于 `recv_thread_`。当收到 `kFileResumeOffset` 帧时，同步调用 `stream_outgoing_files` 连续发送所有数据块。在此期间，`recv_thread_` 无法调用 `recv_frame()`。当双向传输时，两端接收缓冲区填满，两端均阻塞在 `send()`，造成完全死锁。 | 将 `stream_outgoing_files` 移至专用的发送工作线程/异步任务中执行，确保 `recv_thread_` 仅专注于报文接收。 |
| **CORE-02** | `core/src/file_transfer.cpp:202, 287, 402` | **HIGH** | 锁倒置/死锁 | 传输大文件并在完成回调中操作引擎 | `finalize_incoming_file` 在持有 `FileTransferManager::mutex_` 期间同步执行大文件全盘 SHA-256 计算、发送 ACK 报文（获取 `channel_mutex_` 与 `send_mutex_`）、并触发外部回调 `on_file_completed_`。若回调中调用引擎方法，导致锁倒置与重入死锁。 | 拷贝必要状态后立即释放 `mutex_`，在锁外执行 SHA-256 计算、ACK 发送及回调通知。 |
| **CORE-03** | `core/src/engine.cpp:194, 276` | **HIGH** | 线程自死锁 | 网络异常断开并在回调中重连 | 连接断开时在 `recv_thread_` 中触发 `on_connection_state_changed(false)`。若监听者在回调中调用 `connect_to_peer` 或 `attach_channel`，其内部执行 `if (recv_thread_.joinable()) recv_thread_.join();`，导致线程自等待抛出 `std::system_error` 崩溃。 | 检查 `std::this_thread::get_id() != recv_thread_.get_id()`，避免在当前接收线程内同步 `join` 自己，改为异步脱离或状态标记清理。 |
| **CORE-04** | `core/include/l2a/protocol.hpp:20`<br>`core/src/tls_channel.cpp:793` | **HIGH** | 协议超限断连 | 发送超过 750 个文件的文件夹 | `kMaxFramePayload` 固定为 66,560 字节。`FileBatchMetaPayload` 将整树文件元数据打入单个帧（每文件约 84 字节）。当文件数 > 750 时，帧长超过限制，接收方判定为畸形报文直接断开连接。 | 实施批量元数据分片传输（分页），或合理扩大元数据帧允许上限。 |
| **CORE-05** | `core/src/clipboard_stream.cpp:234, 266, 297` | **HIGH** | 资源泄漏 | 剪贴板大文本/图片传输失败或完成 | 超过 512KB 的剪贴板数据在 `temp_dir_` 中生成临时文件（`l2a_clip_*.tmp`）。在传输中断、校验失败或正常消费后，代码均未调用 `std::filesystem::remove`，导致磁盘永久泄漏。 | 引入 RAII 临时文件守卫类，在失败路径和消费完毕后主动清理，并在启动时清理历史残留 `.tmp` 文件。 |
| **CORE-06** | `core/src/file_transfer.cpp:119, 209`<br>`core/src/clipboard_stream.cpp:54` | **MEDIUM** | 内存持续膨胀 | 长时间运行多次传输任务 | `outgoing_batches_`、`incoming_batches_` 和 `lazy_sources_` 在批次完成后未做清理和剔除，Map 容器无限增长。 | 在批次传输结束或超时后擦除对应批次条目，对 `lazy_sources_` 引入 LRU 或 TTL 淘汰。 |
| **CORE-07** | `core/src/engine.cpp:133, 157, 239` | **MEDIUM** | 异常断线容错 | 网络物理拔出或链路静默中断 | 引擎虽然实现了 `kPing`/`kPong` 的响应处理，但没有任何定时器周期性发送 `kPing`，且 TCP 套接字未开启 `SO_KEEPALIVE` 和超时，静默断线时线程永久僵死。 | 增加心跳定时器（每 5 秒发送 Ping，15 秒无响应断开），并在 Socket 上配置 `SO_RCVTIMEO` 与 `SO_SNDTIMEO`。 |
| **CORE-08** | `core/src/tls_channel.cpp:443, 492` | **MEDIUM** | 状态解析错乱 | 设备名称包含空格或特殊字符 | `trusted_peers.tsv` 和 `identity.cfg` 采用空格分隔行解析。设备名包含空格时导致字段解析偏移，使公钥匹配失败并反复重置身份。 | 对设备名进行转义或使用定长/十六进制编码，采用健壮的行格式解析。 |
| **CORE-09** | `core/src/tls_channel.cpp:535-541` | **MEDIUM** | 数据竞争 | 关闭连接与多线程读写并发 | `sock_fd_` 为普通 `int`，在 `close()` 中未加锁修改为 `-1`，而 `write_all` 和 `read_exact` 在其他线程并发读取，存在数据竞争和竞态失效。 | 将 `sock_fd_` 改为 `std::atomic<int>`，并在关闭时通过原子交换和 `shutdown` 确保优雅退出。 |
| **CORE-10** | `core/src/protocol.cpp:383, 422` | **MEDIUM** | 畸形报文/DoS | 收到特制恶意数据报文 | `decode` 中直接根据报文中的 `count` 执行 `files.reserve(count)`，仅上限 100,000，未校验剩余数据流是否足以提供 `count` 项，畸形报文可引发空耗内存 DoS。 | 校验 `count <= remaining_bytes / min_entry_size` 后再进行 `reserve`。 |
| **CORE-11** | `core/src/file_transfer.cpp:402-419` | **MEDIUM** | 断点续传死循环 | 传输中途损坏的分块文件 | 最终 SHA-256 校验失败时，残留的损坏 `.l2apart` 文件未被清除，再次触发续传时会基于损坏的文件尾继续追加，造成无限次校验失败。 | 在校验失败时删除或截断损坏的 `.l2apart` 文件。 |
| **CORE-12** | `core/src/tls_channel.cpp:746` | **LOW** | 密码学侧信道 | TLS Finished 握手验证 | `got_peer_fin != expected_peer_fin` 使用标准非恒定时间比较，存在时间侧信道攻击风险。 | 采用恒定时间缓冲区比对函数 (`CRYPTO_memcmp` 语义)。 |
| **CORE-13** | `core/CMakeLists.txt:5-28` | **LOW** | 构建规范 | Android JNI 动态库链接 | `l2a_core` 作为静态库缺少 `POSITION_INDEPENDENT_CODE ON`，在 Android 64 位平台编译为动态库时存在重定位风险。 | 在 CMake 中开启 `POSITION_INDEPENDENT_CODE ON` 并增加 Sanitizer 支持。 |

---

### 2.2 Linux 原生桌面端 (`linux/`)

| 编号 | 模块/文件:行号 | 严重级别 | 类别 | 触发场景 | 根因分析 | 修复方案 |
|:---|:---|:---:|:---:|:---|:---|:---|
| **LINUX-01** | `linux/src/wayland_clipboard.cpp:83` | **CRITICAL** | 远程代码执行 (RCE) | 接收带有特殊字符的剪贴板 MIME | `wl-copy --type '` + `item.mime_type` + `'` 通过 `popen` 传入 `/bin/sh` 执行。远程对端伪造含单引号与分号的 MIME 类型可实现任意 Shell 命令注入。 | 校验并白名单限制 MIME 字符集 (`^[a-zA-Z0-9_\-+./]+$`)，直接通过 `fork` + `execvp` 传递参数避免 Shell 解释。 |
| **LINUX-02** | `linux/src/app_window.cpp:345` | **HIGH** | 远程代码执行 (RCE) | 接收文件名含单引号的文件 | `send_desktop_notification` 使用 `::system("notify-send ... '" + body + "'")`，文件名直接拼接在 Shell 命令中，恶意文件名可导致任意代码执行。 | 使用 `fork` + `execvp` 调用 `notify-send`，避免经过 Shell 展开。 |
| **LINUX-03** | `linux/src/main.cpp` 等多处 | **HIGH** | 进程崩溃 | 子进程管道异常退出或套接字断开 | 全工程未忽略 `SIGPIPE` 信号。在调用 `popen` 管道写入 (`wl-copy`) 或套接字写入时，一旦管道破裂立即触发默认 `SIGPIPE` 终止整个进程。 | 在 `main.cpp` 初始化阶段执行 `::signal(SIGPIPE, SIG_IGN)`。 |
| **LINUX-04** | `linux/src/wayland_clipboard.cpp:19` | **HIGH** | 稳定性/功能失效 | 窗口最小化到托盘后剪贴板同步停止 | 纯 Wayland 协议的安全模型要求客户端必须具有活动窗口焦点才能接收剪贴板 `owner-change`。当窗口隐藏至托盘时，GTK 剪贴板事件被 Compositor 彻底静音。 | 引入对 `wl-paste --watch` 守护进程管道监听支持，在后台无焦点状态下依然捕获剪贴板变化。 |
| **LINUX-05** | `linux/src/app_window.cpp:141` | **HIGH** | Use-After-Free | 关闭或销毁 AppWindow | `g_timeout_add(50, flush_ui_events, this)` 的返回 Source ID 被丢弃，析构时未调用 `g_source_remove`，窗口销毁后定时器依然触发解引用野指针。 | 保存 `timeout_source_id_`，并在析构函数中调用 `g_source_remove`。 |
| **LINUX-06** | `linux/src/app_window.cpp:210, 242` | **HIGH** | UI 卡死 | 拖拽添加大文件或多文件 | `engine_.send_files(paths)` 在 GTK 主事件线程上同步扫描磁盘并全盘计算 SHA-256，导致桌面 UI 无响应数秒至数十秒。 | 将文件扫描与校验移入后台线程执行，通过 `g_idle_add` 异步将进度和结果投递回 GTK 主线程。 |
| **LINUX-07** | `linux/src/app_window.cpp:250-258` | **HIGH** | UI 卡死 | 点击 ADB 一键配置 | `run_adb_setup` 在 GTK 主线程同步执行多条阻塞 `adb` 命令并调用阻塞的 `connect_to_peer`，在设备未授权或未插入时直接卡死 UI。 | 将 ADB 执行与网络连接移至后台独立线程，完成后通过 `g_idle_add` 更新 UI 状态。 |
| **LINUX-08** | `linux/src/adb_helper.cpp:15-24` | **MEDIUM** | 线程僵死 | ADB 子进程挂起 | `run_cmd_capture` 调用 `popen` 同步读取且无任何超时控制。若 ADB 守护进程卡死，调用线程永久挂起。 | 实现基于非阻塞管道及 `poll` 的子进程运行器，配置 5 秒硬超时。 |
| **LINUX-09** | `linux/src/adb_helper.cpp:23` | **MEDIUM** | 状态误判 | ADB 命令退出状态码检查 | 直接返回 `pclose` 的原始返回值，未通过 `WIFEXITED` 与 `WEXITSTATUS` 宏解析真实退出码，导致非 0 退出状态码误判。 | 使用 `WIFEXITED(st) ? WEXITSTATUS(st) : -1` 正确提取退出码。 |
| **LINUX-10** | `linux/src/adb_helper.cpp:82-90` | **MEDIUM** | 有线连接失效 | 通过 USB ADB 有线直连 | 1) `rc_fwd == 0 || rc_rev == 0` 中逻辑或使正向转发失败被掩盖；2) `adb forward tcp:53322 tcp:<port>` 目标端口错误传入了 PC 本地端口，而 Android 端监听在 53321。 | 要求两项转发均成功，并修正 Android 端目标端口为固定 53321。 |
| **LINUX-11** | `linux/src/wayland_clipboard.cpp:27-56` | **MEDIUM** | 冗余开销/回环 | 设置来自对端的剪贴板内容 | `gtk_clipboard_set_text` 立即触发本端 `owner-change`，缺乏本地变更来源标志，引发不必要的二次读取与广播。 | 增加 `is_applying_remote_` 标志与内容哈希防重，抑制本地自触发。 |
| **LINUX-12** | `linux/src/nfc_ntag5332.cpp:98, 123` | **MEDIUM** | 硬件通信容错 | 触控板 NFC 写入时总线瞬态忙 | 硬编码 `/dev/i2c-0` 且在遇到 `EBUSY` / `EAGAIN` 时直接放弃，缺乏指数退避重试，易导致 EEPROM 数据写入半途损坏。 | 动态探测 sysfs 适配器编号，增加 3 次指数退避重试。 |
| **LINUX-13** | `linux/src/nfc_ntag5332.cpp:67` | **LOW** | 协议畸形 | URI 长度大于 255 字节 | NDEF TLV 长度字节直接强转 `uint8_t`，超长 URI 发生数值截断导致 NDEF 数据损坏。 | 校验 URI 长度不超过 254 字节并返回错误，防止数据损坏。 |
| **LINUX-14** | `linux/src/app_window.cpp:385` | **LOW** | 内存膨胀 | 持续运行日志追加 | `append_log` 未设置行数上限，长期运行下 GTK TextBuffer 持续占用内存。 | 保留最近 1000 行日志，超出自动截断头部旧日志。 |
| **LINUX-15** | `linux/CMakeLists.txt:21-31` | **LOW** | 构建兼容性 | 在非 Ubuntu / 非 x86_64 平台编译 | 硬编码 `/usr/lib/x86_64-linux-gnu`，破坏 ARM64、Fedora、Arch 构建。 | 采用 `pkg-config` 动态查找 GTK3 与 AppIndicator。 |

---

### 2.3 Android 移动端 (`android/`)

| 编号 | 模块/文件:行号 | 严重级别 | 类别 | 触发场景 | 根因分析 | 修复方案 |
|:---|:---|:---:|:---:|:---|:---|:---|
| **ANDROID-01** | `android/app/src/main/cpp/jni_bridge.cpp:108-115` | **CRITICAL** | JNI 运行时崩溃 | 原生线程销毁时触发 ART 断言 | 核心库后台线程在回调 Java 时调用 `AttachCurrentThread`，但线程销毁时从未调用 `DetachCurrentThread`。在 Android ART 运行时中直接引发致命 `SIGABRT`（`art/runtime/thread.cc: Native thread exited without detaching`）。 | 引入线程局部析构守卫（RAII 或 `pthread_key_create`），在原生线程退出时自动调用 `DetachCurrentThread`。 |
| **ANDROID-02** | `android/app/src/main/cpp/jni_bridge.cpp:19-105` | **HIGH** | Use-After-Free/数据竞争 | 停止服务与后台回调并发 | `g_callback_obj` 在各回调函数中无锁并发访问，而 `nativeStop` 释放全局引用并置空。并发时导致解引用已释放引用，ART 崩溃。 | 加锁保护全局引用或缓存 Class 对象使用静态回调，析构时安全注销。 |
| **ANDROID-03** | `android/app/src/main/cpp/jni_bridge.cpp:176-187` | **HIGH** | UI 冻结 (ANR) | 后台连接网络阻塞时停止服务 | `nativeConnect` 在持有全局 `g_engine_mtx` 的情况下调用阻塞的网络连接（超时达 75-120 秒）。若主线程调用 `nativeStop`，将阻塞在 `g_engine_mtx` 上超过 5 秒触发系统 ANR 弹窗。 | 锁内仅获取引擎共享指针，在锁外执行耗时网络连接。 |
| **ANDROID-04** | `android/app/src/main/cpp/jni_bridge.cpp:30, 52, 76, 99` | **HIGH** | JNI 异常状态污染 | Java 回调抛出异常 | 调用 Java 方法后未检查 `ExceptionCheck()`，带着未决异常继续调用后续 JNI 函数，触发 ART `JNI DETECTED ERROR: pending exception` 致命终止。 | 每次调用 Java 方法后必须执行 `ExceptionCheck()`、打印日志并 `ExceptionClear()`。 |
| **ANDROID-05** | `android/app/src/main/java/com/reid/l2a/ShareReceiverActivity.kt:35-55` | **CRITICAL** | 存储写穿/延迟与权限丢失 | 分享大视频/大文件至 Linux | 采用全量拷贝至 `cacheDir` 后再发送，4GB 视频造成双倍磁盘消耗并耗时 20 秒，且传输后从未删除产生永久磁盘垃圾；过早调用 `finish()` 导致临时 URI 授权过早撤销抛出 `SecurityException`。 | 实现基于 `ParcelFileDescriptor` 的直接流式传输 (`nativeSendFileFd`)，彻底消除中间磁盘缓存。 |
| **ANDROID-06** | `android/app/src/main/java/com/reid/l2a/L2AService.kt:39-43, 104-110` | **HIGH** | Scoped Storage 兼容性 | 接收照片、视频等媒体文件 | C++ 核心库将文件保存至 `Download/LinuxToAndroid/` 后，未调用 `MediaScannerConnection.scanFile`。相册与媒体库无法感知文件，小米相册中彻底不可见。 | 文件接收完成后通过 `MediaScannerConnection.scanFile` 主动刷新 MediaStore 索引。 |
| **ANDROID-07** | `android/app/src/main/java/com/reid/l2a/MainActivity.kt:28`<br>`AndroidManifest.xml:10` | **HIGH** | 权限/服务保活 | Android 13/14 设备上运行 | `POST_NOTIFICATIONS` 属于运行时权限，应用仅在 Manifest 中声明但从未动态申请，导致前台服务通知被系统静默屏蔽。 | 在 `MainActivity` 启动时动态请求通知权限。 |
| **ANDROID-08** | `android/app/src/main/java/com/reid/l2a/MainActivity.kt` | **HIGH** | HyperOS 进程保活 | 息屏后台运行 10-15 分钟 | 小米 HyperOS "神隐模式"（MIUI Battery Saver）会在息屏后主动切断网络并杀除后台服务。缺少申请忽略电池优化 (`REQUEST_IGNORE_BATTERY_OPTIMIZATIONS`) 及自启动引导。 | 增加忽略电池优化弹窗提示，并提供跳转 HyperOS 自启动与后台无限制设置引导。 |
| **ANDROID-09** | `android/app/src/main/java/com/reid/l2a/L2AService.kt:30-49`<br>`AndroidManifest.xml:4-11` | **MEDIUM** | Wi-Fi 发现失效 | 手机锁屏或节能模式下的局域网发现 | 缺少 `CHANGE_WIFI_MULTICAST_STATE` 权限与 `WifiManager.MulticastLock`，Wi-Fi 芯片在节能模式下直接硬件过滤 UDP 广播组播报文，导致设备无法被发现。 | 声明权限并在服务启动时获取 `MulticastLock`。 |
| **ANDROID-10** | `android/app/src/main/java/com/reid/l2a/L2AService.kt:25-28` | **MEDIUM** | 剪贴板循环自触发 | 接收 Linux 剪贴板内容 | Android 剪贴板更新后立即触发本地监听器重新通过 JNI 向上推送，产生冗余跨进程与网络往返。 | 过滤带有 `LinuxClipboard` 标签的剪贴板事件及内容哈希比对。 |
| **ANDROID-11** | `android/app/src/main/java/com/reid/l2a/L2AService.kt:95-102` | **MEDIUM** | 功能缺失/静默丢弃 | 传输 >512KB 文本或图片剪贴板 | 核心库大文本流通过临时文件保存，此时 `data` 为空。Android 端未做临时文件回退读取直接丢弃；且未处理图片 MIME 类型。 | 支持从临时文件读取大文本并生成 `ClipData`，增加图片剪贴板支持。 |
| **ANDROID-12** | `android/app/src/main/java/com/reid/l2a/ShareReceiverActivity.kt:18-33` | **MEDIUM** | 功能缺失 | 在相册中多选文件分享 | 仅注册了 `ACTION_SEND`，多选文件分享时系统发送 `ACTION_SEND_MULTIPLE`，导致 L2A 无法出现在系统多选分享列表中。 | 增加 `ACTION_SEND_MULTIPLE` Intent 过滤器并在 Activity 中解析 `EXTRA_STREAM` 数组。 |
| **ANDROID-13** | `android/app/src/main/cpp/jni_bridge.cpp:23-25` | **LOW** | 性能损耗 | 频繁网络与传输回调 | 每次回调均重新通过 `GetObjectClass` 和 `GetMethodID` 进行字符串反射查找。 | 在 `JNI_OnLoad` 中一次性缓存 `jmethodID` 与类引用。 |

---

## 3. 修复计划与实施里程碑划分

```
[Phase 1: M1 - C++ Core 核心库并发、死锁与传输健壮性修复]
  ├── 解耦 stream_outgoing_files 发送循环与 recv_thread_ (解决双向传输死锁 CORE-01)
  ├── 细化 FileTransferManager::mutex_ 粒度，锁外执行 SHA-256、ACK 发送与完成回调 (解决 CORE-02)
  ├── 解决 attach_channel 内部自等待死锁 (CORE-03)
  ├── 解决目录 >750 文件单帧元数据溢出断开 (CORE-04)
  ├── 临时剪贴板文件 RAII 清理与启动清理 (CORE-05)
  ├── 批次与源映射内存清理 (CORE-06)
  ├── 增加心跳定时器、Socket 超时与断线容错 (CORE-07)
  ├── 转义修复设备名空格导致的 TSV/CFG 状态解析错乱 (CORE-08)
  ├── 原子化 sock_fd_ 与常量时间 HMAC 比对 (CORE-09, CORE-12)
  └── 协议畸形报文防御与损坏 .l2apart 清理 (CORE-10, CORE-11)

[Phase 2: M2 - Linux 原生桌面端安全、UI 主线程解耦与子系统加固]
  ├── 消除 wayland_clipboard 与 app_window 中的 Shell 注入命令执行漏洞 (LINUX-01, LINUX-02)
  ├── 忽略全局 SIGPIPE 信号防止管道破裂猝死 (LINUX-03)
  ├── 引入 wl-paste --watch 支持 Wayland 纯后台静默监听 (LINUX-04)
  ├── 妥善管理 g_timeout_add Source 消除 Use-After-Free (LINUX-05)
  ├── 拖拽大文件 SHA-256 计算与 ADB 连接全面移入后台线程 (LINUX-06, LINUX-07)
  ├── 健壮 ADB 子进程运行器（带超时与真实退出码解析）与端口转发修正 (LINUX-08, LINUX-09, LINUX-10)
  ├── 剪贴板本地自触发抑制 (LINUX-11)
  └── 触控板 NFC I2C 动态探测与退避重试 (LINUX-12, LINUX-13)

[Phase 3: M3 - Android 客户端 JNI 线程安全、Scoped Storage 与平台适配]
  ├── 原生线程退出时自动 DetachCurrentThread，根除 ART 致命崩溃 (ANDROID-01)
  ├── 强化 g_callback_obj 生命周期与锁保护，根除停止服务时的野指针竞态 (ANDROID-02)
  ├── 解耦 nativeConnect 与全局互斥锁，根除 Android 主线程 ANR (ANDROID-03)
  ├── 增加全局 JNI 异常捕获与清理守卫 (ANDROID-04)
  ├── 基于 ParcelFileDescriptor 实现直接流式传输，消除多 GB 本地缓存写穿 (ANDROID-05)
  ├── 文件接收完成主动扫描并索引至 MediaStore 相册 (ANDROID-06)
  ├── 适配 Android 13/14 POST_NOTIFICATIONS 动态权限与 HyperOS 神隐模式保活 (ANDROID-07, ANDROID-08)
  ├── 补充 Wi-Fi 多播锁与多选文件分享支持 (ANDROID-09, ANDROID-12)
  └── 剪贴板回环抑制与大文本/图片支持 (ANDROID-10, ANDROID-11)

[Phase 4: M4 - 自动化测试增强与全量回归验证]
  ├── 编写双向并发无死锁回归测试用例 (test_bidirectional_concurrency)
  ├── 编写超大目录（>1000 文件）分片传输测试用例 (test_large_directory_pagination)
  ├── 编写重入回调死锁测试用例 (test_reentrant_callback)
  ├── 编写异常断线与损坏文件恢复测试用例 (test_disconnect_and_corrupt_recovery)
  ├── 编写畸形报文与畸形设备名模糊防御测试用例 (test_malformed_packet_defense)
  ├── 验证全量 ctest 100% 通过且无编译警告、无内存泄漏、无 Segfault
  └── 执行独立法医审计 (Forensic Integrity Audit)
```
