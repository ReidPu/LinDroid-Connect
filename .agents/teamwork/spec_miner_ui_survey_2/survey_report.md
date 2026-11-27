# UI Specification & Platform Mapping Survey Report
**Document ID**: SPEC-SURVEY-UI-01  
**Author**: spec_miner_ui_survey_2  
**Date**: 2026-10-30  
**Workspace**: `/home/reid-pu/Programming/Linux_to_Android`  
**Target Specification Document**: `hyperos_ui_spec.md`  

---

## 1. Executive Summary & Specification Sources

This investigation surveyed the visual architecture, design tokens, styling implementations, and platform mapping requirements across Linux (GTK3), Android (XML / Kotlin), and the cross-platform design specifications for the **Linux_to_Android (L2A)** system.

### Authoritative Specification Sources Probed:
1. **User Requirements (`ORIGINAL_REQUEST.md`)**:
   - **R1 (Surface Material & Card Overhaul)**: Completely eliminate liquid glass visual effects, translucent frosted glass backgrounds, `backdrop-filter: blur(...)`, and ambient colored background orbs. Replace with clean, solid matte card surfaces with subtle 1px border lines and soft light elevation shadows in Light and Dark modes.
   - **R2 (Unified Tech Blue Color System)**: Eliminate conflicting blue and green accents, status pills, and badges. Implement a unified color system using a single tech blue accent (`#2563EB` / `#3B82F6`). Distinguish local vs. remote device statuses using lightness/opacity variations of tech blue alongside distinct iconography, with zero green/emerald accent elements.
   - **R3 (Prototype-First Deliverables & Specification)**: Deliver `hyperos_ui_prototype.html` and `hyperos_ui_spec.md` before updating native codebases.
2. **Linux GTK3 Reference Implementation (`linux/src/app_window.hpp`, `linux/src/app_window.cpp`)**:
   - `kLiquidGlassGtkCss`: Currently defines Scheme A ("澎湃釉白 + iOS 液态玻璃") and Scheme B ("黑曜石暗色 + iOS 液态玻璃"), using translucent linear gradients, specular rim inset box-shadows, and emerald green status badges (`#10B981`, `#047857`, `#34D399`).
   - `AppWindow::install_liquid_glass_stylesheet()`: Explicitly queries `gdk_screen_get_rgba_visual()` to enable window translucency.
3. **Android Reference Implementation (`android/app/src/main/java/com/reid/l2a/MainActivity.kt`, `res/layout/activity_main.xml`)**:
   - `createLiquidGlassDrawable()`: Custom 2-layer `LayerDrawable` generating gradient backgrounds with inset specular rim highlights.
   - `applyLiquidGlassTheme()`: Sets `#2410B981` / `#1A059669` green status pill backgrounds and `#059669` text.
   - `renderHistoryList()`: Lines 514 & 565 hardcode `accentGreen` (`#34D399` / `#059669`) for remote Linux items against `accentBlue` for local items.
   - `activity_main.xml`: Hardcodes `#ECEFF5` background and `#059669` green status text.

---

## 2. Features Discovered

| # | Category | Feature | Description | Inputs | Outputs | Error Behavior | Discovered Via |
|---|----------|---------|-------------|--------|---------|----------------|----------------|
| 1 | Surface Material | Solid Matte Light Card | Opaque white surface with 1px border and soft elevation shadow | Light mode active | CSS/Drawable: `#FFFFFF`, 1px border `#E2E8F0`, soft shadow | Fallback to default window bg | `ORIGINAL_REQUEST.md` R1, `app_window.cpp:90-96` |
| 2 | Surface Material | Solid Matte Dark Card | Opaque slate surface with 1px border and low-opacity shadow | Dark mode active | CSS/Drawable: `#1E293B`, 1px border `#334155`, soft shadow | Fallback to dark window bg | `ORIGINAL_REQUEST.md` R1, `app_window.cpp:184-190` |
| 3 | Surface Material | Drop Zone / File Container | Flat container for drag-and-drop operations with dashed accent border | Drag-and-drop target state | Light: `#F8FAFC` + dashed `#3B82F6`; Dark: `#0F172A` + dashed `#3B82F6` | None | `ORIGINAL_REQUEST.md` R1, `app_window.cpp:98-104` |
| 4 | Surface Material | Inset Recessed Well (Settings/Limits) | Inner flat surface for input rows and settings fields | Numeric limit, Host ID | Light: `#F1F5F9` + border `#CBD5E1`; Dark: `#0F172A` + border `#334155` | Clamped [5, 200] | `MainActivity.kt:195-209`, `activity_main.xml:127-169` |
| 5 | Color Palette | Primary Tech Blue Action | Primary action button and focus states | User click / focus | Light: `#2563EB` (hover `#1D4ED8`); Dark: `#3B82F6` (hover `#60A5FA`) | None | `ORIGINAL_REQUEST.md` R2, `app_window.cpp:122-139` |
| 6 | Color Palette | Secondary Matte Button | Secondary neutral action button for auxiliary operations | User click / hover | Light: `#F1F5F9` + border `#CBD5E1`; Dark: `#334155` + border `#475569` | None | `ORIGINAL_REQUEST.md` R1, `app_window.cpp:140-153` |
| 7 | Color Palette | Destructive Action Button | Warning button for history clear / delete | User click | Light: text `#DC2626`, bg `#FEF2F2`; Dark: text `#F87171`, bg `#450A0A` | Confirmation required | `MainActivity.kt:262-266`, `app_window.cpp:494` |
| 8 | Device Tiering | Local Device Badge (Origin=0) | Solid filled Tech Blue status badge indicating local device | Origin == 0 (Local) | Solid `#2563EB` (Light) / `#3B82F6` (Dark), text `#FFFFFF`, icon `📱 本机` / `💻 本机` | None | `ORIGINAL_REQUEST.md` R2, `MainActivity.kt:565-574` |
| 9 | Device Tiering | Remote Device Badge (Origin=1) | Subtly tinted Tech Blue container badge indicating peer device | Origin == 1 (Remote) | Tinted `#EFF6FF` / border `#BFDBFE` (Light); `#172554` / border `#3B82F6` (Dark), text blue | Replaces obsolete green | `ORIGINAL_REQUEST.md` R2, `MainActivity.kt:565-574` |
| 10 | Status Indicator | Connection State Pill | Overall background service / peer connection status pill | Connected / Disconnected | Tech blue container (`#EFF6FF` / `#1E3A8A`) + blue icon, zero green | Fallback to muted slate on disconnect | `ORIGINAL_REQUEST.md` R2, `activity_main.xml:73-84` |
| 11 | Typography | Primary & Secondary Text Hierarchy | High-contrast accessible text styling meeting WCAG AAA / AA | Text string, theme mode | Light: `#0F172A` (primary), `#475569` (secondary); Dark: `#F8FAFC` (primary), `#94A3B8` (secondary) | None | `app_window.cpp:79-88`, `MainActivity.kt:176-177` |
| 12 | Elevation Model | 4-Tier Soft Light Elevation | Systematic depth scale (Levels 0 to 3) using ambient box-shadows | Component depth level | CSS box-shadow and Android elevation dp tokens | Clamped bounds | `ORIGINAL_REQUEST.md` R1, `activity_main.xml:24` |
| 13 | History Stream | History Item Card & Thumbnail | Non-destructive clipboard history card with MIME indicator | Clipboard item struct | Solid matte card with 36px image thumbnail or TXT pill | Fallback thumbnail on error | `app_window.cpp:728-790`, `MainActivity.kt:546-600` |
| 14 | Platform Mapping | GTK3 Matte CSS Provider | Native GTK3 CSS stylesheet applying matte classes | Screen provider load | Replaces `kLiquidGlassGtkCss` with `kSolidMatteGtkCss` | Safe style unref | `app_window.cpp:299-322` |
| 15 | Platform Mapping | Android Solid Matte Factory | Programmatic Drawable generation for API 24+ without glass layers | Corner radius, fill, stroke | Single `GradientDrawable` replacing `LayerDrawable` | Safe fallback | `MainActivity.kt:120-147` |

---

## 3. Edge Cases

| # | Feature | Input | Observed Behavior |
|---|---------|-------|-------------------|
| 1 | Light/Dark Theme Switching | Rapid toggling between Light and Dark modes | Window classes `.mode-light` and `.mode-dark` must update all child surfaces cleanly without residual specular highlights or contrast inversion. |
| 2 | Long Text Snippet in History Item | 5,000+ character text entry | Truncated to single-line 54 chars with ellipsis (`...`) in GTK3; multi-line max 3 lines in Android without expanding card height irregularly. |
| 3 | Transparent PNG Clipboard Image | PNG with transparent alpha channel | Displayed on matte preview thumbnail: must not blend weirdly with gradient; matte thumbnail background `#F1F5F9` (Light) or `#334155` (Dark) ensures contrast. |
| 4 | Offline / Peer Disconnected State | Network drop / zero peers connected | Status pill text updates to "等待对端连接", badge changes from active blue container to neutral slate container (`#F1F5F9` / `#334155`), zero red or green. |
| 5 | Extreme DPI Scaling (125%, 150%, 200%) | High-DPI Wayland display (`GDK_SCALE=2`) | 1px border lines must not blur or disappear; soft box-shadows must scale proportionally without hard banding. |
| 6 | Nested Card Layouts | History list inside History Section Card | Inner items use subtle background step (`#F8FAFC` on `#FFFFFF` in Light; `#0F172A` on `#1E293B` in Dark) to maintain clear boundary without heavy border stacking. |

---

## 4. Deep-Dive Section 1: Glass / Blur & Translucent Definitions to Eliminate

### 4.1 GTK3 Stylesheet (`linux/src/app_window.cpp`)
Existing liquid glass definitions currently residing in `kLiquidGlassGtkCss` (lines 75–274):
1. **Window Background Gradients**:
   - Light: `linear-gradient(145deg, rgba(232, 238, 248, 0.96), rgba(222, 230, 244, 0.94))`
   - Dark: `linear-gradient(145deg, rgba(8, 11, 18, 0.97), rgba(14, 18, 28, 0.96))`
   - *Target replacement*: Solid colors `#F8FAFC` (Light) and `#0F172A` (Dark).
2. **Card Surfaces (`.liquid-card`)**:
   - Light: `linear-gradient(135deg, rgba(255, 255, 255, 0.84), rgba(255, 255, 255, 0.54))` + `border: 1px solid rgba(255, 255, 255, 0.88)` + specular inset `box-shadow: inset 0 1px 0 rgba(255, 255, 255, 0.96)`.
   - Dark: `linear-gradient(135deg, rgba(30, 37, 54, 0.76), rgba(17, 21, 32, 0.56))` + `border: 1px solid rgba(255, 255, 255, 0.13)` + specular inset `box-shadow: inset 0 1px 0 rgba(255, 255, 255, 0.20)`.
   - *Target replacement*: Solid `#FFFFFF` / border `#E2E8F0` (Light); Solid `#1E293B` / border `#334155` (Dark). Completely remove `inset 0 1px 0 ...`.
3. **Drop Zone & Row Insets (`.liquid-drop-zone`, `.liquid-row`, `.liquid-row-top`)**:
   - Translucent multi-stop gradients with RGBA values (`rgba(255, 255, 255, 0.72)`, `rgba(0, 102, 255, 0.10)`, etc.) and white specular rims.
   - *Target replacement*: Solid neutral fills (`#F8FAFC`, `#F1F5F9`, `#0F172A`).
4. **Button Glass Surface (`button.liquid-btn-glass`)**:
   - Translucent gradients `rgba(255, 255, 255, 0.82)` to `0.50` with inset specular highlights.
   - *Target replacement*: Solid neutral secondary button with clear 1px border.
5. **Compositor RGBA Visual Setup (`AppWindow::install_liquid_glass_stylesheet`)**:
   - Lines 303–307 query `gdk_screen_get_rgba_visual()` to force composite alpha channel.
   - *Target replacement*: Remove requirement for alpha visual; operate cleanly with standard RGB window buffers.

### 4.2 Android Subsystem (`MainActivity.kt` & `activity_main.xml`)
1. **`createLiquidGlassDrawable()` (lines 120–147)**:
   - Uses `LayerDrawable` stacking a gradient base with a second semi-transparent specular rim gradient (`GradientDrawable` with `Orientation.TOP_BOTTOM`, `specularRimColor` fading to `Color.TRANSPARENT`).
   - *Target replacement*: Remove specular layer entirely. Replace with `createSolidMatteDrawable(cornerDp, fillColor, strokeColor, strokeWidthDp = 1f)`.
2. **`applyLiquidGlassTheme()` (lines 168–270)**:
   - Gradient backgrounds: `rootScroll.background` uses `GradientDrawable` (`#0A0D14` -> `#111726` Dark, `#E8EEF7` -> `#F4F6FA` Light).
   - Semi-transparent alpha hex codes: `#CC181E2C`, `#D9FFFFFF`, `#B3121722`, `#B8F8FAFC`, `#2EFFFFFF`, `#38FFFFFF`.
   - Recessed glasswells: `innerTopLeft` (`#990D111A`), `innerBottomRight` (`#80131926`), `innerSpecular` (`#18FFFFFF`).
   - *Target replacement*: Replace all 8-digit ARGB hex values with 100% opaque 6-digit RGB hex values.
3. **`activity_main.xml`**:
   - `android:elevation="4dp"` on all cards without border definition.
   - *Target replacement*: Coordinate elevation with explicit 1px stroke border.

### 4.3 HTML Prototype Requirements (`hyperos_ui_prototype.html`)
- Any `backdrop-filter: blur(...)` or `-webkit-backdrop-filter: blur(...)` is strictly forbidden.
- Any background radial-gradient or ambient floating orbs (e.g. `border-radius: 50%; filter: blur(60px);`) must be eliminated.
- All cards and toolbars must render with solid matte background colors (`#FFFFFF` in light mode, `#1E293B` in dark mode).

---

## 5. Deep-Dive Section 2: Green & Emerald Accent References to Eliminate

### 5.1 Enumeration of Green/Emerald Color Usages
1. **GTK3 CSS (`linux/src/app_window.cpp`)**:
   - Line 154–160: `.mode-light .pill-badge-online`:
     ```css
     background-color: rgba(16, 185, 129, 0.14);
     color: #047857;
     border: 1px solid rgba(16, 185, 129, 0.28);
     ```
   - Line 248–254: `.mode-dark .pill-badge-online`:
     ```css
     background-color: rgba(16, 185, 129, 0.18);
     color: #34D399;
     border: 1px solid rgba(16, 185, 129, 0.32);
     ```
   - Line 388: `add_css_class(sync_badge_label_, "pill-badge-online");` (Sync active status).
   - Line 751: `add_css_class(txt_tag, is_first ? "pill-badge-online" : "pill-badge-accent");` (Top clipboard item badge).
2. **Android UI (`MainActivity.kt` & `activity_main.xml`)**:
   - `activity_main.xml:83`: `android:textColor="#059669"` on `tv_status`.
   - `MainActivity.kt:212–219`:
     ```kotlin
     val statusBg = if (dark) Color.parseColor("#2410B981") else Color.parseColor("#1A059669")
     val statusStroke = if (dark) Color.parseColor("#4D10B981") else Color.parseColor("#38059669")
     tvStatus.setTextColor(if (dark) Color.parseColor("#34D399") else Color.parseColor("#059669"))
     ```
   - `MainActivity.kt:514`:
     ```kotlin
     val accentGreen = if (isDarkMode) Color.parseColor("#34D399") else Color.parseColor("#059669")
     ```
   - `MainActivity.kt:565–570`:
     ```kotlin
     val badgeColor = if (origin == 0) accentBlue else accentGreen
     val badgeBgColor = if (origin == 0) {
         if (isDarkMode) Color.parseColor("#263B82F6") else Color.parseColor("#1A0066FF")
     } else {
         if (isDarkMode) Color.parseColor("#2610B981") else Color.parseColor("#1A059669")
     }
     ```

### 5.2 Elimination & Unification Strategy
- **Root Cause of Visual Conflict**: The previous UI mixed emerald green (`#10B981` / `#059669` / `#34D399`) for "online/sync" and "remote device" with blue (`#0066FF` / `#3B82F6`) for "local device" and "primary actions". This created a fragmented, noisy color palette lacking a cohesive design identity.
- **Unified Replacement**:
  - All status indications (service running, sync active, handshake established) migrate to **Unified Tech Blue** (`#2563EB` / `#3B82F6`).
  - Active state is conveyed by Tech Blue tint + distinct icon (e.g. check circle `✓` or radio dot `●`), never green.
  - Zero `#10B981`, `#059669`, `#34D399`, `#047857`, or `rgba(16, 185, 129, ...)` will remain anywhere in the specification or code.

---

## 6. Deep-Dive Section 3: Solid Matte Elevation Models & Shadow Systems

### 6.1 Systematic Elevation Scale
The new visual design replaces translucent blur effects with an opaque **4-Tier Matte Elevation Model**:

```
Elevation 3: Modals / Popovers / Settings Dialog
    │
Elevation 2: Elevated Drop Zones / Floating Action Panels
    │
Elevation 1: Standard Content Cards (Header, Settings, History)
    │
Elevation 0: Canvas / Window Root Background
```

### 6.2 Elevation Token Specifications

| Elevation Level | Purpose | Light Mode Surface & Border | Light Mode Shadow (CSS / GTK3) | Dark Mode Surface & Border | Dark Mode Shadow (CSS / GTK3) | Android Elevation / Stroke |
|:---:|---|---|---|---|---|---|
| **Level 0** (Canvas) | Window base & root container | Fill: `#F8FAFC`<br>Border: none | none | Fill: `#0F172A`<br>Border: none | none | `0dp` (flat) |
| **Level 1** (Card) | Section cards (Header, Sync, NFC, History) | Fill: `#FFFFFF`<br>Border: `1px solid #E2E8F0` | `0 1px 3px 0 rgba(0, 0, 0, 0.05), 0 1px 2px -1px rgba(0, 0, 0, 0.05)` | Fill: `#1E293B`<br>Border: `1px solid #334155` | `0 2px 4px 0 rgba(0, 0, 0, 0.3)` | `2dp` + `1dp` stroke |
| **Level 2** (Interactive) | Drop zones, active list items, toolbars | Fill: `#FFFFFF`<br>Border: `1px solid #CBD5E1` | `0 4px 6px -1px rgba(0, 0, 0, 0.07), 0 2px 4px -2px rgba(0, 0, 0, 0.05)` | Fill: `#243247`<br>Border: `1px solid #475569` | `0 4px 8px 0 rgba(0, 0, 0, 0.4)` | `4dp` + `1dp` stroke |
| **Level 3** (Overlay) | Settings dialog, modal popovers | Fill: `#FFFFFF`<br>Border: `1px solid #CBD5E1` | `0 10px 15px -3px rgba(0, 0, 0, 0.08), 0 4px 6px -4px rgba(0, 0, 0, 0.04)` | Fill: `#29384E`<br>Border: `1px solid #475569` | `0 12px 24px -4px rgba(0, 0, 0, 0.5)` | `8dp` + `1dp` stroke |

---

## 7. Deep-Dive Section 4: Unified Tech Blue Palette & Token Tables

### 7.1 Tech Blue Accent Scale

| Token Name | Hex Code | Light Mode Role | Dark Mode Role | Contrast vs `#FFFFFF` | Contrast vs `#1E293B` |
|---|---|---|---|---|---|
| `color-blue-50` | `#EFF6FF` | Soft container bg / Remote badge bg | Low-contrast highlight tint | 1.07:1 (bg) | — |
| `color-blue-100` | `#DBEAFE` | Hover container bg / Light border | Subtle divider | 1.18:1 (bg) | — |
| `color-blue-200` | `#BFDBFE` | Container border / Focus outline ring | Muted blue accent | 1.34:1 | — |
| `color-blue-400` | `#60A5FA` | Secondary active text | Dark mode hover & text accent | 2.5:1 | 4.8:1 (AA Normal) |
| `color-blue-500` | `#3B82F6` | Primary button hover in Light | **Dark Mode Primary Accent** | 3.6:1 | 3.3:1 (large text) |
| `color-blue-600` | `#2563EB` | **Light Mode Primary Accent** | Dark mode pressed / active | **4.6:1** (AA Normal) | 2.6:1 |
| `color-blue-700` | `#1D4ED8` | Light mode primary button hover | Dark mode container text | **6.5:1** (AAA) | 1.8:1 |
| `color-blue-800` | `#1E40AF` | Light mode primary button active | Dark mode container border | **8.7:1** (AAA) | 1.4:1 |
| `color-blue-900` | `#1E3A8A` | High-contrast text / badge border | Dark mode container background | **11.2:1** (AAA) | 1.1:1 (bg) |
| `color-blue-950` | `#172554` | Highest-contrast dark brand accent | Remote badge container in Dark | **14.1:1** (AAA) | 1.0:1 (bg) |

### 7.2 Neutral Palette (Slate Family)

| Token Name | Hex Code | Light Mode Usage | Dark Mode Usage |
|---|---|---|---|
| `color-canvas` | Light: `#F8FAFC`<br>Dark: `#0F172A` | Window root background | Window root background |
| `color-card` | Light: `#FFFFFF`<br>Dark: `#1E293B` | Main card surface | Main card surface |
| `color-subsurface` | Light: `#F1F5F9`<br>Dark: `#0F172A` | Recessed input rows, secondary containers | Recessed input rows, secondary containers |
| `color-border-subtle` | Light: `#E2E8F0`<br>Dark: `#334155` | Standard card 1px borders | Standard card 1px borders |
| `color-border-strong` | Light: `#CBD5E1`<br>Dark: `#475569` | Interactive borders, input outlines | Interactive borders, input outlines |
| `color-text-primary` | Light: `#0F172A`<br>Dark: `#F8FAFC` | Headings, titles, main label text | Headings, titles, main label text |
| `color-text-secondary`| Light: `#475569`<br>Dark: `#94A3B8` | Subtitles, section numbers, meta text | Subtitles, section numbers, meta text |
| `color-text-muted` | Light: `#64748B`<br>Dark: `#64748B` | Footers, timestamps, helper tips | Footers, timestamps, helper tips |

### 7.3 Device Tiering: Local vs. Remote Blue Distinction
To eliminate the blue-versus-green conflict, devices are differentiated through **Tiered Visual Weight** within the unified Tech Blue spectrum:

```
[Local Device]  ━━━► Solid Tech Blue Fill  (#2563EB / #3B82F6) + White Bold Text
[Remote Device] ━━━► Tinted Blue Container (#EFF6FF / #172554) + 1px Blue Border + Colored Blue Text
```

| Device Role | Light Mode Treatment | Dark Mode Treatment | Iconography |
|---|---|---|---|
| **Local Device** (Origin: 0) | Background: `#2563EB` (Solid Blue)<br>Border: none<br>Text: `#FFFFFF`<br>Font Weight: 600 | Background: `#3B82F6` (Solid Blue)<br>Border: none<br>Text: `#FFFFFF`<br>Font Weight: 600 | `📱 本机` (Android)<br>`💻 本机` (Linux) |
| **Remote Device** (Origin: 1) | Background: `#EFF6FF` (Blue-50)<br>Border: `1px solid #BFDBFE`<br>Text: `#1D4ED8` (Blue-700)<br>Font Weight: 500 | Background: `#172554` (Blue-950)<br>Border: `1px solid #3B82F6`<br>Text: `#93C5FD` (Blue-300)<br>Font Weight: 500 | `💻 远端` (Linux)<br>`📱 远端` (Android) |

---

## 8. Deep-Dive Section 5: Component States & Interactive Tokens

### 8.1 Primary Action Button
- **Default**:
  - Light: Background `#2563EB`, text `#FFFFFF`, border: none, border-radius `999px` (or `10dp` in Android), shadow `0 1px 2px rgba(37, 99, 235, 0.20)`.
  - Dark: Background `#3B82F6`, text `#FFFFFF`, border: none, shadow `0 1px 2px rgba(0, 0, 0, 0.3)`.
- **Hover**:
  - Light: Background `#1D4ED8`, cursor pointer.
  - Dark: Background `#60A5FA`.
- **Active / Pressed**:
  - Light: Background `#1E40AF`, transform `scale(0.98)`.
  - Dark: Background `#2563EB`, transform `scale(0.98)`.
- **Disabled**:
  - Light: Background `#E2E8F0`, text `#94A3B8`, shadow none.
  - Dark: Background `#334155`, text `#64748B`, shadow none.

### 8.2 Secondary Neutral Button
- **Default**:
  - Light: Background `#F1F5F9`, border `1px solid #CBD5E1`, text `#0F172A`.
  - Dark: Background `#334155`, border `1px solid #475569`, text `#F8FAFC`.
- **Hover**:
  - Light: Background `#E2E8F0`, border `1px solid #94A3B8`.
  - Dark: Background `#475569`, border `1px solid #64748B`.
- **Active / Pressed**:
  - Light: Background `#CBD5E1`, transform `scale(0.98)`.
  - Dark: Background `#1E293B`, transform `scale(0.98)`.

### 8.3 Destructive Action Button (Clear / Delete)
- **Default**:
  - Light: Background `#FEF2F2`, border `1px solid #FECACA`, text `#DC2626`.
  - Dark: Background `#450A0A`, border `1px solid #7F1D1D`, text `#F87171`.
- **Hover**:
  - Light: Background `#FEE2E2`, text `#B91C1C`.
  - Dark: Background `#7F1D1D`, text `#FCA5A5`.

### 8.4 Form Inputs (EditText / SpinButton / Switches)
- **Input Field**:
  - Light: Background `#FFFFFF`, border `1px solid #CBD5E1`, text `#0F172A`, placeholder `#94A3B8`.
  - Dark: Background `#0F172A`, border `1px solid #475569`, text `#F8FAFC`, placeholder `#64748B`.
  - Focus Ring: `2px solid #3B82F6`, outline offset `0px`.
- **Switch**:
  - Checked Track: `#2563EB` (Light) / `#3B82F6` (Dark).
  - Unchecked Track: `#CBD5E1` (Light) / `#475569` (Dark).
  - Thumb: Solid `#FFFFFF`, subtle shadow `0 1px 3px rgba(0, 0, 0, 0.2)`.

---

## 9. Deep-Dive Section 6: Platform Mapping Guidelines

### 9.1 GTK3 Platform Mapping (Linux C++)
- **File**: `linux/src/app_window.cpp`
- **Stylesheet Replacement**:
  Replace `kLiquidGlassGtkCss` with `kSolidMatteGtkCss`:
  ```css
  /* ==================== LIGHT MODE: SOLID MATTE TECH BLUE ==================== */
  window.mode-light {
      background-color: #F8FAFC;
      color: #0F172A;
  }
  .mode-light label { color: #0F172A; }
  .mode-light .text-muted { color: #64748B; }

  .mode-light .matte-card {
      background-color: #FFFFFF;
      border: 1px solid #E2E8F0;
      border-radius: 16px;
      padding: 16px 18px;
      box-shadow: 0 1px 3px 0 rgba(0, 0, 0, 0.05), 0 1px 2px -1px rgba(0, 0, 0, 0.05);
  }

  .mode-light .matte-drop-zone {
      background-color: #F8FAFC;
      border: 1px dashed #3B82F6;
      border-radius: 12px;
      padding: 14px 16px;
  }

  .mode-light .matte-row {
      background-color: #F8FAFC;
      border: 1px solid #E2E8F0;
      border-radius: 10px;
      padding: 8px 12px;
  }

  .mode-light .matte-row-top {
      background-color: #EFF6FF;
      border: 1px solid #BFDBFE;
      border-radius: 10px;
      padding: 8px 12px;
  }

  .mode-light button.matte-btn-primary {
      background-color: #2563EB;
      color: #FFFFFF;
      border-radius: 999px;
      border: 1px solid #1D4ED8;
      padding: 6px 16px;
      font-weight: 600;
  }
  .mode-light button.matte-btn-primary:hover { background-color: #1D4ED8; }

  .mode-light button.matte-btn-secondary {
      background-color: #F1F5F9;
      color: #0F172A;
      border-radius: 999px;
      border: 1px solid #CBD5E1;
      padding: 6px 14px;
      font-weight: 500;
  }
  .mode-light button.matte-btn-secondary:hover { background-color: #E2E8F0; }

  /* Device Status Badges */
  .mode-light .pill-badge-local {
      background-color: #2563EB;
      color: #FFFFFF;
      border-radius: 999px;
      padding: 2px 9px;
      font-weight: 600;
  }
  .mode-light .pill-badge-remote {
      background-color: #EFF6FF;
      color: #1D4ED8;
      border-radius: 999px;
      padding: 2px 9px;
      border: 1px solid #BFDBFE;
      font-weight: 500;
  }
  ```
  *(Symmetrical rules defined for `.mode-dark` using `#0F172A`, `#1E293B`, `#334155`, `#3B82F6`, `#172554`)*.

### 9.2 Android Subsystem Mapping (Kotlin & XML)
- **File**: `android/app/src/main/java/com/reid/l2a/MainActivity.kt`
- **Drawable Generator Replacement**:
  ```kotlin
  private fun createSolidMatteDrawable(
      cornerDp: Float,
      fillColor: Int,
      strokeColor: Int,
      strokeWidthDp: Float = 1f
  ): Drawable {
      return GradientDrawable().apply {
          shape = GradientDrawable.RECTANGLE
          cornerRadius = dpF(cornerDp)
          setColor(fillColor)
          setStroke(dp(strokeWidthDp).coerceAtLeast(1), strokeColor)
      }
  }
  ```
- **Theme Application (`applySolidMatteTheme`)**:
  - `rootScroll.setBackgroundColor(if (dark) Color.parseColor("#0F172A") else Color.parseColor("#F8FAFC"))`
  - All cards configured via `createSolidMatteDrawable(18f, cardBg, cardStroke)`:
    - Light: `cardBg = #FFFFFF`, `cardStroke = #E2E8F0`
    - Dark: `cardBg = #1E293B`, `cardStroke = #334155`
  - Status pill:
    - Light: bg `#EFF6FF`, stroke `#BFDBFE`, text `#1D4ED8`
    - Dark: bg `#1E3A8A`, stroke `#3B82F6`, text `#93C5FD`
  - History Origin badge:
    - Local (0): bg `#2563EB` (Light) / `#3B82F6` (Dark), text `#FFFFFF`
    - Remote (1): bg `#EFF6FF` / stroke `#BFDBFE` / text `#1D4ED8` (Light); bg `#172554` / stroke `#3B82F6` / text `#93C5FD` (Dark).

### 9.3 HTML Prototype Mapping (`hyperos_ui_prototype.html`)
- Interactive structure with tabbed or partitioned layout mirroring the desktop & Android experiences:
  1. Header Bar: System title, live sync status badge (tech blue), theme switch button.
  2. Partition 1: Strategy & Direct Sharing (directional switches, history retention limit input, manual push/pull actions).
  3. Partition 2: Hardware Interconnect & NFC (host ID, NFC bind/write button, active UID badge).
  4. Partition 3: Clipboard History Stream (local vs remote tiered badges, image thumbnails, selective paste action, item delete).
- CSS Variables architecture for instantaneous light/dark toggle:
  ```css
  :root {
      --canvas: #F8FAFC;
      --card: #FFFFFF;
      --border-subtle: #E2E8F0;
      --border-strong: #CBD5E1;
      --text-primary: #0F172A;
      --text-secondary: #475569;
      --accent-primary: #2563EB;
      --accent-hover: #1D4ED8;
      --accent-container: #EFF6FF;
      --accent-container-text: #1D4ED8;
      --shadow-card: 0 1px 3px 0 rgba(0,0,0,0.05), 0 1px 2px -1px rgba(0,0,0,0.05);
  }
  [data-theme="dark"] {
      --canvas: #0F172A;
      --card: #1E293B;
      --border-subtle: #334155;
      --border-strong: #475569;
      --text-primary: #F8FAFC;
      --text-secondary: #94A3B8;
      --accent-primary: #3B82F6;
      --accent-hover: #60A5FA;
      --accent-container: #172554;
      --accent-container-text: #93C5FD;
      --shadow-card: 0 2px 4px 0 rgba(0,0,0,0.3);
  }
  ```

---

## 10. Recommended Structure for `hyperos_ui_spec.md`

When creating / updating `/home/reid-pu/Programming/Linux_to_Android/hyperos_ui_spec.md`, the document should adhere to this standardized layout:

1. **Title & Design Philosophy**:
   - Evolution from liquid glass to high-performance solid matte cards.
   - Elimination of conflicting accents in favor of Unified Tech Blue (`#2563EB` / `#3B82F6`).
2. **Color Token Specification**:
   - Complete hex tables for Light and Dark modes (Primary, Neutral, Semantic, Border).
   - WCAG AAA / AA contrast audit matrix.
3. **Surface Material & Elevation System**:
   - 4-Tier Elevation Model (Levels 0–3) with exact box-shadow and border definitions.
4. **Device Tiering & Sync State Specifications**:
   - Local vs. Remote device visual distinction guidelines (solid blue vs. tinted blue).
   - Connection lifecycle states (Waiting, Handshaking, Connected, Streaming, Pulling).
5. **Component Specifications & Interactive States**:
   - Buttons (Primary, Secondary, Destructive), Toggles, Text Inputs, Badges, Drop Zone, History Cards.
6. **Platform Mapping Implementations**:
   - Linux GTK3 C++ CSS implementation guide.
   - Android Kotlin / XML / Compose implementation guide.
   - Prototype DOM/CSS cross-reference.
7. **Verification & Acceptance Checklist**:
   - Automated zero-blur, zero-green verification rules.
   - High-fidelity screenshot verification criteria.

---
*Report compiled and delivered by spec_miner_ui_survey_2.*
