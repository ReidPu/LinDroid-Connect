package com.reid.l2a

import android.app.Activity
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.ParcelFileDescriptor
import android.provider.OpenableColumns
import android.util.Log
import android.webkit.MimeTypeMap
import android.widget.Toast
import java.io.File

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
                            val ok = if (connected) CoreBridge.nativePushClipboardTextEx(sharedText, true, false) else false
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

            cleanStaleStagingFiles()
            val fdList = mutableListOf<Int>()
            val nameList = mutableListOf<String>()
            val fallbackList = mutableListOf<String>()
            val stagingFiles = mutableListOf<File>()

            for (uri in uris) {
                try {
                    val displayName = resolveCleanDisplayName(uri)

                    // Try zero-copy Scoped Storage streaming first
                    var pfd: ParcelFileDescriptor? = null
                    try {
                        pfd = contentResolver.openFileDescriptor(uri, "r")
                    } catch (_: Exception) {}

                    if (pfd != null) {
                        val fd = pfd.detachFd()
                        fdList.add(fd)
                        nameList.add(displayName)
                        fallbackList.add("")
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
                            fdList.add(-1)
                            nameList.add(displayName)
                            fallbackList.add(tempFile.absolutePath)
                            stagingFiles.add(tempFile)
                        } else {
                            tempFile.delete()
                        }
                    }
                } catch (e: Exception) {
                    Log.e("ShareReceiver", "Failed to prepare uri $uri: ${e.message}")
                }
            }

            var batchId = 0L
            if (fdList.isNotEmpty()) {
                batchId = CoreBridge.nativeSendFilesBatch(
                    fdList.toIntArray(),
                    nameList.toTypedArray(),
                    fallbackList.toTypedArray()
                )
                if (batchId == 0L) {
                    stagingFiles.forEach { it.delete() }
                }
            }

            val sentCount = if (batchId != 0L) fdList.size else 0
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
        var rawName: String? = null
        try {
            contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
                val idx = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                if (idx >= 0 && cursor.moveToFirst()) {
                    rawName = cursor.getString(idx)
                }
            }
        } catch (_: Exception) {}

        if (rawName.isNullOrBlank()) {
            val segment = uri.lastPathSegment?.substringAfterLast('/')?.substringAfterLast(':')
            if (!segment.isNullOrBlank()) {
                rawName = segment
            }
        }

        val baseName: String = if (rawName.isNullOrBlank()) {
            "shared_${System.currentTimeMillis()}"
        } else {
            rawName!!
        }
        var cleanName = File(baseName).name
        if (cleanName.isBlank() || cleanName == "." || cleanName == "..") {
            cleanName = "shared_${System.currentTimeMillis()}"
        }

        if (!cleanName.contains('.')) {
            val mime = contentResolver.getType(uri)
            if (!mime.isNullOrBlank()) {
                val ext = MimeTypeMap.getSingleton().getExtensionFromMimeType(mime)
                if (!ext.isNullOrBlank()) {
                    cleanName = "$cleanName.$ext"
                }
            }
        }
        return cleanName
    }

    override fun onDestroy() {
        super.onDestroy()
    }

    private fun cleanStaleStagingFiles() {
        try {
            val stagingDir = File(cacheDir, "share_staging")
            if (stagingDir.exists() && stagingDir.isDirectory) {
                val cutoff = System.currentTimeMillis() - 10 * 60 * 1000L
                stagingDir.listFiles()?.forEach { file ->
                    if (file.lastModified() < cutoff) {
                        file.delete()
                    }
                }
            }
        } catch (_: Exception) {}
    }
}
