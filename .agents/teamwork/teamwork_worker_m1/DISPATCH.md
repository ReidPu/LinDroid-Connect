# Task Assignment: M1-C4 Interactive Showcase Prototype

## Objective
Author the interactive showcase prototype `/home/reid-pu/Programming/Linux_to_Android/hyperos_ui_prototype.html` implementing clean solid matte card surfaces, unified Tech Blue palette (#2563EB / #3B82F6), zero green accents, and interactive controls.

## File Ownership
- You EXCLUSIVELY own: `/home/reid-pu/Programming/Linux_to_Android/hyperos_ui_prototype.html`
- Do NOT modify any other project files.

## Required References
Read these files before starting:
- `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/ORIGINAL_REQUEST.md`
- `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/PROJECT.md`
- `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/explorer_ui_survey_1/survey_report.md`
- `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/spec_miner_ui_survey_2/survey_report.md`
- `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/explorer_ui_survey_3/survey_report.md`

## Concrete Deliverable Requirements
1. **Surface Material Overhaul (R1)**:
   - ZERO `backdrop-filter: blur(...)` or `-webkit-backdrop-filter`.
   - ZERO ambient background colored orbs or blurry radial gradients.
   - All cards and toolbars use solid matte surfaces (`#FFFFFF` in Light, `#1E293B` in Dark) with subtle 1px borders (`#E2E8F0` in Light, `#334155` in Dark) and soft elevation box-shadows.
   - Root canvas is flat solid `#F8FAFC` (Light) / `#0F172A` (Dark).

2. **Unified Tech Blue Color System (R2)**:
   - ZERO emerald/green tokens (`#059669`, `#10B981`, `#34D399`, `#047857`, `rgba(16, 185, 129, ...)`).
   - Primary action buttons, tabs, selection states, and focus rings use unified Tech Blue (`#2563EB` Light / `#3B82F6` Dark).
   - Device origin distinction:
     * Local device (Origin 0): Solid Tech Blue badge (`#2563EB` / `#3B82F6`), text `#FFFFFF`, bold font weight, `💻 本机` / `📱 本机` icon.
     * Remote device (Origin 1): Tinted Tech Blue container (`#EFF6FF` / border `#BFDBFE` Light; `#172554` / border `#3B82F6` Dark), blue text (`#1D4ED8` Light / `#93C5FD` Dark), `💻 远端` / `📱 远端` icon.
   - Service / sync status pill: Tech blue container badge, zero green.

3. **Showcase Partitioned Experience & Interactivity (R3)**:
   - Partitioned showcase representing both the Linux Desktop UI and Android Mobile UI.
   - Header with system title, live connection status pill, and interactive Light/Dark mode toggle button (`data-theme="light"` / `data-theme="dark"`).
   - Zone 1: Strategy & Directional Sharing (toggle switches for PC->Phone, Phone->PC, numeric history limit input 5–200, manual "Send Clipboard" / "Pull Remote Clipboard" buttons).
   - Zone 2: Hardware Interconnect (Host ID input, Touchpad NFC NTAG5332 bind/write button, active UID badge, file drop zone with dashed border).
   - Zone 3: Clipboard History Stream (counter badge, refresh, clear, sample text and image thumbnail history items with origin badges, selective paste button, item delete).
   - Responsive and fully self-contained (HTML5, inline CSS variables, inline vanilla JavaScript for toggling themes, switches, and actions).

4. **Testing & Verification**:
   - Verify the prototype loads cleanly without console errors or layout issues.
   - Run the automated verification tools documented in `explorer_ui_survey_3` (e.g. `python3` headless WebKit2 script or static inspection) to confirm 0 blurs and 0 green tokens.

## Mandatory Integrity Warning
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A teamwork_auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.

## Output
Write `/home/reid-pu/Programming/Linux_to_Android/hyperos_ui_prototype.html` and write your handoff report to `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/teamwork_worker_m1/handoff.md`.

## 2026-10-30T10:03:11Z
You are teamwork_worker_m1.
Your working directory is: /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/teamwork_worker_m1/
Please read your instructions at /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/teamwork_worker_m1/DISPATCH.md and the original user request at /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/ORIGINAL_REQUEST.md.

You are responsible for authoring /home/reid-pu/Programming/Linux_to_Android/hyperos_ui_prototype.html per Milestone M1-C4.
Implement an interactive single-page partitioned showcase prototype featuring clean solid matte card surfaces, subtle 1px border lines, soft light elevation shadows, unified Tech Blue palette (#2563EB / #3B82F6), zero green accents, tiered local vs remote device status, and interactive Light/Dark mode toggling and controls.
