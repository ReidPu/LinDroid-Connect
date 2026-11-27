# Comprehensive UI Architecture & Visual Style Survey Report

**Author**: `explorer_ui_survey_1`  
**Date**: 2026-10-30  
**Target Workspace**: `/home/reid-pu/Programming/Linux_to_Android`  
**Reference Mandates**: `ORIGINAL_REQUEST.md` (R1: Surface Material & Card Overhaul, R2: Unified Tech Blue Color System, R3: Prototype-First Deliverables and Specification)

---

## 1. Executive Summary

This investigation provides an exhaustive visual styling, color token, surface elevation, and device state survey across the Linux_to_Android workspace. The primary objective is to lay the complete empirical foundation for replacing legacy "Liquid Glass" (Apple iOS/HyperOS iridescent frosted glass, specular highlights, refraction rims, multi-stop gradients, ambient blurred orbs, and conflicting emerald green status indicators) with **solid matte card surfaces**, **subtle 1px border lines**, **soft light elevation shadows**, and a **unified Tech Blue palette (`#2563EB` / `#3B82F6`) with zero green accents**.

### Core Findings Summary
1. **Target Deliverable Status**: Neither `hyperos_ui_prototype.html` nor `hyperos_ui_spec.md` currently exists in the workspace. They represent the new R3 prototype-first deliverables that must be authored as the central visual showcase prior to native code updates.
2. **Existing Implementation Footprint**: The visual language is currently implemented across two synchronized native subsystems:
   - **Linux GTK3 Desktop Client**: `linux/src/app_window.cpp` (specifically the 200-line embedded CSS string `kLiquidGlassGtkCss` [lines 75–274] and widget styling in `init_ui()` [lines 360–540], `refresh_history_ui()` [lines 720–800], and `flush_ui_events()` [lines 1098–1107]).
   - **Android Kotlin Mobile Client**: `android/app/src/main/res/layout/activity_main.xml` (layout hierarchy and hardcoded colors) and `android/app/src/main/java/com/reid/l2a/MainActivity.kt` (custom multi-layer `createLiquidGlassDrawable` engine [lines 115–147], theme switching in `applyLiquidGlassTheme` [lines 164–270], and history row styling in `renderHistoryList` [lines 500–710]).
3. **Liquid Glass & Translucency Infestation**:
   - Both platforms simulate refractive glass through translucent gradient fills (alpha values from `0.03` to `0.96`), white specular top-rim insets (`inset 0 1px 0 rgba(255,255,255,...)`), and semi-transparent borders.
   - The Linux GTK window explicitly enables an RGBA visual context on X11/Wayland to support translucent window backgrounds (`app_window.cpp:303–307`).
   - Android constructs two-layer `LayerDrawable` stacks where Layer 1 is a 1dp top-to-bottom specular rim highlight fading to `Color.TRANSPARENT`.
4. **Conflicting Green/Emerald Tokens (Strict Violation of R2)**:
   - An entire secondary emerald/green palette (`#059669`, `#10B981`, `#34D399`, `#047857`, and rgba variants) is hardcoded to represent "Online", "Connected", "Running", and "Remote Linux Peer" statuses, directly conflicting with the primary blue palette.
   - Crucially, remote clipboard entries in Android are painted green (`#059669` / `#34D399`) while local entries are painted blue (`#0066FF` / `#60A5FA`). In GTK3, the top history item is painted green (`pill-badge-online`).
5. **Color System Fragmentation**:
   - The current UI uses over 12 disparate shades of blue (`#0066FF`, `#1E80FF`, `#005CE6`, `#0D74FF`, `#0055D4`, `#38BDF8`, `#3B82F6`, `#2563EB`, `#60A5FA`, `#93C5FD`, etc.).
   - R2 requires unifying this into a disciplined, single Tech Blue system centered on `#2563EB` (primary light / dark accent `#3B82F6`), differentiating local versus remote states solely via lightness/opacity tiers and iconography.

---

## 2. Inventory of UI Surface & Layout Architecture

The user interface is split into symmetric desktop and mobile experiences that share common functional zones.

### 2.1 Zone Architecture Comparison Table

| Zone / Component | Linux GTK3 Desktop (`linux/src/app_window.cpp`) | Android Kotlin (`MainActivity.kt` & `activity_main.xml`) | Visual Role & Content |
| :--- | :--- | :--- | :--- |
| **Window Canvas / Root** | `window.mode-light` / `window.mode-dark` (`app_window.cpp:77, 171`) | `ScrollView#root_scroll` (`activity_main.xml:2-8`) | Background wrapper with diagonal gradient |
| **Header Bar** | `header_card` (`.liquid-card`, lines 363–404) | `LinearLayout#card_header` (`activity_main.xml:18–84`) | Brand title, peer metadata, TLS 1.3 indicator, connection status pill, theme switch button, settings button |
| **Zone 1: Peer & Sync Controls** | `peer_card` (`.liquid-card`, lines 406–428) | `LinearLayout#card_settings` (`activity_main.xml:87–200`) | Directional auto-share switches (`pc_to_phone`, `phone_to_pc`), history limit spin/edit, manual push/pull buttons |
| **Zone 2: Hardware Interconnect** | Drop Zone in `drop_card` (`.liquid-card`, lines 430–473) | `LinearLayout#card_nfc` (`activity_main.xml:203–264`) | Linux: File/Folder drag-and-drop zone + 64KB chunk progress bar + ADB/NFC buttons.<br>Android: Touchpad NFC (NTAG5332) Host ID input + binding button + status + HyperOS guide. |
| **Zone 3: Clipboard Stream** | `hist_card` (`.liquid-card`, lines 480–511) | `LinearLayout#card_history` (`activity_main.xml:266–338`) | Counter badge (`0 / 20`), refresh button, clear button, scrolling list of history items (text / thumbnail, timestamp, origin badge, selective paste, delete) |
| **Footer / Diagnostics** | `footer_row` + `log_frame_` (lines 513–532) | Collapsible diagnostic logs in UI and persistent notification | One-line status ticker, log expander, headless XFixes / alert window status |

---

## 3. Catalog of Glassmorphism, Blur, and Translucency Effects

### 3.1 Linux GTK3 Subsystem (`linux/src/app_window.cpp`)

The Linux desktop client uses custom GTK CSS (`kLiquidGlassGtkCss`, lines 75–274) designed to imitate Apple iOS and Xiaomi HyperOS glazed liquid glass:

#### 1. Translucent Gradient Backgrounds on Cards & Zones
- **Light Mode `.mode-light .liquid-card` (Line 91)**:
  `background-image: linear-gradient(135deg, rgba(255, 255, 255, 0.84), rgba(255, 255, 255, 0.54));`
  *Defect*: Translucent white wash allowing window background gradient to bleed through.
- **Dark Mode `.mode-dark .liquid-card` (Line 185)**:
  `background-image: linear-gradient(135deg, rgba(30, 37, 54, 0.76), rgba(17, 21, 32, 0.56));`
  *Defect*: Semi-transparent dark slate gradient.
- **Drop Zone Light `.mode-light .liquid-drop-zone` (Line 99)**:
  `background-image: linear-gradient(135deg, rgba(255, 255, 255, 0.64), rgba(240, 246, 255, 0.42));`
- **Drop Zone Dark `.mode-dark .liquid-drop-zone` (Line 193)**:
  `background-image: linear-gradient(135deg, rgba(37, 99, 235, 0.14), rgba(255, 255, 255, 0.04));`
- **History Rows Light `.mode-light .liquid-row` (Line 107)**:
  `background-image: linear-gradient(135deg, rgba(255, 255, 255, 0.72), rgba(255, 255, 255, 0.44));`
- **History Rows Dark `.mode-dark .liquid-row` (Line 201)**:
  `background-image: linear-gradient(135deg, rgba(255, 255, 255, 0.07), rgba(255, 255, 255, 0.03));`
- **Top Row Accent Light `.mode-light .liquid-row-top` (Line 115)**:
  `background-image: linear-gradient(135deg, rgba(0, 102, 255, 0.10), rgba(255, 255, 255, 0.68));`
- **Top Row Accent Dark `.mode-dark .liquid-row-top` (Line 209)**:
  `background-image: linear-gradient(135deg, rgba(59, 130, 246, 0.20), rgba(255, 255, 255, 0.05));`

#### 2. Specular Top Highlight Rims & Multi-layer Box Shadows
- **Light Card (Line 95)**:
  `box-shadow: inset 0 1px 0 rgba(255, 255, 255, 0.96), 0 8px 24px rgba(15, 23, 42, 0.06);`
  *Defect*: Internal top 1px glow line mimicking light reflecting off the bevel of thick glass.
- **Dark Card (Line 189)**:
  `box-shadow: inset 0 1px 0 rgba(255, 255, 255, 0.20), 0 14px 32px rgba(0, 0, 0, 0.48);`
- **Buttons Glass Light (Line 147)**:
  `box-shadow: inset 0 1px 0 rgba(255, 255, 255, 0.92), 0 2px 6px rgba(15, 23, 42, 0.04);`
- **Buttons Glass Dark (Line 238)**:
  `box-shadow: inset 0 1px 0 rgba(255, 255, 255, 0.18);`

#### 3. Translucent Hairline Borders
- Light card: `border: 1px solid rgba(255, 255, 255, 0.88);` (Line 92)
- Dark card: `border: 1px solid rgba(255, 255, 255, 0.13);` (Line 186)
- Light buttons: `border: 1px solid rgba(255, 255, 255, 0.86);` (Line 144)
- Dark buttons: `border: 1px solid rgba(255, 255, 255, 0.15);` (Line 234)

---

### 3.2 Android Kotlin Subsystem (`MainActivity.kt`)

In Android, since native Views do not support CSS `backdrop-filter: blur()`, liquid glass was engineered manually using programmatically synthesized multi-layer drawables:

#### 1. Specular Refraction Engine: `createLiquidGlassDrawable` (Lines 120–147)
```kotlin
private fun createLiquidGlassDrawable(
    cornerDp: Float,
    topLeftColor: Int,
    bottomRightColor: Int,
    outerStrokeColor: Int,
    specularRimColor: Int
): Drawable {
    val radius = dpF(cornerDp)
    // Layer 0: Base translucent refractive gradient fill with outer hair-line border
    val base = GradientDrawable(
        GradientDrawable.Orientation.TL_BR,
        intArrayOf(topLeftColor, bottomRightColor)
    ).apply {
        cornerRadius = radius
        setStroke(dp(1f).coerceAtLeast(1), outerStrokeColor)
    }

    // Layer 1: Inner 1dp top specular highlight refraction rim fading to transparent
    val specular = GradientDrawable(
        GradientDrawable.Orientation.TOP_BOTTOM,
        intArrayOf(specularRimColor, Color.TRANSPARENT)
    ).apply {
        cornerRadius = (radius - dpF(1f)).coerceAtLeast(0f)
        setStroke(dp(1f).coerceAtLeast(1), specularRimColor)
    }

    return LayerDrawable(arrayOf(base, specular)).apply {
        setLayerInset(1, dp(1f), dp(1f), dp(1f), dp(1f))
    }
}
```
*Impact*: Every card, button, text input, and history row in Android renders an artificial specular glass rim.

#### 2. Translucent Color Arguments Passed in `applyLiquidGlassTheme`
- **Main Cards (Lines 180–183)**:
  - Dark: `cardTopLeft = #CC181E2C` (80% opacity), `cardBottomRight = #B3121722` (70% opacity), `cardOuterStroke = #2EFFFFFF` (18% white), `cardSpecular = #38FFFFFF` (22% white).
  - Light: `cardTopLeft = #D9FFFFFF` (85% opacity), `cardBottomRight = #B8F8FAFC` (72% opacity), `cardOuterStroke = #B3FFFFFF` (70% white), `cardSpecular = #CCFFFFFF` (80% white).
- **Recessed Inner Wells (Lines 196–199)**:
  - Dark: `innerTopLeft = #990D111A`, `innerBottomRight = #80131926`, `innerStroke = #22FFFFFF`, `innerSpecular = #18FFFFFF`.
  - Light: `innerTopLeft = #B8F1F5F9`, `innerBottomRight = #99E2E8F0`, `innerStroke = #180F172A`, `innerSpecular = #80FFFFFF`.
- **Secondary Buttons (Lines 251–254)**:
  - Dark: `secTop = #CC1E2638`, `secBottom = #B3161C2A`, `secStroke = #33FFFFFF`, `secSpecular = #2EFFFFFF`.
  - Light: `secTop = #E6FFFFFF`, `secBottom = #CCF1F5F9`, `secStroke = #240F172A`, `secSpecular = #CCFFFFFF`.
- **History Items (Lines 530–533)**:
  - Dark: `itemTop = #A6182030`, `itemBottom = #8C111724`, `itemStroke = #26FFFFFF`, `itemSpecular = #29FFFFFF`.
  - Light: `itemTop = #D9FFFFFF`, `itemBottom = #B8F1F5F9`, `itemStroke = #1A0F172A`, `itemSpecular = #CCFFFFFF`.

---

## 4. Catalog of Background Ambient Orbs and Gradients

### 4.1 Requirements Mandate (R1)
> *"Completely remove all liquid glass visual effects across the interface, including translucent frosted glass backgrounds, backdrop-filter blurs, and ambient colored background orbs. Replace them with clean, solid matte card surfaces featuring subtle 1px border lines and soft light elevation shadows in both Light and Dark modes."*

### 4.2 Current State in Existing Codebases
- **Linux Window Background**:
  - Light (`app_window.cpp:78`): `background-image: linear-gradient(145deg, rgba(232, 238, 248, 0.96), rgba(222, 230, 244, 0.94));`
  - Dark (`app_window.cpp:172`): `background-image: linear-gradient(145deg, rgba(8, 11, 18, 0.97), rgba(14, 18, 28, 0.96));`
- **Android Root Canvas Background**:
  - Light (`MainActivity.kt:169–174`): Diagonal `TL_BR` gradient from `#E8EEF7` to `#F4F6FA`.
  - Dark (`MainActivity.kt:169–174`): Diagonal `TL_BR` gradient from `#0A0D14` to `#111726`.
- **Prototype HTML Requirements**:
  In typical web implementations of the "HyperOS Liquid Glass" style, developers insert decorative blurred SVG or CSS circular orbs behind the glass containers, e.g.:
  ```html
  <!-- FORBIDDEN IN REDESIGN: Ambient blur orbs -->
  <div class="ambient-orb orb-1"></div>
  <div class="ambient-orb orb-2"></div>
  ```
  ```css
  /* FORBIDDEN IN REDESIGN: Ambient blur orbs and backdrop filter blurs */
  .ambient-orb {
      position: absolute;
      border-radius: 50%;
      filter: blur(80px);
      opacity: 0.45;
  }
  .glass-card {
      backdrop-filter: blur(20px);
      -webkit-backdrop-filter: blur(20px);
  }
  ```
  **Strict Redesign Rule**: When constructing `hyperos_ui_prototype.html`, all such ambient orbs, radial gradients, floating blur spheres, and `backdrop-filter: blur(...)` styles are **strictly banned**. The root background must be a clean, solid, neutral canvas (`#F8FAFC` in Light mode, `#0B0F19` or `#0F172A` in Dark mode), and all cards must use solid matte surfaces (`#FFFFFF` in Light mode, `#1E293B` in Dark mode).

---

## 5. Exhaustive Catalog of Color Tokens

### 5.1 Green / Emerald Tokens (Target of Complete Removal per R2)

The following table documents **every green/emerald token** currently used in the codebase. Every instance must be replaced with the unified Tech Blue system.

| Hex / RGB Code | RGB Decimal / RGBA | Exact File Path | Line Number | Visual Component & Role | Redesign Replacement Strategy |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `#059669` | `rgb(5, 150, 105)` | `android/app/src/main/res/layout/activity_main.xml` | 83 | `TextView#tv_status` default text color (green online message) | Replace with Tech Blue `#2563EB` |
| `#059669` | `rgb(5, 150, 105)` | `android/app/src/main/java/com/reid/l2a/MainActivity.kt` | 212 | Light mode `statusBg`: `Color.parseColor("#1A059669")` (10% green) | Replace with 8% Tech Blue `rgba(37, 99, 235, 0.08)` / `#142563EB` |
| `#059669` | `rgb(5, 150, 105)` | `android/app/src/main/java/com/reid/l2a/MainActivity.kt` | 213 | Light mode `statusStroke`: `Color.parseColor("#38059669")` (22% green) | Replace with 20% Tech Blue `rgba(37, 99, 235, 0.20)` / `#332563EB` |
| `#059669` | `rgb(5, 150, 105)` | `android/app/src/main/java/com/reid/l2a/MainActivity.kt` | 219 | Light mode `tvStatus` text color | Replace with solid Tech Blue `#2563EB` |
| `#059669` | `rgb(5, 150, 105)` | `android/app/src/main/java/com/reid/l2a/MainActivity.kt` | 514 | `val accentGreen = ... Color.parseColor("#059669")` | Delete variable; use Tech Blue remote tier |
| `#059669` | `rgb(5, 150, 105)` | `android/app/src/main/java/com/reid/l2a/MainActivity.kt` | 570 | Light mode remote history badge background: `Color.parseColor("#1A059669")` | Replace with subtle light blue container `#EFF6FF` or `rgba(37,99,235,0.08)` |
| `#10B981` | `rgb(16, 185, 129)` | `android/app/src/main/java/com/reid/l2a/MainActivity.kt` | 212 | Dark mode `statusBg`: `Color.parseColor("#2410B981")` (14% green) | Replace with 15% Tech Blue `rgba(59, 130, 246, 0.15)` / `#263B82F6` |
| `#10B981` | `rgb(16, 185, 129)` | `android/app/src/main/java/com/reid/l2a/MainActivity.kt` | 213 | Dark mode `statusStroke`: `Color.parseColor("#4D10B981")` (30% green) | Replace with 25% Tech Blue `rgba(59, 130, 246, 0.25)` / `#403B82F6` |
| `#10B981` | `rgb(16, 185, 129)` | `android/app/src/main/java/com/reid/l2a/MainActivity.kt` | 569 | Dark mode remote history badge background: `Color.parseColor("#2610B981")` | Replace with Dark mode remote blue container `rgba(59, 130, 246, 0.12)` |
| `#10b981` | `rgb(16, 185, 129)` | `linux/src/app_window.cpp` | 1100 | GTK Label markup: `<span foreground='#10b981'><b>已加密连接至 ...</b></span>` | Replace with Tech Blue `<span foreground='#2563EB'>` (Light) or `#3B82F6` (Dark) |
| `rgba(16, 185, 129, 0.14)` | `rgba(...)` | `linux/src/app_window.cpp` | 155 | `.mode-light .pill-badge-online` background | Replace with `rgba(37, 99, 235, 0.08)` |
| `#047857` | `rgb(4, 120, 87)` | `linux/src/app_window.cpp` | 156 | `.mode-light .pill-badge-online` text color | Replace with solid Tech Blue `#2563EB` |
| `rgba(16, 185, 129, 0.28)` | `rgba(...)` | `linux/src/app_window.cpp` | 159 | `.mode-light .pill-badge-online` border | Replace with `rgba(37, 99, 235, 0.20)` |
| `rgba(16, 185, 129, 0.18)` | `rgba(...)` | `linux/src/app_window.cpp` | 249 | `.mode-dark .pill-badge-online` background | Replace with `rgba(59, 130, 246, 0.15)` |
| `#34D399` | `rgb(52, 211, 153)` | `linux/src/app_window.cpp` | 250 | `.mode-dark .pill-badge-online` text color | Replace with solid Tech Blue `#3B82F6` |
| `rgba(16, 185, 129, 0.32)` | `rgba(...)` | `linux/src/app_window.cpp` | 253 | `.mode-dark .pill-badge-online` border | Replace with `rgba(59, 130, 246, 0.25)` |
| `#34D399` | `rgb(52, 211, 153)` | `android/app/src/main/java/com/reid/l2a/MainActivity.kt` | 219 | Dark mode `tvStatus` text color | Replace with Tech Blue `#3B82F6` |
| `#34D399` | `rgb(52, 211, 153)` | `android/app/src/main/java/com/reid/l2a/MainActivity.kt` | 514 | `val accentGreen = ... Color.parseColor("#34D399")` | Delete variable; use Tech Blue remote tier |

---

### 5.2 Legacy Blue & Secondary Tokens (To Be Unified)

The codebase currently exhibits severe fragmentation across blue and neutral tokens:

| Token | Current Usage Location | Description & Role | Unified Role in Redesign |
| :--- | :--- | :--- | :--- |
| `#0066FF` | `app_window.cpp:123, 270`, `activity_main.xml:253`, `MainActivity.kt:178, 237` | Legacy primary blue in Light mode | **Replace with Unified Tech Blue `#2563EB`** |
| `#1E80FF` | `app_window.cpp:123` | Gradient stop for primary button | Remove gradient; use solid `#2563EB` |
| `#005CE6` | `app_window.cpp:137` | Hover gradient stop for primary button | Use hover color `#1D4ED8` |
| `#0D74FF` | `app_window.cpp:137` | Hover gradient stop for primary button | Remove gradient; use solid `#1D4ED8` |
| `#0055D4` | `app_window.cpp:164` | Text color for `.pill-badge-accent` | Use `#2563EB` |
| `#38BDF8` | `app_window.cpp:270` | Sky blue gradient stop for progress bar | Use solid `#2563EB` |
| `#2563EB` | `app_window.cpp:217`, `MainActivity.kt:238` | Dark button gradient bottom stop | **Canonical Primary Tech Blue (Light Mode)** |
| `#3B82F6` | `app_window.cpp:217`, `MainActivity.kt:178, 237` | Dark mode accent / primary button top | **Canonical Primary Tech Blue (Dark Mode)** |
| `#60A5FA` | `app_window.cpp:258`, `MainActivity.kt:513` | Dark mode badge text / accent text | Secondary highlight / remote badge text in Dark mode |
| `#93C5FD` | `MainActivity.kt:239` (`#8093C5FD`, `#6693C5FD`) | Stroke colors for buttons | Subtle Tech Blue border |
| `#0D131F` | `app_window.cpp:79, 83, 142` | Light mode text primary | Standardize to `#0F172A` (Slate 900) |
| `#111318` | `activity_main.xml:46, 113`, `MainActivity.kt:176` | Android Light mode text primary | Standardize to `#0F172A` (Slate 900) |
| `#5E6B7E` | `app_window.cpp:87` | Light mode text secondary / muted | Standardize to `#64748B` (Slate 500) |
| `#5A6270` | `activity_main.xml:55, 104`, `MainActivity.kt:177` | Android Light mode text secondary | Standardize to `#64748B` (Slate 500) |
| `#F4F7FB` | `app_window.cpp:173, 177, 232` | Dark mode text primary | Standardize to `#F8FAFC` (Slate 50) |
| `#F3F5F8` | `MainActivity.kt:176` | Android Dark mode text primary | Standardize to `#F8FAFC` (Slate 50) |
| `#8E9BAE` | `app_window.cpp:181` | Dark mode text secondary / muted | Standardize to `#94A3B8` (Slate 400) |
| `#94A0B8` | `MainActivity.kt:177` | Android Dark mode text secondary | Standardize to `#94A3B8` (Slate 400) |

---

### 5.3 Unified Tech Blue Color Specification (R2 Target)

Per R2, all visual indicators are unified under the Tech Blue family:

```
/* ==========================================================================
   UNIFIED TECH BLUE SYSTEM (ZERO GREEN ACCENTS)
   ========================================================================== */
--tech-blue-50:  #EFF6FF;   /* Subtle tinted container in Light mode */
--tech-blue-100: #DBEAFE;   /* Secondary badge container / Light mode hover */
--tech-blue-200: #BFDBFE;   /* Light mode subtle border */
--tech-blue-500: #3B82F6;   /* Canonical Primary Accent for DARK MODE */
--tech-blue-600: #2563EB;   /* Canonical Primary Accent for LIGHT MODE */
--tech-blue-700: #1D4ED8;   /* Primary hover state in Light mode */
--tech-blue-900: #1E3A8A;   /* Dark mode tinted container */
```

#### Tiered Local vs Remote Status Distinction:
- **Local Device Status (Host / Self)**:
  - Visual Treatment: **Solid Filled Blue Pill Badge**
  - Light Mode: Background `#2563EB`, Text `#FFFFFF`, Border none / 1px solid `#1D4ED8`. Icon: Laptop / PC icon.
  - Dark Mode: Background `#3B82F6`, Text `#FFFFFF`, Border none / 1px solid `#2563EB`.
- **Remote Device Status (Peer / Linked Phone)**:
  - Visual Treatment: **Subtle Tinted Blue Container Badge**
  - Light Mode: Background `#EFF6FF` (or `rgba(37, 99, 235, 0.08)`), Text `#1D4ED8`, Border 1px solid `#BFDBFE` (or `rgba(37, 99, 235, 0.20)`). Icon: Smartphone icon.
  - Dark Mode: Background `rgba(59, 130, 246, 0.12)`, Text `#93C5FD`, Border 1px solid `rgba(59, 130, 246, 0.25)`.
- **Connection State Badge ("在线 / 已连接")**:
  - Light Mode: Tinted blue container `rgba(37, 99, 235, 0.08)`, text `#2563EB`, border 1px solid `rgba(37, 99, 235, 0.20)`, with a pulsing or steady blue dot (`#2563EB`).
  - Dark Mode: Tinted blue container `rgba(59, 130, 246, 0.15)`, text `#60A5FA`, border 1px solid `rgba(59, 130, 246, 0.30)`, with a blue dot (`#3B82F6`).

---

## 6. Light Mode and Dark Mode Implementation Analysis

### 6.1 Desktop GTK3 Switching Mechanism
- **Trigger**: `theme_btn_` in `header_card` connects to `on_btn_toggle_theme` (`app_window.cpp:394, 1046`).
- **Mechanism**: Calls `toggle_theme_mode()` (`app_window.cpp:335–341`), toggling boolean `dark_mode_`, and calling `update_theme_classes()`.
- **DOM/CSS Classes**: Swaps CSS classes `mode-light` and `mode-dark` on `window_` (`gtk_style_context_remove_class` and `gtk_style_context_add_class`).
- **Defects in Current CSS**:
  - Relies on translucent RGBA visual contexts (`gdk_screen_get_rgba_visual`) to create semi-transparent glass cards.
  - Background gradients use irregular opacity stops (`rgba(232, 238, 248, 0.96)` and `rgba(8, 11, 18, 0.97)`), leading to color banding and compositor overhead.

### 6.2 Android Kotlin Switching Mechanism
- **Trigger**: `btnToggleTheme` in `activity_main.xml` (`MainActivity.kt:311–316`).
- **Mechanism**: Flips `isDarkMode`, saves to `SharedPreferences("l2a_prefs")`, invokes `applyLiquidGlassTheme(isDarkMode)`, and calls `renderHistoryList()`.
- **Defects in Current Implementation**:
  - Dynamically synthesizes dozens of `LayerDrawable` instances with gradient layers and 1dp specular stroke offsets.
  - Generates high GC churn during theme switching or list rendering.
  - In dark mode, card background alphas (`#CC181E2C` = 80%) cause dark theme contrast failures against nested rows.

### 6.3 Solid Matte Card Hierarchy Specification (R1 Target)

The solid matte redesign eliminates all translucency in favor of opaque, high-contrast surface layers:

| Layer Level | Light Mode Hex | Dark Mode Hex | Border Specification | Elevation / Shadow |
| :--- | :--- | :--- | :--- | :--- |
| **0. App Canvas / Background** | `#F8FAFC` (Slate 50) | `#0B0F19` (Deep Charcoal) | None | None |
| **1. Primary Surface / Card** | `#FFFFFF` (Pure White) | `#1E293B` (Slate 800) | 1px solid `#E2E8F0` (Light) / `#334155` (Dark) | `0 1px 3px 0 rgba(0,0,0,0.06), 0 1px 2px 0 rgba(0,0,0,0.04)` (Light)<br>`0 4px 6px -1px rgba(0,0,0,0.25)` (Dark) |
| **2. Recessed Well / Inset Container** | `#F1F5F9` (Slate 100) | `#0F172A` (Slate 900) | 1px solid `#E2E8F0` (Light) / `#1E293B` (Dark) | None (Flat / Recessed) |
| **3. Interactive Rows / Table Items** | `#FFFFFF` (Hover: `#F8FAFC`) | `#1E293B` (Hover: `#283548`) | 1px solid `#E2E8F0` (Light) / `#334155` (Dark) | `0 1px 2px 0 rgba(0,0,0,0.03)` |
| **4. Primary Buttons** | `#2563EB` (Hover: `#1D4ED8`) | `#3B82F6` (Hover: `#2563EB`) | 1px solid `#1D4ED8` (Light) / `#2563EB` (Dark) | `0 1px 2px 0 rgba(0,0,0,0.05)` |
| **5. Secondary Buttons** | `#FFFFFF` (Hover: `#F8FAFC`) | `#334155` (Hover: `#475569`) | 1px solid `#CBD5E1` (Light) / `#475569` (Dark) | `0 1px 2px 0 rgba(0,0,0,0.03)` |

---

## 7. Device Sync State Visual Representations

The interface tracks three orthogonal state dimensions: Connection Lifecycle, Data Streaming Progress, and Clipboard Item Provenance.

### 7.1 Connection Lifecycle States
1. **Disconnected / Listening**:
   - Status Text: `等待手机连接 · 本机 ID: l2a-XXXX · TCP/TLS 端口: 53321`
   - Pill Badge: `未连接 / 等待中`
   - Redesign Visual: Subtle neutral grey/blue badge (`#F1F5F9` light / `#1E293B` dark), text `#64748B` / `#94A3B8`, hollow circular outline dot.
2. **Connecting / TLS Handshake**:
   - Status Text: `正在建立 TLS 1.3 会话与 X25519 秘钥协商...`
   - Pill Badge: `连接中...`
   - Redesign Visual: Tech Blue badge with subtle spinner/pulsing dot.
3. **Connected / Encrypted Active Session**:
   - Status Text: `已加密连接至 Redmi G Pro 2024 (TLS 1.3 / ChaCha20-Poly1305)`
   - Pill Badge: `双向实时同步就绪`
   - Current Defect: Styled in Emerald Green (`#059669` / `#10B981` / `#34D399`).
   - Redesign Visual: Tech Blue container `rgba(37, 99, 235, 0.08)` / `#EFF6FF`, text `#2563EB`, border 1px solid `rgba(37, 99, 235, 0.20)` with steady solid blue dot (`#2563EB`).

### 7.2 File & Chunk Streaming States
- Progress Bar: Currently styled with a cyan/blue gradient (`linear-gradient(90deg, #0066FF, #38BDF8)` in GTK `app_window.cpp:270`).
- Redesign Visual: Clean solid Tech Blue track fill (`#2563EB` in Light, `#3B82F6` in Dark) with a neutral trough background (`#E2E8F0` in Light, `#334155` in Dark). Text shows exact throughput, e.g., `42.5 MB / 120.0 MB · 64KB 流式分块 · 18.2 MB/s`.

### 7.3 Clipboard Item Origin Disambiguation
- **Local Origin (电脑 / 本机 Android)**:
  - Current: Blue badge `#0066FF` / `#0055D4`.
  - Redesign: **Solid Tech Blue Pill** (`#2563EB` Light / `#3B82F6` Dark), white bold text, laptop/phone icon.
- **Remote Origin (手机 / 远端 Linux)**:
  - Current: **EMERALD GREEN BADGE** (`#059669` / `#10B981` / `#34D399` / `#047857`). **CRITICAL FLAW**.
  - Redesign: **Light Blue Tinted Container Pill** (`#EFF6FF` Light / `rgba(59, 130, 246, 0.12)` Dark), text `#1D4ED8` Light / `#93C5FD` Dark, 1px border `#BFDBFE` / `rgba(59, 130, 246, 0.25)`, remote device icon. Zero green elements!

---

## 8. Concrete Blueprint for Deliverables

### 8.1 Deliverable 1: `hyperos_ui_prototype.html`

The interactive HTML prototype must serve as the authoritative single-page partitioned visual showcase.

#### Architecture of the Prototype:
1. **Global Header & Mode Controller**:
   - Brand logo & title: `Linux_to_Android Continuity Showcase`.
   - Visual Style Switcher: Shows active design standard (Solid Matte Cards · Unified Tech Blue).
   - Theme Toggle: Interactive Light / Dark Mode switch button (toggles `data-theme="light"` / `data-theme="dark"` on `<html>`).
   - View Partition Controls: Tabs to view **Dual-Pane Side-by-Side (Desktop + Mobile)**, **Linux Desktop Workstation Only**, or **Android Mobile Only**.
   - Interactive State Simulator Bar: Buttons to simulate `Connected`, `Waiting`, `Transferring 64KB Batch`, and `Inject Remote Clipboard`.
2. **Left Partition: Linux Desktop Workstation (GTK3 Emulation)**:
   - Window Titlebar with window controls (close, minimize, maximize).
   - Header Card: Device identity, TLS 1.3 indicator, Tech Blue status pill, Theme toggle, Preferences button.
   - Zone 1 Card: Touchpad NFC status, "发送剪贴板" (Primary Tech Blue button), "拉取手机" (Secondary button).
   - Zone 2 Card: Solid drop zone with dashed 1px Tech Blue border (`#2563EB` / `#3B82F6`), file chooser buttons, and 64KB chunk streaming progress bar.
   - Zone 3 Card: Clipboard history header with counter badge (`4 / 20`), refresh/clear buttons, and item list displaying real text snippets and PNG image thumbnails.
   - Footer Bar: Diagnostic log drawer button and background listener status.
3. **Right Partition: Android HyperConnect Companion (Mobile Screen Emulation)**:
   - Mobile Device Shell: Smartphone bezel with rounded corners.
   - Status Bar: Time, Wi-Fi, 5G, Battery.
   - Header Card: App title, active connection pill (Tech Blue container with blue dot), Theme toggle button.
   - Zone 1 Card: Sync strategy toggle switches (PC to Phone, Phone to PC), history limit input and save button, quick manual push/pull buttons.
   - Zone 2 Card: Touchpad NFC (NTAG5332) Host ID input, "写入或绑定笔记本触控板 NFC 标签" button, bound UID status (Tech Blue).
   - Zone 3 Card: Dynamic clipboard history stream demonstrating solid matte item cards, tiered Tech Blue origin pills (Solid Blue for Local Android vs Tinted Blue for Remote Linux), and action buttons.
4. **Strict CSS Verification Guarantee**:
   - Zero occurrences of `backdrop-filter` or `-webkit-backdrop-filter`.
   - Zero occurrences of `filter: blur(...)`.
   - Zero occurrences of emerald/green hex tokens (`#059669`, `#10B981`, `#34D399`, `#047857`, etc.).
   - Zero ambient colored orbs or background radial gradients.
   - 100% solid matte cards with 1px border lines and soft light elevation shadows.

---

### 8.2 Deliverable 2: `hyperos_ui_spec.md`

The companion specification must document:
1. Design tokens: exact hex codes for Canvas, Card, Border, Text, and Unified Tech Blue (`#2563EB` / `#3B82F6`).
2. Typography scales and font weights for GTK3 (`Cantarell` / `Ubuntu` / `Noto Sans SC`) and Android (`Roboto` / `MiSans`).
3. Surface metrics: Card border-radius (Desktop: 16px cards, 10px buttons; Mobile: 20px cards, 12px buttons), 1px border weights, and box-shadow elevation models.
4. State transition matrices: Full truth tables for Disconnected, Connecting, Connected, Transferring, Local Clipboard, and Remote Clipboard.
5. Platform mapping guidelines: Concrete GTK3 CSS snippet and Android Kotlin drawable architecture for immediate downstream adoption.

---

### 8.3 Native Codebase Remediation Map

When native implementations are updated following prototype sign-off:

| File | Target Lines | Current Code / Defect | Remediation Action |
| :--- | :--- | :--- | :--- |
| `linux/src/app_window.cpp` | 75–274 | `kLiquidGlassGtkCss` with blurs, frosted translucent cards, specular insets, and green badges | Replace entire CSS block with `kSolidMatteGtkCss`: solid white/slate card backgrounds, 1px solid borders (`#E2E8F0` / `#334155`), soft box shadows, unified Tech Blue (`#2563EB` / `#3B82F6`), zero green tokens. |
| `linux/src/app_window.cpp` | 303–307 | `gdk_screen_get_rgba_visual` visual transparency setup | Remove RGBA visual requirement; standard RGB visual is sufficient for solid matte cards. |
| `linux/src/app_window.cpp` | 354 | Window title: `"... (Liquid Glass)"` | Update title to `"L2A Continuity — Linux ⇄ Android"`. |
| `linux/src/app_window.cpp` | 388 | `add_css_class(sync_badge_label_, "pill-badge-online");` | Update class to use Tech Blue online styling (`pill-badge-connected`). |
| `linux/src/app_window.cpp` | 751 | `is_first ? "pill-badge-online" : "pill-badge-accent"` | Remove green `pill-badge-online`; use solid Tech Blue badge for top item and tinted Tech Blue for standard items. |
| `linux/src/app_window.cpp` | 1100 | `<span foreground='#10b981'><b>已加密连接至 ...</b></span>` | Replace `#10b981` with `#2563EB` (Light) / `#3B82F6` (Dark). |
| `android/.../activity_main.xml` | 83 | `android:textColor="#059669"` on `tv_status` | Replace with `#2563EB`. |
| `android/.../MainActivity.kt` | 116–147 | `createLiquidGlassDrawable` with specular refraction rim | Replace with `createSolidMatteDrawable(cornerDp, fillColor, strokeColor, elevationDp)` returning clean single-layer `GradientDrawable`. |
| `android/.../MainActivity.kt` | 168–270 | `applyLiquidGlassTheme` with translucent alpha colors and green status pills | Replace with solid matte colors: canvas `#F8FAFC` / `#0B0F19`, cards `#FFFFFF` / `#1E293B`, status pill Tech Blue `#2563EB` / `#3B82F6`, button solids. |
| `android/.../MainActivity.kt` | 514, 565–570 | `accentGreen` and green remote badge (`#059669` / `#10B981` / `#34D399`) | Remove `accentGreen`; style remote badge with light blue tinted container (`#EFF6FF` / `rgba(59,130,246,0.12)`) and Tech Blue text. |

---

## 9. Conclusion

The current UI contains extensive liquid glass effects and pervasive conflicting emerald green tokens across both Linux GTK3 and Android Kotlin codebases. By cataloging every occurrence of translucent gradients, specular rims, and green hex values, this survey provides the exact specifications necessary to implement:
1. `hyperos_ui_prototype.html`: An interactive, single-page partitioned showcase featuring solid matte surfaces, subtle 1px borders, soft elevation shadows, and a unified Tech Blue palette with zero green elements.
2. `hyperos_ui_spec.md`: An exhaustive design and platform mapping document.
3. Subsequent native codebase refactoring for GTK3 and Android Kotlin.
