package com.reid.l2a

import android.content.Context

object CoreBridge {
    init {
        System.loadLibrary("l2a_jni")
    }

    @Volatile
    var lastPeerIp: String? = null
    @Volatile
    var lastPeerPort: Int = 53321
    @Volatile
    var isConnected: Boolean = false

    interface Listener {
        fun onPeerDiscovered(deviceId: String, deviceName: String, ip: String, port: Int)
        fun onConnectionChanged(connected: Boolean, peerId: String, peerName: String)
        fun onClipboardReceived(mimeType: String, data: ByteArray, tempFilePath: String, isPullResponse: Boolean)
        fun onClipboardPullRequested(): Boolean
        fun onFileCompleted(
            batchId: Long,
            fileIndex: Int,
            savedPath: String,
            isSending: Boolean,
            success: Boolean,
            errorMsg: String
        )
        fun onFileProgress(bytesTransferred: Long, totalBytes: Long, isSending: Boolean)
    }

    var listener: Listener? = null

    external fun nativeInit(storageDir: String, receiveDir: String, deviceName: String): Boolean
    external fun nativeStop()
    external fun nativeConnect(ip: String, port: Int): Boolean
    external fun nativeIsConnected(): Boolean
    external fun nativePushClipboardText(text: String): Boolean
    external fun nativePushClipboardTextEx(text: String, bypassLoopback: Boolean, isPullResponse: Boolean): Boolean
    external fun nativePushClipboardBytes(mimeType: String, data: ByteArray): Boolean
    external fun nativePushClipboardBytesEx(
        mimeType: String,
        data: ByteArray,
        bypassLoopback: Boolean,
        isPullResponse: Boolean
    ): Boolean
    external fun nativePullRemoteClipboard(): Boolean
    external fun nativeSetMaxHistoryLimit(limit: Int)
    external fun nativeGetClipboardHistoryJson(): String
    external fun nativeDeleteHistoryItem(id: String): Boolean
    external fun nativeClearHistory()
    external fun nativeSendFile(filePath: String): Long
    external fun nativeSendFileFd(fd: Int, displayName: String, fileSize: Long): Long
    external fun nativeSendFilesBatch(
        fds: IntArray,
        displayNames: Array<String>,
        fallbackPaths: Array<String>
    ): Long

    fun ensureEngineInitialized(context: Context) {
        val appCtx = context.applicationContext ?: context
        val storageDir = java.io.File(appCtx.filesDir, "l2a_state").apply { mkdirs() }
        val recvDir = java.io.File(
            android.os.Environment.getExternalStoragePublicDirectory(android.os.Environment.DIRECTORY_DOWNLOADS),
            "LinuxToAndroid"
        ).apply { mkdirs() }
        nativeInit(
            storageDir.absolutePath,
            recvDir.absolutePath,
            android.os.Build.MODEL ?: "Android-Phone"
        )
    }

    fun ensureConnected(context: Context? = null, timeoutMs: Long = 3000): Boolean {
        if (context != null) {
            try {
                ensureEngineInitialized(context)
            } catch (_: Exception) {}
        }

        if (nativeIsConnected()) {
            isConnected = true
            return true
        }

        if (lastPeerIp.isNullOrEmpty() && context != null) {
            val prefs = context.getSharedPreferences("l2a_prefs", Context.MODE_PRIVATE)
            lastPeerIp = prefs.getString("last_peer_ip", null)
            lastPeerPort = prefs.getInt("last_peer_port", 53321)
        }

        val startTime = System.currentTimeMillis()
        while (System.currentTimeMillis() - startTime < timeoutMs) {
            if (nativeIsConnected()) {
                isConnected = true
                return true
            }

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

            try { Thread.sleep(200) } catch (_: InterruptedException) {}
            if (nativeIsConnected()) {
                isConnected = true
                return true
            }
        }
        return nativeIsConnected()
    }

    // Invoked from JNI C++ threads
    fun onPeerDiscovered(deviceId: String, deviceName: String, ip: String, port: Int) {
        listener?.onPeerDiscovered(deviceId, deviceName, ip, port)
    }

    fun onConnectionChanged(connected: Boolean, peerId: String, peerName: String) {
        isConnected = connected
        listener?.onConnectionChanged(connected, peerId, peerName)
    }

    fun onClipboardReceived(mimeType: String, data: ByteArray, tempFilePath: String, isPullResponse: Boolean) {
        listener?.onClipboardReceived(mimeType, data, tempFilePath, isPullResponse)
    }

    fun onClipboardPullRequested(): Boolean {
        return listener?.onClipboardPullRequested() ?: false
    }

    fun onFileCompleted(
        batchId: Long,
        fileIndex: Int,
        savedPath: String,
        isSending: Boolean,
        success: Boolean,
        errorMsg: String
    ) {
        listener?.onFileCompleted(batchId, fileIndex, savedPath, isSending, success, errorMsg)
    }

    fun onFileProgress(bytesTransferred: Long, totalBytes: Long, isSending: Boolean) {
        listener?.onFileProgress(bytesTransferred, totalBytes, isSending)
    }
}
