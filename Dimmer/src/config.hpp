#pragma once

// ==============================================================================
// >>> DIMMER — GPU SHADER & UI TUNING CONSTANTS <<<
// ==============================================================================
// Edit any constant below, press Build (F7), and test live!

namespace Config {

// 1. DIM SLIDER LIMIT (0 to 80% — prevents total darkness)
inline constexpr int MAX_DIM_PERCENT = 80;

// 2. SLIDER +/- BUTTON STEP (hold Shift while clicking -/+ for fine 1% steps)
inline constexpr int SLIDER_STEP = 1;

// 3. VARIANCE-WEIGHTED PER-PIXEL WHITE CUT PARAMETERS (at White Cut = 100%)
//    - WHITE_ANCHOR_IN_255  : Brightness where highlight cut anchors (192 = #C0C0C0).
//                             Pixels below this are protected for borders & real images.
//    - WHITE_ANCHOR_OUT_255 : Output brightness for #C0C0C0 at 100% (128 = #808080).
//    - PURE_WHITE_OUT_255   : Output brightness for #FFFFFF at 100% (160 = #A0A0A0).
inline constexpr double WHITE_ANCHOR_IN_255  = 192.0;
inline constexpr double WHITE_ANCHOR_OUT_255 = 128.0;
inline constexpr double PURE_WHITE_OUT_255   = 160.0;

//    - RGB VARIANCE / SPREAD GATE (spread = max(R,G,B) - min(R,G,B) in 0.0..1.0):
//      * <= NEUTRAL_SPREAD_TOLERANCE (0.05 = 5%): Full 100% White Cut weight.
//      * >= NEUTRAL_SPREAD_FALLOFF   (0.15 = 15%): 0% weight (vivid colors untouched).
inline constexpr double NEUTRAL_SPREAD_TOLERANCE = 0.05;
inline constexpr double NEUTRAL_SPREAD_FALLOFF   = 0.15;

//    - CURVE SHAPE:
//      * LOWER_ROLLOFF_POWER: Protects darks/borders below 192 exponentially (keep <= 2.9).
//      * UPPER_CURVE_POWER  : Smooth transition from #C0C0C0->#808080 up to #FFFFFF->#A0A0A0.
inline constexpr double LOWER_ROLLOFF_POWER = 2.80;
inline constexpr double UPPER_CURVE_POWER   = 1.35;

// 4. ITU-R BT.709 PERCEPTUAL LUMINANCE WEIGHTS (for the Color slider)
inline constexpr double REC709_R = 0.2126;
inline constexpr double REC709_G = 0.7152;
inline constexpr double REC709_B = 0.0722;

} // namespace Config