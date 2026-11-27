# Handoff Report: Milestone M1-C3 Empirical Challenge (Image Clipboard & Nautilus Resolution)

- **Agent**: `challenger_m1_c3_2`
- **Role**: Empirical Challenger (critic, specialist)
- **Target Audience**: Orchestrator (`8eeb0c45-a89c-4e54-a47f-ce415553534a`), Reviewers, Auditor
- **Date**: 2026-10-24
- **Verdict**: **APPROVE**

---

## 1. Observation

Direct empirical observations and execution outputs from independent test executions:

### 1.1 Empirical Stress Test Suite Execution (`build/test_challenger_m1_c3_image_uri`)
A standalone empirical verification test harness (`build/test_challenger_m1_c3_image_uri.cpp`) was constructed and compiled against the core library and Linux subsystem objects.

Compilation Command:
```bash
/usr/bin/c++ -std=c++20 -Wall -Wextra -Wpedantic \
  -I/home/reid-pu/Programming/Linux_to_Android/linux/src \
  -I/home/reid-pu/Programming/Linux_to_Android/core/include \
  /home/reid-pu/Programming/Linux_to_Android/build/test_challenger_m1_c3_image_uri.cpp \
  /home/reid-pu/Programming/Linux_to_Android/build/linux/CMakeFiles/test_linux_subsystem.dir/src/wayland_clipboard.cpp.o \
  /home/reid-pu/Programming/Linux_to_Android/build/linux/CMakeFiles/test_linux_subsystem.dir/src/settings_manager.cpp.o \
  -o /home/reid-pu/Programming/Linux_to_Android/build/test_challenger_m1_c3_image_uri \
  /home/reid-pu/Programming/Linux_to_Android/build/core/libl2a_core.a \
  /usr/lib/x86_64-linux-gnu/libgtk-3.so.0 \
  /usr/lib/x86_64-linux-gnu/libgdk-3.so.0 \
  /usr/lib/x86_64-linux-gnu/libgdk_pixbuf-2.0.so.0 \
  /usr/lib/x86_64-linux-gnu/libX11.so.6 \
  /usr/lib/x86_64-linux-gnu/libgobject-2.0.so.0 \
  /usr/lib/x86_64-linux-gnu/libglib-2.0.so.0 \
  /usr/lib/x86_64-linux-gnu/libayatana-appindicator3.so.1 \
  -ldl
```

Execution Command & Verbatim Output:
```bash
$ ./build/test_challenger_m1_c3_image_uri
=========================================================
[CHALLENGER M1-C3] Empirical Test Suite: Image Clipboard & Nautilus Resolution
=========================================================

--- SUITE 1: URI Parsing & Image Extraction Stress Tests ---
[TEST 1.1] Standard file:/// URI...
  -> PASSED: file:///tmp/l2a_challenger_m1_c3_30/sample.png resolved to image/png.
[TEST 1.2] file://localhost/ URI prefix...
  -> PASSED: file://localhost prefix resolved correctly.
[TEST 1.3] Raw absolute path (/path/to/img.png)...
  -> PASSED: Raw absolute path resolved correctly.
[TEST 1.4] Percent-encoded space (file:///.../holiday%20photo%202026.jpg)...
  -> PASSED: Percent-encoded single space resolved to "/tmp/l2a_challenger_m1_c3_30/holiday photo 2026.jpg"
[TEST 1.5] Multiple percent-encoded spaces and .jpeg extension...
  -> PASSED: Multiple spaces and .jpeg resolved correctly.
[TEST 1.6] Uppercase and mixed case extensions (.PNG, .JPEG, .Jpg, .WebP, .GIF, .BMP)...
  -> PASSED: All uppercase / mixed-case image extensions handled case-insensitively.
[TEST 1.7] Nautilus prefixes: copy\n..., cut\n..., CRLF, and comment lines...
  -> PASSED: Nautilus copy, cut, CRLF, and # comments handled cleanly.
[TEST 1.8] Multiple URIs in text/uri-list...
  -> PASSED: Multiple URIs properly prioritized; non-image lead yields is_image=false.
[TEST 1.9] Non-image URIs (PDF, TXT, TAR.GZ, directories) and fallbacks...
  -> PASSED: All non-image files, zero-byte files, and arbitrary text rejected cleanly.

--- SUITE 2 & 3: Connected End-to-End Pipeline & Remote Pull ---
[INFO] Node A and Node B connected over TLS.
[TEST 2.1] Non-image URI fallback to plain text push over TLS...
  -> PASSED: Non-image URI (file:///tmp/l2a_challenger_m1_c3_30/document.pdf) transferred cleanly as text/plain (NOT binary image).
[TEST 2.2] Image URI resolution to push_clipboard_file over TLS...
  -> PASSED: Image URI successfully resolved to binary image/png stream with matching SHA-256.
[TEST 3.1] Pull latest text clipboard (clip_id == 0)...
  -> PASSED: Pulled latest text correctly (Important text clipboard #42).
[TEST 3.2] Pull latest binary image clipboard (clip_id == 0)...
  -> PASSED: Pulled latest binary image correctly (75 KB, SHA-256 verified).
[TEST 3.3] Pull latest file-backed image clipboard (clip_id == 0)...
  -> PASSED: Pulled latest file-backed image correctly.

--- SUITE 4: Large Image Binary Streams (>512KB) Stress Tests ---
[TEST 4.1] Push 1.2 MB binary image via push_clipboard_bytes...
  -> PASSED: 1.2 MB image streamed via temp file ("/tmp/l2a_challenger_m1_c3_30/node_b_storage/tmp_clip/l2a_clip_9.tmp") with 100% SHA-256 match.
[TEST 4.2] WaylandClipboardBridge::apply_remote_clipboard on large temp file image...
  -> PASSED: Large image written to wl-copy from temp file (1228800 bytes piped).
[TEST 4.3] Push 3.5 MB image file via push_clipboard_file...
  -> PASSED: 3.5 MB image file streamed successfully with matching SHA-256.
[TEST 4.4] Verify ClipboardHistoryManager captures large image stream...
  -> PASSED: History manager correctly captured large streamed image.

=========================================================
[CHALLENGER M1-C3] ALL EMPIRICAL STRESS TESTS PASSED (100%)!
=========================================================
```

### 1.2 Full CTest Regression Suite Execution
Command:
```bash
ctest --test-dir build --output-on-failure
```
Output:
```
Test project /home/reid-pu/Programming/Linux_to_Android/build
      Start  1: ProtocolAndCryptoTest
 1/15 Test  #1: ProtocolAndCryptoTest .............   Passed    0.03 sec
      Start  2: LoopbackTransferAndResumeTest
 2/15 Test  #2: LoopbackTransferAndResumeTest .....   Passed    0.10 sec
      Start  3: Test_BidirectionalConcurrency
 3/15 Test  #3: Test_BidirectionalConcurrency .....   Passed    0.09 sec
      Start  4: Test_LargeBatchMetadata
 4/15 Test  #4: Test_LargeBatchMetadata ...........   Passed    0.19 sec
      Start  5: Test_ReentrantCallback
 5/15 Test  #5: Test_ReentrantCallback ............   Passed    0.15 sec
      Start  6: Test_DisconnectAndResume
 6/15 Test  #6: Test_DisconnectAndResume ..........   Passed    0.10 sec
      Start  7: Test_CorruptRecovery
 7/15 Test  #7: Test_CorruptRecovery ..............   Passed    0.10 sec
      Start  8: Test_MalformedPacketFuzz
 8/15 Test  #8: Test_MalformedPacketFuzz ..........   Passed    0.02 sec
      Start  9: Test_TempCleanup
 9/15 Test  #9: Test_TempCleanup ..................   Passed    0.08 sec
      Start 10: Test_InitialClipboardSync
10/15 Test #10: Test_InitialClipboardSync .........   Passed    0.06 sec
      Start 11: Test_ReverseTransferAndFallback
11/15 Test #11: Test_ReverseTransferAndFallback ...   Passed    0.12 sec
      Start 12: Test_SettingsAndHistory
12/15 Test #12: Test_SettingsAndHistory ...........   Passed    0.03 sec
      Start 13: Test_ImageClipboardSync
13/15 Test #13: Test_ImageClipboardSync ...........   Passed    0.26 sec
      Start 14: Test_ChallengerHarness
14/15 Test #14: Test_ChallengerHarness ............   Passed    0.38 sec
      Start 15: LinuxSubsystemTest
15/15 Test #15: LinuxSubsystemTest ................   Passed    3.31 sec

100% tests passed, 0 tests failed out of 15

Total Test time (real) =   5.01 sec
```

---

## 2. Logic Chain

1. **Local File Manager URI Variations & Decoding (`linux/src/wayland_clipboard.cpp:70-143`)**:
   - *Observation*: Tests 1.1 through 1.7 evaluated standard `file:///` URIs, `file://localhost` prefix, raw paths, single `%20` space, multiple spaces, CRLF linebreaks, Nautilus `copy` and `cut` prefixes, and `#` URI-list comments.
   - *Trace*: In `try_resolve_local_image_file()`, the token scanner loops through lines delimited by `\r\n`, filtering out `"copy"`, `"cut"`, and lines starting with `'#'`. Trimming strips leading/trailing spaces. The percent decoder successfully converts `%20` to `' '` and checks file existence via `std::filesystem::exists` and `is_regular_file`. Extension extraction normalizes to lowercase (`std::transform(..., ::tolower)`), matching `.png`, `.jpg`, `.jpeg`, `.webp`, `.gif`, `.bmp` regardless of case.
   - *Empirical Proof*: All image variations were correctly identified with valid MIME types and canonical filesystem paths.

2. **Non-Image URIs & Clean Fallback to Text (`linux/src/wayland_clipboard.cpp:121-140, 389-438`)**:
   - *Observation*: In Tests 1.8, 1.9, and 2.1, non-image files (`.pdf`, `.txt`, `.tar.gz`), directory paths, zero-byte files, and arbitrary web text were evaluated.
   - *Trace*: `try_resolve_local_image_file()` returns `{is_image: false}` whenever the file extension is not an image, when the target is a directory, when file size is 0 (`sz == 0`), or when the file does not exist. In `on_gtk_text_received()`, when `resolved.is_image` is false, execution falls through Priority 2 and enters Priority 3 (`push_clipboard_text`), pushing the raw URI string (`file:///path/to/document.pdf`) as `text/plain`.
   - *Empirical Proof*: Node B received `rec_pdf_as_text` with `mime_type == "text/plain"` and data matching `"file:///.../document.pdf"`. No binary image frame was generated.

3. **Multi-URI in `text/uri-list` Prioritization (`linux/src/wayland_clipboard.cpp:73-86`)**:
   - *Observation*: In Test 1.8, multiple URIs separated by `\n` were supplied.
   - *Trace*: When multiple images are copied (`img1.png\nimg2.jpg`), the first candidate (`img1.png`) is selected and streamed. When mixed files are copied with a non-image leading (`doc.pdf\nimg1.png`), `doc.pdf` is evaluated as the candidate and rejected, causing the entire selection to fall back cleanly to text.
   - *Empirical Proof*: Multi-image selection correctly streamed the primary image; non-image lead cleanly yielded `is_image = false` and preserved text list semantics.

4. **Remote Clipboard Pull (`clip_id == 0`) (`core/src/clipboard_stream.cpp:357-406`)**:
   - *Observation*: In Tests 3.1, 3.2, and 3.3, Node B issued `pull_remote_clipboard()` after Node A had set (1) text, (2) binary image (`image/png`), and (3) file-backed image (`image/jpeg`).
   - *Trace*: Node A's `latest_local_item_` cache recorded the latest item in `push_clipboard_bytes` or `push_clipboard_file`. When `kClipPullReq` with `clip_id == 0` arrived at Node A, it retrieved `latest_local_item_`, generated a new `stream_id`, emitted `CLIP_OFFER`, and streamed the data payload back to Node B via `stream_bytes_now` or `stream_file_now`.
   - *Empirical Proof*: Node B successfully received all three payloads on demand with byte-level fidelity and exact SHA-256 match.

5. **Large Image Binary Streams (>512KB) & Disk Streaming (`core/src/clipboard_stream.cpp:9-10, 280-288, 300-349`)**:
   - *Observation*: In Tests 4.1, 4.2, 4.3, and 4.4, payloads of 1.2 MB and 3.5 MB were transferred.
   - *Trace*: `kMaxInlineMemoryBytes` is configured at 512 KB. When `offer->total_size > kMaxInlineMemoryBytes`, the receiver sets `stream.use_temp_file = true`, allocates `l2a_clip_<id>.tmp` in `temp_dir_`, streams each 64KB chunk directly into the file on disk, and leaves `completed_item.data` empty while populating `completed_item.temp_file_path`. `apply_remote_clipboard` detects non-empty `temp_file_path`, streams to `wl-copy` via `write_file_to_wl_copy`, and loads the Pixbuf via `gdk_pixbuf_new_from_file`.
   - *Empirical Proof*: Received items had `data.empty() == true`, `fs::exists(temp_file_path) == true`, and disk SHA-256 matched sender SHA-256 100%. Piped bytes to mock `wl-copy` matched 1,228,800 bytes exactly.

---

## 3. Caveats

- No caveats. All 15 CTest regression targets and all 13 empirical challenge cases passed with 100% success rate without memory faults or deadlocks.

---

## 4. Conclusion

The image clipboard and Nautilus URI resolution logic in Milestone M1-C3 is robust, fully compliant with requirements, and resilient against adverse edge cases:
- URI variations (spaces, `%20`, uppercase extensions, Nautilus `copy`/`cut` prefixes, CRLF, spec comments) are parsed and normalized reliably.
- Non-image files (.pdf, .txt, .tar.gz, empty files, directories) never trigger binary image streaming and cleanly fall back to plain text.
- Remote clipboard pull (`clip_id == 0`) successfully fetches the latest text, binary image, or file-backed image.
- Large image streams (>512KB) stream cleanly to disk temporary files, avoiding heap blowup, while keeping SHA-256 verification and `wl-copy` pipe forwarding intact.

Verdict: **APPROVE**.

---

## 5. Verification Method

To independently reproduce and verify this challenge suite:

```bash
# 1. Compile the empirical test suite
/usr/bin/c++ -std=c++20 -Wall -Wextra -Wpedantic \
  -I/home/reid-pu/Programming/Linux_to_Android/linux/src \
  -I/home/reid-pu/Programming/Linux_to_Android/core/include \
  /home/reid-pu/Programming/Linux_to_Android/build/test_challenger_m1_c3_image_uri.cpp \
  /home/reid-pu/Programming/Linux_to_Android/build/linux/CMakeFiles/test_linux_subsystem.dir/src/wayland_clipboard.cpp.o \
  /home/reid-pu/Programming/Linux_to_Android/build/linux/CMakeFiles/test_linux_subsystem.dir/src/settings_manager.cpp.o \
  -o /home/reid-pu/Programming/Linux_to_Android/build/test_challenger_m1_c3_image_uri \
  /home/reid-pu/Programming/Linux_to_Android/build/core/libl2a_core.a \
  /usr/lib/x86_64-linux-gnu/libgtk-3.so.0 \
  /usr/lib/x86_64-linux-gnu/libgdk-3.so.0 \
  /usr/lib/x86_64-linux-gnu/libgdk_pixbuf-2.0.so.0 \
  /usr/lib/x86_64-linux-gnu/libX11.so.6 \
  /usr/lib/x86_64-linux-gnu/libgobject-2.0.so.0 \
  /usr/lib/x86_64-linux-gnu/libglib-2.0.so.0 \
  /usr/lib/x86_64-linux-gnu/libayatana-appindicator3.so.1 \
  -ldl

# 2. Run the empirical test binary
./build/test_challenger_m1_c3_image_uri

# 3. Run full CTest suite
ctest --test-dir build --output-on-failure
```

Expected result: 100% tests pass (15/15 in CTest, 13/13 in empirical challenger harness).
