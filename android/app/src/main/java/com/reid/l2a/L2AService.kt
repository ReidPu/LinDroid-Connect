package com.reid.l2a

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.media.MediaScannerConnection
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Environment
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.util.Log
import android.widget.Toast
import java.io.File
import java.security.MessageDigest
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

class L2AService : Service(), CoreBridge.Listener {

    private lateinit var clipboardManager: ClipboardManager
    private val mainHandler = Handler(Looper.getMainLooper())
    private val clipboardExecutor = Executors.newSingleThreadExecutor()
    private val connectingInProgress = AtomicBoolean(false)

    @Volatile
    private var isConnected = false

    // Multicast Lock for UDP peer discovery on port 53322 (ANDROID-09)
    private var multicastLock: WifiManager.MulticastLock? = null

    // Anti-loopback state (SHA-256 + short suppression window)
    @Volatile
    private var lastReceivedHash: ByteArray? = null
    @Volatile
    private var suppressUntilMs: Long = 0L

    private var lastProgressNotificationTime: Long = 0L

    private fun sha256(bytes: ByteArray): ByteArray {
        return MessageDigest.getInstance("SHA-256").digest(bytes)
    }

    private fun isSuppressedEcho(bytes: ByteArray): Boolean {
        if (System.currentTimeMillis() < suppressUntilMs) {
            return true
        }
        val prev = lastReceivedHash ?: return false
        return prev.contentEquals(sha256(bytes))
    }

    private val clipListener = ClipboardManager.OnPrimaryClipChangedListener {
        if (System.currentTimeMillis() < suppressUntilMs) {
            return@OnPrimaryClipChangedListener
        }
        val clip = try { clipboardManager.primaryClip } catch (_: Exception) { null } ?: return@OnPrimaryClipChangedListener
        val label = clip.description?.label?.toString()
        if (label == "LinuxClipboard" || label == "L2A Image" || label == "L2A_Temp_Selective_Paste") {
            return@OnPrimaryClipChangedListener
        }

        // Directional auto-share check (phone -> pc)
        val prefs = getSharedPreferences("l2a_prefs", Context.MODE_PRIVATE)
        if (!prefs.getBoolean("phone_to_pc_auto_share", true)) {
            return@OnPrimaryClipChangedListener
        }

        if (clip.itemCount > 0) {
            val item = clip.getItemAt(0)
            val uri = item.uri
            if (uri != null) {
                val type = contentResolver.getType(uri)
                    ?: (if (clip.description.mimeTypeCount > 0) clip.description.getMimeType(0) else null)
                if (type != null && type.startsWith("image/")) {
                    clipboardExecutor.execute {
                        try {
                            contentResolver.openInputStream(uri)?.use { input ->
                                val bytes = input.readBytes()
                                if (bytes.isNotEmpty() &&
                                    !isSuppressedEcho(bytes) &&
                                    CoreBridge.ensureConnected(this@L2AService, 3000)) {
                                    CoreBridge.nativePushClipboardBytesEx(type, bytes, false, false)
                                }
                            }
                        } catch (e: Exception) {
                            Log.e("L2AService", "Failed to push image from clipListener: ${e.message}")
                        }
                    }
                    return@OnPrimaryClipChangedListener
                }
            }

            val text = item.coerceToText(this)?.toString()
            if (!text.isNullOrEmpty()) {
                val textBytes = text.toByteArray(Charsets.UTF_8)
                if (isSuppressedEcho(textBytes)) {
                    return@OnPrimaryClipChangedListener
                }
                clipboardExecutor.execute {
                    if (!isSuppressedEcho(textBytes) &&
                        CoreBridge.ensureConnected(this@L2AService, 3000)) {
                        CoreBridge.nativePushClipboardTextEx(text, false, false)
                    }
                }
            }
        }
    }

    override fun onCreate() {
        super.onCreate()
        clipboardManager = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        clipboardManager.addPrimaryClipChangedListener(clipListener)

        // Prune stale staging files from share_staging on service startup
        cleanStaleStagingFiles()

        // Acquire Wi-Fi Multicast Lock so UDP 53322 beacons are not dropped by chipset (ANDROID-09)
        try {
            val wifiManager = applicationContext.getSystemService(Context.WIFI_SERVICE) as? WifiManager
            multicastLock = wifiManager?.createMulticastLock("l2a_discovery_multicast_lock")?.apply {
                setReferenceCounted(true)
                acquire()
            }
        } catch (e: Exception) {
            Log.w("L2AService", "Failed to acquire MulticastLock: ${e.message}")
        }

        startForegroundWithNotification("等待连接 Linux (碰触控板 NFC 或局域网/USB 自动连接)")

        CoreBridge.listener = this
        val storageDir = File(filesDir, "l2a_state").apply { mkdirs() }
        val recvDir = File(
            Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS),
            "LinuxToAndroid"
        ).apply { mkdirs() }

        CoreBridge.nativeInit(
            storageDir.absolutePath,
            recvDir.absolutePath,
            Build.MODEL ?: "Android-Phone"
        )

        // Attempt USB ADB Reverse Tunnel (127.0.0.1:53323) immediately if plugged in via USB
        Thread {
            CoreBridge.nativeConnect("127.0.0.1", 53323)
        }.start()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_PUSH_CLIPBOARD_NOW -> pushCurrentClipboardToLinux()
            ACTION_PULL_CLIPBOARD_NOW -> pullRemoteClipboard()
        }
        return START_STICKY
    }

    fun pushCurrentClipboardToLinux() {
        val clip = try { clipboardManager.primaryClip } catch (_: Exception) { null } ?: return
        if (clip.itemCount > 0) {
            val item = clip.getItemAt(0)
            val uri = item.uri
            if (uri != null) {
                val type = contentResolver.getType(uri)
                    ?: (if (clip.description.mimeTypeCount > 0) clip.description.getMimeType(0) else null)
                if (type != null && type.startsWith("image/")) {
                    clipboardExecutor.execute {
                        try {
                            contentResolver.openInputStream(uri)?.use { input ->
                                val bytes = input.readBytes()
                                if (bytes.isNotEmpty() && CoreBridge.ensureConnected(this@L2AService, 3000)) {
                                    val ok = CoreBridge.nativePushClipboardBytesEx(type, bytes, true, false)
                                    mainHandler.post {
                                        Toast.makeText(
                                            this@L2AService,
                                            if (ok) "已同步图片至电脑" else "图片发送失败",
                                            Toast.LENGTH_SHORT
                                        ).show()
                                    }
                                }
                            }
                        } catch (e: Exception) {
                            Log.e("L2AService", "Failed to push image to Linux: ${e.message}")
                        }
                    }
                    return
                }
            }

            val text = item.coerceToText(this)?.toString()
            if (!text.isNullOrEmpty()) {
                clipboardExecutor.execute {
                    if (CoreBridge.ensureConnected(this@L2AService, 3000)) {
                        val ok = CoreBridge.nativePushClipboardTextEx(text, true, false)
                        mainHandler.post {
                            Toast.makeText(
                                this@L2AService,
                                if (ok) "已同步剪贴板至电脑" else "剪贴板发送失败",
                                Toast.LENGTH_SHORT
                            ).show()
                        }
                    } else {
                        mainHandler.post {
                            Toast.makeText(this@L2AService, "未连接电脑", Toast.LENGTH_SHORT).show()
                        }
                    }
                }
            }
        }
    }

    fun pullRemoteClipboard() {
        clipboardExecutor.execute {
            if (CoreBridge.ensureConnected(this@L2AService, 3000)) {
                val ok = CoreBridge.nativePullRemoteClipboard()
                mainHandler.post {
                    if (ok) {
                        Toast.makeText(this@L2AService, "已向电脑请求最新剪贴板", Toast.LENGTH_SHORT).show()
                    } else {
                        Toast.makeText(this@L2AService, "拉取电脑剪贴板失败", Toast.LENGTH_SHORT).show()
                    }
                }
            } else {
                mainHandler.post {
                    Toast.makeText(this@L2AService, "未连接电脑", Toast.LENGTH_SHORT).show()
                }
            }
        }
    }

    override fun onPeerDiscovered(deviceId: String, deviceName: String, ip: String, port: Int) {
        CoreBridge.lastPeerIp = ip
        CoreBridge.lastPeerPort = port
        val prefs = getSharedPreferences("l2a_prefs", Context.MODE_PRIVATE)
        prefs.edit()
            .putString("last_peer_ip", ip)
            .putInt("last_peer_port", port)
            .putString("last_peer_id", deviceId)
            .putString("last_peer_name", deviceName)
            .apply()

        if (!isConnected && connectingInProgress.compareAndSet(false, true)) {
            Thread {
                try {
                    if (!CoreBridge.nativeIsConnected()) {
                        CoreBridge.nativeConnect(ip, port)
                    }
                } finally {
                    connectingInProgress.set(false)
                }
            }.start()
        }
    }

    override fun onConnectionChanged(connected: Boolean, peerId: String, peerName: String) {
        isConnected = connected
        CoreBridge.isConnected = connected
        if (connected && peerId.isNotEmpty()) {
            val prefs = getSharedPreferences("l2a_prefs", Context.MODE_PRIVATE)
            prefs.edit()
                .putString("last_peer_id", peerId)
                .putString("last_peer_name", peerName)
                .apply()
        }
        mainHandler.post {
            val status = if (connected) {
                "已连接 Linux: $peerName (双向剪贴板 & 极速互传已激活)"
            } else {
                "已断开连接，正在等待自动重连..."
            }
            startForegroundWithNotification(status)
        }
    }

    override fun onClipboardReceived(
        mimeType: String,
        data: ByteArray,
        tempFilePath: String,
        isPullResponse: Boolean
    ) {
        val prefs = getSharedPreferences("l2a_prefs", Context.MODE_PRIVATE)
        if (!prefs.getBoolean("pc_to_phone_auto_share", true) && !isPullResponse) {
            Log.i("L2AService", "pc_to_phone_auto_share is disabled, skipping primaryClip update")
            return
        }

        when {
            mimeType.startsWith("text/") -> {
                val textBytes = if (data.isNotEmpty()) {
                    data
                } else if (tempFilePath.isNotEmpty()) {
                    val tempFile = File(tempFilePath)
                    if (tempFile.exists()) tempFile.readBytes() else null
                } else {
                    null
                }
                val text = textBytes?.let { String(it, Charsets.UTF_8) }
                if (!text.isNullOrEmpty() && textBytes != null) {
                    lastReceivedHash = sha256(textBytes)
                    suppressUntilMs = System.currentTimeMillis() + 800L
                    mainHandler.post {
                        suppressUntilMs = System.currentTimeMillis() + 800L
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
                        lastReceivedHash = sha256(imageBytes)
                        suppressUntilMs = System.currentTimeMillis() + 800L
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
                            suppressUntilMs = System.currentTimeMillis() + 800L
                            val clipData = ClipData.newUri(contentResolver, "L2A Image", contentUri)
                            clipboardManager.setPrimaryClip(clipData)
                            Toast.makeText(this@L2AService, "已同步电脑剪贴板 (图片)", Toast.LENGTH_SHORT).show()
                        }
                    } catch (e: Exception) {
                        Log.e("L2AService", "Failed to save and set image clipboard: ${e.message}")
                    }
                }
            }
        }
    }

    override fun onClipboardPullRequested(): Boolean {
        val clip = try { clipboardManager.primaryClip } catch (_: Exception) { null } ?: return false
        if (clip.itemCount <= 0) return false
        val item = clip.getItemAt(0)

        val uri = item.uri
        if (uri != null) {
            val type = contentResolver.getType(uri)
                ?: (if (clip.description.mimeTypeCount > 0) clip.description.getMimeType(0) else null)
            if (type != null && type.startsWith("image/")) {
                return try {
                    contentResolver.openInputStream(uri)?.use { input ->
                        val bytes = input.readBytes()
                        bytes.isNotEmpty() && CoreBridge.nativePushClipboardBytesEx(type, bytes, true, true)
                    } ?: false
                } catch (_: Exception) {
                    false
                }
            }
        }

        val text = item.coerceToText(this)?.toString()
        if (!text.isNullOrEmpty()) {
            return CoreBridge.nativePushClipboardTextEx(text, true, true)
        }
        return false
    }

    override fun onFileProgress(bytesTransferred: Long, totalBytes: Long, isSending: Boolean) {
        if (totalBytes > 0) {
            val percent = ((bytesTransferred * 100) / totalBytes).toInt()
            val now = System.currentTimeMillis()
            if (now - lastProgressNotificationTime > 500 || percent == 100) {
                lastProgressNotificationTime = now
                mainHandler.post {
                    val mbTransferred = bytesTransferred / (1024 * 1024)
                    val mbTotal = totalBytes / (1024 * 1024)
                    val prefix = if (isSending) "发送中" else "接收中"
                    updateProgressNotification("$prefix: $percent% ($mbTransferred MB / $mbTotal MB)", percent)
                }
            }
        }
    }

    override fun onFileCompleted(
        batchId: Long,
        fileIndex: Int,
        savedPath: String,
        isSending: Boolean,
        success: Boolean,
        errorMsg: String
    ) {
        val file = if (savedPath.isNotEmpty()) File(savedPath) else null
        if (isSending) {
            // Delete ONLY the staging file belonging to this completed/failed item
            if (file != null) {
                try {
                    val stagingDir = File(cacheDir, "share_staging").canonicalFile
                    if (file.canonicalFile.parentFile == stagingDir) {
                        file.delete()
                    }
                } catch (_: Exception) {}
            }
            mainHandler.post {
                val name = file?.name ?: "#$batchId/$fileIndex"
                val msg = if (success) "已发送文件: $name" else "文件发送失败 ($name): $errorMsg"
                startForegroundWithNotification(msg)
            }
            return
        }

        if (success && file != null) {
            MediaScannerConnection.scanFile(
                this,
                arrayOf(file.absolutePath),
                null
            ) { path, uri ->
                Log.i("L2AService", "MediaScanner indexed: $path -> $uri")
            }
            mainHandler.post {
                startForegroundWithNotification("已收到文件: ${file.name}")
            }
        } else {
            mainHandler.post {
                startForegroundWithNotification("文件接收失败: $errorMsg")
            }
        }
    }

    private fun startForegroundWithNotification(contentText: String) {
        val channelId = "l2a_sync_channel"
        val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                channelId,
                "Linux ⇄ Android 跨端同步服务",
                NotificationManager.IMPORTANCE_LOW
            )
            nm.createNotificationChannel(channel)
        }

        val pushIntent = Intent(this, NfcTriggerActivity::class.java)
        val pendingPush = PendingIntent.getActivity(
            this, 1, pushIntent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        val pullIntent = Intent(this, L2AService::class.java).apply {
            action = ACTION_PULL_CLIPBOARD_NOW
        }
        val pendingPull = PendingIntent.getService(
            this, 2, pullIntent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        val notification = Notification.Builder(this, channelId)
            .setContentTitle("Linux ⇄ Android 跨端互联")
            .setContentText(contentText)
            .setSmallIcon(android.R.drawable.stat_sys_upload_done)
            .addAction(
                Notification.Action.Builder(
                    null, "立即发送剪贴板", pendingPush
                ).build()
            )
            .addAction(
                Notification.Action.Builder(
                    null, "拉取电脑剪贴板", pendingPull
                ).build()
            )
            .build()

        startForeground(1001, notification)
    }

    private fun updateProgressNotification(statusText: String, progressPercent: Int) {
        val channelId = "l2a_sync_channel"
        val pushIntent = Intent(this, NfcTriggerActivity::class.java)
        val pendingPush = PendingIntent.getActivity(
            this, 1, pushIntent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        val pullIntent = Intent(this, L2AService::class.java).apply {
            action = ACTION_PULL_CLIPBOARD_NOW
        }
        val pendingPull = PendingIntent.getService(
            this, 2, pullIntent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        val notification = Notification.Builder(this, channelId)
            .setContentTitle("Linux ⇄ Android 跨端互联")
            .setContentText(statusText)
            .setSmallIcon(android.R.drawable.stat_sys_upload)
            .setProgress(100, progressPercent, false)
            .addAction(
                Notification.Action.Builder(
                    null, "立即发送剪贴板", pendingPush
                ).build()
            )
            .addAction(
                Notification.Action.Builder(
                    null, "拉取电脑剪贴板", pendingPull
                ).build()
            )
            .build()

        val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        nm.notify(1001, notification)
    }

    override fun onDestroy() {
        cleanStaleStagingFiles()
        clipboardManager.removePrimaryClipChangedListener(clipListener)
        clipboardExecutor.shutdownNow()
        try {
            if (multicastLock?.isHeld == true) {
                multicastLock?.release()
            }
        } catch (_: Exception) {}
        multicastLock = null

        CoreBridge.nativeStop()
        super.onDestroy()
    }

    private fun cleanStaleStagingFiles() {
        try {
            val stagingDir = File(cacheDir, "share_staging")
            if (stagingDir.exists() && stagingDir.isDirectory) {
                stagingDir.listFiles()?.forEach { file ->
                    file.delete()
                }
            }
        } catch (e: Exception) {
            Log.w("L2AService", "Failed to clean share_staging: ${e.message}")
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    companion object {
        const val ACTION_PUSH_CLIPBOARD_NOW = "com.reid.l2a.ACTION_PUSH_CLIPBOARD_NOW"
        const val ACTION_PULL_CLIPBOARD_NOW = "com.reid.l2a.ACTION_PULL_CLIPBOARD_NOW"
    }
}
