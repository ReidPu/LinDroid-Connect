## 2026-10-24T03:13:51Z

Sender: 8eeb0c45-a89c-4e54-a47f-ce415553534a
Priority: MESSAGE_PRIORITY_HIGH

Focus: Requirements R3 & R4:
1. Requirement R3: Dual-end Clipboard History Viewer & Non-destructive Selective Paste:
   - Clipboard History Manager (C++ core or Linux desktop & Android):
     - Data model: text preview, image thumbnail preview, origin tag (Local/Remote), timestamp, ID.
     - Strict adherence to max history limit N (5-200, default 20), dynamic trimming on config change, item deletion, clear all.
     - Selective Paste Mechanism without overwriting #0 newest item:
       - Normal paste (Ctrl+V / long press) pastes #0 newest item.
       - Selective paste: When clicking history item #k:
         - Linux: how to paste item #k into target window without permanently replacing or displacing the newest #0 item in the clipboard (e.g. inject paste or temporary clipboard switch followed by immediate restoration of #0 item, preserving history order).
         - Android: how to provide one-time paste or temporary clipboard setting with delayed restoration of #0, keeping history order intact.
2. Requirement R4: Touchpad NFC (NTAG5332) Write & Tap Trigger with Read-Only UID Fallback:
   - Inspect `android/app/src/main/` (`AndroidManifest.xml`, `MainActivity.kt`, `NfcTriggerActivity.kt`).
   - Root cause analysis: Why did Xiaomi 14 Pro vibrate/chime when touching Redmi G Pro 2024 touchpad, but fail to write or trigger in app?
   - Tag writing: Support `Ndef`, `NdefFormatable`, and `MifareUltralight`/`NfcA` for writing `l2a://tap`.
   - Crucial Fallback: Hardware Tag UID Binding:
     - Redmi G Pro touchpad NTAG5332 is often factory-locked as read-only by Xiaomi Share. NDEF write returns failure or read-only error.
     - When tapping in binding mode: regardless of whether NDEF overwrite succeeds or fails, read the tag's hardware unique UID (via `tag.id.toHexString()`), bind and persist this UID in Android SharedPreferences, and update UI to display "Bound Touchpad NFC UID: XX:XX:XX:XX:XX:XX:XX".
   - NFC Trigger Handling:
     - Register comprehensive intent-filters (`ACTION_NDEF_DISCOVERED` with `l2a://tap`, `ACTION_TECH_DISCOVERED`, `ACTION_TAG_DISCOVERED`).
     - In `NfcTriggerActivity.kt` and `MainActivity.kt`, check if the tapped tag matches the bound hardware UID (or NDEF URI). If matched, instantly execute clipboard sync & connection handshake!
