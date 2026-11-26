/*
================================================================================
DIMMER — NATIVE C++20 / DIRECT3D 11 VARIANCE-WEIGHTED WHITE CUTTER & DIMMER
================================================================================
Architecture:
  - Captures each attached monitor via DXGI Desktop Duplication on the exact
    GPU adapter wired to that display (multi-monitor & hybrid-GPU safe).
  - Embedded HLSL Pixel Shader (ps_5_0) evaluates every pixel's ORIGINAL RGB:
      * Computes RGB spread = max(R,G,B) - min(R,G,B).
      * Neutral pixels (spread <= 5%) receive full White Cut weight:
          #FFFFFF -> #A0A0A0, #C0C0C0 -> #808080 at White Cut = 100%,
          with exponential shadow protection below 192.
      * High-variance colors (spread >= 15%, e.g. #FF0000) receive 0% weight.
      * Followed by Rec. 709 Color Saturation and overall Dimness (0-80%).
  - Renders to a click-through topmost overlay excluded from capture via
    WDA_EXCLUDEFROMCAPTURE (no feedback loop & screenshots stay unmodified).
================================================================================
*/

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "config.hpp"

using Microsoft::WRL::ComPtr;

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

// ==============================================================================
// THEME, CUSTOM MESSAGES & CONTROL IDS
// ==============================================================================
namespace Theme {
    inline constexpr COLORREF BG            = RGB(24, 24, 26);
    inline constexpr COLORREF SURFACE       = RGB(42, 43, 46);
    inline constexpr COLORREF SURFACE_HOVER = RGB(56, 58, 62);
    inline constexpr COLORREF SURFACE_PRESS = RGB(68, 70, 76);
    inline constexpr COLORREF TRACK_EMPTY   = RGB(14, 15, 17);
    inline constexpr COLORREF TRACK_FILL    = RGB(118, 125, 136);
    inline constexpr COLORREF KNOB          = RGB(132, 139, 150);
    inline constexpr COLORREF KNOB_HOVER    = RGB(158, 166, 178);
    inline constexpr COLORREF BORDER        = RGB(68, 70, 74);
    inline constexpr COLORREF TEXT          = RGB(220, 222, 226);
    inline constexpr COLORREF MUTED         = RGB(145, 150, 158);
    inline constexpr COLORREF ACCENT        = RGB(74, 144, 226);
    inline constexpr COLORREF ACCENT_PRESS  = RGB(54, 118, 196);
    inline constexpr COLORREF SUCCESS       = RGB(38, 118, 66);   // Green: Turn ON
    inline constexpr COLORREF SUCCESS_PRESS = RGB(50, 145, 82);
    inline constexpr COLORREF DANGER        = RGB(122, 42, 42);   // Red: Turn OFF
    inline constexpr COLORREF DANGER_PRESS  = RGB(152, 54, 54);
}

enum AppMessages : UINT {
    WM_APP_SLIDER_CHANGED     = WM_APP + 1,
    WM_APP_OVERLAY_VISIBILITY = WM_APP + 2
};

enum ControlID : int {
    IDC_BTN_GUIDE = 101,
    IDC_SLIDER_DIM,
    IDC_SLIDER_CUT,
    IDC_SLIDER_COLOR,
    IDC_BTN_SAVE,
    IDC_BTN_PRESET_0,
    IDC_BTN_PRESET_1,
    IDC_BTN_PRESET_2,
    IDC_BTN_TOGGLE,

    // Save Dialog IDs
    IDC_SAVE_RADIO_0 = 201,
    IDC_SAVE_RADIO_1,
    IDC_SAVE_RADIO_2,
    IDC_SAVE_EDIT_NAME,
    IDC_SAVE_SUMMARY,
    IDC_SAVE_RESET,

    // Guide Window IDs
    IDC_GUIDE_TEXT = 301,
    IDC_GUIDE_CLOSE
};

inline constexpr UINT_PTR TIMER_ZORDER_WATCH = 1;
inline constexpr wchar_t MAIN_WINDOW_CLASS[]    = L"DimmerMainWindowClass";
inline constexpr wchar_t OVERLAY_WINDOW_CLASS[] = L"DimmerGpuOverlayClass";
inline constexpr wchar_t SLIDER_WINDOW_CLASS[]  = L"DimmerCustomSliderClass";

// ==============================================================================
// PRESET MODEL & SPARSE Dimmer.json PERSISTENCE (ZERO EXTERNAL LIBRARIES)
// ==============================================================================
struct ColorPreset {
    std::wstring name;
    int dim;
    int whiteCut;
    int color;

    bool operator==(const ColorPreset& o) const {
        return name == o.name && dim == o.dim && whiteCut == o.whiteCut && color == o.color;
    }
    bool operator!=(const ColorPreset& o) const { return !(*this == o); }
};

// Built-in presets compiled directly into Dimmer.exe: { Name, Dim%, WhiteCut%, Color% }
static const std::array<ColorPreset, 3> DEFAULT_PRESETS = {{
    {L"Low",    0, 80,   100},
    {L"Medium", 20, 100,  100},
    {L"High",   60, 100, 100}
}};

static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), len, nullptr, nullptr);
    return out;
}

static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

static std::string EscapeJsonString(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char ch : s) {
        if (ch == '\\') out += "\\\\";
        else if (ch == '"') out += "\\\"";
        else if (static_cast<unsigned char>(ch) >= 0x20) out += ch;
    }
    return out;
}

class PresetStore {
public:
    PresetStore() {
        wchar_t exePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::wstring full(exePath);
        size_t pos = full.find_last_of(L"\\/");
        std::wstring dir = (pos != std::wstring::npos) ? full.substr(0, pos) : L".";
        jsonPath_ = dir + L"\\Dimmer.json";
    }

    std::array<ColorPreset, 3> Load() const {
        auto presets = DEFAULT_PRESETS;
        std::ifstream file(jsonPath_, std::ios::binary);
        if (!file.is_open()) return presets;

        std::ostringstream buffer;
        buffer << file.rdbuf();
        const std::string text = buffer.str();
        if (text.empty()) return presets;

        for (int i = 0; i < 3; ++i) {
            std::string key = "slot" + std::to_string(i + 1);
            std::string obj;
            if (ExtractSlotObject(text, key, obj)) {
                ParseSlotFields(obj, presets[i]);
            }
        }
        return presets;
    }

    // Sparse Save: writes ONLY slots modified from built-in defaults.
    // Deletes Dimmer.json automatically if all 3 slots match defaults.
    bool Save(const std::array<ColorPreset, 3>& presets) const {
        std::vector<int> modified;
        for (int i = 0; i < 3; ++i) {
            if (presets[i] != DEFAULT_PRESETS[i]) modified.push_back(i);
        }

        if (modified.empty()) {
            if (GetFileAttributesW(jsonPath_.c_str()) != INVALID_FILE_ATTRIBUTES) {
                DeleteFileW(jsonPath_.c_str());
            }
            return true;
        }

        std::ostringstream ss;
        ss << "{\n";
        for (size_t k = 0; k < modified.size(); ++k) {
            int idx = modified[k];
            const auto& p = presets[idx];
            ss << "  \"slot" << (idx + 1) << "\": {\n"
               << "    \"name\": \"" << EscapeJsonString(WideToUtf8(p.name)) << "\",\n"
               << "    \"dim\": " << p.dim << ",\n"
               << "    \"white_cut\": " << p.whiteCut << ",\n"
               << "    \"color\": " << p.color << "\n"
               << "  }" << ((k + 1 < modified.size()) ? "," : "") << "\n";
        }
        ss << "}\n";

        std::ofstream out(jsonPath_, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return false;
        out << ss.str();
        return out.good();
    }

    const std::wstring& Path() const { return jsonPath_; }

private:
    static void SkipWs(const std::string& s, size_t& pos) {
        while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
    }

    static bool ReadQuotedString(const std::string& s, size_t& pos, std::string& out) {
        SkipWs(s, pos);
        if (pos >= s.size() || s[pos] != '"') return false;
        ++pos;
        out.clear();
        while (pos < s.size()) {
            char ch = s[pos++];
            if (ch == '\\' && pos < s.size()) out.push_back(s[pos++]);
            else if (ch == '"') return true;
            else out.push_back(ch);
        }
        return false;
    }

    static bool ExtractSlotObject(const std::string& json, const std::string& slotKey, std::string& outObj) {
        size_t pos = 0;
        SkipWs(json, pos);
        if (pos >= json.size() || json[pos] != '{') return false;
        ++pos;

        while (pos < json.size()) {
            SkipWs(json, pos);
            if (pos >= json.size() || json[pos] == '}') break;
            if (json[pos] == ',') { ++pos; continue; }

            std::string key;
            if (!ReadQuotedString(json, pos, key)) break;
            SkipWs(json, pos);
            if (pos >= json.size() || json[pos] != ':') break;
            ++pos;
            SkipWs(json, pos);

            if (pos < json.size() && json[pos] == '{') {
                size_t start = pos;
                int depth = 0;
                bool inStr = false;
                bool esc = false;
                while (pos < json.size()) {
                    char ch = json[pos++];
                    if (inStr) {
                        if (esc) esc = false;
                        else if (ch == '\\') esc = true;
                        else if (ch == '"') inStr = false;
                    } else {
                        if (ch == '"') inStr = true;
                        else if (ch == '{') ++depth;
                        else if (ch == '}') {
                            if (--depth == 0) break;
                        }
                    }
                }
                if (key == slotKey) {
                    outObj = json.substr(start, pos - start);
                    return true;
                }
            } else {
                while (pos < json.size() && json[pos] != ',' && json[pos] != '}') ++pos;
            }
        }
        return false;
    }

    static void ParseSlotFields(const std::string& obj, ColorPreset& preset) {
        size_t pos = 0;
        SkipWs(obj, pos);
        if (pos >= obj.size() || obj[pos] != '{') return;
        ++pos;

        while (pos < obj.size()) {
            SkipWs(obj, pos);
            if (pos >= obj.size() || obj[pos] == '}') break;
            if (obj[pos] == ',') { ++pos; continue; }

            std::string key;
            if (!ReadQuotedString(obj, pos, key)) break;
            SkipWs(obj, pos);
            if (pos >= obj.size() || obj[pos] != ':') break;
            ++pos;
            SkipWs(obj, pos);

            if (pos < obj.size() && obj[pos] == '"') {
                std::string strVal;
                if (ReadQuotedString(obj, pos, strVal) && key == "name") {
                    std::wstring wName = Utf8ToWide(strVal);
                    if (!wName.empty()) {
                        if (wName.size() > 10) wName.resize(10);
                        preset.name = wName;
                    }
                }
            } else {
                size_t valStart = pos;
                while (pos < obj.size() && obj[pos] != ',' && obj[pos] != '}' &&
                       !std::isspace(static_cast<unsigned char>(obj[pos]))) {
                    ++pos;
                }
                std::string tok = obj.substr(valStart, pos - valStart);
                try {
                    if (key == "dim") preset.dim = std::clamp(std::stoi(tok), 0, Config::MAX_DIM_PERCENT);
                    else if (key == "white_cut") preset.whiteCut = std::clamp(std::stoi(tok), 0, 100);
                    else if (key == "color") preset.color = std::clamp(std::stoi(tok), 0, 100);
                } catch (...) {}
            }
        }
    }

    std::wstring jsonPath_;
};

// ==============================================================================
// EMBEDDED HLSL SHADER — VARIANCE-WEIGHTED WHITE CUT (>192, <=5% RGB SPREAD)
// ==============================================================================
static constexpr char SHADER_SOURCE[] = R"HLSL(
cbuffer TunerParams : register(b0) {
    float dimRatio;        // 0.0 .. 0.80
    float whiteCutRatio;   // 0.0 .. 1.00
    float colorSat;        // 0.0 .. 1.00
    float anchorIn;        // 192.0 / 255.0 (#C0C0C0)

    float anchorOut;       // 128.0 / 255.0 (#808080 at 100% White Cut)
    float pureWhiteOut;    // 160.0 / 255.0 (#A0A0A0 at 100% White Cut)
    float spreadTol;       // 0.05 (<= 5% RGB spread gets full weight)
    float spreadFalloff;   // 0.15 (>= 15% RGB spread gets 0% weight)

    float lowerPower;      // 2.80 (exponential shadow protection below 192)
    float upperPower;      // 1.35 (smooth ramp from #808080 to #A0A0A0)
    uint  winCount;        // Active OS window rects (0 .. 16, front-to-back)
    uint  _pad0;

    float4 winRects[16];   // (left, top, right, bottom) in monitor-local pixels
};

Texture2D srcTex      : register(t0);
Texture2D winThemeTex : register(t1);

struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    o.uv  = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float CalcNeutralScale(float v) {
    if (v <= 0.0001) return 1.0;
    float targetY = v;
    if (v >= anchorIn) {
        float u = saturate((v - anchorIn) / max(0.0001, 1.0 - anchorIn));
        targetY = lerp(anchorOut, pureWhiteOut, pow(u, upperPower));
    } else {
        float t = saturate(v / max(0.0001, anchorIn));
        float drop = max(0.0, anchorIn - anchorOut);
        float safeP = min(lowerPower, (anchorIn / max(0.0001, drop)) - 0.05);
        targetY = v - drop * pow(t, max(1.0, safeP));
    }
    return saturate(targetY / v);
}

// PASS 1 (16x1 tiny texture): Classifies each OS window ONCE as Light (1.0) or Dark (0.0)
// Samples an 8x8 grid inside the visible (unoccluded) region of each window's 4 corners.
float4 PSClassifyWindows(VSOut i) : SV_TARGET {
    uint wIdx = (uint)i.pos.x;
    if (wIdx >= winCount || wIdx >= 16u) {
        return float4(0.0, anchorIn, 0.0, 1.0);
    }

    uint texW, texH;
    srcTex.GetDimensions(texW, texH);
    int2 maxCoord = int2(max(1u, texW) - 1u, max(1u, texH) - 1u);

    float4 r  = winRects[wIdx];
    float rw  = max(1.0, r.z - r.x);
    float rh  = max(1.0, r.w - r.y);
    float brightGate = anchorIn - 0.005;

    float hits     = 0.0;
    float valid    = 0.0;
    float bgBright = anchorIn;

    for (int dy = 0; dy < 8; ++dy) {
        for (int dx = 0; dx < 8; ++dx) {
            int2 sp = clamp(
                int2(r.x + ((float)dx + 0.5) * (rw / 8.0),
                     r.y + ((float)dy + 0.5) * (rh / 8.0)),
                int2(0, 0), maxCoord
            );

            // Skip sample points covered by another floating window in front
            bool occluded = false;
            for (int k = 0; k < (int)wIdx; ++k) {
                float4 fr = winRects[k];
                if (sp.x >= fr.x && sp.x < fr.z && sp.y >= fr.y && sp.y < fr.w) {
                    occluded = true;
                    break;
                }
            }
            if (occluded) continue;

            valid += 1.0;
            float3 n = srcTex.Load(int3(sp, 0)).rgb;
            float nMax = max(n.r, max(n.g, n.b));
            float nMin = min(n.r, min(n.g, n.b));
            if (nMax >= brightGate && (nMax - nMin) <= spreadTol) {
                hits += 1.0;
                bgBright = max(bgBright, nMax);
            }
        }
    }

    // Dark-mode apps have < 10% bright neutral pixels; Light-mode apps (WPS/Word/PDF) have > 30%.
    float brightRatio = (valid > 0.5) ? (hits / valid) : 0.0;
    float isLightWin  = (brightRatio >= 0.25) ? 1.0 : 0.0;
    return float4(isLightWin, bgBright, 0.0, 1.0);
}

// PASS 2: Pure 1x1 Variance-Weighted White Cut + 3x3 ClearType Filter (Light-Mode Windows Only)
float4 PSMain(VSOut i) : SV_TARGET {
    uint texW, texH;
    srcTex.GetDimensions(texW, texH);
    int2 maxCoord = int2(max(1u, texW) - 1u, max(1u, texH) - 1u);
    int2 p = clamp(int2(i.pos.xy), int2(0, 0), maxCoord);

    float3 c = srcTex.Load(int3(p, 0)).rgb;

    // 1. PER-WINDOW LIGHT-MODE WHITE CUT + 3x3 CLEARTYPE TEXT FILTER
    if (whiteCutRatio > 0.0001) {
        int count = (int)min(winCount, 16u);
        int ownerIdx = count;
        int4 box = int4(0, 0, (int)texW, (int)texH);

        // Find the topmost OS window owning pixel p (exact 4-corner DWM bounds)
        for (int w = 0; w < count; ++w) {
            float4 r = winRects[w];
            if (p.x >= r.x && p.x < r.z && p.y >= r.y && p.y < r.w) {
                ownerIdx = w;
                box = int4((int)r.x, (int)r.y, (int)r.z, (int)r.w);
                break;
            }
        }

        // Only apply White Cut if the owning window is classified as Light Mode
        if (ownerIdx < count) {
            float2 winInfo = winThemeTex.Load(int3(ownerIdx, 0, 0)).rg;
            if (winInfo.x > 0.5) {
                float maxC         = max(c.r, max(c.g, c.b));
                float minC         = min(c.r, min(c.g, c.b));
                float spread       = maxC - minC;
                float chromaWeight = 1.0 - smoothstep(spreadTol, spreadFalloff, spread);
                float finalScale   = 1.0;

                if (chromaWeight > 0.0001 && maxC > 0.0001) {
                    finalScale = lerp(1.0, CalcNeutralScale(maxC), whiteCutRatio * chromaWeight);
                }

                // Clean 3x3 ClearType fringe filter (runs ONLY on non-neutral edge pixels)
                if (chromaWeight < 0.999 && maxC > 0.12) {
                    float brightGate = anchorIn - 0.005;
                    int   brightN    = 0;
                    float bgBright   = max(anchorIn, winInfo.y);
                    float minStroke  = (spread <= 0.55) ? maxC : 1.0;
                    int2  boxMax     = max(box.xy, box.zw - int2(1, 1));

                    [unroll]
                    for (int dy = -1; dy <= 1; ++dy) {
                        [unroll]
                        for (int dx = -1; dx <= 1; ++dx) {
                            if (dx == 0 && dy == 0) continue;
                            int2 np = clamp(p + int2(dx, dy), box.xy, boxMax);
                            float3 n = srcTex.Load(int3(np, 0)).rgb;
                            float nMax    = max(n.r, max(n.g, n.b));
                            float nMin    = min(n.r, min(n.g, n.b));
                            float nSpread = nMax - nMin;

                            if (nMax >= brightGate && nSpread <= spreadTol) {
                                brightN += 1;
                                bgBright = max(bgBright, nMax);
                            }
                            if (nSpread <= 0.55) {
                                minStroke = min(minStroke, nMax);
                            }
                        }
                    }

                    // Require >= 3 bright paper neighbors -> fixes black text on white paper
                    // without touching thin white text on a dark top toolbar inside a hybrid browser
                    if (brightN >= 3 && minStroke <= max(0.25, bgBright - 0.12)) {
                        float textScale = lerp(1.0, CalcNeutralScale(bgBright), whiteCutRatio);
                        finalScale = min(finalScale, textScale);
                    }
                }

                c *= finalScale;
            }
        }
    }

    // 2. COLOR SATURATION (ITU-R BT.709 perceptual luminance)
    float luma = dot(c, float3(0.2126, 0.7152, 0.0722));
    c = lerp(float3(luma, luma, luma), c, colorSat);

    // 3. OVERALL DIMNESS (0% .. 80%)
    c *= (1.0 - dimRatio);

    return float4(saturate(c), 1.0);
}
)HLSL";

inline constexpr UINT MAX_WIN_RECTS = 16;

struct alignas(16) ShaderCB {
    float dimRatio;
    float whiteCutRatio;
    float colorSat;
    float anchorIn;

    float anchorOut;
    float pureWhiteOut;
    float spreadTol;
    float spreadFalloff;

    float lowerPower;
    float upperPower;
    UINT  winCount;
    UINT  pad0;

    float winRects[MAX_WIN_RECTS][4];
};
static_assert(sizeof(ShaderCB) % 16 == 0, "ShaderCB must be 16-byte aligned");

// ==============================================================================
// DIRECT3D 11 + DXGI DESKTOP DUPLICATION ENGINE (MULTI-MONITOR & HYBRID-GPU SAFE)
// ==============================================================================
struct MonitorTarget {
    RECT desktopRect{};
    UINT width = 0;
    UINT height = 0;

    HWND overlayHwnd = nullptr;
    HWND blockedTopHwnd = nullptr;
    bool overlayVisible = false;

    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput1> output1;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain1> swapChain;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<IDXGIOutputDuplication> duplication;

    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> psWinClassify;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Buffer> cb;

    ComPtr<ID3D11Texture2D> cachedFrame;
    ComPtr<ID3D11ShaderResourceView> cachedSrv;

    ComPtr<ID3D11Texture2D> winThemeTex;
    ComPtr<ID3D11RenderTargetView> winThemeRtv;
    ComPtr<ID3D11ShaderResourceView> winThemeSrv;

    bool hasValidFrame = false;
    bool needsDuplRecovery = false;
    std::chrono::steady_clock::time_point nextRecoveryTime{};
};

class GpuShaderEngine {
public:
    ~GpuShaderEngine() { Shutdown(); }

    bool Init(HINSTANCE hInst, HWND notifyHwnd, std::wstring& outError) {
        notifyHwnd_ = notifyHwnd;

        auto appendShaderErr = [](const wchar_t* label, ID3DBlob* err) {
            std::wstring msg(label);
            if (err && err->GetBufferPointer() && err->GetBufferSize() > 0) {
                std::string raw(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize());
                msg += L"\n\n" + Utf8ToWide(raw);
            }
            return msg;
        };

        ComPtr<ID3DBlob> vsBlob, psWinBlob, psBlob, errBlob;
        HRESULT hr = D3DCompile(SHADER_SOURCE, sizeof(SHADER_SOURCE) - 1, nullptr, nullptr, nullptr,
            "VSMain", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsBlob, &errBlob);
        if (FAILED(hr)) {
            outError = appendShaderErr(L"Failed to compile HLSL vertex shader.", errBlob.Get());
            return false;
        }
        errBlob.Reset();
        hr = D3DCompile(SHADER_SOURCE, sizeof(SHADER_SOURCE) - 1, nullptr, nullptr, nullptr,
            "PSClassifyWindows", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psWinBlob, &errBlob);
        if (FAILED(hr)) {
            outError = appendShaderErr(L"Failed to compile HLSL window classifier shader.", errBlob.Get());
            return false;
        }
        errBlob.Reset();
        hr = D3DCompile(SHADER_SOURCE, sizeof(SHADER_SOURCE) - 1, nullptr, nullptr, nullptr,
            "PSMain", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psBlob, &errBlob);
        if (FAILED(hr)) {
            outError = appendShaderErr(L"Failed to compile HLSL pixel shader.", errBlob.Get());
            return false;
        }

        ComPtr<IDXGIFactory1> factory1;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory1)))) {
            outError = L"Failed to create DXGI factory.";
            return false;
        }

        for (UINT aIdx = 0;; ++aIdx) {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory1->EnumAdapters1(aIdx, &adapter) == DXGI_ERROR_NOT_FOUND) break;

            DXGI_ADAPTER_DESC1 aDesc{};
            adapter->GetDesc1(&aDesc);
            if (aDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

            for (UINT oIdx = 0;; ++oIdx) {
                ComPtr<IDXGIOutput> output;
                if (adapter->EnumOutputs(oIdx, &output) == DXGI_ERROR_NOT_FOUND) break;

                DXGI_OUTPUT_DESC oDesc{};
                output->GetDesc(&oDesc);
                if (!oDesc.AttachedToDesktop) continue;

                ComPtr<IDXGIOutput1> output1;
                if (FAILED(output.As(&output1))) continue;

                auto target = std::make_unique<MonitorTarget>();
                target->desktopRect = oDesc.DesktopCoordinates;
                target->width  = static_cast<UINT>(oDesc.DesktopCoordinates.right - oDesc.DesktopCoordinates.left);
                target->height = static_cast<UINT>(oDesc.DesktopCoordinates.bottom - oDesc.DesktopCoordinates.top);
                target->adapter = adapter;
                target->output1 = output1;

                if (SetupMonitorTarget(*target, hInst, vsBlob.Get(), psWinBlob.Get(), psBlob.Get())) {
                    overlayHwnds_.push_back(target->overlayHwnd);
                    monitors_.push_back(std::move(target));
                }
            }
        }

        if (monitors_.empty()) {
            outError = L"Could not initialize Direct3D 11 Desktop Duplication on any display.\n"
                       L"(Requires Windows 10 2004+ or Windows 11 with WDDM GPU driver.)";
            return false;
        }

        running_.store(true);
        worker_ = std::thread(&GpuShaderEngine::RenderLoop, this);
        return true;
    }

    void UpdateParams(bool enabled, int dim, int whiteCut, int color) {
        bool wantActive = enabled && (dim > 0 || whiteCut > 0 || color < 100);
        {
            std::lock_guard<std::mutex> lock(mtx_);
            active_   = wantActive;
            dim_      = dim;
            whiteCut_ = whiteCut;
            color_    = color;
            ++paramGen_;
        }
        if (!wantActive) {
            SetAllOverlaysVisible(false);
        }
        cv_.notify_one();
    }

    void HandleOverlayVisibilityMsg(size_t monIdx, bool show) {
        if (monIdx >= monitors_.size()) return;
        auto& m = *monitors_[monIdx];
        if (!m.overlayHwnd) return;
        if (show) {
            bool isActive = false;
            {
                std::lock_guard<std::mutex> lock(mtx_);
                isActive = active_;
            }
            if (!isActive) return;

            ShowWindow(m.overlayHwnd, SW_SHOWNOACTIVATE);
            SetWindowPos(m.overlayHwnd, HWND_TOPMOST,
                m.desktopRect.left, m.desktopRect.top,
                static_cast<int>(m.width), static_cast<int>(m.height),
                SWP_NOACTIVATE);
            m.overlayVisible = true;
        } else {
            ShowWindow(m.overlayHwnd, SW_HIDE);
            m.overlayVisible = false;
        }
    }

    void SetAllOverlaysVisible(bool show) {
        for (size_t i = 0; i < monitors_.size(); ++i) {
            HandleOverlayVisibilityMsg(i, show);
        }
    }

    void MaintainTopmost() {
        bool isActive = false;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            isActive = active_;
        }
        if (!isActive) return;

        for (auto& m : monitors_) {
            if (!m->overlayVisible || !m->overlayHwnd) continue;
            HWND prev = GetWindow(m->overlayHwnd, GW_HWNDPREV);
            if (prev && IsWindowVisible(prev)) {
                if (prev != m->blockedTopHwnd) {
                    SetWindowPos(m->overlayHwnd, HWND_TOPMOST, 0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                    HWND after = GetWindow(m->overlayHwnd, GW_HWNDPREV);
                    m->blockedTopHwnd = (after == prev) ? after : nullptr;
                }
            } else {
                m->blockedTopHwnd = nullptr;
            }
        }
    }

    size_t MonitorCount() const { return monitors_.size(); }

    void Shutdown() {
        running_.store(false);
        cv_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
        for (auto& m : monitors_) {
            if (m->overlayHwnd && IsWindow(m->overlayHwnd)) {
                DestroyWindow(m->overlayHwnd);
                m->overlayHwnd = nullptr;
            }
        }
        monitors_.clear();
        overlayHwnds_.clear();
    }

private:
    struct WinEnumCtx {
        const std::vector<HWND>* skipHwnds = nullptr;
        std::vector<RECT> rects;
    };

    static BOOL CALLBACK EnumDesktopWindowsProc(HWND hwnd, LPARAM lParam) {
        auto* ctx = reinterpret_cast<WinEnumCtx*>(lParam);
        if (ctx->skipHwnds) {
            for (HWND skip : *ctx->skipHwnds) {
                if (hwnd == skip) return TRUE;
            }
        }
        if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;

        LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if ((exStyle & WS_EX_TRANSPARENT) && (exStyle & WS_EX_LAYERED)) return TRUE;
        if (exStyle & WS_EX_LAYERED) {
            BYTE alpha = 255;
            DWORD lFlags = 0;
            if (GetLayeredWindowAttributes(hwnd, nullptr, &alpha, &lFlags) &&
                (lFlags & LWA_ALPHA) && alpha == 0) {
                return TRUE;
            }
        }

        DWORD cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0) {
            return TRUE;
        }

        wchar_t cls[64]{};
        int clsLen = GetClassNameW(hwnd, cls, 64);
        if (clsLen > 0) {
            std::wstring clsName(cls, static_cast<size_t>(clsLen));
            if (clsName == L"Progman" || clsName == L"WorkerW" ||
                clsName == L"IME" || clsName == L"MSCTFIME UI" ||
                clsName == OVERLAY_WINDOW_CLASS) {
                return TRUE;
            }
        }

        // Exact 4-corner frame bounds excluding Windows 11 U-shaped drop shadows
        RECT rc{};
        if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rc, sizeof(rc)))) {
            if (!GetWindowRect(hwnd, &rc)) return TRUE;
        }
        if ((rc.right - rc.left) < 16 || (rc.bottom - rc.top) < 16) return TRUE;

        ctx->rects.push_back(rc);
        return TRUE;
    }

    void CollectDesktopWindows(std::vector<RECT>& outRects) const {
        WinEnumCtx ctx;
        ctx.skipHwnds = &overlayHwnds_;
        ctx.rects.reserve(24);
        EnumWindows(EnumDesktopWindowsProc, reinterpret_cast<LPARAM>(&ctx));
        outRects.swap(ctx.rects);
    }

    bool SetupMonitorTarget(MonitorTarget& m, HINSTANCE hInst,
                            ID3DBlob* vsBlob, ID3DBlob* psWinBlob, ID3DBlob* psBlob) {
        DWORD exStyle = WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT |
                        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
        m.overlayHwnd = CreateWindowExW(
            exStyle, OVERLAY_WINDOW_CLASS, L"", WS_POPUP,
            m.desktopRect.left, m.desktopRect.top,
            static_cast<int>(m.width), static_cast<int>(m.height),
            nullptr, nullptr, hInst, nullptr
        );
        if (!m.overlayHwnd) return false;

        SetLayeredWindowAttributes(m.overlayHwnd, 0, 255, LWA_ALPHA);

        if (!SetWindowDisplayAffinity(m.overlayHwnd, WDA_EXCLUDEFROMCAPTURE)) {
            DestroyWindow(m.overlayHwnd);
            m.overlayHwnd = nullptr;
            return false;
        }

        D3D_FEATURE_LEVEL flOut{};
        const D3D_FEATURE_LEVEL flReq[] = { D3D_FEATURE_LEVEL_11_0 };
        HRESULT hr = D3D11CreateDevice(
            m.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, flReq, 1, D3D11_SDK_VERSION,
            &m.device, &flOut, &m.context
        );
        if (FAILED(hr)) {
            DestroyWindow(m.overlayHwnd);
            m.overlayHwnd = nullptr;
            return false;
        }

        ComPtr<IDXGIFactory2> factory2;
        if (FAILED(m.adapter->GetParent(IID_PPV_ARGS(&factory2)))) {
            DestroyWindow(m.overlayHwnd);
            m.overlayHwnd = nullptr;
            return false;
        }

        DXGI_SWAP_CHAIN_DESC1 scDesc{};
        scDesc.Width       = m.width;
        scDesc.Height      = m.height;
        scDesc.Format      = DXGI_FORMAT_B8G8R8A8_UNORM;
        scDesc.SampleDesc  = {1, 0};
        scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scDesc.BufferCount = 2;
        scDesc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        scDesc.AlphaMode   = DXGI_ALPHA_MODE_IGNORE;

        hr = factory2->CreateSwapChainForHwnd(m.device.Get(), m.overlayHwnd, &scDesc, nullptr, nullptr, &m.swapChain);
        if (FAILED(hr)) {
            scDesc.BufferCount = 1;
            scDesc.SwapEffect  = DXGI_SWAP_EFFECT_DISCARD;
            hr = factory2->CreateSwapChainForHwnd(m.device.Get(), m.overlayHwnd, &scDesc, nullptr, nullptr, &m.swapChain);
        }
        if (FAILED(hr)) {
            DestroyWindow(m.overlayHwnd);
            m.overlayHwnd = nullptr;
            return false;
        }
        factory2->MakeWindowAssociation(m.overlayHwnd, DXGI_MWA_NO_ALT_ENTER);

        ComPtr<ID3D11Texture2D> backBuf;
        if (FAILED(m.swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuf))) ||
            FAILED(m.device->CreateRenderTargetView(backBuf.Get(), nullptr, &m.rtv))) {
            DestroyWindow(m.overlayHwnd);
            m.overlayHwnd = nullptr;
            return false;
        }

        if (FAILED(m.device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m.vs)) ||
            FAILED(m.device->CreatePixelShader(psWinBlob->GetBufferPointer(), psWinBlob->GetBufferSize(), nullptr, &m.psWinClassify)) ||
            FAILED(m.device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m.ps))) {
            DestroyWindow(m.overlayHwnd);
            m.overlayHwnd = nullptr;
            return false;
        }

        D3D11_BUFFER_DESC cbDesc{};
        cbDesc.ByteWidth      = sizeof(ShaderCB);
        cbDesc.Usage          = D3D11_USAGE_DYNAMIC;
        cbDesc.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(m.device->CreateBuffer(&cbDesc, nullptr, &m.cb))) {
            DestroyWindow(m.overlayHwnd);
            m.overlayHwnd = nullptr;
            return false;
        }

        // Tiny 16x1 texture holding each window's binary Light/Dark verdict (32 bytes VRAM!)
        D3D11_TEXTURE2D_DESC wtDesc{};
        wtDesc.Width      = MAX_WIN_RECTS;
        wtDesc.Height     = 1;
        wtDesc.MipLevels  = 1;
        wtDesc.ArraySize  = 1;
        wtDesc.Format     = DXGI_FORMAT_R8G8_UNORM;
        wtDesc.SampleDesc = {1, 0};
        wtDesc.Usage      = D3D11_USAGE_DEFAULT;
        wtDesc.BindFlags  = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(m.device->CreateTexture2D(&wtDesc, nullptr, &m.winThemeTex)) ||
            FAILED(m.device->CreateRenderTargetView(m.winThemeTex.Get(), nullptr, &m.winThemeRtv)) ||
            FAILED(m.device->CreateShaderResourceView(m.winThemeTex.Get(), nullptr, &m.winThemeSrv))) {
            DestroyWindow(m.overlayHwnd);
            m.overlayHwnd = nullptr;
            return false;
        }

        hr = m.output1->DuplicateOutput(m.device.Get(), &m.duplication);
        if (FAILED(hr)) {
            DestroyWindow(m.overlayHwnd);
            m.overlayHwnd = nullptr;
            return false;
        }

        return true;
    }

    static bool EnsureCachedTexture(MonitorTarget& m, ID3D11Texture2D* srcTex) {
        D3D11_TEXTURE2D_DESC srcDesc{};
        srcTex->GetDesc(&srcDesc);

        if (m.cachedFrame) {
            D3D11_TEXTURE2D_DESC curDesc{};
            m.cachedFrame->GetDesc(&curDesc);
            if (curDesc.Width == srcDesc.Width && curDesc.Height == srcDesc.Height &&
                curDesc.Format == srcDesc.Format) {
                return true;
            }
            m.cachedSrv.Reset();
            m.cachedFrame.Reset();
        }

        D3D11_TEXTURE2D_DESC cacheDesc{};
        cacheDesc.Width      = srcDesc.Width;
        cacheDesc.Height     = srcDesc.Height;
        cacheDesc.MipLevels  = 1;
        cacheDesc.ArraySize  = 1;
        cacheDesc.Format     = srcDesc.Format;
        cacheDesc.SampleDesc = {1, 0};
        cacheDesc.Usage      = D3D11_USAGE_DEFAULT;
        cacheDesc.BindFlags  = D3D11_BIND_SHADER_RESOURCE;

        if (FAILED(m.device->CreateTexture2D(&cacheDesc, nullptr, &m.cachedFrame))) return false;
        if (FAILED(m.device->CreateShaderResourceView(m.cachedFrame.Get(), nullptr, &m.cachedSrv))) {
            m.cachedFrame.Reset();
            return false;
        }
        return true;
    }

    static void RenderMonitorFrame(MonitorTarget& m, int dim, int whiteCut, int color,
                                   const std::vector<RECT>& desktopWindows, bool vsync) {
        if (!m.hasValidFrame || !m.cachedSrv || !m.winThemeSrv) return;

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(m.context->Map(m.cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            auto* cb = static_cast<ShaderCB*>(mapped.pData);
            cb->dimRatio      = static_cast<float>(dim) / 100.0f;
            cb->whiteCutRatio = static_cast<float>(whiteCut) / 100.0f;
            cb->colorSat      = static_cast<float>(color) / 100.0f;
            cb->anchorIn      = static_cast<float>(Config::WHITE_ANCHOR_IN_255 / 255.0);
            cb->anchorOut     = static_cast<float>(Config::WHITE_ANCHOR_OUT_255 / 255.0);
            cb->pureWhiteOut  = static_cast<float>(Config::PURE_WHITE_OUT_255 / 255.0);
            cb->spreadTol     = static_cast<float>(Config::NEUTRAL_SPREAD_TOLERANCE);
            cb->spreadFalloff = static_cast<float>(Config::NEUTRAL_SPREAD_FALLOFF);
            cb->lowerPower    = static_cast<float>(Config::LOWER_ROLLOFF_POWER);
            cb->upperPower    = static_cast<float>(Config::UPPER_CURVE_POWER);
            cb->pad0          = 0;

            UINT count = 0;
            if (whiteCut > 0) {
                for (const RECT& wr : desktopWindows) {
                    if (count >= MAX_WIN_RECTS) break;
                    LONG l = (std::max)(wr.left,   m.desktopRect.left)   - m.desktopRect.left;
                    LONG t = (std::max)(wr.top,    m.desktopRect.top)    - m.desktopRect.top;
                    LONG r = (std::min)(wr.right,  m.desktopRect.right)  - m.desktopRect.left;
                    LONG b = (std::min)(wr.bottom, m.desktopRect.bottom) - m.desktopRect.top;
                    if (r - l < 16 || b - t < 16) continue;

                    cb->winRects[count][0] = static_cast<float>(l);
                    cb->winRects[count][1] = static_cast<float>(t);
                    cb->winRects[count][2] = static_cast<float>(r);
                    cb->winRects[count][3] = static_cast<float>(b);
                    ++count;

                    if (l <= 0 && t <= 0 && r >= static_cast<LONG>(m.width) && b >= static_cast<LONG>(m.height)) {
                        break;
                    }
                }
            }
            cb->winCount = count;
            m.context->Unmap(m.cb.Get(), 0);
        }

        m.context->IASetInputLayout(nullptr);
        m.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m.context->VSSetShader(m.vs.Get(), nullptr, 0);

        ID3D11Buffer* cbs[] = { m.cb.Get() };
        m.context->PSSetConstantBuffers(0, 1, cbs);

        ID3D11ShaderResourceView* nullSrvs[] = { nullptr, nullptr };

        // PASS 1: Classify up to 16 OS windows (Light vs Dark) into 16x1 texture
        if (whiteCut > 0) {
            m.context->PSSetShaderResources(0, 2, nullSrvs);
            ID3D11RenderTargetView* wtRtv = m.winThemeRtv.Get();
            m.context->OMSetRenderTargets(1, &wtRtv, nullptr);

            D3D11_VIEWPORT wtVp{0.0f, 0.0f, static_cast<float>(MAX_WIN_RECTS), 1.0f, 0.0f, 1.0f};
            m.context->RSSetViewports(1, &wtVp);

            ID3D11ShaderResourceView* srvs1[] = { m.cachedSrv.Get(), nullptr };
            m.context->PSSetShaderResources(0, 2, srvs1);
            m.context->PSSetShader(m.psWinClassify.Get(), nullptr, 0);
            m.context->Draw(3, 0);
        }

        // PASS 2: Fullscreen Variance White Cut + 3x3 ClearType (Light windows only)
        m.context->PSSetShaderResources(0, 2, nullSrvs);
        ID3D11RenderTargetView* rtv = m.rtv.Get();
        m.context->OMSetRenderTargets(1, &rtv, nullptr);

        D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(m.width), static_cast<float>(m.height), 0.0f, 1.0f};
        m.context->RSSetViewports(1, &vp);

        m.context->PSSetShader(m.ps.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srvs2[] = { m.cachedSrv.Get(), m.winThemeSrv.Get() };
        m.context->PSSetShaderResources(0, 2, srvs2);
        m.context->Draw(3, 0);

        m.context->PSSetShaderResources(0, 2, nullSrvs);
        m.swapChain->Present(vsync ? 1 : 0, 0);
    }

    void RenderLoop() {
        uint64_t lastAppliedGen = 0;
        bool wasActive = false;
        std::vector<RECT> desktopWindows;

        while (running_.load()) {
            bool isActive = false;
            int dim = 0, whiteCut = 0, color = 100;
            uint64_t curGen = 0;

            {
                std::unique_lock<std::mutex> lock(mtx_);
                cv_.wait(lock, [this]() { return !running_.load() || active_; });
                if (!running_.load()) break;
                isActive = active_;
                dim      = dim_;
                whiteCut = whiteCut_;
                color    = color_;
                curGen   = paramGen_;
            }

            bool paramsChanged = (curGen != lastAppliedGen) || !wasActive;
            if (!wasActive && isActive) {
                for (auto& m : monitors_) m->hasValidFrame = false;
            }
            wasActive = isActive;
            lastAppliedGen = curGen;

            const bool singleMonitor = (monitors_.size() == 1);
            const UINT timeoutMs = paramsChanged ? 0 : (singleMonitor ? 20 : 8);
            bool windowsCapturedThisTick = false;

            for (size_t idx = 0; idx < monitors_.size(); ++idx) {
                if (!running_.load()) break;
                auto& m = *monitors_[idx];

                if (m.needsDuplRecovery) {
                    if (std::chrono::steady_clock::now() < m.nextRecoveryTime) continue;
                    m.duplication.Reset();
                    if (SUCCEEDED(m.output1->DuplicateOutput(m.device.Get(), &m.duplication))) {
                        m.needsDuplRecovery = false;
                        m.hasValidFrame = false;
                    } else {
                        m.nextRecoveryTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
                        continue;
                    }
                }

                DXGI_OUTDUPL_FRAME_INFO frameInfo{};
                ComPtr<IDXGIResource> desktopRes;
                HRESULT hr = m.duplication->AcquireNextFrame(timeoutMs, &frameInfo, &desktopRes);

                bool newPixels = false;
                if (SUCCEEDED(hr)) {
                    if (frameInfo.LastPresentTime.QuadPart != 0 || !m.hasValidFrame) {
                        ComPtr<ID3D11Texture2D> desktopTex;
                        if (SUCCEEDED(desktopRes.As(&desktopTex)) && EnsureCachedTexture(m, desktopTex.Get())) {
                            m.context->CopyResource(m.cachedFrame.Get(), desktopTex.Get());
                            m.hasValidFrame = true;
                            newPixels = true;
                        }
                    }
                    m.duplication->ReleaseFrame();
                } else if (hr == DXGI_ERROR_ACCESS_LOST) {
                    m.duplication.Reset();
                    m.hasValidFrame = false;
                    m.needsDuplRecovery = true;
                    m.nextRecoveryTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
                    PostMessageW(notifyHwnd_, WM_APP_OVERLAY_VISIBILITY, static_cast<WPARAM>(idx), FALSE);
                    continue;
                }

                if (m.hasValidFrame && (newPixels || paramsChanged)) {
                    if (whiteCut > 0 && !windowsCapturedThisTick) {
                        CollectDesktopWindows(desktopWindows);
                        windowsCapturedThisTick = true;
                    }
                    RenderMonitorFrame(m, dim, whiteCut, color, desktopWindows, singleMonitor);
                    if (!m.overlayVisible) {
                        PostMessageW(notifyHwnd_, WM_APP_OVERLAY_VISIBILITY, static_cast<WPARAM>(idx), TRUE);
                    }
                }
            }
        }
    }

    HWND notifyHwnd_ = nullptr;
    std::vector<std::unique_ptr<MonitorTarget>> monitors_;
    std::vector<HWND> overlayHwnds_;
    std::thread worker_;
    std::atomic<bool> running_{false};

    std::mutex mtx_;
    std::condition_variable cv_;
    bool active_ = false;
    int dim_ = 30;
    int whiteCut_ = 0;
    int color_ = 100;
    uint64_t paramGen_ = 0;
};

// ==============================================================================
// CUSTOM ANTI-ALIASED "-" [SLIDER] "+" CONTROL (2x SUPERSAMPLED GDI)
// ==============================================================================
struct CustomSliderData {
    int id = 0;
    int minVal = 0;
    int maxVal = 100;
    int val = 0;
    bool dragging = false;
    int hoverPart = 0;
    int pressPart = 0;
    bool trackingMouse = false;
};

struct SliderMetrics {
    int btnSize;
    int gap;
    int trackLeft;
    int trackRight;
    int trackY;
    int trackH;
    int knobR;
    int knobX;
};

static SliderMetrics CalcSliderMetrics(const RECT& rc, const CustomSliderData* d) {
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    SliderMetrics m{};
    m.btnSize    = (std::min)(h - 2, (std::max)(16, h - 2));
    m.gap        = (std::max)(7, m.btnSize / 3);
    m.knobR      = (std::max)(5, m.btnSize * 32 / 100);
    m.trackLeft  = m.btnSize + m.gap + m.knobR;
    m.trackRight = (std::max)(m.trackLeft + 1, w - m.btnSize - m.gap - m.knobR);
    m.trackY     = h / 2;
    m.trackH     = (std::max)(5, m.btnSize / 4);

    double ratio = (d->maxVal > d->minVal)
        ? static_cast<double>(d->val - d->minVal) / static_cast<double>(d->maxVal - d->minVal)
        : 0.0;
    ratio = std::clamp(ratio, 0.0, 1.0);
    m.knobX = m.trackLeft + static_cast<int>(std::lround(ratio * (m.trackRight - m.trackLeft)));
    return m;
}

static int HitTestSlider(const RECT& rc, const CustomSliderData* d, int x, int y) {
    (void)y;
    SliderMetrics m = CalcSliderMetrics(rc, d);
    if (x <= m.btnSize + 2) return 1;
    if (x >= (rc.right - rc.left) - m.btnSize - 2) return 3;
    return 2;
}

static void SetSliderValFromX(HWND hwnd, CustomSliderData* d, int x) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    SliderMetrics m = CalcSliderMetrics(rc, d);
    double ratio = static_cast<double>(x - m.trackLeft) / static_cast<double>(m.trackRight - m.trackLeft);
    ratio = std::clamp(ratio, 0.0, 1.0);
    int newVal = d->minVal + static_cast<int>(std::lround(ratio * (d->maxVal - d->minVal)));
    newVal = std::clamp(newVal, d->minVal, d->maxVal);
    if (newVal != d->val) {
        d->val = newVal;
        InvalidateRect(hwnd, nullptr, FALSE);
        SendMessageW(GetParent(hwnd), WM_APP_SLIDER_CHANGED, static_cast<WPARAM>(d->id), static_cast<LPARAM>(d->val));
    }
}

static void PaintCustomSlider(HWND hwnd, HDC hdc, const CustomSliderData* d) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    const int sw = w * 2;
    const int sh = h * 2;
    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP memBmp = CreateCompatibleBitmap(hdc, sw, sh);
    HGDIOBJ oldBmp = SelectObject(memDC, memBmp);

    RECT sRc{0, 0, sw, sh};
    HBRUSH bgBr = CreateSolidBrush(Theme::BG);
    FillRect(memDC, &sRc, bgBr);
    DeleteObject(bgBr);

    SliderMetrics m = CalcSliderMetrics(rc, d);
    const int btnS = m.btnSize * 2;
    const int btnTop = (sh - btnS) / 2;

    auto drawCircleBtn = [&](int left, bool isPlus, bool hover, bool press) {
        COLORREF fill = press ? Theme::SURFACE_PRESS : (hover ? Theme::SURFACE_HOVER : Theme::SURFACE);
        HBRUSH br = CreateSolidBrush(fill);
        HPEN pen = CreatePen(PS_SOLID, 2, Theme::BORDER);
        HGDIOBJ ob = SelectObject(memDC, br);
        HGDIOBJ op = SelectObject(memDC, pen);
        Ellipse(memDC, left, btnTop, left + btnS, btnTop + btnS);
        SelectObject(memDC, op);
        SelectObject(memDC, ob);
        DeleteObject(pen);
        DeleteObject(br);

        int cx = left + btnS / 2;
        int cy = btnTop + btnS / 2;
        int arm = (std::max)(5, btnS * 22 / 100);
        int thick = (std::max)(3, btnS / 11);
        HBRUSH glyphBr = CreateSolidBrush(Theme::TEXT);
        RECT hBar{cx - arm, cy - thick / 2, cx + arm, cy + (thick - thick / 2)};
        FillRect(memDC, &hBar, glyphBr);
        if (isPlus) {
            RECT vBar{cx - thick / 2, cy - arm, cx + (thick - thick / 2), cy + arm};
            FillRect(memDC, &vBar, glyphBr);
        }
        DeleteObject(glyphBr);
    };

    drawCircleBtn(2, false, d->hoverPart == 1, d->pressPart == 1);
    drawCircleBtn(sw - btnS - 2, true, d->hoverPart == 3, d->pressPart == 3);

    int tL = m.trackLeft * 2;
    int tR = m.trackRight * 2;
    int tY = m.trackY * 2;
    int tH = m.trackH * 2;
    int tTop = tY - tH / 2;
    int tBot = tTop + tH;
    int kX = m.knobX * 2;
    int kR = m.knobR * 2;

    {
        HBRUSH emptyBr = CreateSolidBrush(Theme::TRACK_EMPTY);
        HPEN borderPen = CreatePen(PS_SOLID, 2, Theme::SURFACE);
        HGDIOBJ ob = SelectObject(memDC, emptyBr);
        HGDIOBJ op = SelectObject(memDC, borderPen);
        RoundRect(memDC, tL - kR / 2, tTop, tR + kR / 2, tBot, tH, tH);
        SelectObject(memDC, op);
        SelectObject(memDC, ob);
        DeleteObject(borderPen);
        DeleteObject(emptyBr);
    }

    if (kX > tL) {
        HBRUSH fillBr = CreateSolidBrush(Theme::TRACK_FILL);
        HPEN nullPen = CreatePen(PS_NULL, 0, 0);
        HGDIOBJ ob = SelectObject(memDC, fillBr);
        HGDIOBJ op = SelectObject(memDC, nullPen);
        RoundRect(memDC, tL - kR / 2, tTop + 1, kX, tBot - 1, tH, tH);
        SelectObject(memDC, op);
        SelectObject(memDC, ob);
        DeleteObject(nullPen);
        DeleteObject(fillBr);
    }

    {
        COLORREF kFill = (d->dragging || d->hoverPart == 2) ? Theme::KNOB_HOVER : Theme::KNOB;
        HBRUSH knobBr = CreateSolidBrush(kFill);
        HPEN knobPen = CreatePen(PS_SOLID, 2, Theme::BORDER);
        HGDIOBJ ob = SelectObject(memDC, knobBr);
        HGDIOBJ op = SelectObject(memDC, knobPen);
        Ellipse(memDC, kX - kR, tY - kR, kX + kR, tY + kR);
        SelectObject(memDC, op);
        SelectObject(memDC, ob);
        DeleteObject(knobPen);
        DeleteObject(knobBr);
    }

    SetStretchBltMode(hdc, HALFTONE);
    SetBrushOrgEx(hdc, 0, 0, nullptr);
    StretchBlt(hdc, 0, 0, w, h, memDC, 0, 0, sw, sh, SRCCOPY);

    SelectObject(memDC, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDC);
}

static LRESULT CALLBACK CustomSliderProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* d = reinterpret_cast<CustomSliderData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        d = new CustomSliderData();
        d->id = static_cast<int>(reinterpret_cast<INT_PTR>(cs->hMenu));
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(d));
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        if (d) PaintCustomSlider(hwnd, hdc, d);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!d) break;
        if (!d->trackingMouse) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            d->trackingMouse = true;
        }
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);
        if (d->dragging) {
            SetSliderValFromX(hwnd, d, x);
        } else {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            int part = HitTestSlider(rc, d, x, y);
            if (part != d->hoverPart) {
                d->hoverPart = part;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (d) {
            d->trackingMouse = false;
            d->hoverPart = 0;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN: {
        if (!d) break;
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        int part = HitTestSlider(rc, d, x, y);
        int step = (GetKeyState(VK_SHIFT) & 0x8000) ? 1 : Config::SLIDER_STEP;

        if (part == 1 || part == 3) {
            d->pressPart = part;
            SetCapture(hwnd);
            int newVal = std::clamp(d->val + (part == 3 ? step : -step), d->minVal, d->maxVal);
            if (newVal != d->val) {
                d->val = newVal;
                SendMessageW(GetParent(hwnd), WM_APP_SLIDER_CHANGED, static_cast<WPARAM>(d->id), static_cast<LPARAM>(d->val));
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        } else {
            d->dragging = true;
            SetCapture(hwnd);
            SetSliderValFromX(hwnd, d, x);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (d && (d->dragging || d->pressPart != 0)) {
            d->dragging = false;
            d->pressPart = 0;
            ReleaseCapture();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSEWHEEL: {
        if (!d) break;
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        int step = (GetKeyState(VK_SHIFT) & 0x8000) ? 1 : Config::SLIDER_STEP;
        int newVal = std::clamp(d->val + (delta > 0 ? step : -step), d->minVal, d->maxVal);
        if (newVal != d->val) {
            d->val = newVal;
            InvalidateRect(hwnd, nullptr, FALSE);
            SendMessageW(GetParent(hwnd), WM_APP_SLIDER_CHANGED, static_cast<WPARAM>(d->id), static_cast<LPARAM>(d->val));
        }
        return 0;
    }
    case WM_NCDESTROY:
        delete d;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void ConfigureCustomSlider(HWND hwnd, int minVal, int maxVal, int val) {
    auto* d = reinterpret_cast<CustomSliderData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!d) return;
    d->minVal = minVal;
    d->maxVal = maxVal;
    d->val = std::clamp(val, minVal, maxVal);
    InvalidateRect(hwnd, nullptr, FALSE);
}

static void SetCustomSliderValue(HWND hwnd, int val) {
    auto* d = reinterpret_cast<CustomSliderData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!d) return;
    d->val = std::clamp(val, d->minVal, d->maxVal);
    InvalidateRect(hwnd, nullptr, FALSE);
}

// ==============================================================================
// APPLICATION UI STATE & HELPERS
// ==============================================================================
struct AppState {
    HINSTANCE hInst = nullptr;
    UINT dpi = 96;

    GpuShaderEngine* engine = nullptr;
    PresetStore store;
    std::array<ColorPreset, 3> presets = DEFAULT_PRESETS;

    bool enabled = true;
    int dim = 30;
    int whiteCut = 0;
    int color = 100;

    HWND hMain = nullptr;
    HWND hGuide = nullptr;
    HWND hGuideEdit = nullptr;
    HWND hSave = nullptr;

    HWND hNameDim = nullptr, hValDim = nullptr, hSldDim = nullptr;
    HWND hNameCut = nullptr, hValCut = nullptr, hSldCut = nullptr;
    HWND hNameCol = nullptr, hValCol = nullptr, hSldCol = nullptr;

    HWND hBtnPreset[3]{};
    HWND hBtnToggle = nullptr;

    int saveSlot = 0;
    HWND hSaveRadios[3]{};
    HWND hSaveEdit = nullptr;

    HFONT hFontRegular = nullptr;
    HFONT hFontBold = nullptr;
    HFONT hFontMono = nullptr;
    HBRUSH hBrushBg = nullptr;
    HBRUSH hBrushSurface = nullptr;

    int S(int px) const { return MulDiv(px, static_cast<int>(dpi), 96); }
};

static AppState g_app;

static void EnableDarkTitleBar(HWND hwnd) {
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
}

static void DrawDarkFlatButton(const DRAWITEMSTRUCT* dis) {
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    bool pressed = (dis->itemState & ODS_SELECTED) != 0;

    COLORREF fill = pressed ? Theme::SURFACE_PRESS : Theme::SURFACE;
    COLORREF txtColor = Theme::TEXT;

    if (dis->CtlID == IDC_BTN_TOGGLE) {
        if (g_app.enabled) {
            fill = pressed ? Theme::DANGER_PRESS : Theme::DANGER;     // Red when ON -> "Turn OFF"
        } else {
            fill = pressed ? Theme::SUCCESS_PRESS : Theme::SUCCESS;   // Green when OFF -> "Turn ON"
        }
    } else if (dis->CtlID == IDOK) {
        fill = pressed ? Theme::ACCENT_PRESS : Theme::ACCENT;
    } else if (dis->CtlID == IDC_BTN_GUIDE) {
        txtColor = Theme::ACCENT;
    }

    HBRUSH br = CreateSolidBrush(fill);
    FillRect(hdc, &rc, br);
    DeleteObject(br);

    HPEN pen = CreatePen(PS_SOLID, 1, Theme::BORDER);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);

    wchar_t text[128]{};
    GetWindowTextW(dis->hwndItem, text, 128);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, txtColor);
    HGDIOBJ oldFont = SelectObject(hdc, g_app.hFontBold);
    DrawTextW(hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, oldFont);
}

static void PushStateToEngine() {
    if (!g_app.enabled) {
        SetWindowTextW(g_app.hValDim, L"OFF");
        SetWindowTextW(g_app.hValCut, L"OFF");
        SetWindowTextW(g_app.hValCol, L"OFF");
    } else {
        SetWindowTextW(g_app.hValDim, (std::to_wstring(g_app.dim) + L"%").c_str());
        SetWindowTextW(g_app.hValCut, (std::to_wstring(g_app.whiteCut) + L"%").c_str());
        SetWindowTextW(g_app.hValCol, (std::to_wstring(g_app.color) + L"%").c_str());
    }
    InvalidateRect(g_app.hValDim, nullptr, TRUE);
    InvalidateRect(g_app.hValCut, nullptr, TRUE);
    InvalidateRect(g_app.hValCol, nullptr, TRUE);

    if (g_app.engine) {
        g_app.engine->UpdateParams(g_app.enabled, g_app.dim, g_app.whiteCut, g_app.color);
    }
}

static void SyncSlidersFromState() {
    SetCustomSliderValue(g_app.hSldDim, g_app.dim);
    SetCustomSliderValue(g_app.hSldCut, g_app.whiteCut);
    SetCustomSliderValue(g_app.hSldCol, g_app.color);
    SetWindowTextW(g_app.hBtnToggle, g_app.enabled ? L"Turn OFF" : L"Turn ON");
    InvalidateRect(g_app.hBtnToggle, nullptr, TRUE);
}

static void LoadPreset(int idx) {
    const auto& p = g_app.presets[idx];
    g_app.enabled  = true;
    g_app.dim      = p.dim;
    g_app.whiteCut = p.whiteCut;
    g_app.color    = p.color;
    SyncSlidersFromState();
    PushStateToEngine();
}

// ==============================================================================
// USER-FRIENDLY + TECHNICAL GUIDE POPUP
// ==============================================================================
static std::wstring BuildGuideText() {
    std::wostringstream ss;
    ss << L"DIMMER — QUICK TECHNICAL GUIDE\r\n"
       << L"======================================================\r\n"
       << L"Engine      : D3D11 + DXGI Desktop Duplication Shader\r\n"
       << L"Displays    : " << (g_app.engine ? g_app.engine->MonitorCount() : 0) << L" active monitor(s)\r\n"
       << L"Screenshots : Unmodified (WDA_EXCLUDEFROMCAPTURE)\r\n\r\n"
       << L"1. CONTROLS\r\n"
       << L"------------------------------------------------------\r\n"
       << L"- Dim (0-" << Config::MAX_DIM_PERCENT << L"%)\r\n"
       << L"  Global brightness reduction, capped at " << Config::MAX_DIM_PERCENT << L"% for safety.\r\n\r\n"
       << L"- White Cut (0-100%) — Selective Light-Mode Cutter\r\n"
       << L"  * Variance Gate : Targets neutral whites/greys (RGB\r\n"
       << L"    spread <= 5%); vivid colors stay at 0% cut weight.\r\n"
       << L"  * Curve (100%)  : #FFFFFF -> #A0A0A0, #C0C0C0 ->\r\n"
       << L"   #C0C0C0 -> #808080, exponential shadow roll-off.\r\n"
       << L"- Color (0-100%)\r\n"
       << L"  Rec. 709 perceptual saturation (0% = grayscale).\r\n\r\n"
       << L"- Sliders\r\n"
       << L"  Drag, click -/+, or scroll wheel (" << Config::SLIDER_STEP << L"% steps).\r\n\r\n"
       << L"2. PRESETS & POWER\r\n"
       << L"------------------------------------------------------\r\n"
       << L"Defaults (Dim / White Cut / Color):\r\n"
       << L"  Low: 30/0/100 | Medium: 30/60/100 | High: 60/100/100\r\n\r\n"
       << L"- Left-click a preset to apply; right-click or use\r\n"
       << L"  'Save Preset' to edit or reset a slot.\r\n"
       << L"- Dimmer.json is created ONLY for modified slots.\r\n"
       << L"  Delete it anytime to restore built-in defaults.\r\n"
       << L"- Turn OFF (Red) pauses overlay (0% GPU); Turn ON\r\n"
       << L"  (Green) resumes. Close (X) exits and cleans up.\r\n";
    return ss.str();
}

static LRESULT CALLBACK GuideWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE: {
        int w = LOWORD(lParam);
        int h = HIWORD(lParam);
        int m = g_app.S(12);
        int btnH = g_app.S(28);
        int btnW = g_app.S(90);
        if (g_app.hGuideEdit) MoveWindow(g_app.hGuideEdit, m, m, w - 2 * m, h - 3 * m - btnH, TRUE);
        HWND hClose = GetDlgItem(hwnd, IDC_GUIDE_CLOSE);
        if (hClose) MoveWindow(hClose, (w - btnW) / 2, h - m - btnH, btnW, btnH, TRUE);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetTextColor(hdc, Theme::TEXT);
        SetBkColor(hdc, Theme::SURFACE);
        return reinterpret_cast<INT_PTR>(g_app.hBrushSurface);
    }
    case WM_DRAWITEM:
        DrawDarkFlatButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_GUIDE_CLOSE) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_DESTROY:
        g_app.hGuide = nullptr;
        g_app.hGuideEdit = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void OpenGuideWindow() {
    if (g_app.hGuide && IsWindow(g_app.hGuide)) {
        SetForegroundWindow(g_app.hGuide);
        return;
    }

    RECT rc{0, 0, g_app.S(545), g_app.S(465)};
    DWORD style = WS_OVERLAPPEDWINDOW;
    AdjustWindowRectExForDpi(&rc, style, FALSE, 0, g_app.dpi);

    g_app.hGuide = CreateWindowExW(
        0, L"DimmerGuideClass", L"Dimmer — Guide",
        style, CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        g_app.hMain, nullptr, g_app.hInst, nullptr
    );
    EnableDarkTitleBar(g_app.hGuide);

    g_app.hGuideEdit = CreateWindowExW(
        0, L"EDIT", BuildGuideText().c_str(),
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        0, 0, 100, 100, g_app.hGuide,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_GUIDE_TEXT)), g_app.hInst, nullptr
    );
    SendMessageW(g_app.hGuideEdit, WM_SETFONT, reinterpret_cast<WPARAM>(g_app.hFontMono), TRUE);

    CreateWindowExW(
        0, L"BUTTON", L"Close",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
        0, 0, 80, 28, g_app.hGuide,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_GUIDE_CLOSE)), g_app.hInst, nullptr
    );

    RECT client{};
    GetClientRect(g_app.hGuide, &client);
    SendMessageW(g_app.hGuide, WM_SIZE, 0, MAKELPARAM(client.right, client.bottom));
    ShowWindow(g_app.hGuide, SW_SHOW);
}

// ==============================================================================
// SAVE / RESET CUSTOM PRESET POPUP
// ==============================================================================
static void CloseSaveDialog() {
    if (g_app.hSave) {
        EnableWindow(g_app.hMain, TRUE);
        DestroyWindow(g_app.hSave);
        g_app.hSave = nullptr;
        SetForegroundWindow(g_app.hMain);
    }
}

static LRESULT CALLBACK SaveDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetTextColor(hdc, (GetDlgCtrlID(reinterpret_cast<HWND>(lParam)) == IDC_SAVE_SUMMARY) ? Theme::MUTED : Theme::TEXT);
        SetBkColor(hdc, Theme::BG);
        return reinterpret_cast<INT_PTR>(g_app.hBrushBg);
    }
    case WM_CTLCOLOREDIT: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetTextColor(hdc, Theme::TEXT);
        SetBkColor(hdc, Theme::SURFACE);
        return reinterpret_cast<INT_PTR>(g_app.hBrushSurface);
    }
    case WM_DRAWITEM:
        DrawDarkFlatButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
        return TRUE;
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id >= IDC_SAVE_RADIO_0 && id <= IDC_SAVE_RADIO_2) {
            g_app.saveSlot = id - IDC_SAVE_RADIO_0;
            SetWindowTextW(g_app.hSaveEdit, g_app.presets[g_app.saveSlot].name.c_str());
            return 0;
        }
        if (id == IDOK) {
            wchar_t buf[32]{};
            GetWindowTextW(g_app.hSaveEdit, buf, 32);
            std::wstring name(buf);
            size_t first = name.find_first_not_of(L" \t");
            size_t last  = name.find_last_not_of(L" \t");
            name = (first == std::wstring::npos) ? L"" : name.substr(first, last - first + 1);
            if (name.empty()) name = DEFAULT_PRESETS[g_app.saveSlot].name;
            if (name.size() > 10) name.resize(10);

            g_app.presets[g_app.saveSlot] = {name, g_app.dim, g_app.whiteCut, g_app.color};
            SetWindowTextW(g_app.hBtnPreset[g_app.saveSlot], name.c_str());
            InvalidateRect(g_app.hBtnPreset[g_app.saveSlot], nullptr, TRUE);

            if (!g_app.store.Save(g_app.presets)) {
                MessageBoxW(hwnd, L"Updated in memory, but failed to write Dimmer.json.", L"Dimmer", MB_ICONWARNING);
            }
            CloseSaveDialog();
            return 0;
        }
        if (id == IDC_SAVE_RESET) {
            g_app.presets[g_app.saveSlot] = DEFAULT_PRESETS[g_app.saveSlot];
            SetWindowTextW(g_app.hBtnPreset[g_app.saveSlot], g_app.presets[g_app.saveSlot].name.c_str());
            InvalidateRect(g_app.hBtnPreset[g_app.saveSlot], nullptr, TRUE);
            g_app.store.Save(g_app.presets);
            LoadPreset(g_app.saveSlot);
            CloseSaveDialog();
            return 0;
        }
        if (id == IDCANCEL) {
            CloseSaveDialog();
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        CloseSaveDialog();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void OpenSaveDialog(int preselectSlot) {
    if (g_app.hSave && IsWindow(g_app.hSave)) CloseSaveDialog();
    g_app.saveSlot = std::clamp(preselectSlot, 0, 2);

    int cw = g_app.S(320);
    int ch = g_app.S(192);
    RECT rc{0, 0, cw, ch};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRectExForDpi(&rc, style, FALSE, 0, g_app.dpi);

    RECT mainRc{};
    GetWindowRect(g_app.hMain, &mainRc);

    g_app.hSave = CreateWindowExW(
        0, L"DimmerSaveDlgClass", L"Save Custom Preset",
        style, mainRc.left + g_app.S(20), mainRc.top + g_app.S(30),
        rc.right - rc.left, rc.bottom - rc.top,
        g_app.hMain, nullptr, g_app.hInst, nullptr
    );
    EnableDarkTitleBar(g_app.hSave);

    int pad = g_app.S(14);
    int innerW = cw - 2 * pad;
    int y = g_app.S(12);

    HWND lbl1 = CreateWindowExW(0, L"STATIC", L"Choose slot to overwrite:",
        WS_CHILD | WS_VISIBLE, pad, y, innerW, g_app.S(18), g_app.hSave, nullptr, g_app.hInst, nullptr);
    SendMessageW(lbl1, WM_SETFONT, reinterpret_cast<WPARAM>(g_app.hFontBold), TRUE);
    y += g_app.S(22);

    int radioW = innerW / 3;
    for (int i = 0; i < 3; ++i) {
        std::wstring label = L"#" + std::to_wstring(i + 1) + L" " + g_app.presets[i].name;
        DWORD rStyle = WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | (i == 0 ? WS_GROUP : 0);
        g_app.hSaveRadios[i] = CreateWindowExW(
            0, L"BUTTON", label.c_str(), rStyle,
            pad + i * radioW, y, radioW, g_app.S(22), g_app.hSave,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SAVE_RADIO_0 + i)), g_app.hInst, nullptr
        );
        SendMessageW(g_app.hSaveRadios[i], WM_SETFONT, reinterpret_cast<WPARAM>(g_app.hFontRegular), TRUE);
        SetWindowTheme(g_app.hSaveRadios[i], L"", L"");
    }
    SendMessageW(g_app.hSaveRadios[g_app.saveSlot], BM_SETCHECK, BST_CHECKED, 0);
    y += g_app.S(28);

    HWND lbl2 = CreateWindowExW(0, L"STATIC", L"Preset name (max 10 chars):",
        WS_CHILD | WS_VISIBLE, pad, y, innerW, g_app.S(18), g_app.hSave, nullptr, g_app.hInst, nullptr);
    SendMessageW(lbl2, WM_SETFONT, reinterpret_cast<WPARAM>(g_app.hFontBold), TRUE);
    y += g_app.S(22);

    g_app.hSaveEdit = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", g_app.presets[g_app.saveSlot].name.c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        pad, y, innerW, g_app.S(26), g_app.hSave,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SAVE_EDIT_NAME)), g_app.hInst, nullptr
    );
    SendMessageW(g_app.hSaveEdit, WM_SETFONT, reinterpret_cast<WPARAM>(g_app.hFontRegular), TRUE);
    SendMessageW(g_app.hSaveEdit, EM_SETLIMITTEXT, 10, 0);
    y += g_app.S(32);

    std::wstring summary = L"Dim " + std::to_wstring(g_app.dim) + L"%  |  White Cut " +
        std::to_wstring(g_app.whiteCut) + L"%  |  Color " + std::to_wstring(g_app.color) + L"%";
    HWND lblSum = CreateWindowExW(0, L"STATIC", summary.c_str(),
        WS_CHILD | WS_VISIBLE, pad, y, innerW, g_app.S(18), g_app.hSave,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SAVE_SUMMARY)), g_app.hInst, nullptr);
    SendMessageW(lblSum, WM_SETFONT, reinterpret_cast<WPARAM>(g_app.hFontRegular), TRUE);
    y += g_app.S(26);

    int btnGap = g_app.S(6);
    int btnW = (innerW - 2 * btnGap) / 3;
    CreateWindowExW(0, L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
        pad, y, btnW, g_app.S(28), g_app.hSave, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), g_app.hInst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"Reset Slot", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
        pad + btnW + btnGap, y, btnW, g_app.S(28), g_app.hSave, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SAVE_RESET)), g_app.hInst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
        pad + 2 * (btnW + btnGap), y, btnW, g_app.S(28), g_app.hSave, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)), g_app.hInst, nullptr);

    EnableWindow(g_app.hMain, FALSE);
    ShowWindow(g_app.hSave, SW_SHOW);
    SetFocus(g_app.hSaveEdit);
    SendMessageW(g_app.hSaveEdit, EM_SETSEL, 0, -1);
}

// ==============================================================================
// OVERLAY & MAIN WINDOW PROCEDURES
// ==============================================================================
static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        HWND ctl = reinterpret_cast<HWND>(lParam);
        bool isVal = (ctl == g_app.hValDim || ctl == g_app.hValCut || ctl == g_app.hValCol);
        COLORREF color = (!g_app.enabled && isVal) ? Theme::MUTED : Theme::TEXT;
        SetTextColor(hdc, color);
        SetBkColor(hdc, Theme::BG);
        return reinterpret_cast<INT_PTR>(g_app.hBrushBg);
    }
    case WM_DRAWITEM:
        DrawDarkFlatButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
        return TRUE;
    case WM_APP_SLIDER_CHANGED: {
        int id  = static_cast<int>(wParam);
        int val = static_cast<int>(lParam);
        if (id == IDC_SLIDER_DIM) g_app.dim = val;
        else if (id == IDC_SLIDER_CUT) g_app.whiteCut = val;
        else if (id == IDC_SLIDER_COLOR) g_app.color = val;
        PushStateToEngine();
        return 0;
    }
    case WM_APP_OVERLAY_VISIBILITY:
        if (g_app.engine) {
            g_app.engine->HandleOverlayVisibilityMsg(static_cast<size_t>(wParam), lParam != 0);
        }
        return 0;
    case WM_TIMER:
        if (wParam == TIMER_ZORDER_WATCH && g_app.engine) {
            g_app.engine->MaintainTopmost();
            return 0;
        }
        break;
    case WM_CONTEXTMENU: {
        HWND target = reinterpret_cast<HWND>(wParam);
        for (int i = 0; i < 3; ++i) {
            if (target == g_app.hBtnPreset[i]) {
                OpenSaveDialog(i);
                return 0;
            }
        }
        break;
    }
    case WM_COMMAND: {
        switch (LOWORD(wParam)) {
        case IDC_BTN_GUIDE:    OpenGuideWindow(); return 0;
        case IDC_BTN_SAVE:     OpenSaveDialog(0); return 0;
        case IDC_BTN_PRESET_0: LoadPreset(0);     return 0;
        case IDC_BTN_PRESET_1: LoadPreset(1);     return 0;
        case IDC_BTN_PRESET_2: LoadPreset(2);     return 0;
        case IDC_BTN_TOGGLE:
            g_app.enabled = !g_app.enabled;
            SetWindowTextW(g_app.hBtnToggle, g_app.enabled ? L"Turn OFF" : L"Turn ON");
            InvalidateRect(g_app.hBtnToggle, nullptr, TRUE);
            PushStateToEngine();
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        // Safe Exit via top-right "X": close popups, stop GPU worker & remove overlays cleanly
        KillTimer(hwnd, TIMER_ZORDER_WATCH);
        if (g_app.hGuide && IsWindow(g_app.hGuide)) DestroyWindow(g_app.hGuide);
        if (g_app.hSave && IsWindow(g_app.hSave)) DestroyWindow(g_app.hSave);
        if (g_app.engine) g_app.engine->Shutdown();
        DestroyWindow(hwnd);
        return 0;
    case WM_ENDSESSION:
        if (wParam && g_app.engine) g_app.engine->Shutdown();
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_ZORDER_WATCH);
        if (g_app.engine) g_app.engine->Shutdown();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ==============================================================================
// ENTRY POINT (wWinMain -> ZERO CONSOLE WINDOW)
// ==============================================================================
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"Local\\DimmerGpuSingleInstanceMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND hExisting = FindWindowW(MAIN_WINDOW_CLASS, nullptr);
        if (hExisting) {
            if (IsIconic(hExisting)) ShowWindow(hExisting, SW_RESTORE);
            SetForegroundWindow(hExisting);
        }
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    g_app.hInst   = hInst;
    g_app.dpi     = GetDpiForSystem();
    g_app.presets = g_app.store.Load();

    // Boot into Slot #1 (Low: Dim 30%, White Cut 0%, Color 100%)
    g_app.dim      = g_app.presets[0].dim;
    g_app.whiteCut = g_app.presets[0].whiteCut;
    g_app.color    = g_app.presets[0].color;

    int dpiInt = static_cast<int>(g_app.dpi);
    g_app.hBrushBg      = CreateSolidBrush(Theme::BG);
    g_app.hBrushSurface = CreateSolidBrush(Theme::SURFACE);
    g_app.hFontRegular  = CreateFontW(-MulDiv(9, dpiInt, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_app.hFontBold     = CreateFontW(-MulDiv(9, dpiInt, 72), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_app.hFontMono     = CreateFontW(-MulDiv(9, dpiInt, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Consolas");

    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = g_app.hBrushBg;

    wc.lpfnWndProc   = MainWndProc;
    wc.lpszClassName = MAIN_WINDOW_CLASS;
    RegisterClassExW(&wc);

    wc.lpfnWndProc   = GuideWndProc;
    wc.lpszClassName = L"DimmerGuideClass";
    RegisterClassExW(&wc);

    wc.lpfnWndProc   = SaveDlgProc;
    wc.lpszClassName = L"DimmerSaveDlgClass";
    RegisterClassExW(&wc);

    wc.lpfnWndProc   = CustomSliderProc;
    wc.lpszClassName = SLIDER_WINDOW_CLASS;
    RegisterClassExW(&wc);

    wc.lpfnWndProc   = OverlayWndProc;
    wc.hbrBackground = nullptr;
    wc.lpszClassName = OVERLAY_WINDOW_CLASS;
    RegisterClassExW(&wc);

    // Compact layout (exact height computed automatically after laying out all rows!)
    int pad    = g_app.S(14);
    int innerW = g_app.S(250);
    int cw     = innerW + 2 * pad;

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    g_app.hMain = CreateWindowExW(
        0, MAIN_WINDOW_CLASS, L"Dimmer",
        style, g_app.S(40), g_app.S(40), cw, g_app.S(260),
        nullptr, nullptr, hInst, nullptr
    );
    EnableDarkTitleBar(g_app.hMain);

    int y = g_app.S(10);

    // Header Row: "Dimmer" (left) + "? Guide" (right)
    HWND hTitle = CreateWindowExW(0, L"STATIC", L"Dimmer",
        WS_CHILD | WS_VISIBLE, pad, y + g_app.S(2), g_app.S(110), g_app.S(18),
        g_app.hMain, nullptr, hInst, nullptr);
    SendMessageW(hTitle, WM_SETFONT, reinterpret_cast<WPARAM>(g_app.hFontBold), TRUE);

    CreateWindowExW(0, L"BUTTON", L"? Guide",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
        pad + innerW - g_app.S(64), y, g_app.S(64), g_app.S(22),
        g_app.hMain, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_GUIDE)), hInst, nullptr);
    y += g_app.S(28);

    // Helper: Centered title + Right-aligned value + Compact "-" [slider] "+" row
    auto addSliderRow = [&](const wchar_t* title, int id, int maxVal, int initVal,
                            HWND& outName, HWND& outVal, HWND& outSld) {
        int valW = g_app.S(48);
        outName = CreateWindowExW(0, L"STATIC", title,
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            pad + valW, y, innerW - 2 * valW, g_app.S(16), g_app.hMain, nullptr, hInst, nullptr);
        SendMessageW(outName, WM_SETFONT, reinterpret_cast<WPARAM>(g_app.hFontBold), TRUE);

        outVal = CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_RIGHT,
            pad + innerW - valW, y, valW, g_app.S(16), g_app.hMain, nullptr, hInst, nullptr);
        SendMessageW(outVal, WM_SETFONT, reinterpret_cast<WPARAM>(g_app.hFontBold), TRUE);
        y += g_app.S(17);

        outSld = CreateWindowExW(0, SLIDER_WINDOW_CLASS, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            pad, y, innerW, g_app.S(22), g_app.hMain,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst, nullptr);
        ConfigureCustomSlider(outSld, 0, maxVal, initVal);
        y += g_app.S(27);
    };

    addSliderRow(L"Dim",       IDC_SLIDER_DIM,   Config::MAX_DIM_PERCENT, g_app.dim,      g_app.hNameDim, g_app.hValDim, g_app.hSldDim);
    addSliderRow(L"White Cut", IDC_SLIDER_CUT,   100,                     g_app.whiteCut, g_app.hNameCut, g_app.hValCut, g_app.hSldCut);
    addSliderRow(L"Color",     IDC_SLIDER_COLOR, 100,                     g_app.color,    g_app.hNameCol, g_app.hValCol, g_app.hSldCol);

    y += g_app.S(4);

    // Button Row 1 (3 buttons): Low | Medium | High
    int pGap = g_app.S(6);
    int pW   = (innerW - 2 * pGap) / 3;
    int pH   = g_app.S(25);
    for (int i = 0; i < 3; ++i) {
        g_app.hBtnPreset[i] = CreateWindowExW(
            0, L"BUTTON", g_app.presets[i].name.c_str(),
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
            pad + i * (pW + pGap), y, pW, pH,
            g_app.hMain, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_PRESET_0 + i)), hInst, nullptr
        );
    }
    y += pH + g_app.S(6);

    // Button Row 2 (2 buttons): Save Preset | Turn OFF (red) / Turn ON (green)
    int aGap = g_app.S(6);
    int aW   = (innerW - aGap) / 2;
    int aH   = g_app.S(27);
    CreateWindowExW(0, L"BUTTON", L"Save Preset",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
        pad, y, aW, aH,
        g_app.hMain, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_SAVE)), hInst, nullptr);

    g_app.hBtnToggle = CreateWindowExW(0, L"BUTTON", L"Turn OFF",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
        pad + aW + aGap, y, aW, aH,
        g_app.hMain, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_TOGGLE)), hInst, nullptr);
    y += aH;

    // Auto-fit exact client height from final control bottom + padding (zero clipping!)
    RECT fitRc{0, 0, cw, y + pad};
    AdjustWindowRectExForDpi(&fitRc, style, FALSE, 0, g_app.dpi);
    SetWindowPos(g_app.hMain, nullptr, 0, 0,
        fitRc.right - fitRc.left, fitRc.bottom - fitRc.top,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    // Initialize Direct3D 11 + DXGI Desktop Duplication GPU engine
    GpuShaderEngine engine;
    std::wstring initErr;
    if (!engine.Init(hInst, g_app.hMain, initErr)) {
        MessageBoxW(g_app.hMain, initErr.c_str(), L"Dimmer — GPU Init Error", MB_ICONERROR);
        DestroyWindow(g_app.hMain);
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }
    g_app.engine = &engine;

    SyncSlidersFromState();
    PushStateToEngine();

    SetTimer(g_app.hMain, TIMER_ZORDER_WATCH, 250, nullptr);
    ShowWindow(g_app.hMain, nCmdShow);
    UpdateWindow(g_app.hMain);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (g_app.hSave && IsDialogMessageW(g_app.hSave, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DeleteObject(g_app.hFontRegular);
    DeleteObject(g_app.hFontBold);
    DeleteObject(g_app.hFontMono);
    DeleteObject(g_app.hBrushBg);
    DeleteObject(g_app.hBrushSurface);
    if (hMutex) CloseHandle(hMutex);
    return 0;
}