# 🌗 Dimmer v1.0 — Selective GPU White Cutter & Screen Dimmer (Release Notes & Learning Crunch)

## <a id="toc"></a>📑 Table of Contents
- [Overview & Build Philosophy](#overview)
- [v1.0 Release Highlights](#release-highlights)
- [The Learning & Exploring Crunch (Step-by-Step Devlog)](#devlog)
  - [Phase 1: Ditching Basic Dimming for a D3D11 GPU Pipeline](#phase-1)
  - [Phase 2: Tuning the Selective White Cut Math](#phase-2)
  - [Phase 3: Ergonomic UI Compression & Custom Controls](#phase-3)
  - [Phase 4: The Visual Artifact Crunch (ClearType, Bubbles & Spikes)](#phase-4)
  - [Phase 5: The Breakthrough — Per-Window OS Theme Classification](#phase-5)
- [Presets, Controls & Usage](#usage)
- [Architecture & Performance Snapshot](#specs)
- [Final Notes](#conclusion)

---

## <a id="overview"></a>📖 Overview & Build Philosophy
**Dimmer** is a lightweight, standalone Windows 10/11 utility (C++20 / Win32 / Direct3D 11) built to solve a specific daily annoyance: staring at blinding `#FFFFFF` document canvases (like WPS Writer, Word, and PDFs) without ruining vivid colors, photos, or dark-mode apps sitting next to them.

Full transparency on how this was built: **this project was vibe-coded through iterative AI-assisted development.** I didn't memorize Direct3D 11 COM boilerplate or write HLSL shaders from scratch on a whiteboard. Instead, the real work was the **learning and exploring crunch**—defining the visual target, stress-testing every build against real apps, catching weird graphical artifacts by eye, figuring out *why* they happened, and steering each iteration until the app actually felt clean enough to ship.

<details>
  <summary>Click here to view Dimmer v1.0 UI</summary>

  <br>
  <img src="dimmer-ui.webp" alt="Dimmer v1.0 Main Interface">

</details>

---

## <a id="release-highlights"></a>🚀 v1.0 Release Highlights
- **Per-Pixel Variance-Weighted White Cut (`0–100%`):** Softens neutral white backgrounds (`#FFFFFF` → `#A0A0A0`, `#C0C0C0` → `#808080`) while leaving saturated colors (`#FF0000`, icons, media) virtually untouched.
- **Automatic Per-Window Dark-Mode Shield:** Reads exact OS window bounds via `DWMWA_EXTENDED_FRAME_BOUNDS` and classifies each window as Light or Dark on the GPU—so dark-mode apps (Notepad, VS Code, browser dark themes) are skipped automatically with zero shadow halos.
- **ClearType Text Halo Filtering:** Applies a 3×3 edge check inside Light-Mode windows so thin black fonts on dimmed white paper stay sharp instead of glowing.
- **Screenshot-Safe Overlay:** Uses `WDA_EXCLUDEFROMCAPTURE`, meaning `Win + Shift + S` and `PrtSc` always capture the original, unmodified brightness.
- **Zero-Bloat Footprint:** Standalone statically linked `Dimmer.exe` (~20–24 MB RAM, ~2–3% GPU only while sliding, ~0% idle when the screen is static or turned OFF).

---

## <a id="devlog"></a>🛠️ The Learning & Exploring Crunch (Step-by-Step Devlog)

### <a id="phase-1"></a>Phase 1: Ditching Basic Dimming for a D3D11 GPU Pipeline
Early dimming approaches (basic GDI overlays, Windows hardware gamma LUTs, and `Magnification.dll` 5×5 color matrices) hit hard walls fast:
- Hardware gamma tables collide with Windows Night Light and display drivers.
- Simple alpha overlays dim *everything* equally—making Dim + Desaturation look almost identical to a fake white cut.

**What changed:** Moved to a real-time **Direct3D 11 + DXGI Desktop Duplication** pipeline with an embedded HLSL pixel shader (`ps_5_0`). Every pixel's original `(R, G, B)` is evaluated directly on the GPU, rendered to a click-through `WS_EX_TRANSPARENT` overlay excluded from capture (`WDA_EXCLUDEFROMCAPTURE`) to prevent infinite feedback loops.

---

### <a id="phase-2"></a>Phase 2: Tuning the Selective White Cut Math
The goal of **White Cut** was never to wash colors into grey (that is already handled by the **Color** slider using ITU-R BT.709 luminance weights).

1. **First Iteration (`> 230` RGB Gate):** Cut only pixels where all three channels exceeded `230` down to `#909090`, capping non-white shifts at `5%`. It proved the concept—giving a forced dark-mode feel on white documents—but still felt abrupt around light UI borders.
2. **Final Iteration (Variance-Weighted `> 192` Curve):** Switched to measuring each pixel's RGB spread ($\text{spread} = \max(R,G,B) - \min(R,G,B)$):
   - **Neutral pixels (`spread <= 5%`):** Full White Cut weight. At `100%`, `#FFFFFF` drops to **`#A0A0A0`** and `#C0C0C0` (`192`) drops to **`#808080`**, with an exponential shadow roll-off below `192` to protect dark UI borders and real image midtones.
   - **Vivid colors (`spread >= 15%`):** Smoothly roll down to **0% weight**, keeping buttons, charts, and photos vibrant even at `White Cut = 100%`.

<details>
  <summary>Click here to view White Cut Comparison (0% vs 100%)</summary>

  <br>
  <img src="white-cut-comparison.webp" alt="White Cut Before and After">

</details>

---

### <a id="phase-3"></a>Phase 3: Ergonomic UI Compression & Custom Controls
Once the GPU pipeline sat comfortably around ~20–24 MB RAM, the focus shifted to daily usability:
- **Layout Compression:** Fixed a bottom-row clipping issue by auto-fitting window height to the final control row and compressing the old 1-3-2 button layout into a tight **3-2 grid**:
  - Row 1: `Low` | `Medium` | `High`
  - Row 2: `Save Preset` | Color-coded power toggle (**Red `Turn OFF`** when active, **Green `Turn ON`** when paused)
- **Safe Exit:** Removed the redundant `Quit` button; clicking the window `X` cleanly stops the GPU worker thread, destroys overlays, and exits.
- **Custom Sliders:** Replaced stock Win32 trackbars with 2× supersampled anti-aliased `-` [slider] `+` controls, centered titles, right-aligned percentages, and **1% step precision** on button clicks and mouse wheel scrolling.
- **Sparse Persistence:** `Dimmer.json` is never created unless a user customizes a preset slot, and deleting it restores the compiled defaults.

---

### <a id="phase-4"></a>Phase 4: The Visual Artifact Crunch (ClearType, Bubbles & Spikes)
This was the hardest—and most educational—part of the entire build. Every fix revealed a deeper quirk in how Windows renders pixels:

| Iteration Tested | Problem Observed | Root Cause Discovered |
| :--- | :--- | :--- |
| **Pure 1×1 Pixel Shader** | Thin black text on white pages looked fuzzy (*kabur*) with faint glowing halos. | Windows **RGB ClearType** paints orange/cyan subpixel fringes (`spread > 15%`) along letter edges. Our variance gate left those fringes bright while cutting the white paper underneath. |
| **3×3 Relative Contrast Filter** | Fixed light-mode text cleanly, **but** made thin white fonts in dark-mode apps (Notepad, Dimmer Guide) look patchy while browser dark mode stayed fine. | Chromium uses grayscale anti-aliasing; GDI apps use RGB ClearType. Worse, a 3×3 "bright next to dark" rule is symmetric—it couldn't tell **black text on white paper** apart from **white text on a dark background**. |
| **8-Point Ring (12×12 Span)** | Protected dark-mode text, **but** created bright uncut "bubbles" around dense black text and borders. | Sampling only 8 points 4–5px out caused tight black ink to pull the bright count down, punching bright holes into white pages. |
| **32×32 & 16×16 Tile Maps** | Cleaner than the 8-point ring, **but** left a smooth U-shaped bubble along the left, bottom, and right borders of floating windows. | Windows 11 draws a wide, soft **U-shaped drop shadow** on 3 sides of a window (not the top titlebar). Pixel-only tiles got confused in the grey shadow ramp. |
| **OS Window Boxes + H/V Rays** | Eliminated drop-shadow confusion, **but** created straight horizontal/vertical spikes and bubbles whenever black text aligned with a ray. | Per-pixel 6-tap cardinal rays break whenever dark ink sits directly in the crosshair path. |

<details>
  <summary>Click here to view Debugging Artifacts (ClearType & U-Shadow Bubbles)</summary>

  <br>
  <img src="debug-artifacts.webp" alt="Visual Artifacts During Shader Iteration">

</details>

---

### <a id="phase-5"></a>Phase 5: The Breakthrough — Per-Window OS Theme Classification
Instead of forcing every pixel to guess whether it belonged to a Light or Dark app using noisy rays or blurry tiles, the final architecture splits the work cleanly between Windows and the GPU:

1. **Exact 4-Corner Bounds (CPU, on frame updates):** Enumerates visible top-level windows in Z-order via `DWMWA_EXTENDED_FRAME_BOUNDS`—giving exact 1-pixel window edges with **Windows 11 drop shadows excluded**.
2. **Pass 1 — Tiny `16×1` Window Classifier (GPU):** Samples an 8×8 unoccluded grid inside each window's rectangle *once* (32 bytes of VRAM). Windows with `< 25%` bright neutral area are flagged **Dark (`0.0`)**; light documents like WPS Writer and PDFs are flagged **Light (`1.0`)**.
3. **Pass 2 — Clean Fullscreen Shader (GPU):**
   - **Dark-mode windows:** White Cut is bypassed completely (thin white fonts in Notepad / Dimmer Guide stay 100% sharp).
   - **Light-mode windows:** Uniform 1×1 Variance-Weighted White Cut + 3×3 ClearType text filtering across the entire window—**zero bubbles, zero spikes, zero shadow halos.**

[⬆️ Back to Table of Contents](#toc)

---

## <a id="usage"></a>🎛️ Presets, Controls & Usage

### Built-In Default Presets
| Preset | Dim | White Cut | Color | Best For |
| :--- | :---: | :---: | :---: | :--- |
| **Low** | `30%` | `0%` | `100%` | Mild ambient dimming without altering white balance |
| **Medium** | `30%` | `60%` | `100%` | Balanced reading & editing on light-mode documents |
| **High** | `60%` | `100%` | `100%` | Late-night forced dark-mode feel on bright pages |

### Quick Actions
- **Sliders:** Drag the thumb, click `-` / `+`, or scroll the mouse wheel for `1%` steps.
- **Customize Presets:** Right-click any preset button (or click **Save Preset**) to overwrite or reset a slot into `Dimmer.json`.
- **Pause / Resume:** Click **Turn OFF** (Red) to hide the overlay immediately (`0%` GPU); click **Turn ON** (Green) to resume.
- **Exit:** Close the window via **`X`** for safe teardown.

---

## <a id="specs"></a>📊 Architecture & Performance Snapshot
- **Language & APIs:** C++20, Win32, Direct3D 11, DXGI 1.2 Desktop Duplication, DWM API, HLSL (`vs_5_0` / `ps_5_0`).
- **Build System:** CMake (`Release` config, statically linked `/MT` MSVC runtime — single standalone `.exe`, zero external DLL dependencies).
- **Resource Usage (Observed in Task Manager):**
  - **RAM:** ~20–24 MB
  - **GPU:** ~2–3% while actively dragging sliders or updating screen frames; ~0% when static or paused.
  - **Multi-Monitor & Hybrid-GPU Safe:** Binds each DXGI duplication session to the exact GPU adapter wired to that display, with automatic recovery after UAC prompts, lock screens, or sleep.

---

## <a id="conclusion"></a>📝 Final Notes
Building **Dimmer v1.0** through a fast learning crunch reinforced one big takeaway: even when AI writes the C++ and HLSL syntax for you, **debugging real-world visuals is still an engineering loop.** Understanding *why* ClearType fringes glow, *why* Windows 11 drop shadows create U-shaped masks, and *why* per-window OS metadata beats per-pixel ray guessing is what turned a glitchy prototype into a clean daily-driver utility.

[⬆️ Back to
