# Handoff Report — challenger_m2_c2_2

**Role**: Empirical Challenger (critic, specialist)
**Milestone**: M2-C2 (Android NFC Reader Mode & Share Sheet Logic)
**Target**: Orchestrator (`af5814b6-fe43-4f2b-b498-cb11fbccdb34`), Worker (`worker_m2_c2`)
**Verdict**: **REQUEST_CHANGES**

---

## 1. Observation

### 1.1 NFC Reader Mode Bytecode Inspection (`MainActivity.kt:183-200`)
Direct disassembly of `/home/reid-pu/Programming/Linux_to_Android/android/app/build/tmp/kotlin-classes/debug/com/reid/l2a/MainActivity.class` via `javap -c -p` reveals:
```bytecode
  protected void onResume();
    Code:
       0: aload_0
       1: invokespecial #344   // Method androidx/appcompat/app/AppCompatActivity.onResume:()V
       4: aload_0
       5: getfield      #66    // Field nfcAdapter:Landroid/nfc/NfcAdapter;
       8: dup
       9: ifnonnull     14
      12: pop
      13: return
      14: astore_1
      15: sipush        271
      18: istore_2
      19: new           #194   // class android/os/Bundle
      22: dup
      23: invokespecial #345   // Method android/os/Bundle."<init>":()V
      ...
      37: ldc_w         #347   // String presence
      40: sipush        250
      43: invokevirtual #351   // Method android/os/Bundle.putInt:(Ljava/lang/String;I)V
      ...
      61: invokevirtual #357   // Method android/nfc/NfcAdapter.enableReaderMode:(Landroid/app/Activity;Landroid/nfc/NfcAdapter$ReaderCallback;ILandroid/os/Bundle;)V
      64: return

  protected void onPause();
    Code:
       0: aload_0
       1: invokespecial #365   // Method androidx/appcompat/app/AppCompatActivity.onPause:()V
       4: aload_0
       5: getfield      #66    // Field nfcAdapter:Landroid/nfc/NfcAdapter;
       8: dup
       9: ifnull        22
      12: aload_0
      13: checkcast     #353   // class android/app/Activity
      16: invokevirtual #369   // Method android/nfc/NfcAdapter.disableReaderMode:(Landroid/app/Activity;)V
      19: goto          23
      22: pop
      23: return
```
- **Flags Bitmask**: `sipush 271` matches `FLAG_READER_NFC_A (1) | FLAG_READER_NFC_B (2) | FLAG_READER_NFC_F (4) | FLAG_READER_NFC_V (8) | FLAG_READER_NO_PLATFORM_SOUNDS (256)` = `271` (`0x010F`).
- **Presence Delay**: Key `"presence"` (`NfcAdapter.EXTRA_READER_PRESENCE_CHECK_DELAY`) is loaded with `sipush 250` into the extras `Bundle`.
- **onPause**: `disableReaderMode(this)` is invoked with a preceding `ifnull` branch check.

### 1.2 NFC Tagging & Thread Safety Inspection (`MainActivity.kt:215-302`)
Disassembly of `writeTagNdefOrFormatable` and `handleTapTrigger` reveals:
```bytecode
  // writeTagNdefOrFormatable
  42: aload_1
  43: invokestatic  #474   // Method android/nfc/tech/Ndef.get:(Landroid/nfc/Tag;)Landroid/nfc/tech/Ndef;
  ...
  61: invokevirtual #480   // Method android/nfc/tech/Ndef.isWritable:()Z
  ...
  89: invokevirtual #496   // Method android/nfc/tech/Ndef.getMaxSize:()I
  ...
 144: invokevirtual #508   // Method android/nfc/tech/Ndef.writeNdefMessage:(Landroid/nfc/NdefMessage;)V
 ...
 203: invokestatic  #520   // Method android/nfc/tech/NdefFormatable.get:(Landroid/nfc/Tag;)Landroid/nfc/tech/NdefFormatable;
 ...
 223: invokevirtual #524   // Method android/nfc/tech/NdefFormatable.format:(Landroid/nfc/NdefMessage;)V
```
- Formatted `Ndef` tags are checked first, checking `isWritable` and `maxSize >= msg.byteArrayLength`.
- Unformatted tags fallback to `NdefFormatable.get(tag)` and `format(msg)`.
- UI updates in `handleTagDiscovered` are dispatched via `runOnUiThread` (`invokevirtual #445 Method runOnUiThread`).
- In non-write mode, `handleTapTrigger` inspects `record.toUri()`, verifies `scheme == "l2a"` and `host == "tap"`, and launches `NfcTriggerActivity` inside `runOnUiThread`.
- **Potential Concurrency Smell**: In `MainActivity.kt:217`, `val hostId = hostIdInput.text.toString().trim()` is executed directly on the NFC background binder thread before dispatching to `runOnUiThread`.

### 1.3 Share Sheet Staging File Creation & Missing Cleanup (`ShareReceiverActivity.kt:111-125`)
In `ShareReceiverActivity.kt:111-125`:
```kotlin
// Fallback to stream copy into cacheDir staging file
val stagingDir = File(cacheDir, "share_staging").apply { mkdirs() }
val tempFile = File(stagingDir, "${System.currentTimeMillis()}_$displayName")
contentResolver.openInputStream(uri)?.use { input ->
    tempFile.outputStream().use { output ->
        input.copyTo(output)
    }
}
if (tempFile.exists() && tempFile.length() > 0) {
    val batchId = CoreBridge.nativeSendFile(tempFile.absolutePath)
    if (batchId != 0L) {
        sentCount++
    }
}
```
Global grep search for `share_staging` across the entire project (`grep_search` query `share_staging`) returned exactly 1 source reference in `ShareReceiverActivity.kt:112`.
- `tempFile.delete()` is never called anywhere in `ShareReceiverActivity.kt`.
- `ShareReceiverActivity` does not implement `onDestroy()`.
- `L2AService.kt` contains 0 references to `share_staging` and does not purge the cache directory on startup or completion.
- In `jni_bridge.cpp:230-260`, cleanup is only performed for `g_active_fd_streams` (`nativeSendFileFd`). Files sent via `nativeSendFile(tempFile.absolutePath)` are untracked by the JNI layer.
- Execution of empirical test harness `tests/test_m2_c2_empirical.py` confirmed:
  ```
  [FAIL] ShareReceiverActivity deletes tempFile after transfer - Empirical check: delete() call on staging file
  [FAIL] ShareReceiverActivity has onDestroy cleanup routine - 
  [FAIL] L2AService purges share_staging on startup or file completion - 
  Empirical stress simulation: 5 files staged, total 5.0 MB accumulated in cacheDir/share_staging
  [PASS] Staging directory accumulates files without automatic cleanup
  ```

---

## 2. Logic Chain

1. **NFC Reader Mode Compliance**:
   - Observation 1.1 confirms that `MainActivity.kt` configures `enableReaderMode` with bitmask 271 (`FLAG_READER_NFC_A | FLAG_READER_NFC_B | FLAG_READER_NFC_F | FLAG_READER_NFC_V | FLAG_READER_NO_PLATFORM_SOUNDS`).
   - The presence check delay is explicitly set to 250ms via `Bundle.putInt("presence", 250)`.
   - `onPause` properly invokes `disableReaderMode(this)` with null-safety checks.
   - Therefore, the NFC Reader Mode requirements are fully satisfied.

2. **NFC Tagging & Thread Safety Compliance**:
   - Observation 1.2 demonstrates that unformatted touchpad tags returning `null` on `Ndef.get(tag)` are intercepted by `NdefFormatable.get(tag)` and formatted.
   - Background binder thread callbacks from `ReaderCallback` do not mutate UI components directly; all Toast invocations, `statusView` updates, and Activity launches are safely wrapped in `runOnUiThread`.
   - Non-write mode tap triggers validate `l2a://tap` and launch `NfcTriggerActivity`.

3. **Share Sheet Sanitization & Staging File Leakage**:
   - Observation 1.3 shows that display name resolution in `resolveCleanDisplayName` correctly strips directory path separators via `File(baseName).name`, prevents directory traversal (`../../etc/shadow` -> `shadow.txt`), appends MIME extensions when missing, and is backed by empty/character sanitization guards in `jni_bridge.cpp` (`_` substitution).
   - However, for stream-based URIs that fail `openFileDescriptor` (cloud providers, virtual files, restricted streams), `ShareReceiverActivity` copies the stream into `cacheDir/share_staging/${System.currentTimeMillis()}_$displayName`.
   - Because `tempFile` is never deleted, the directory is never purged on startup, and `L2AService` does not evict staging files upon transmission completion, files staged in `cacheDir/share_staging` accumulate permanently on the device.
   - For users transferring photos or video clips, this introduces a cumulative internal storage leak and a data privacy issue (unencrypted files lingering indefinitely in cache).

---

## 3. Caveats

1. **Xiaomi 14 Pro Physical USB Prompt**: The connected Xiaomi 14 Pro device (`52599bc3`) requires manual user interaction on the screen to confirm `INSTALL_FAILED_USER_RESTRICTED` when installing APKs via USB. Automated APK push to `/data/local/tmp/app-debug.apk` succeeded, but runtime execution on device relies on user screen confirmation.
2. **Real Hardware RF Coupling**: Hardware radio-frequency touch interaction on the physical Redmi G Pro 2024 touchpad was verified via bytecode analysis and URI parser simulation, as physical proximity cannot be actuated remotely.

---

## 4. Conclusion

- **Verdict**: **REQUEST_CHANGES**
- **Action Required**: Resolve `DEFECT-M2C2-01` by adding an automatic staging cleanup mechanism.
- **Recommended Remediation**:
  1. In `ShareReceiverActivity.kt` or `L2AService.kt` (e.g. in `onCreate()`), purge stale staging files older than 10 minutes from `File(cacheDir, "share_staging")`.
  2. Alternatively, in `L2AService.onFileCompleted`, or via a background thread worker in `ShareReceiverActivity`, delete `tempFile` once the file transfer completes or when the staging directory exceeds a reasonable quota (e.g., 50MB).
  3. (Minor) In `MainActivity.kt:217`, capture `val hostId = hostIdInput.text.toString().trim()` on the UI thread when clicking `btnWriteNfc`, rather than reading the `EditText` on the NFC binder thread.

---

## 5. Verification Method

### 5.1 Run Empirical Verification Suite
```bash
python3 /home/reid-pu/Programming/Linux_to_Android/tests/test_m2_c2_empirical.py
```
**Current Execution Result**:
```
=== [1] NFC Reader Mode Flags & Lifecycle Verification ===
  [PASS] Bitmask math: NFC_A | NFC_B | NFC_F | NFC_V | NO_PLATFORM_SOUNDS (Computed: 271 (0x10f))
  [PASS] MainActivity.class exists and is readable
  [PASS] onResume bytecode loads flags 271 (0x10F) (sipush 271 found in bytecode)
  [PASS] onResume bytecode invokes enableReaderMode
  [PASS] EXTRA_READER_PRESENCE_CHECK_DELAY ('presence') used in options bundle
  [PASS] Presence check delay set to 250ms (sipush 250)
  [PASS] onPause bytecode invokes disableReaderMode
  [PASS] onPause includes null-safety check before disableReaderMode

=== [2] NFC Tagging, NdefFormatable & Thread Safety Verification ===
  [PASS] writeTag queries Ndef.get(tag)
  [PASS] writeTag queries NdefFormatable.get(tag) fallback
  [PASS] writeTag invokes formatable.format(msg) for unformatted tags
  [PASS] writeTag invokes ndef.writeNdefMessage(msg) for formatted tags
  [PASS] handleTagDiscovered dispatches UI updates via runOnUiThread
  [PASS] handleTapTrigger dispatches Intent launch via runOnUiThread
  [PASS] NFC Tap URI matching oracle (scheme=='l2a' && host=='tap')

=== [3] Share Sheet Display Name Sanitization & Staging File Creation/Cleanup ===
  [PASS] ShareReceiverActivity.class exists and is readable
  [PASS] Display name sanitization handles slashes, percent encoding, and MIME fallback
  [PASS] jni_bridge.cpp provides empty filename fallback ('shared_file')
  [PASS] jni_bridge.cpp sanitizes slashes, backslashes, colons to underscore
  [PASS] ShareReceiverActivity creates staging directory 'cacheDir/share_staging'
  [PASS] ShareReceiverActivity copies stream into staging file as fallback
  [FAIL] ShareReceiverActivity deletes tempFile after transfer - Empirical check: delete() call on staging file
  [FAIL] ShareReceiverActivity has onDestroy cleanup routine - 
  [FAIL] L2AService purges share_staging on startup or file completion - 
  Empirical stress simulation: 5 files staged, total 5.0 MB accumulated in cacheDir/share_staging
  [PASS] Staging directory accumulates files without automatic cleanup

EMPIRICAL TEST SUMMARY: Total=25, Passed=22, Failed=3
```

### 5.2 Build and Full Regression Test
```bash
# Core & Linux test suite
ctest --test-dir build --output-on-failure

# Android debug APK assembly
JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64 \
ANDROID_USER_HOME=/home/reid-pu/Programming/Linux_to_Android/.android \
./android/gradlew -p android assembleDebug -Duser.home=/home/reid-pu/Programming/Linux_to_Android
```
**Result**: 10/10 CTest suites passed; `assembleDebug` succeeded in 7s.
