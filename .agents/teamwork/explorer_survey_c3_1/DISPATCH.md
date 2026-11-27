## 2026-10-24T03:13:51Z

You are explorer_survey_c3_1.
Your working directory is: /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/explorer_survey_c3_1
The authoritative user request is in: /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/ORIGINAL_REQUEST.md (specifically check the latest section under `## 2026-10-24T03:09:08Z`, as well as previous sections for historical context).
Read ORIGINAL_REQUEST.md before starting work.
Also check /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/PROJECT.md.

YOUR FOCUS:
Map the technical scope, existing architecture, and concrete implementation strategy for **Requirement R1**:
1. Dual-end Settings Module:
   - Linux GTK desktop: settings dialog/panel or tab in `app_window.cpp`, persistent config storage (e.g. JSON or key-value config in `~/.config/l2a/settings.json` or glib keyfile).
   - Android app: settings UI in `MainActivity.kt` or preferences screen, `SharedPreferences` persistence.
   - Configuration items:
     - Independent directional auto-share toggles: "PC -> Phone real-time auto-share" (default enabled) & "Phone -> PC real-time auto-share" (default enabled).
     - Clipboard history retention limit (5-200 items, default 20 items).
2. True Background Seamless Sync without Window Focus:
   - Linux (Ubuntu GNOME Wayland / mutter):
     - Analyze why Wayland window loses clipboard events when unfocused.
     - Investigate how to capture system clipboard in the background without focus (e.g. XWayland/X11 bridge via X11 clipboard polling or XFixesSelectionNotify on `:0` DISPLAY, or `wl-paste` alternatives / background helper process).
     - Investigate writing to Wayland clipboard in the background so Ctrl+V works in any external application without clicking `l2a-desktop`.
   - Android (Xiaomi 14 Pro HyperOS / Android 14):
     - Investigate background clipboard capture limitations on Android 10+ / HyperOS.
     - Evaluate implementation options: 1x1 transparent SYSTEM_ALERT_WINDOW floating window on `OnPrimaryClipChangedListener`, accessibility service, or quick action notification button.
3. Manual Control Mode (when auto-share is disabled):
   - Linux: "Send current clipboard" and "Pull remote clipboard" buttons in GTK window / tray menu.
   - Android: "Send current clipboard" and "Pull remote clipboard" in UI and in foreground service persistent notification (Notification Action Buttons).

Investigate the codebase (e.g., `linux/src/wayland_clipboard.cpp`, `linux/src/app_window.cpp`, `linux/src/main.cpp`, `core/include/l2a/engine.hpp`, `core/src/engine.cpp`, `android/app/src/main/java/com/reid/l2a/L2AService.kt`, `MainActivity.kt`, `CoreBridge.kt`, `AndroidManifest.xml`).
Write a thorough, evidence-grounded report to `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/explorer_survey_c3_1/handoff.md`. Include file paths, line numbers, root causes, exact architectural designs, and recommended changes.
