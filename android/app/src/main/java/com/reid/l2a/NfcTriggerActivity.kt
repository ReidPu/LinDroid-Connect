package com.reid.l2a

import android.app.Activity
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.graphics.Color
import android.net.Uri
import android.nfc.NdefMessage
import android.nfc.NfcAdapter
import android.nfc.Tag
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.View
import android.widget.Toast

/**
 * Translucent zero-UI Activity launched when the phone taps the Redmi G Pro 2024
 * touchpad NFC tag (NTAG5332 `l2a://tap`, factory Xiaomi tag, or raw Type 5/Type 2 tag)
 * or taps the notification quick-send action.
 * Because this Activity briefly acquires foreground window focus, it can always read
 * the Android 10+ clipboard even before ADB READ_CLIPBOARD_IN_BACKGROUND is granted.
 */
class NfcTriggerActivity : Activity() {

    private var handled = false
    private val mainHandler = Handler(Looper.getMainLooper())
    private val fallbackRunnable = Runnable {
        if (!handled) {
            handled = true
            triggerClipboardSync()
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // Attach invisible dummy view to guarantee window focus on HyperOS
        val dummy = View(this).apply {
            setBackgroundColor(Color.TRANSPARENT)
        }
        setContentView(dummy)

        // Ensure background service and native engine are running
        try {
            startForegroundService(Intent(this, L2AService::class.java))
        } catch (_: Exception) {}
        try {
            CoreBridge.ensureEngineInitialized(applicationContext)
        } catch (_: Exception) {}

        parseNfcIntent(intent)
    }

    override fun onNewIntent(intent: Intent?) {
        super.onNewIntent(intent)
        if (intent != null) {
            setIntent(intent)
            parseNfcIntent(intent)
            if (handled) {
                handled = false
                mainHandler.postDelayed({
                    if (!handled) {
                        handled = true
                        triggerClipboardSync()
                    }
                }, 50)
            }
        }
    }

    override fun onResume() {
        super.onResume()
        // Suppress repeated NFC re-dispatch while the phone is still resting on the touchpad
        try {
            val adapter = NfcAdapter.getDefaultAdapter(this)
            val flags = NfcAdapter.FLAG_READER_NFC_A or
                    NfcAdapter.FLAG_READER_NFC_B or
                    NfcAdapter.FLAG_READER_NFC_F or
                    NfcAdapter.FLAG_READER_NFC_V or
                    NfcAdapter.FLAG_READER_NO_PLATFORM_SOUNDS
            adapter?.enableReaderMode(this, { /* ignore while syncing */ }, flags, null)
        } catch (_: Exception) {}

        // Fallback timer in case HyperOS NFC overlay delays or suppresses onWindowFocusChanged(true)
        mainHandler.removeCallbacks(fallbackRunnable)
        mainHandler.postDelayed(fallbackRunnable, 250)
    }

    override fun onPause() {
        super.onPause()
        mainHandler.removeCallbacks(fallbackRunnable)
        try {
            NfcAdapter.getDefaultAdapter(this)?.disableReaderMode(this)
        } catch (_: Exception) {}
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus && !handled) {
            handled = true
            mainHandler.removeCallbacks(fallbackRunnable)
            // Allow 50ms for HyperOS AppOps to register active window focus
            window.decorView.postDelayed({
                triggerClipboardSync()
            }, 50)
        }
    }

    private fun parseNfcIntent(intent: Intent) {
        try {
            val tag: Tag? = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                intent.getParcelableExtra(NfcAdapter.EXTRA_TAG, Tag::class.java)
            } else {
                @Suppress("DEPRECATION")
                intent.getParcelableExtra(NfcAdapter.EXTRA_TAG)
            }
            if (tag != null) {
                val uidHex = tag.id?.joinToString(":") { "%02X".format(it) } ?: ""
                val prefs = getSharedPreferences("l2a_prefs", Context.MODE_PRIVATE)
                val boundUid = prefs.getString("bound_touchpad_uid", null)
                if (uidHex.isNotEmpty()) {
                    if (uidHex.equals(boundUid, ignoreCase = true)) {
                        android.util.Log.i("NfcTriggerActivity", "Discovered tag matches bound_touchpad_uid: $uidHex")
                    }
                }
            }

            intent.data?.let { applyTapUriParams(it) }

            val rawMsgs = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                intent.getParcelableArrayExtra(NfcAdapter.EXTRA_NDEF_MESSAGES, NdefMessage::class.java)
            } else {
                @Suppress("DEPRECATION")
                intent.getParcelableArrayExtra(NfcAdapter.EXTRA_NDEF_MESSAGES)
            }
            if (rawMsgs != null) {
                for (raw in rawMsgs) {
                    val msg = raw as? NdefMessage ?: continue
                    for (rec in msg.records) {
                        rec.toUri()?.let { applyTapUriParams(it) }
                    }
                }
            }
        } catch (_: Exception) {}
    }

    private fun applyTapUriParams(uri: Uri) {
        if (uri.scheme != "l2a") return
        val prefs = getSharedPreferences("l2a_prefs", Context.MODE_PRIVATE)
        val editor = prefs.edit()
        val hostId = uri.getQueryParameter("host_id") ?: uri.getQueryParameter("h")
        if (!hostId.isNullOrEmpty()) {
            editor.putString("last_peer_id", hostId)
            editor.putString("bound_host_id", hostId)
        }
        val ip = uri.getQueryParameter("ip")
        if (!ip.isNullOrEmpty()) {
            CoreBridge.lastPeerIp = ip
            editor.putString("last_peer_ip", ip)
        }
        val portStr = uri.getQueryParameter("port") ?: uri.getQueryParameter("p")
        val port = portStr?.toIntOrNull()
        if (port != null && port in 1..65535) {
            CoreBridge.lastPeerPort = port
            editor.putInt("last_peer_port", port)
        }
        editor.apply()
    }

    private fun triggerClipboardSync() {
        val cm = getSystemService(Context.CLIPBOARD_SERVICE) as? ClipboardManager
        val clip = try { cm?.primaryClip } catch (_: Exception) { null }

        Thread {
            val connected = CoreBridge.ensureConnected(this, 3000)
            if (clip != null && clip.itemCount > 0) {
                val item = clip.getItemAt(0)
                val uri = item.uri
                if (uri != null) {
                    val type = contentResolver.getType(uri) ?: (if (clip.description.mimeTypeCount > 0) clip.description.getMimeType(0) else null)
                    if (type != null && type.startsWith("image/")) {
                        var imgOk = false
                        try {
                            contentResolver.openInputStream(uri)?.use { input ->
                                val bytes = input.readBytes()
                                if (bytes.isNotEmpty() && connected) {
                                    imgOk = CoreBridge.nativePushClipboardBytesEx(type, bytes, true, false)
                                }
                            }
                        } catch (e: Exception) {
                            android.util.Log.e("NfcTriggerActivity", "Failed to push image: ${e.message}")
                        }
                        runOnUiThread {
                            if (imgOk) {
                                Toast.makeText(this, "已通过触控板一碰传同步图片至 Linux", Toast.LENGTH_SHORT).show()
                            } else {
                                Toast.makeText(this, "连接电脑失败，请检查 Wi-Fi 或 USB", Toast.LENGTH_LONG).show()
                            }
                            finish()
                            overridePendingTransition(0, 0)
                        }
                        return@Thread
                    }
                }

                val text = item.coerceToText(this)?.toString()
                if (!text.isNullOrEmpty()) {
                    val ok = if (connected) CoreBridge.nativePushClipboardTextEx(text, true, false) else false
                    runOnUiThread {
                        if (ok) {
                            Toast.makeText(this, "已通过触控板一碰传同步剪贴板至 Linux", Toast.LENGTH_SHORT).show()
                        } else {
                            Toast.makeText(this, "连接电脑失败，请检查 Wi-Fi 或 USB", Toast.LENGTH_LONG).show()
                        }
                        finish()
                        overridePendingTransition(0, 0)
                    }
                    return@Thread
                }
            }

            // If clip was empty, attempt pull remote clipboard to wake up
            if (connected) {
                CoreBridge.nativePullRemoteClipboard()
            }
            runOnUiThread {
                if (connected) {
                    Toast.makeText(this, "触控板一碰传：已唤醒并连接 Linux 电脑", Toast.LENGTH_SHORT).show()
                } else {
                    Toast.makeText(this, "未连接电脑，请检查 Wi-Fi 或 USB", Toast.LENGTH_LONG).show()
                }
                finish()
                overridePendingTransition(0, 0)
            }
        }.start()
    }
}
