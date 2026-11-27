# Task Assignment: M2-C4 UI Design Specification & Platform Mappings

## Objective
Author the comprehensive design specification `/home/reid-pu/Programming/Linux_to_Android/hyperos_ui_spec.md` documenting design tokens, 4-tier elevation models, component states, and native platform mappings for GTK3 and Android.

## File Ownership
- You EXCLUSIVELY own: `/home/reid-pu/Programming/Linux_to_Android/hyperos_ui_spec.md`
- Do NOT modify any other project files.

## Required References
Read these files before starting:
- `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/ORIGINAL_REQUEST.md`
- `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/PROJECT.md`
- `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/spec_miner_ui_survey_2/survey_report.md`
- `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/explorer_ui_survey_1/survey_report.md`
- `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/explorer_ui_survey_3/survey_report.md`

## Concrete Deliverable Requirements
Author `hyperos_ui_spec.md` matching the structure recommended in Section 10 of `spec_miner_ui_survey_2/survey_report.md`:
1. **Title & Design Philosophy**:
   - Architectural transition from legacy simulated liquid glass (translucent gradient fills, specular rims, blurred orbs) to high-performance solid matte card surfaces.
   - Elimination of visual conflict: unified Tech Blue palette (`#2563EB` / `#3B82F6`) with zero green accents.
2. **Color Token Specification**:
   - Exact hex token tables for Light and Dark modes:
     * Primary Tech Blue Scale (`color-blue-50` through `color-blue-950`).
     * Neutral Slate Family (`color-canvas`, `color-card`, `color-subsurface`, `color-border-subtle`, `color-border-strong`, text hierarchy).
     * Semantic tokens (Destructive `#DC2626`/`#F87171`, Warning, Info).
   - WCAG AAA / AA contrast ratio validation matrix for all text and interactive elements against `#FFFFFF`, `#F8FAFC`, `#1E293B`, and `#0F172A`.
3. **Surface Material & 4-Tier Elevation System**:
   - Level 0 (Canvas): Base window background `#F8FAFC` (Light) / `#0F172A` (Dark), 0dp elevation.
   - Level 1 (Card): Main content cards `#FFFFFF` / `#1E293B`, 1px border (`#E2E8F0` / `#334155`), soft elevation shadow (`0 1px 3px 0 rgba(0,0,0,0.05), 0 1px 2px -1px rgba(0,0,0,0.05)` Light / `0 2px 4px 0 rgba(0,0,0,0.3)` Dark), 2dp Android elevation.
   - Level 2 (Interactive/Drop Zone): Active containers, drop zones (`1px dashed #3B82F6`), 4dp Android elevation.
   - Level 3 (Overlay/Modal): Settings dialog, popovers, 8dp Android elevation.
   - Explicit prohibition of `backdrop-filter: blur(...)` and specular highlight insets.
4. **Device Tiering & Sync State Specifications**:
   - Visual distinction between Local and Remote devices within the unified Tech Blue system:
     * Local device (Origin 0): Solid Tech Blue fill (`#2563EB` / `#3B82F6`), white bold text, distinct local icon (`💻 本机` / `📱 本机`).
     * Remote device (Origin 1): Subtle tinted Tech Blue container (`#EFF6FF` / border `#BFDBFE` Light; `#172554` / border `#3B82F6` Dark), blue text (`#1D4ED8` Light / `#93C5FD` Dark), remote icon (`💻 远端` / `📱 远端`).
   - Connection lifecycle states: Waiting, Handshaking, Connected, Streaming, Pulling, with exact visual treatments (all tech blue, zero green).
5. **Component Specifications & Interactive States**:
   - Buttons (Primary Tech Blue, Secondary Matte Neutral, Destructive Red) across Default, Hover, Active/Pressed, and Disabled states.
   - Switches, Form inputs (EditText, SpinButton), Badges, File Drop Zone, Clipboard History Cards (text & image thumbnail items, selective paste token).
6. **Platform Mapping Implementations**:
   - GTK3 C++ CSS: Complete ready-to-integrate `kSolidMatteGtkCss` stylesheet replacing `kLiquidGlassGtkCss` in `linux/src/app_window.cpp`.
   - Android Kotlin / XML: Complete `createSolidMatteDrawable` implementation replacing `createLiquidGlassDrawable` and `applySolidMatteTheme` in `MainActivity.kt`.
   - Cross-reference with `hyperos_ui_prototype.html` CSS variables and DOM classes.
7. **Verification & Acceptance Checklist**:
   - Exact criteria for automated zero-blur, zero-green verification and browser screenshot capture.

## Mandatory Integrity Warning
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A teamwork_auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.

## Output
Write `/home/reid-pu/Programming/Linux_to_Android/hyperos_ui_spec.md` and write your handoff report to `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/teamwork_worker_m2/handoff.md`.


## 2026-10-30T10:03:20Z
You are teamwork_worker_m2.
Your working directory is: /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/teamwork_worker_m2/
Please read your instructions at /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/teamwork_worker_m2/DISPATCH.md and the original user request at /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/ORIGINAL_REQUEST.md.

You are responsible for authoring /home/reid-pu/Programming/Linux_to_Android/hyperos_ui_spec.md per Milestone M2-C4.
Author the comprehensive UI design specification covering design philosophy, complete color token tables for Light and Dark modes, WCAG contrast verification matrix, 4-tier solid matte elevation model, tiered local vs remote device status treatments, component state matrices, and native platform mapping code for GTK3 C++ and Android Kotlin / XML.

MANDATORY INTEGRITY WARNING:
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A teamwork_auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.

Deliver your handoff report to /home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/teamwork_worker_m2/handoff.md and notify me via send_message when complete.
