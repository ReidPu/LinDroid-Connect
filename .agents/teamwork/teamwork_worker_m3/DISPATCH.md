# Task Assignment: M3-C4 Automated Verification & Screenshot Capture

## Objective
Execute automated verification scripts, capture authoritative Light and Dark mode screenshots of `/home/reid-pu/Programming/Linux_to_Android/hyperos_ui_prototype.html`, verify `/home/reid-pu/Programming/Linux_to_Android/hyperos_ui_spec.md`, and verify both Linux and Android workspace builds.

## Scope of Verification
1. **Automated CSS/DOM & Static Verification Script**:
   - Run both static regex/AST inspection and dynamic in-DOM WebKit2 computed style inspection on `hyperos_ui_prototype.html`:
     * Assert 0 `backdrop-filter: blur(...)` or `-webkit-backdrop-filter`.
     * Assert 0 `filter: blur(...)` or ambient colored orbs.
     * Assert 0 emerald/green color tokens (`#059669`, `#10B981`, `#34D399`, `#047857`, `#22C55E`, `#16A34A`, `rgba(16, 185, 129, ...)`).
     * Assert primary accent is unified to Tech Blue (`#2563EB` / `#3B82F6`).
     * Assert solid matte card surfaces with 1px borders and soft elevation shadows.
     * Assert local vs remote origin tiering (solid blue badge local vs tinted blue container remote).
   - Write and save the standalone verification script to `/home/reid-pu/Programming/Linux_to_Android/verify_ui_showcase.py` (or execute and document it).

2. **Authoritative Browser Screenshot Captures**:
   - Create directory `/home/reid-pu/Programming/Linux_to_Android/screenshots/` (if it does not exist).
   - Capture high-resolution screenshots of `hyperos_ui_prototype.html` in:
     * Light Theme: `/home/reid-pu/Programming/Linux_to_Android/screenshots/prototype_light.png`
     * Dark Theme: `/home/reid-pu/Programming/Linux_to_Android/screenshots/prototype_dark.png`
   - Verify image dimensions, color channels, and that both themes render with correct contrast, solid surface hierarchies, and legible typography.

3. **Specification Verification**:
   - Audit `/home/reid-pu/Programming/Linux_to_Android/hyperos_ui_spec.md`:
     * Verify exact hex tokens matching prototype.
     * Verify WCAG contrast matrices.
     * Verify 4-tier elevation model.
     * Verify platform mappings for GTK3 (`kSolidMatteGtkCss`) and Android (`createSolidMatteDrawable`, `applySolidMatteTheme`).

4. **Dual Workspace Compilation & Test Suite**:
   - Linux: Run `cmake --build build -j4 && ctest --test-dir build --output-on-failure`. Document exact output (expected 15/15 passed).
   - Android: Run `ANDROID_USER_HOME=/tmp/android_home ./android/gradlew -p android --no-daemon --offline -Dorg.gradle.java.home=/usr/lib/jvm/java-21-openjdk-amd64 assembleDebug`. Document exact output (expected BUILD SUCCESSFUL).

## Mandatory Integrity Warning
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A teamwork_auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.

## Output
Deliver your findings, command outputs, and handoff report to `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/teamwork_worker_m3/handoff.md`.

## 2026-10-30T10:12:18Z
You are teamwork_worker_m3.
Your working directory is: /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/teamwork_worker_m3/
Please read your instructions at /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/teamwork_worker_m3/DISPATCH.md and the original user request at /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/ORIGINAL_REQUEST.md.

You are responsible for Milestone M3-C4 Automated Verification & Screenshot Capture:
1. Run automated CSS/DOM static and dynamic inspection verifying zero blur, zero green, unified Tech Blue palette (#2563EB / #3B82F6), and solid matte cards on /home/reid-pu/Programming/Linux_to_Android/hyperos_ui_prototype.html.
2. Render and capture authoritative Light and Dark mode screenshots into /home/reid-pu/Programming/Linux_to_Android/screenshots/ (prototype_light.png, prototype_dark.png).
3. Verify /home/reid-pu/Programming/Linux_to_Android/hyperos_ui_spec.md matches all tokens and models.
4. Run both Linux and Android build & test commands (Linux CMake + CTest 15/15; Android gradlew assembleDebug).

MANDATORY INTEGRITY WARNING:
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A teamwork_auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.

Deliver your handoff report to /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/teamwork_worker_m3/handoff.md and notify me via send_message when complete.

## 2026-10-30T10:28:40Z
**Context**: Milestone M3-C4 Automated Verification & Screenshot Capture
**Content**: Checking on status of verification script execution, final screenshot naming (prototype_light.png & prototype_dark.png), and workspace builds.
**Action**: Please report current status or deliver handoff.md when ready.
