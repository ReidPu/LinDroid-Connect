# DISPATCH — explorer_survey_r2_1

## Task Assignment
**Role**: Codebase Researcher & Protocol Explorer (Core & Linux Clipboard / Channel Architecture)
**Working Directory**: `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/explorer_survey_r2_1`
**Source of Truth**: `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/ORIGINAL_REQUEST.md` (read section `## 2026-10-23T12:34:22Z`)

## Scope & Objective
Investigate Requirements R1 and R2 (Linux desktop & C++ core side):
1. **R1**: Why does Computer -> Phone clipboard sync fail initially after Wi-Fi / USB connection until a file/folder is sent?
   - Inspect `core/src/engine.cpp`, `core/include/l2a/engine.hpp`, `core/src/clipboard_stream.cpp`, `core/src/file_transfer.cpp`, `core/src/discovery.cpp`, `core/src/tls_channel.cpp`.
   - Analyze how `active_channel_` is created, stored, and used in `send_clipboard` vs `send_file`. Does `send_clipboard` fail to establish an on-demand connection to known discovered peers / ADB forwarding (`127.0.0.1:53322`) when `active_channel_` is null or disconnected?
   - Compare with `send_file` which establishes a connection and caches the channel.
2. **R2 (Linux side)**: How does Linux desktop receive and apply clipboard updates sent from Android?
   - Inspect `linux/src/wayland_clipboard.cpp`, `linux/src/app_window.cpp`, `linux/src/main.cpp`.
   - Verify how incoming clipboard text and image frames are passed to Wayland (`wl-copy`) and GTK clipboard, ensuring `Ctrl+V` immediately pastes the received text.

## Deliverables
Produce `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/explorer_survey_r2_1/handoff.md` detailing:
- Exact root causes with file names and line numbers.
- Proposed concrete code changes and architectural fixes.
- Test strategy to verify R1 without any prior file transfer.


## 2026-10-23T12:38:35Z
You are explorer_survey_r2_1.
Your working directory is: `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/explorer_survey_r2_1`
Please read `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/ORIGINAL_REQUEST.md` (section `## 2026-10-23T12:34:22Z`) and your dispatch file `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/explorer_survey_r2_1/DISPATCH.md`.

Investigate Requirements R1 and R2 (Linux desktop & C++ core side):
1. R1: Computer -> Phone initial clipboard sync failure (must work immediately upon connection/discovery without needing prior file transfer).
   - Investigate `core/src/engine.cpp`, `core/include/l2a/engine.hpp`, `core/src/clipboard_stream.cpp`, `core/src/file_transfer.cpp`, `core/src/discovery.cpp`, `core/src/tls_channel.cpp`.
   - Analyze how `active_channel_` is created, stored, and used in `send_clipboard` vs `send_file`. Does `send_clipboard` fail to establish an on-demand connection to known discovered peers / ADB forwarding (`127.0.0.1:53322`) when `active_channel_` is null or disconnected?
2. R2 (Linux side): How does Linux desktop receive and apply clipboard updates sent from Android?
   - Investigate `linux/src/wayland_clipboard.cpp`, `linux/src/app_window.cpp`, `linux/src/main.cpp`.
   - Verify how incoming clipboard text and image frames are passed to Wayland (`wl-copy`) and GTK clipboard, ensuring `Ctrl+V` immediately pastes the received text.

You are read-only. Do NOT modify source code. Produce `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/explorer_survey_r2_1/handoff.md` with detailed evidence, file lines, root cause analysis, and concrete recommended fixes. Update your `progress.md` regularly and send a completion message when done.
