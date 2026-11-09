// EVE1G Settings - the settings window for EVE1G.ini and preset.json.
// Lives in plugins\EVE1G next to those files. Changes apply to the next export (EVE1G re-reads them every time).
// The window draws its own dark, purple-accented controls (GDI+), so it looks the same on Windows 10 and 11.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <CommCtrl.h>
#include <ShlObj.h>
#include <shellapi.h>
#include <Uxtheme.h>
#include <dwmapi.h>
#include <objidl.h>

#include <algorithm>
namespace Gdiplus {
using std::max;
using std::min;
} // namespace Gdiplus
#include <gdiplus.h>

#include <INIReader.h>
#include <wrl/client.h>
#include "VoukoderTypeLib_h.h"
#include <nlohmann/json.hpp>
#include "../gta5-extended-video-export/version.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

const wchar_t* kApp = L"EVE1G";
const wchar_t* kGitHub = L"https://github.com/tex1gprod-cloud/EVE1G";

// ---------------------------------------------------------------------------------------------------- codec presets
// Grouped by what the file is for, not by codec name.
enum class Q { None, Crf, Cq };
struct Codec {
    const wchar_t* label;
    const char* encoder;
    const char* container;
    const char* options; // %d = quality where used
    Q quality;
    int defQuality;
    bool bitrate;        // NVENC: optional target bitrate
    const char* audioEncoder;
    const char* audioOptions;
    const wchar_t* note;
};

const Codec kCodecs[] = {
    {L"Edit master  -  ProRes 422 HQ  (.mov, 10-bit 4:2:2)", "prores_ks", "mov", "_pixelFormat=yuv422p10le|profile=3",
     Q::None, 0, false, "pcm_s16le", "_sampleFormat=s16",
     L"Best for After Effects / Premiere / Resolve. Grades without banding. About 700 Mbit/s at 1080p."},
    {L"Grade master  -  ProRes 4444  (.mov, 10-bit 4:4:4)", "prores_ks", "mov", "_pixelFormat=yuv444p10le|profile=4",
     Q::None, 0, false, "pcm_s16le", "_sampleFormat=s16",
     L"Full colour resolution, for heavy grading, keying and VFX. About 1.5x the size of 422 HQ."},
    {L"Light master  -  ProRes 422 LT  (.mov, 10-bit)", "prores_ks", "mov", "_pixelFormat=yuv422p10le|profile=1",
     Q::None, 0, false, "pcm_s16le", "_sampleFormat=s16", L"About half the size of 422 HQ. Fine for light grading."},
    {L"GPU master  -  HEVC NVENC 4:2:2 10-bit  (.mp4, RTX 50)", "hevc_nvenc", "mp4",
     "_pixelFormat=p210le|preset=p7|rc=vbr|cq=%d|profile=rext", Q::Cq, 16, true, "aac",
     "_sampleFormat=fltp|b=320000|profile=aac_low",
     L"Near-ProRes quality at a fraction of the size, encoded on the RTX 50 GPU. Needs a Voukoder build with "
     L"FFmpeg 7.1+. If the export fails, use ProRes. CQ: lower = better."},
    {L"GPU delivery  -  HEVC NVENC 10-bit  (.mp4)", "hevc_nvenc", "mp4", "_pixelFormat=p010le|preset=p7|rc=vbr|cq=%d",
     Q::Cq, 18, true, "aac", "_sampleFormat=fltp|b=320000|profile=aac_low",
     L"Fast 10-bit 4:2:0 on the GPU. Good for client previews. CQ: lower = better. Bitrate 0 = quality only."},
    {L"GPU fast  -  H.264 NVENC  (.mp4)", "h264_nvenc", "mp4", "_pixelFormat=yuv420p|preset=p7|rc=vbr|cq=%d", Q::Cq, 16,
     true, "aac", "_sampleFormat=fltp|b=320000|profile=aac_low",
     L"Plays everywhere. 8-bit - avoid for grading. CQ: lower = better. Bitrate 0 = quality only."},
    {L"Upload  -  H.264 x264  (.mp4, CRF)", "libx264", "mp4", "_pixelFormat=yuv420p|rc=crf|preset=slow|crf=%d", Q::Crf,
     14, false, "aac", "_sampleFormat=fltp|b=320000|profile=aac_low",
     L"CPU encode. CRF: lower = better. 12-16 near-lossless, 18-20 for upload."},
    {L"Upload small  -  H.265 x265  (.mp4, 10-bit CRF)", "libx265", "mp4",
     "_pixelFormat=yuv420p10le|rc=crf|preset=slow|crf=%d", Q::Crf, 16, false, "aac",
     "_sampleFormat=fltp|b=320000|profile=aac_low", L"Smaller than H.264 at the same quality. Slow CPU encode."},
    {L"Avid / Resolve  -  DNxHR HQX  (.mov, 10-bit)", "dnxhd", "mov", "_pixelFormat=yuv422p10le|profile=dnxhr_hqx",
     Q::None, 0, false, "pcm_s16le", "_sampleFormat=s16", L"Intermediate codec for Avid and Resolve."},
    {L"Avid / Resolve  -  DNxHR HQ  (.mov, 8-bit)", "dnxhd", "mov", "_pixelFormat=yuv422p|profile=dnxhr_hq", Q::None, 0,
     false, "pcm_s16le", "_sampleFormat=s16", L"Smaller than HQX, 8-bit."},
    {L"Archive  -  FFV1 lossless  (.mkv, 10-bit)", "ffv1", "mkv", "_pixelFormat=yuv444p10le|level=3|slicecrc=1",
     Q::None, 0, false, "flac", "_sampleFormat=s16", L"Bit-exact archive. Very large files."},
    {L"Archive  -  UT Video lossless RGB  (.avi)", "utvideo", "avi", "_pixelFormat=gbrp", Q::None, 0, false,
     "pcm_s16le", "_sampleFormat=s16", L"Lossless RGB that editors read straight off the timeline."},
    {L"Custom  -  set in Voukoder's own window", nullptr, nullptr, nullptr, Q::None, 0, false, nullptr, nullptr,
     L"Keeps preset.json exactly as the Voukoder window saved it."},
};
constexpr int kCustom = static_cast<int>(sizeof(kCodecs) / sizeof(kCodecs[0])) - 1;
// Stable names for formats.ini (same order as kCodecs, without Custom)
const char* kCodecIds[] = {"prores_422hq", "prores_4444", "prores_422lt", "hevc_nvenc_422", "hevc_nvenc_10bit",
                           "h264_nvenc",   "x264",        "x265",         "dnxhr_hqx",      "dnxhr_hq",
                           "ffv1",         "utvideo"};
static_assert(sizeof(kCodecIds) / sizeof(kCodecIds[0]) == kCustom, "one id per preset");

struct Fps { const wchar_t* label; const char* value; double rate; };
const Fps kFps[] = {
    {L"23.976", "24000/1001", 24000.0 / 1001.0}, {L"24", "24", 24}, {L"25", "25", 25},
    {L"29.97", "30000/1001", 30000.0 / 1001.0},  {L"30", "30", 30}, {L"48", "48", 48},
    {L"50", "50", 50}, {L"59.94", "60000/1001", 60000.0 / 1001.0}, {L"60", "60", 60},
};
constexpr int kFpsCount = static_cast<int>(sizeof(kFps) / sizeof(kFps[0]));

struct Shutter { const wchar_t* label; double strength; };
const Shutter kShutter[] = {
    {L"45°  - very crisp", 0.125}, {L"90°", 0.25},   {L"144°  - crisp film", 0.4},
    {L"172.8°  - film (50 Hz)", 0.48}, {L"180°  - film standard", 0.5},
    {L"270°", 0.75}, {L"360°  - maximum blur", 1.0},
};

// Rolling shutter: how long a CMOS sensor takes to read the image top to bottom.
struct Rolling { const wchar_t* label; double ms; };
const Rolling kRolling[] = {
    {L"Off - global shutter (cinema camera)", 0}, {L"8 ms - fast sensor", 8},
    {L"16 ms - mirrorless / DSLR", 16}, {L"25 ms - phone", 25},
};

struct Choice { const wchar_t* label; const char* value; };
const Choice kCapture[] = {
    {L"Clean  -  before the game UI, with ReShade  [recommended]", "clean"},
    {L"Screen  -  what is on screen (progress bar + cursor)", "present"},
    {L"Rockstar  -  the editor's own export image, no ReShade", "encode"},
};
const Choice kLog[] = {{L"error", "error"}, {L"warn", "warn"}, {L"info", "info"}, {L"debug", "debug"}};
const Choice kHfrFormat[] = {
    {L"ProRes 422 HQ video  -  one .mov, 10-bit  [recommended]", "prores"},
    {L"PNG sequence  -  lossless 8-bit frames", "png"},
};
struct Every { const wchar_t* label; int n; };
const Every kHfrEvery[] = {
    {L"Every game frame", 1}, {L"Every 2nd (half the frame rate)", 2}, {L"Every 4th (quarter)", 4},
};

const Choice kDepthFormat[] = {
    {L"PNG 16-bit  -  lossless sequence for AE / Premiere / Resolve  [recommended]", "png16"},
    {L"OpenEXR float  -  real distance in metres (Nuke / Fusion)", "exr"},
};
const Choice kMaster[] = {
    {L"8-bit  -  as original EVE, the safest", "false"},
    {L"Auto  -  16-bit into 10-bit and lossless formats", "auto"},
    {L"16-bit  -  always, the smoothest gradients", "true"},
};
const Choice kOsdSize[] = {
    {L"Normal  -  grows with the screen, 2x at 4K", "1"},
    {L"Large", "1.5"},
    {L"Extra large", "2"},
};
const Choice kDepthCurve[] = {{L"Log  -  detail close up", "log"}, {L"Linear", "linear"}};
const Choice kDepthWhite[] = {{L"White = near", "near"}, {L"White = far", "far"}};

// ------------------------------------------------------------------------------------------------------------ state
std::string g_dir;   // folder of this exe, with trailing backslash
HWND g_wnd;
HFONT g_font, g_small, g_semi, g_cardTitle, g_pageTitle, g_brand, g_tag, g_num;
double g_scale = 1.0;
HWND hOut, hProject, hShot, hTake, hSidecar, hPreview;
HWND hCodec, hQuality, hQualityLabel, hBitrate, hBitrateLabel, hNote, hFormats;
HWND hFps, hFpsCustom, hSamples, hShutter, hInfo, hRolling;
HWND hCapture, hReshade, hLivePreview, hProgress, hLog;
HWND hDepthOn, hDepthFormat, hDepthNear, hDepthFar, hDepthAuto, hDepthCurve, hDepthWhite, hDepthNote;
HWND hNormalsOn, hNormalsFov, hPlateOn, hProxyOn, hPassNote, hMaster, hNotify, hCaptureNote, hOsdSize;
HWND hStabilizer;
HWND hCamFiles, hCamFolder;
HWND hStatus, hGitHub;
std::unordered_map<HWND, bool> g_toggle;   // the on / off state of every switch

enum : int {
    ID_BROWSE = 100, ID_FPS, ID_SAMPLES, ID_SHUTTER, ID_CODEC, ID_SAVE, ID_CLOSE, ID_VOUKODER, ID_OPENFOLDER,
    ID_OPENLOG, ID_NAMING, ID_CAPTURE, ID_TESTFORMATS, ID_DEPTH, ID_GITHUB, ID_NAV = 200
};

std::wstring W(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
std::string N(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring GetText(HWND h) {
    const int n = GetWindowTextLengthW(h);
    std::wstring s(n, 0);
    GetWindowTextW(h, s.data(), n + 1);
    return s;
}
std::string Trim(std::string s) {
    while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && isspace((unsigned char)s[i])) ++i;
    return s.substr(i);
}
int Sel(HWND h) { return (int)SendMessageW(h, CB_GETCURSEL, 0, 0); }
bool Checked(HWND h) {
    const auto it = g_toggle.find(h);
    return it != g_toggle.end() ? it->second : SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
}
void Check(HWND h, bool on) {
    const auto it = g_toggle.find(h);
    if (it == g_toggle.end()) {
        SendMessageW(h, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
    } else if (it->second != on) {
        it->second = on;
        InvalidateRect(h, nullptr, FALSE);
    }
}

// The preset combo only lists formats that work on this PC; item data = index into kCodecs.
int CodecSel() {
    const int i = Sel(hCodec);
    return i < 0 ? -1 : (int)SendMessageW(hCodec, CB_GETITEMDATA, i, 0);
}
void SelectCodec(int idx) {
    const int n = (int)SendMessageW(hCodec, CB_GETCOUNT, 0, 0);
    int custom = 0;
    for (int i = 0; i < n; ++i) {
        const int d = (int)SendMessageW(hCodec, CB_GETITEMDATA, i, 0);
        if (d == idx) {
            SendMessageW(hCodec, CB_SETCURSEL, i, 0);
            return;
        }
        if (d == kCustom) custom = i;
    }
    SendMessageW(hCodec, CB_SETCURSEL, custom, 0);
}

// ------------------------------------------------------------------------------------------------------------- theme
// Monochrome: black, white and greys only. White marks what is on / chosen / the main action.
// Every control is drawn here, not by Windows, so it looks the same on Windows 10 and 11.
namespace col {
constexpr COLORREF bg = RGB(17, 17, 18);          // page
constexpr COLORREF side = RGB(10, 10, 11);        // sidebar
constexpr COLORREF foot = RGB(17, 17, 18);        // bottom bar (same as the page)
constexpr COLORREF card = RGB(26, 26, 28);
constexpr COLORREF cardLine = RGB(33, 33, 36);
constexpr COLORREF input = RGB(37, 37, 40);
constexpr COLORREF inputLine = RGB(37, 37, 40);   // no visible border at rest
constexpr COLORREF inputHover = RGB(70, 70, 75);
constexpr COLORREF button = RGB(40, 40, 43);
constexpr COLORREF buttonHover = RGB(54, 54, 58);
constexpr COLORREF accent = RGB(242, 242, 243);   // white: on, chosen, the main action
constexpr COLORREF accentHi = RGB(255, 255, 255);
constexpr COLORREF accentLo = RGB(208, 208, 212);
constexpr COLORREF accentSoft = RGB(50, 50, 54);  // chosen row in a list, tags
constexpr COLORREF accentText = RGB(236, 236, 238);
constexpr COLORREF ink = RGB(12, 12, 13);         // text and knobs on white
constexpr COLORREF text = RGB(242, 242, 243);
constexpr COLORREF text2 = RGB(160, 160, 166);
constexpr COLORREF hint = RGB(118, 118, 124);
constexpr COLORREF dim = RGB(74, 74, 79);         // disabled
constexpr COLORREF track = RGB(56, 56, 60);       // switch, off
constexpr COLORREF line = RGB(36, 36, 39);
constexpr COLORREF good = RGB(242, 242, 243);
constexpr COLORREF bad = RGB(255, 120, 120);
constexpr COLORREF navHover = RGB(24, 24, 26);
constexpr COLORREF navSel = RGB(242, 242, 243);
} // namespace col
HBRUSH g_brBg, g_brSide, g_brFoot, g_brCard, g_brInput, g_brSoft;

HBRUSH BrushOf(COLORREF c) {
    if (c == col::side) return g_brSide;
    if (c == col::card) return g_brCard;
    if (c == col::input) return g_brInput;
    return g_brBg;
}

Gdiplus::Color GC(COLORREF c, BYTE a = 255) { return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c)); }

// Anti-aliased rounded rectangle, optionally with a 1 px border.
void Round(HDC dc, const RECT& r, float radius, COLORREF fill, COLORREF border = CLR_INVALID, float bw = 1.0f) {
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);   // pixel i covers [i, i+1]: borders stay inside the rect
    const float in = border != CLR_INVALID ? bw / 2.0f : 0.0f;
    const float x = r.left + in, y = r.top + in;
    const float w = (r.right - r.left) - 2 * in, h = (r.bottom - r.top) - 2 * in;
    const float d = std::min(radius * 2.0f, std::min(w, h));
    Gdiplus::GraphicsPath p;
    p.AddArc(x, y, d, d, 180, 90);
    p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
    if (fill != CLR_INVALID) {
        Gdiplus::SolidBrush b(GC(fill));
        g.FillPath(&b, &p);
    }
    if (border != CLR_INVALID) {
        Gdiplus::Pen pen(GC(border), bw);
        g.DrawPath(&pen, &p);
    }
}

// spacing = extra pixels between letters (the wide, uppercase headings)
void Txt(HDC dc, const std::wstring& s, RECT r, HFONT f, COLORREF c,
         UINT fmt = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, int spacing = 0) {
    const HGDIOBJ o = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    const int old = SetTextCharacterExtra(dc, spacing);
    DrawTextW(dc, s.c_str(), (int)s.size(), &r, fmt);
    SetTextCharacterExtra(dc, old);
    SelectObject(dc, o);
}
int TextW(HDC dc, const std::wstring& s, HFONT f, int spacing = 0) {
    const HGDIOBJ o = SelectObject(dc, f);
    const int old = SetTextCharacterExtra(dc, spacing);
    SIZE sz{};
    GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
    SetTextCharacterExtra(dc, old);
    SelectObject(dc, o);
    return sz.cx;
}
std::wstring Upper(std::wstring s) {
    if (!s.empty()) CharUpperBuffW(s.data(), (DWORD)s.size());
    return s;
}

// ------------------------------------------------------------------------------------------------------------- icons
// Simple line icons, drawn on a 24 x 24 grid.
enum Ico : int {
    I_Export, I_Motion, I_Frame, I_Layers, I_Video,                     // the five pages
    I_Folder, I_Film, I_Camera, I_Monitor, I_Doc, I_Pulse, I_Sliders,   // cards and buttons
    I_Refresh, I_Check, I_Arrow
};

void Icon(HDC dc, int id, int x, int y, int size, COLORREF c) {
    if (id < 0) return;
    using namespace Gdiplus;
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    const float k = size / 24.0f;
    g.TranslateTransform((float)x, (float)y);
    g.ScaleTransform(k, k);
    Pen pen(GC(c), std::max(1.4f, 1.6f * (float)g_scale) / k);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    pen.SetLineJoin(LineJoinRound);
    auto L = [&](float a, float b, float cc, float d) { g.DrawLine(&pen, a, b, cc, d); };
    auto P = [&](std::initializer_list<float> v, bool closed) {
        std::vector<PointF> p;
        for (auto i = v.begin(); i != v.end(); i += 2) p.emplace_back(*i, *(i + 1));
        if (closed) g.DrawPolygon(&pen, p.data(), (INT)p.size());
        else g.DrawLines(&pen, p.data(), (INT)p.size());
    };
    auto R = [&](float l, float t, float r, float b, float rad) {
        GraphicsPath p;
        const float d = rad * 2;
        p.AddArc(l, t, d, d, 180, 90);
        p.AddArc(r - d, t, d, d, 270, 90);
        p.AddArc(r - d, b - d, d, d, 0, 90);
        p.AddArc(l, b - d, d, d, 90, 90);
        p.CloseFigure();
        g.DrawPath(&pen, &p);
    };
    auto C = [&](float cx, float cy, float r) { g.DrawEllipse(&pen, cx - r, cy - r, 2 * r, 2 * r); };
    switch (id) {
    case I_Export:   // a tray with an arrow out of it
        P({4, 14, 4, 20, 20, 20, 20, 14}, false);
        L(12, 4, 12, 15);
        P({7.5f, 8.5f, 12, 4, 16.5f, 8.5f}, false);
        break;
    case I_Motion:   // a ball with speed lines
        C(15.5f, 12, 5);
        L(5, 8, 8.5f, 8);
        L(2.5f, 12, 8.5f, 12);
        L(5, 16, 8.5f, 16);
        break;
    case I_Frame:    // viewfinder corners and a dot
        P({3, 8, 3, 3, 8, 3}, false);
        P({16, 3, 21, 3, 21, 8}, false);
        P({21, 16, 21, 21, 16, 21}, false);
        P({8, 21, 3, 21, 3, 16}, false);
        C(12, 12, 2.5f);
        break;
    case I_Layers:
        P({12, 3, 21, 8, 12, 13, 3, 8}, true);
        P({3, 12, 12, 17, 21, 12}, false);
        P({3, 16, 12, 21, 21, 16}, false);
        break;
    case I_Video:    // a film camera
        R(2.5f, 7, 15.5f, 17, 2);
        P({15.5f, 10.5f, 21.5f, 7.5f, 21.5f, 16.5f, 15.5f, 13.5f}, false);
        break;
    case I_Folder:
        P({3, 6, 9, 6, 11, 8, 21, 8, 21, 19, 3, 19}, true);
        break;
    case I_Film:     // a strip of film
        R(3, 4, 21, 20, 2);
        L(7.5f, 4, 7.5f, 20);
        L(16.5f, 4, 16.5f, 20);
        L(3, 9.5f, 7.5f, 9.5f);
        L(3, 14.5f, 7.5f, 14.5f);
        L(16.5f, 9.5f, 21, 9.5f);
        L(16.5f, 14.5f, 21, 14.5f);
        break;
    case I_Camera:   // a stills camera
        R(3, 7, 21, 19, 2);
        P({8, 7, 9.5f, 4.5f, 14.5f, 4.5f, 16, 7}, false);
        C(12, 13, 3.5f);
        break;
    case I_Monitor:
        R(3, 4, 21, 16, 2);
        L(12, 16, 12, 20);
        L(8, 20, 16, 20);
        break;
    case I_Doc:
        P({6, 3, 14, 3, 19, 8, 19, 21, 6, 21}, true);
        P({14, 3, 14, 8, 19, 8}, false);
        L(9, 13, 16, 13);
        L(9, 17, 16, 17);
        break;
    case I_Pulse:
        P({2.5f, 12, 6.5f, 12, 9.5f, 5, 13.5f, 19, 16.5f, 12, 21.5f, 12}, false);
        break;
    case I_Sliders:
        L(4, 7, 7, 7);
        L(11, 7, 20, 7);
        C(9, 7, 2);
        L(4, 17, 13, 17);
        L(17, 17, 20, 17);
        C(15, 17, 2);
        break;
    case I_Refresh:
        g.DrawArc(&pen, 5.0f, 5.0f, 14.0f, 14.0f, 30.0f, 300.0f);
        P({18.6f, 4.2f, 18.1f, 8.5f, 13.9f, 7.6f}, false);
        break;
    case I_Check:
        P({5, 12.5f, 10, 17.5f, 19, 7}, false);
        break;
    case I_Arrow:    // out to the web
        L(7, 17, 17, 7);
        P({9, 7, 17, 7, 17, 15}, false);
        break;
    }
}

// ------------------------------------------------------------------------------------------------------------- layout
// All positions are in 96-dpi units and scaled, so the window looks the same at 125 % / 150 % Windows scaling.
int S(int v) { return (int)std::lround(v * g_scale); }

constexpr int kSideW = 232;              // sidebar
constexpr int kPad = 28;                 // page margin
constexpr int kGap = 16;                 // between the two columns
constexpr int kColW = 448;               // a card
constexpr int kIn = 22;                  // inside a card
constexpr int kLeftX = kSideW + kPad, kRightX = kLeftX + kColW + kGap;
constexpr int kWinW = kRightX + kColW + kPad;
constexpr int kLabelW = 112;             // label column inside a card
constexpr int kRow = 42;                 // one row: a 32 px control + 10 px air
constexpr int kTop = 130;                // first card, below the page title
constexpr int kFootH = 76;               // bottom bar
constexpr int kIndent = 56;              // text under a switch lines up with the switch's label

// Pages, picked in the sidebar: every control made while g_page >= 0 belongs to that page.
constexpr int kPages = 5;
struct PageInfo {
    const wchar_t* nav;
    const wchar_t* title;
    const wchar_t* sub;
};
const PageInfo kPage[kPages] = {
    {L"Output", L"Output", L"Where every export goes, how it is named and the video format it is written in."},
    {L"Motion", L"Motion", L"Frame rate, motion blur and how the camera moves."},
    {L"Capture", L"Capture", L"Where EVE1G takes the frame, and what you see while it exports."},
    {L"Passes", L"Passes", L"Extra files written with every video, frame for frame."},
    {L"Camera data", L"Camera data", L"Camera files for After Effects and Blender, and the log."},
};
int g_page = -1, g_curPage = 0, g_height = 0;
std::vector<HWND> g_pages[kPages];

enum class K { Text, Edit, Combo, Toggle, Button, Nav };
enum class B { Primary, Secondary, Ghost };
struct Ctl {
    K kind = K::Text;
    int page = -1;
    COLORREF fg = col::text;
    COLORREF bgc = col::bg;   // what is behind the control
    RECT frame{};             // edits: the box drawn around them
    B btn = B::Secondary;
    int nav = -1;
    int icon = -1;
    bool hover = false;
};
std::unordered_map<HWND, Ctl> g_ctl;

struct Card {
    int page;
    int x, y, w, h;
    std::wstring title;
    int icon;
};
std::vector<Card> g_cards;
bool g_onCard = false;   // controls made now sit on a card

LRESULT CALLBACK ComboProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
LRESULT CALLBACK HoverProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
LRESULT CALLBACK EditProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id = 0, DWORD ex = 0,
          HFONT font = nullptr) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g_wnd,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)(font ? font : g_font), TRUE);
    if (g_page >= 0) g_pages[g_page].push_back(c);   // shown only on its page
    return c;
}
Ctl& Reg(HWND h, K kind) {
    Ctl& c = g_ctl[h];
    c.kind = kind;
    c.page = g_page;
    c.bgc = g_onCard ? col::card : col::bg;
    return c;
}

HWND Text(int x, int y, int w, int h, const wchar_t* text, COLORREF fg, HFONT font, DWORD style = SS_LEFT) {
    HWND s = Make(L"STATIC", text, style | SS_NOPREFIX, x, y, w, h, 0, 0, font);
    Reg(s, K::Text).fg = fg;
    return s;
}
// Labels and hints are never disabled (Windows would emboss them); they only change colour.
void Dim(HWND h, bool on) {
    Ctl& c = g_ctl[h];
    const COLORREF want = on ? col::text2 : col::dim;
    if (c.fg != want) {
        c.fg = want;
        InvalidateRect(h, nullptr, TRUE);
    }
}

// A card with a title. Controls are placed inside it by the caller.
struct Group {
    int x, y, w;
    int row;   // current row y
    size_t card;
};
Group BeginGroup(const wchar_t* title, int x, int y, int w, int icon = -1) {
    g_cards.push_back({g_page, x, y, w, 60, title, icon});
    g_onCard = true;
    return Group{x, y, w, y + 58, g_cards.size() - 1};
}
int EndGroup(Group& g) {   // sizes the card to its content, returns the y below it
    const int h = g.row - g.y + 14;
    g_cards[g.card].h = h;
    g_onCard = false;
    return g.y + h;
}
HWND Label(Group& g, const wchar_t* text) { return Text(g.x + kIn, g.row + 7, kLabelW - 10, 20, text, col::text2, g_font); }
int CtrlX(const Group& g) { return g.x + kIn + kLabelW; }
int CtrlW(const Group& g) { return g.w - 2 * kIn - kLabelW; }
HWND Hint(int x, int y, int w, int h, const wchar_t* text = L"") { return Text(x, y, w, h, text, col::hint, g_small); }

HWND Combo(int id, int x, int y, int w) {
    HWND c = Make(WC_COMBOBOXW, L"",
                  CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP, x, y, w, 420, id);
    Reg(c, K::Combo);
    // The closed box is exactly as tall as a text field (Windows adds its own border to the item height).
    RECT wr{};
    GetWindowRect(c, &wr);
    const int ih = (int)SendMessageW(c, CB_GETITEMHEIGHT, (WPARAM)-1, 0);
    SendMessageW(c, CB_SETITEMHEIGHT, (WPARAM)-1, ih + S(32) - (wr.bottom - wr.top));
    SetWindowSubclass(c, ComboProc, 1, 0);
    SendMessageW(c, CB_SETDROPPEDWIDTH, S(std::max(w, 430)), 0);
    COMBOBOXINFO ci{sizeof(ci)};
    if (GetComboBoxInfo(c, &ci) && ci.hwndList) SetWindowTheme(ci.hwndList, L"DarkMode_Explorer", nullptr);
    return c;
}
HWND Edit(int x, int y, int w, DWORD style = 0, int id = 0) {
    HWND e = Make(WC_EDITW, L"", ES_AUTOHSCROLL | WS_TABSTOP | style, x + 12, y + 7, w - 24, 19, id);
    Ctl& c = Reg(e, K::Edit);
    c.frame = {S(x), S(y), S(x + w), S(y + 32)};
    SetWindowSubclass(e, EditProc, 2, 0);
    return e;
}
// An on / off switch with its label.
HWND CheckBox(const wchar_t* text, int x, int y, int w, int id = 0, int h = 24) {
    HWND b = Make(WC_BUTTONW, text, BS_OWNERDRAW | WS_TABSTOP, x, y, w, h, id);
    Reg(b, K::Toggle);
    g_toggle[b] = false;
    SetWindowSubclass(b, HoverProc, 3, 0);
    return b;
}
HWND Button(const wchar_t* text, int x, int y, int w, int h, int id, B kind, int icon = -1) {
    HWND b = Make(WC_BUTTONW, text, BS_OWNERDRAW | WS_TABSTOP, x, y, w, h, id, 0, kind == B::Primary ? g_semi : g_font);
    Ctl& c = Reg(b, K::Button);
    c.btn = kind;
    c.icon = icon;
    SetWindowSubclass(b, HoverProc, 3, 0);
    return b;
}
void Add(HWND combo, const wchar_t* s) { SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)s); }

// ---------------------------------------------------------------------------------------------- naming (as EVE1G)
// path -> UTF-8 std::string (u8string() is std::u8string in C++20)
std::string U8(const std::filesystem::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::string CleanName(const std::string& in) {
    std::string r;
    for (const char c : in) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 128 && (std::isalnum(u) || c == '-' || c == '_')) r += c;
        else if ((c == ' ' || c == '.') && !r.empty() && r.back() != '-') r += '-';
    }
    while (!r.empty() && (r.back() == '-' || r.back() == '_')) r.pop_back();
    return r;
}

double CurrentFps() {
    const int i = Sel(hFps);
    if (i >= 0 && i < kFpsCount) return kFps[i].rate;
    const std::string v = Trim(N(GetText(hFpsCustom)));
    int a = 0, b = 0;
    if (sscanf_s(v.c_str(), "%d/%d", &a, &b) == 2 && b > 0) return (double)a / b;
    return atof(v.c_str());
}

std::string FpsLabel() {
    const double f = CurrentFps();
    if (std::fabs(f - std::round(f)) < 1e-6) return std::to_string((int)std::lround(f));
    char b[32];
    snprintf(b, sizeof(b), "%.3f", f);
    std::string v = b;
    while (!v.empty() && v.back() == '0') v.pop_back();
    if (!v.empty() && v.back() == '.') v.pop_back();
    return v;
}

std::string PresetPath() { return g_dir + "preset.json"; }
std::string IniPath() { return g_dir + "EVE1G.ini"; }

// Focus Puller's own camera-file setting (plugins\FocusPuller.ini, one folder up): what decides while EVE1G.ini does
// not have camera_files / camera_folder yet. -1 = unknown.
int FocusPullerSetting(const char* key) {
    std::ifstream f(g_dir + "..\\FocusPuller.ini");
    std::string line;
    const std::string k = std::string(key) + "=";
    while (std::getline(f, line)) {
        if (line.rfind(k, 0) == 0 && line.size() > k.size()) return line[k.size()] == '1' ? 1 : 0;
    }
    return -1;
}
std::string LogPath() { return g_dir + "EVE1G.log"; }

std::string CurrentContainer() {
    const int c = CodecSel();
    if (c >= 0 && c != kCustom) return kCodecs[c].container;
    try {
        std::ifstream f(PresetPath());
        const nlohmann::json j = nlohmann::json::parse(f);
        return j["format"]["container"].get<std::string>();
    } catch (...) {
        return "mp4";
    }
}

std::string OutputFolder() {
    std::string out = Trim(N(GetText(hOut)));
    if (out.empty()) {
        PWSTR vids = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &vids))) {
            out = N(vids);
            CoTaskMemFree(vids);
        }
    }
    while (!out.empty() && (out.back() == '\\' || out.back() == '/')) out.pop_back();
    return out;
}

int NextTake(const std::string& dir, const std::string& prefix) {
    int best = 0;
    const std::string key = prefix + "_t";
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator(std::filesystem::u8path(dir), ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        const std::string name = U8(it->path().filename());
        if (name.size() > key.size() && name.compare(0, key.size(), key) == 0 &&
            std::isdigit(static_cast<unsigned char>(name[key.size()])))
            best = std::max(best, atoi(name.c_str() + key.size()));
    }
    return best + 1;
}

// ------------------------------------------------------------------------------------------------------- live hints
void UpdateHints() {
    const int c = CodecSel();
    if (c >= 0) {
        const Codec& k = kCodecs[c];
        const bool q = k.quality != Q::None;
        EnableWindow(hQuality, q);
        Dim(hQualityLabel, q);
        SetWindowTextW(hQualityLabel, k.quality == Q::Cq ? L"Quality (CQ)" : L"Quality (CRF)");
        EnableWindow(hBitrate, k.bitrate);
        Dim(hBitrateLabel, k.bitrate);
        SetWindowTextW(hNote, k.note);
    }
    ShowWindow(hFpsCustom, (g_curPage == 1 && Sel(hFps) == kFpsCount) ? SW_SHOW : SW_HIDE);

    const int samples = std::max(0, _wtoi(GetText(hSamples).c_str()));
    const double fps = CurrentFps();
    const double game = fps * (samples + 1);
    const int sh = Sel(hShutter);
    const double strength = sh >= 0 ? kShutter[sh].strength : 0.5;
    const int used = samples == 0 ? 1 : (int)std::ceil(strength * samples + 1e-6);
    wchar_t buf[512];
    swprintf_s(buf, L"Each video frame = %d game frames, %d blended. The game runs at %.0f fps.\r\n%s", samples + 1,
               used, game,
               game > 60.0 ? L"Above 60 game fps audio is OFF - record sound separately."
                           : L"Audio is exported with the video.");
    SetWindowTextW(hInfo, buf);

    // file name preview, exactly as EVE1G will name it
    const std::string folder = OutputFolder();
    const std::string project = CleanName(N(GetText(hProject)));
    const std::string shot = CleanName(N(GetText(hShot)));
    std::string name;
    if (project.empty() && shot.empty()) {
        name = "EVE1G-<date><time>";
    } else {
        std::string prefix = project;
        if (!shot.empty()) prefix += (prefix.empty() ? "" : "_") + shot;
        int take = _wtoi(GetText(hTake).c_str());
        if (take <= 0) take = NextTake(folder, prefix);
        char t[16];
        snprintf(t, sizeof(t), "_t%02d", take);
        name = prefix + t + "_" + FpsLabel() + "fps";
        if (samples > 0) name += "_" + std::to_string(samples) + "s_" + std::to_string((int)std::lround(strength * 360));
        else name += "_noblur";
    }
    std::wstring preview = L"Next file:  " + W(folder) + L"\\" + W(name) + L"." + W(CurrentContainer());
    if (Checked(hSidecar)) preview += L"  + .txt";
    SetWindowTextW(hPreview, preview.c_str());

    const bool clean = Sel(hCapture) == 0;
    EnableWindow(hReshade, clean);
    EnableWindow(hLivePreview, clean && Checked(hReshade));


    const bool depthOn = Checked(hDepthOn);
    const bool normalsOn = Checked(hNormalsOn);
    const int df = std::max(0, Sel(hDepthFormat));
    const bool exr = df == 1;
    EnableWindow(hDepthFormat, depthOn || normalsOn);
    for (HWND c : {hDepthAuto, hDepthCurve, hDepthWhite}) EnableWindow(c, depthOn && !exr);
    for (HWND c : {hDepthNear, hDepthFar}) EnableWindow(c, depthOn && !exr && !Checked(hDepthAuto));
    EnableWindow(hNormalsFov, normalsOn);
    std::wstring note;
    if (!depthOn && !normalsOn) {
        note = L"Off. When on, every export also writes a depth map and / or normals, frame for frame with the video.";
    } else {
        note = L"Next:";
        if (exr) {
            note += L" " + W(name) + L"_depth_exr\\*.exr" + (depthOn && normalsOn ? L" (Z + N.X/N.Y/N.Z)"
                                                              : normalsOn ? L" (N.X/N.Y/N.Z)" : L" (Z in metres)");
        } else {
            if (depthOn) note += L" " + W(name) + L"_depth\\*.png";
            if (normalsOn) note += L"  " + W(name) + L"_normals\\*.png";
        }
        note += L"\r\nNeeds EVE1G_Depth.fx in reshade-shaders\\Shaders.";
    }
    SetWindowTextW(hDepthNote, note.c_str());

    {
        const bool cleanReshade = clean && Checked(hReshade);
        std::wstring pn;
        if (Checked(hPlateOn)) {
            pn += cleanReshade ? L"Plate: " + W(name) + L"_plate.mov   "
                               : L"The plate needs Clean capture with ReShade applied (Capture page).   ";
        }
        if (Checked(hProxyOn)) pn += L"Proxy: " + W(name) + L"_proxy.mp4 (half size)   ";
        if (pn.empty()) pn = L"Written next to the video, frame for frame. The proxy relinks to the master in "
                             L"Premiere / Resolve by name.";
        SetWindowTextW(hPassNote, pn.c_str());
    }
    {
        static const wchar_t* capNotes[] = {
            L"Taken before the game draws its UI, with your ReShade preset on it. Needed for the clean plate and the "
            L"on-screen progress.",
            L"Reads the finished screen - progress bar and cursor end up in the video.",
            L"Rockstar's own export image: no ReShade, no DLSS 5 effects drawn at present.",
        };
        const int ci = std::clamp(Sel(hCapture), 0, 2);
        SetWindowTextW(hCaptureNote, capNotes[ci]);
    }

    InvalidateRect(g_wnd, nullptr, FALSE);   // the boxes of fields that were switched on / off
}

// Summary of the last export, read from EVE1G.log.
void UpdateStatus() {
    std::ifstream f(LogPath());
    std::string line, file, done, reshade, err;
    while (std::getline(f, line)) {
        if (line.find("Output file: ") != std::string::npos) {
            file = line.substr(line.find("Output file: ") + 13);
            done.clear(); reshade.clear(); err.clear();
        } else if (line.find("export done - ") != std::string::npos) {
            done = line.substr(line.find("export done - ") + 14);
        } else if (line.find("ReShade preset rendered on ") != std::string::npos) {
            reshade = line.substr(line.find("ReShade preset rendered on ") + 27);
        } else if (line.find(" ERR ") != std::string::npos) {
            const size_t colon = line.find(": ", line.find(" ERR "));
            err = colon == std::string::npos ? line : line.substr(colon + 2);
        }
    }
    std::wstring s;
    if (file.empty()) {
        s = L"No export recorded yet in EVE1G.log.";
    } else {
        while (!file.empty() && (file.back() == '\r' || file.back() == '\n')) file.pop_back();
        s = L"Last export:  " + std::filesystem::u8path(file).filename().wstring();
        if (!done.empty()) {
            const size_t p = done.find(" game frame(s)");
            s += L"   •   " + W(done.substr(0, p)) + L" game frames";
        }
        if (!reshade.empty()) {
            int ok = 0, missing = 0;
            sscanf_s(reshade.c_str(), "%d", &ok);
            const size_t m = reshade.find("missing on ");
            if (m != std::string::npos) missing = atoi(reshade.c_str() + m + 11);
            s += missing == 0 ? L"   •   ReShade on every frame"
                              : L"   •   ReShade missing on " + std::to_wstring(missing) + L" frame(s)";
        }
        if (done.empty()) s += L"   •   in progress or interrupted";
        if (!err.empty()) s += L"\r\nLast error: " + W(Trim(err));
    }
    g_ctl[hStatus].fg = err.empty() ? col::text2 : col::bad;
    SetWindowTextW(hStatus, s.c_str());
    InvalidateRect(hStatus, nullptr, TRUE);
}

// ----------------------------------------------------------------------------------------------- format test
// Every preset is tried for real with Voukoder: open a 4K file, encode a few frames, close it. What fails here would
// fail in the game too (e.g. HEVC 4:2:2 with a Voukoder whose FFmpeg is too old), so it is hidden from the list.
std::string FormatsPath() { return g_dir + "formats.ini"; }

VKENCODERCONFIG ConfigFor(const Codec& k) {
    VKENCODERCONFIG cfg{};
    cfg.version = 1;
    char opts[512];
    sprintf_s(opts, k.options, k.defQuality);
    std::string v = opts;
    if (k.bitrate) v += "|b=0";
    strncpy_s(cfg.video.encoder, k.encoder, _TRUNCATE);
    strncpy_s(cfg.video.options, v.c_str(), _TRUNCATE);
    strncpy_s(cfg.audio.encoder, k.audioEncoder, _TRUNCATE);
    strncpy_s(cfg.audio.options, k.audioOptions, _TRUNCATE);
    strncpy_s(cfg.format.container, k.container, _TRUNCATE);
    cfg.format.faststart = std::string(k.container) == "mp4";
    return cfg;
}

struct ProbeJob {
    int index;
    HRESULT result;
    std::string step;
};

DWORD WINAPI ProbeThread(LPVOID param) {
    ProbeJob* job = static_cast<ProbeJob*>(param);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const Codec& k = kCodecs[job->index];
    const int w = 3840, h = 2160;
    wchar_t tmpDir[MAX_PATH];
    GetTempPathW(MAX_PATH, tmpDir);
    const std::wstring file = std::wstring(tmpDir) + L"eve1g_probe_" + W(kCodecIds[job->index]) + L"." + W(k.container);
    {
        Microsoft::WRL::ComPtr<IVoukoder> v;
        job->step = "create";
        job->result = CoCreateInstance(CLSID_CoVoukoder, nullptr, CLSCTX_INPROC_SERVER, IID_IVoukoder,
                                       reinterpret_cast<void**>(v.GetAddressOf()));
        if (SUCCEEDED(job->result)) {
            job->step = "config";
            job->result = v->SetConfig(ConfigFor(k));
        }
        if (SUCCEEDED(job->result)) {
            VKENCODERINFO info{};
            file.copy(info.filename, std::size(info.filename) - 1);
            wcscpy_s(info.application, L"EVE1G");
            info.video.enabled = true;
            info.video.width = w;
            info.video.height = h;
            info.video.timebase = {1, 24};
            info.video.aspectratio = {1, 1};
            info.video.fieldorder = FieldOrder::Progressive;
            info.audio.enabled = true;
            info.audio.samplerate = 48000;
            info.audio.channellayout = ChannelLayout::Stereo;
            info.audio.numberChannels = 2;
            job->step = "open";
            job->result = v->Open(info);
        }
        if (SUCCEEDED(job->result)) {
            std::vector<BYTE> pixels((size_t)w * h * 4, 32);
            BYTE* planes[1] = {pixels.data()};
            int rows[1] = {w * 4};
            VKVIDEOFRAME frame{};
            frame.buffer = planes;
            frame.rowsize = rows;
            frame.planes = 1;
            frame.width = w;
            frame.height = h;
            frame.pass = 1;
            strcpy_s(frame.format, "rgba");
            job->step = "encode";
            for (int i = 0; i < 3 && SUCCEEDED(job->result); ++i) job->result = v->SendVideoFrame(frame);
            job->step = "close";
            const HRESULT c = v->Close(true);
            if (SUCCEEDED(job->result)) job->result = c;
        }
    }
    DeleteFileW(file.c_str());
    CoUninitialize();
    return 0;
}

// Tests every preset (about a second each) and writes formats.ini. Returns how many work.
int ProbeAll() {
    std::string ini = "; EVE1G - which video formats Voukoder can encode on this PC (tested at 3840x2160).\r\n"
                      "; Written by EVE1G-Settings.exe. Delete this file or press \"Test formats again\" to re-test.\r\n"
                      "[formats]\r\n";
    int ok = 0;
    for (int i = 0; i < kCustom; ++i) {
        ProbeJob* job = new ProbeJob{i, E_FAIL, "start"};
        HANDLE t = CreateThread(nullptr, 0, ProbeThread, job, 0, nullptr);
        if (!t) {
            delete job;
            continue;
        }
        const bool done = WaitForSingleObject(t, 60000) == WAIT_OBJECT_0;
        CloseHandle(t);
        char line[160];
        if (!done) {
            // the hung thread still uses job, so it is deliberately not freed
            snprintf(line, sizeof(line), "%s = fail (timed out at %s)\r\n", kCodecIds[i], job->step.c_str());
        } else {
            if (SUCCEEDED(job->result)) {
                ++ok;
                snprintf(line, sizeof(line), "%s = ok\r\n", kCodecIds[i]);
            } else {
                snprintf(line, sizeof(line), "%s = fail (0x%08lX at %s)\r\n", kCodecIds[i],
                         (unsigned long)job->result, job->step.c_str());
            }
            delete job;
        }
        ini += line;
    }
    char when[64];
    time_t now = time(nullptr);
    tm t{};
    localtime_s(&t, &now);
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &t);
    ini += "tested = " + std::string(when) + "\r\n";
    std::ofstream f(FormatsPath(), std::ios::binary | std::ios::trunc);
    f << ini;
    return ok;
}

// Fills the preset list with the formats that work (all of them if nothing was tested yet).
void PopulateCodecs() {
    INIReader formats(FormatsPath());
    const bool tested = formats.ParseError() == 0 && formats.HasValue("formats", "tested");
    const int keep = CodecSel();
    SendMessageW(hCodec, CB_RESETCONTENT, 0, 0);
    int shown = 0;
    for (int i = 0; i <= kCustom; ++i) {
        if (i < kCustom && tested) {
            const std::string r = formats.GetString("formats", kCodecIds[i], "ok");
            if (r.rfind("ok", 0) != 0) continue;   // failed on this PC
        }
        const int pos = (int)SendMessageW(hCodec, CB_ADDSTRING, 0, (LPARAM)kCodecs[i].label);
        SendMessageW(hCodec, CB_SETITEMDATA, pos, i);
        if (i < kCustom) ++shown;
    }
    if (keep >= 0) SelectCodec(keep);
    std::wstring line = tested ? std::to_wstring(shown) + L" of " + std::to_wstring(kCustom) +
                                     L" formats work on this PC  ·  tested " +
                                     W(formats.GetString("formats", "tested", ""))
                               : L"Formats not tested yet on this PC.";
    SetWindowTextW(hFormats, line.c_str());
}

// ------------------------------------------------------------------------------------------------------- load / save
void Load() {
    INIReader ini(IniPath());
    SetWindowTextW(hOut, W(Trim(ini.GetString("", "output_folder", ""))).c_str());
    SetWindowTextW(hProject, W(Trim(ini.GetString("", "project", ""))).c_str());
    SetWindowTextW(hShot, W(Trim(ini.GetString("", "shot", ""))).c_str());
    SetWindowTextW(hTake, std::to_wstring(std::max(0L, ini.GetInteger("", "take", 0))).c_str());
    Check(hSidecar, ini.GetBoolean("", "sidecar", true));

    const std::string fps = Trim(ini.GetString("EXPORT", "fps", "24"));
    int fi = -1;
    for (int i = 0; i < kFpsCount; ++i)
        if (fps == kFps[i].value || fps == N(kFps[i].label)) fi = i;
    if (fi < 0) {
        fi = kFpsCount;
        SetWindowTextW(hFpsCustom, W(fps).c_str());
    }
    SendMessageW(hFps, CB_SETCURSEL, fi, 0);

    SetWindowTextW(hSamples, std::to_wstring(ini.GetInteger("EXPORT", "motion_blur_samples", 0)).c_str());
    const double strength = ini.GetReal("EXPORT", "motion_blur_strength", 0.5);
    int si = 2;
    double best = 9;
    for (int i = 0; i < (int)(sizeof(kShutter) / sizeof(kShutter[0])); ++i)
        if (std::fabs(kShutter[i].strength - strength) < best) best = std::fabs(kShutter[i].strength - strength), si = i;
    SendMessageW(hShutter, CB_SETCURSEL, si, 0);
    {
        const double ms = ini.GetReal("EXPORT", "rolling_shutter_ms", 0.0);
        int ri = 0;
        double rb = 1e9;
        for (int i = 0; i < (int)(sizeof(kRolling) / sizeof(kRolling[0])); ++i)
            if (std::fabs(kRolling[i].ms - ms) < rb) rb = std::fabs(kRolling[i].ms - ms), ri = i;
        SendMessageW(hRolling, CB_SETCURSEL, ri, 0);
    }

    const std::string cap = Trim(ini.GetString("EXPORT", "capture_source", "clean"));
    int ci = 0;
    for (int i = 0; i < (int)(sizeof(kCapture) / sizeof(kCapture[0])); ++i)
        if (cap == kCapture[i].value) ci = i;
    SendMessageW(hCapture, CB_SETCURSEL, ci, 0);
    Check(hReshade, ini.GetBoolean("EXPORT", "clean_reshade", true));
    Check(hLivePreview, ini.GetBoolean("EXPORT", "clean_preview", true));
    Check(hProgress, ini.GetBoolean("EXPORT", "progress_osd", true));
    {
        const int fpFiles = FocusPullerSetting("export_camera"), fpFolder = FocusPullerSetting("export_subfolder");
        Check(hCamFiles, ini.GetBoolean("EXPORT", "camera_files", fpFiles == 1));
        Check(hCamFolder, ini.GetBoolean("EXPORT", "camera_folder", fpFolder != 0));
    }
    {
        const double os = ini.GetReal("EXPORT", "osd_scale", 1.0);
        SendMessageW(hOsdSize, CB_SETCURSEL, os >= 1.75 ? 2 : os >= 1.25 ? 1 : 0, 0);
    }

    Check(hDepthOn, ini.GetBoolean("DEPTH", "enabled", false));
    {
        const std::string f = Trim(ini.GetString("DEPTH", "format", "png16"));
        int di = 0;
        for (int i = 0; i < (int)(sizeof(kDepthFormat) / sizeof(kDepthFormat[0])); ++i)
            if (f == kDepthFormat[i].value) di = i;
        SendMessageW(hDepthFormat, CB_SETCURSEL, di, 0);
        char b[32];
        snprintf(b, sizeof(b), "%g", ini.GetReal("DEPTH", "near", 0.5));
        SetWindowTextW(hDepthNear, W(b).c_str());
        snprintf(b, sizeof(b), "%g", ini.GetReal("DEPTH", "far", 200.0));
        SetWindowTextW(hDepthFar, W(b).c_str());
        Check(hDepthAuto, Trim(ini.GetString("DEPTH", "range", "auto")) != "fixed");
        SendMessageW(hDepthCurve, CB_SETCURSEL, Trim(ini.GetString("DEPTH", "curve", "log")) == "linear" ? 1 : 0, 0);
        SendMessageW(hDepthWhite, CB_SETCURSEL, Trim(ini.GetString("DEPTH", "white", "near")) == "far" ? 1 : 0, 0);
        Check(hNormalsOn, ini.GetBoolean("PASSES", "normals", false));
        snprintf(b, sizeof(b), "%g", ini.GetReal("PASSES", "normals_fov", 50.0));
        SetWindowTextW(hNormalsFov, W(b).c_str());
        Check(hPlateOn, ini.GetBoolean("PASSES", "clean_plate", false));
        Check(hProxyOn, ini.GetBoolean("PASSES", "proxy", false));
        Check(hNotify, ini.GetBoolean("EXPORT", "notify_done", false));
        Check(hStabilizer, ini.GetBoolean("EXPORT", "camera_stabilizer", true));
        {
            const std::string m = Trim(ini.GetString("EXPORT", "master_16bit", "false"));
            SendMessageW(hMaster, CB_SETCURSEL, (m == "true" || m == "1" || m == "on") ? 2
                                                : (m == "auto") ? 1 : 0, 0);
        }
    }

    const std::string log = Trim(ini.GetString("", "log_level", "info"));
    int li = 2;
    for (int i = 0; i < 4; ++i)
        if (log == kLog[i].value) li = i;
    SendMessageW(hLog, CB_SETCURSEL, li, 0);

    // Which preset is preset.json? Match on encoder + options with the number taken out.
    int codec = kCustom, quality = -1;
    long long bitrate = 0;
    try {
        std::ifstream f(PresetPath());
        nlohmann::json j = nlohmann::json::parse(f);
        const std::string enc = j["video"]["encoder"].get<std::string>();
        std::string opts = j["video"]["options"].get<std::string>();
        const size_t bpos = opts.find("|b=");
        if (bpos != std::string::npos) {
            bitrate = atoll(opts.c_str() + bpos + 3);
            opts = opts.substr(0, bpos);
        }
        for (int i = 0; i < kCustom; ++i) {
            if (enc != kCodecs[i].encoder) continue;
            const std::string tpl = kCodecs[i].options;
            const size_t p = tpl.find("%d");
            if (p == std::string::npos) {
                if (opts == tpl) codec = i;
            } else if (opts.compare(0, p, tpl, 0, p) == 0 &&
                       opts.compare(opts.size() - std::min(opts.size(), tpl.size() - p - 2), std::string::npos, tpl,
                                    p + 2, std::string::npos) == 0) {
                codec = i;
                quality = atoi(opts.c_str() + p);
            }
        }
    } catch (...) {
    }
    SelectCodec(codec);
    if (quality <= 0) quality = kCodecs[codec].defQuality;
    SetWindowTextW(hQuality, std::to_wstring(quality).c_str());
    SetWindowTextW(hBitrate, std::to_wstring(bitrate / 1000000).c_str());
    UpdateHints();
    UpdateStatus();
}

bool Save() {
    const std::string out = Trim(N(GetText(hOut)));
    if (!out.empty() && GetFileAttributesW(W(out).c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (MessageBoxW(g_wnd, L"That output folder does not exist. Create it?", kApp, MB_YESNO | MB_ICONQUESTION) !=
            IDYES)
            return false;
        SHCreateDirectoryExW(nullptr, W(out).c_str(), nullptr);
    }
    const int fi = Sel(hFps);
    std::string fps = fi < kFpsCount ? kFps[fi].value : Trim(N(GetText(hFpsCustom)));
    if (fps.empty()) fps = "24";
    const int samples = std::clamp(_wtoi(GetText(hSamples).c_str()), 0, 255);
    char strength[16];
    snprintf(strength, sizeof(strength), "%.3f", kShutter[Sel(hShutter)].strength);
    const int take = std::max(0, _wtoi(GetText(hTake).c_str()));
    auto b = [](bool v) { return std::string(v ? "true" : "false"); };

    std::string ini;
    ini += "; EVE1G - written by EVE1G-Settings.exe. Read again at the start of every export.\r\n\r\n";
    ini += "enable_mod = true\r\nauto_reload_config = true\r\n";
    ini += "; Empty = your Windows Videos folder\r\noutput_folder = " + out + "\r\n";
    ini += "; error / warn / info / debug\r\nlog_level = " + std::string(kLog[Sel(hLog)].value) + "\r\n\r\n";
    ini += "; File naming: <project>_<shot>_t<take>_<fps>fps_<samples>s_<shutter>.<ext>\r\n";
    ini += "; Leave project and shot empty for EVE1G-<date><time>. take = 0 picks the next free take number.\r\n";
    ini += "project = " + Trim(N(GetText(hProject))) + "\r\n";
    ini += "shot = " + Trim(N(GetText(hShot))) + "\r\n";
    ini += "take = " + std::to_string(take) + "\r\n";
    ini += "; Write a .txt next to every video with the settings it was made with\r\n";
    ini += "sidecar = " + b(Checked(hSidecar)) + "\r\n\r\n";
    ini += "[EXPORT]\r\nfps = " + fps + "\r\n";
    ini += "; Motion blur: the game renders (samples + 1) frames per video frame and blends them. 0 = off.\r\n";
    ini += "; Audio is only exported while fps * (samples + 1) stays at or below 60.\r\n";
    ini += "motion_blur_samples = " + std::to_string(samples) + "\r\n";
    ini += "; Share of each frame's interval that is blended = shutter angle / 360 (0.5 = 180 degrees)\r\n";
    ini += "motion_blur_strength = " + std::string(strength) + "\r\n";
    {
        const int ri = std::max(0, Sel(hRolling));
        char ms[16];
        snprintf(ms, sizeof(ms), "%.0f", kRolling[ri].ms);
        ini += "; Light-correct blending, soft shutter edges and light streaks are off in this version\r\n";
        ini += "blend_linear = false\r\nshutter_soft = 0\r\nlight_streaks = 0\r\n";
        ini += "; Rolling shutter: ms the sensor takes to read the image top to bottom (0 = global shutter, like a\r\n";
        ini += ";   cinema camera; 8-25 = mirrorless / phone - fast pans lean). Needs motion blur samples.\r\n";
        ini += "rolling_shutter_ms = " + std::string(ms) + "\r\n";
    }
    ini += "export_openexr = false\r\n\r\n";
    ini += "; clean   = frame taken before the game UI, your ReShade preset applied (no progress bar, no cursor)\r\n";
    ini += "; present = what is on screen (includes the progress bar and cursor)\r\n";
    ini += "; encode  = Rockstar's export image, no ReShade\r\n";
    ini += "capture_source = " + std::string(kCapture[Sel(hCapture)].value) + "\r\n";
    ini += "; clean: apply your ReShade preset (DOF, grade) to the video\r\n";
    ini += "clean_reshade = " + b(Checked(hReshade)) + "\r\n";
    ini += "; clean: true = the screen shows the finished frame while exporting (DOF visible for focus pulls,\r\n";
    ini += ";   no progress bar); false = the screen shows the game with its UI but without ReShade\r\n";
    ini += "clean_preview = " + b(Checked(hLivePreview)) + "\r\n";
    ini += "; Export progress (%, frames, time left) in a corner of the screen - drawn by ReShade after the frame\r\n";
    ini += ";   is captured, so it is never in the video (only shown with capture clean / encode / effects)\r\n";
    ini += "progress_osd = " + b(Checked(hProgress)) + "\r\n";
    ini += "; Its size: 1 = normal (it already grows with the screen, 2x at 4K), 1.5 = large, 2 = extra large\r\n";
    ini += "osd_scale = " + std::string(kOsdSize[std::max(0, Sel(hOsdSize))].value) + "\r\n";
    ini += "; Focus Puller camera files (After Effects .jsx / Blender .py / .csv) with every export: false = only the video\r\n";
    ini += "camera_files = " + b(Checked(hCamFiles)) + "\r\n";
    ini += "; ...in a \"camera\" folder next to the video (false = right beside it)\r\n";
    ini += "camera_folder = " + b(Checked(hCamFolder)) + "\r\n";
    ini += "; When an export finishes: a sound, the taskbar button flashes and a note shows on screen\r\n";
    ini += "notify_done = " + b(Checked(hNotify)) + "\r\n";
    ini += "; Smooth Blend camera markers: no shake in the export (the spring gets the export's time step)\r\n";
    ini += "camera_stabilizer = " + b(Checked(hStabilizer)) + "\r\n";
    ini += "; Frames into the encoder at 16 bits per channel: auto = for 10/12-bit and lossless formats,\r\n";
    ini += ";   true = always, false = 8-bit like original EVE (use if an export fails to start)\r\n";
    ini += "master_16bit = " + std::string(kMaster[std::max(0, Sel(hMaster))].value) + "\r\n\r\n";
    {
        double nearM = _wtof(GetText(hDepthNear).c_str()), farM = _wtof(GetText(hDepthFar).c_str());
        if (!(nearM > 0.0)) nearM = 0.5;
        if (!(farM > nearM)) farM = std::max(nearM * 10.0, 200.0);
        char nb[32], fb[32];
        snprintf(nb, sizeof(nb), "%g", nearM);
        snprintf(fb, sizeof(fb), "%g", farM);
        ini += "[DEPTH]\r\n";
        ini += "; Depth pass: a depth map exported with every video, frame for frame (needs EVE1G_Depth.fx in "
               "reshade-shaders\\Shaders)\r\n";
        ini += "enabled = " + b(Checked(hDepthOn)) + "\r\n";
        ini += "; png16 = 16-bit PNG sequence,  exr = float OpenEXR sequence (metres)\r\n";
        ini += "format = " + std::string(kDepthFormat[std::max(0, Sel(hDepthFormat))].value) + "\r\n";
        ini += "; png16: distance range in metres, curve (log / linear) and which end is white (near / far)\r\n";
        ini += "; range = auto: near / far worked out from the first frame of each clip (full black to white).\r\n"
               ";         fixed: the near / far below\r\n";
        ini += std::string("range = ") + (Checked(hDepthAuto) ? "auto" : "fixed") + "\r\n";
        ini += "near = " + std::string(nb) + "\r\nfar = " + std::string(fb) + "\r\n";
        ini += "curve = " + std::string(kDepthCurve[std::max(0, Sel(hDepthCurve))].value) + "\r\n";
        ini += "white = " + std::string(kDepthWhite[std::max(0, Sel(hDepthWhite))].value) + "\r\n\r\n";
        ini += "[HFR]\r\n";
        ini += "; High-frame-rate export is off in this version\r\n";
        ini += "enabled = false\r\nformat = prores\r\nevery = 1\r\n\r\n";
        int fov = _wtoi(GetText(hNormalsFov).c_str());
        if (fov < 5 || fov > 150) fov = 50;
        ini += "[PASSES]\r\n";
        ini += "; More files with every video, frame for frame:\r\n";
        ini += "; clean_plate = <video>_plate.mov, the same shot without the ReShade preset (needs capture_source = clean)\r\n";
        ini += "clean_plate = " + b(Checked(hPlateOn)) + "\r\n";
        ini += "; proxy = <video>_proxy.mp4, half size H.264 for fast editing\r\n";
        ini += "proxy = " + b(Checked(hProxyOn)) + "\r\n";
        ini += "; normals = surface normals from the depth, in the [DEPTH] format (needs EVE1G_Depth.fx)\r\n";
        ini += "normals = " + b(Checked(hNormalsOn)) + "\r\n";
        ini += "; field of view for the normals when Focus Puller is not running (degrees, vertical)\r\n";
        ini += "normals_fov = " + std::to_string(fov) + "\r\n";
        ini += "id_pass = false\r\n";
    }
    {
        std::ofstream f(IniPath(), std::ios::binary | std::ios::trunc);
        if (!f) {
            MessageBoxW(g_wnd, L"Could not write EVE1G.ini. Is the game folder read-only? (A GTA V folder inside Program Files may need EVE1G-Settings run as administrator.)", kApp, MB_ICONERROR);
            return false;
        }
        f << ini;
    }

    const int c = CodecSel();
    if (c >= 0 && c != kCustom) {
        const Codec& k = kCodecs[c];
        char opts[512];
        int q = _wtoi(GetText(hQuality).c_str());
        if (k.quality != Q::None && (q <= 0 || q > 51)) q = k.defQuality;   // 0 would mean "no quality target"
        sprintf_s(opts, k.options, q);
        std::string v = opts;
        if (k.bitrate) {
            const long long mbps = _wtoi64(GetText(hBitrate).c_str());
            v += "|b=" + std::to_string(mbps > 0 ? mbps * 1000000 : 0);
        }
        nlohmann::json j;
        j["version"] = 1;
        j["format"]["container"] = k.container;
        j["format"]["faststart"] = std::string(k.container) == "mp4";
        j["video"]["encoder"] = k.encoder;
        j["video"]["options"] = v;
        j["video"]["filters"] = "";
        j["video"]["sidedata"] = "";
        j["audio"]["encoder"] = k.audioEncoder;
        j["audio"]["options"] = k.audioOptions;
        j["audio"]["filters"] = "";
        j["audio"]["sidedata"] = "";
        std::ofstream f(PresetPath(), std::ios::binary | std::ios::trunc);
        f << j.dump(4);
    }
    return true;
}

// --------------------------------------------------------------------------------------------------------- window
std::wstring VersionText() {
    std::wstring v = L"v" + W(EVE1G_VERSION);
    std::replace(v.begin(), v.end(), L'-', L' ');
    return v;   // "v0.11.2 beta"
}

void Build() {
    // ---------------- sidebar (all pages)
    g_page = -1;
    static const int kNavIcon[kPages] = {I_Export, I_Motion, I_Frame, I_Layers, I_Video};
    for (int i = 0; i < kPages; ++i) {
        HWND b = Make(WC_BUTTONW, kPage[i].nav, BS_OWNERDRAW | WS_TABSTOP, 16, 146 + i * 50, kSideW - 32, 44,
                      ID_NAV + i, 0, g_semi);
        Ctl& c = Reg(b, K::Nav);
        c.nav = i;
        c.icon = kNavIcon[i];
        c.bgc = col::side;
        SetWindowSubclass(b, HoverProc, 3, 0);
    }

    // One grid on every card: labels on the left, controls from CtrlX to the right edge. Number fields are 64 wide;
    // a switch with a one-line note under it takes 58, a plain row kRow.

    // ================= 1. Output: where and in what format
    g_page = 0;
    Group out = BeginGroup(L"Files", kLeftX, kTop, kColW, I_Folder);
    Label(out, L"Folder");
    hOut = Edit(CtrlX(out), out.row, CtrlW(out) - 112, 0, ID_NAMING);
    Button(L"Browse", CtrlX(out) + CtrlW(out) - 100, out.row, 100, 32, ID_BROWSE, B::Secondary, I_Folder);
    out.row += kRow;
    Label(out, L"Project");
    const int half = (CtrlW(out) - 56) / 2;   // Project and Shot: two equal fields
    hProject = Edit(CtrlX(out), out.row, half, 0, ID_NAMING);
    Text(CtrlX(out) + half, out.row + 7, 56, 20, L"Shot", col::text2, g_font, SS_CENTER);
    hShot = Edit(CtrlX(out) + CtrlW(out) - half, out.row, half, 0, ID_NAMING);
    out.row += kRow;
    Label(out, L"Take");
    hTake = Edit(CtrlX(out), out.row, 64, ES_NUMBER, ID_NAMING);
    Hint(CtrlX(out) + 76, out.row + 8, CtrlW(out) - 76, 18, L"0 = the next free take");
    out.row += kRow;
    hSidecar = CheckBox(L"Save a settings .txt beside each video", out.x + kIn, out.row + 4, out.w - 2 * kIn, ID_NAMING);
    out.row += kRow;
    hPreview = Hint(out.x + kIn, out.row, out.w - 2 * kIn, 36);
    out.row += 38;
    EndGroup(out);

    Group enc = BeginGroup(L"Video format", kRightX, kTop, kColW, I_Film);
    Label(enc, L"Preset");
    hCodec = Combo(ID_CODEC, CtrlX(enc), enc.row, CtrlW(enc));
    enc.row += kRow;
    hQualityLabel = Label(enc, L"Quality (CRF)");
    hQuality = Edit(CtrlX(enc), enc.row, 64, ES_NUMBER);
    hBitrateLabel = Text(CtrlX(enc) + 70, enc.row + 7, CtrlW(enc) - 70 - 76, 20, L"Bitrate Mbit/s", col::text2,
                         g_font, SS_RIGHT);
    hBitrate = Edit(CtrlX(enc) + CtrlW(enc) - 64, enc.row, 64, ES_NUMBER);
    enc.row += kRow;
    Label(enc, L"Precision");
    hMaster = Combo(ID_CAPTURE, CtrlX(enc), enc.row, CtrlW(enc));
    for (const Choice& c : kMaster) Add(hMaster, c.label);
    enc.row += kRow;
    hNote = Hint(enc.x + kIn, enc.row, enc.w - 2 * kIn, 36);
    enc.row += 38;
    hFormats = Hint(enc.x + kIn, enc.row, enc.w - 2 * kIn, 18);
    enc.row += 30;
    const int bw = (enc.w - 2 * kIn - 12) / 2;   // two equal buttons
    Button(L"Voukoder settings", enc.x + kIn, enc.row, bw, 36, ID_VOUKODER, B::Secondary, I_Sliders);
    Button(L"Test formats again", enc.x + enc.w - kIn - bw, enc.row, bw, 36, ID_TESTFORMATS, B::Secondary, I_Refresh);
    enc.row += 46;
    EndGroup(enc);

    // ================= 2. Motion: frame rate, blur, camera
    g_page = 1;
    Group tm = BeginGroup(L"Frame rate and motion blur", kLeftX, kTop, kColW, I_Motion);
    Label(tm, L"Frame rate");
    const int thalf = (CtrlW(tm) - 12) / 2;
    hFps = Combo(ID_FPS, CtrlX(tm), tm.row, thalf);
    for (const Fps& f : kFps) Add(hFps, f.label);
    Add(hFps, L"Custom...");
    hFpsCustom = Edit(CtrlX(tm) + thalf + 12, tm.row, CtrlW(tm) - thalf - 12, 0, ID_FPS);
    tm.row += kRow;
    Label(tm, L"Blur samples");
    hSamples = Edit(CtrlX(tm), tm.row, 64, ES_NUMBER, ID_SAMPLES);
    Hint(CtrlX(tm) + 76, tm.row + 8, CtrlW(tm) - 76, 18, L"0 off  ·  12 cinematic  ·  39 max");
    tm.row += kRow;
    Label(tm, L"Shutter angle");
    hShutter = Combo(ID_SHUTTER, CtrlX(tm), tm.row, CtrlW(tm));
    for (const Shutter& s : kShutter) Add(hShutter, s.label);
    tm.row += kRow;
    hInfo = Hint(tm.x + kIn, tm.row, tm.w - 2 * kIn, 54);
    tm.row += 56;
    EndGroup(tm);

    Group cam = BeginGroup(L"Camera", kRightX, kTop, kColW, I_Camera);
    hStabilizer = CheckBox(L"Camera stabilizer", cam.x + kIn, cam.row + 4, cam.w - 2 * kIn);
    Hint(cam.x + kIn + kIndent, cam.row + 30, cam.w - 2 * kIn - kIndent, 18,
         L"No shake on slow Smooth Blend moves. Recommended.");
    cam.row += 58;
    Label(cam, L"Rolling shutter");
    hRolling = Combo(0, CtrlX(cam), cam.row, CtrlW(cam));
    for (const Rolling& r : kRolling) Add(hRolling, r.label);
    cam.row += kRow;
    Hint(cam.x + kIn, cam.row, cam.w - 2 * kIn, 36,
         L"Off is a cinema camera. 8-25 ms makes fast pans lean like a phone. Needs blur samples.");
    cam.row += 38;
    EndGroup(cam);

    // ================= 3. Capture: where the frame is taken, what the screen shows
    g_page = 2;
    Group cap = BeginGroup(L"Capture", kLeftX, kTop, kColW, I_Frame);
    Label(cap, L"Take the frame");
    hCapture = Combo(ID_CAPTURE, CtrlX(cap), cap.row, CtrlW(cap));
    for (const Choice& c : kCapture) Add(hCapture, c.label);
    cap.row += kRow;
    hCaptureNote = Hint(cap.x + kIn, cap.row, cap.w - 2 * kIn, 36);
    cap.row += 46;
    hReshade = CheckBox(L"Apply my ReShade preset to the video", cap.x + kIn, cap.row + 4, cap.w - 2 * kIn, ID_CAPTURE);
    Hint(cap.x + kIn + kIndent, cap.row + 30, cap.w - 2 * kIn - kIndent, 18,
         L"Depth of field, grade - everything in your preset.");
    cap.row += 58;
    hLivePreview = CheckBox(L"Show the finished frame while exporting", cap.x + kIn, cap.row + 4, cap.w - 2 * kIn,
                            ID_CAPTURE);
    Hint(cap.x + kIn + kIndent, cap.row + 30, cap.w - 2 * kIn - kIndent, 18,
         L"See the depth of field on screen to check focus pulls.");
    cap.row += 58;
    EndGroup(cap);

    Group osd = BeginGroup(L"On screen while exporting", kRightX, kTop, kColW, I_Monitor);
    hProgress = CheckBox(L"Show export progress", osd.x + kIn, osd.row + 4, osd.w - 2 * kIn);
    Hint(osd.x + kIn + kIndent, osd.row + 30, osd.w - 2 * kIn - kIndent, 36,
         L"%, frames, motion blur and time left. Drawn after the frame is taken, so never in the video.");
    osd.row += 78;
    Label(osd, L"Text size");
    hOsdSize = Combo(0, CtrlX(osd), osd.row, CtrlW(osd));
    for (const Choice& c : kOsdSize) Add(hOsdSize, c.label);
    osd.row += kRow + 4;
    hNotify = CheckBox(L"Sound and a note when an export finishes", osd.x + kIn, osd.row + 4, osd.w - 2 * kIn);
    Hint(osd.x + kIn + kIndent, osd.row + 30, osd.w - 2 * kIn - kIndent, 18, L"The taskbar button flashes too.");
    osd.row += 58;
    EndGroup(osd);

    // ================= 4. Passes: extra files written with every video
    g_page = 3;
    Group dp = BeginGroup(L"Depth and normals", kLeftX, kTop, kColW, I_Layers);
    hDepthOn = CheckBox(L"Depth map with every video", dp.x + kIn, dp.row + 4, dp.w - 2 * kIn, ID_DEPTH);
    dp.row += kRow;
    hNormalsOn = CheckBox(L"Surface normals from the depth", dp.x + kIn, dp.row + 4, dp.w - 2 * kIn, ID_DEPTH);
    Hint(dp.x + kIn + kIndent, dp.row + 30, dp.w - 2 * kIn - kIndent, 18, L"For relighting and ambient occlusion in post.");
    dp.row += 58;
    Label(dp, L"Format");
    hDepthFormat = Combo(ID_DEPTH, CtrlX(dp), dp.row, CtrlW(dp));
    for (const Choice& c : kDepthFormat) Add(hDepthFormat, c.label);
    dp.row += kRow;
    Label(dp, L"Range (m)");
    hDepthNear = Edit(CtrlX(dp), dp.row, 64, 0, ID_DEPTH);
    Text(CtrlX(dp) + 64, dp.row + 7, 28, 20, L"to", col::text2, g_font, SS_CENTER);
    hDepthFar = Edit(CtrlX(dp) + 92, dp.row, 64, 0, ID_DEPTH);
    hDepthAuto = CheckBox(L"Auto", CtrlX(dp) + CtrlW(dp) - 100, dp.row + 4, 100, ID_DEPTH);
    dp.row += kRow;
    Label(dp, L"Look");
    const int dhalf = (CtrlW(dp) - 12) / 2;
    hDepthCurve = Combo(ID_DEPTH, CtrlX(dp), dp.row, dhalf);
    for (const Choice& c : kDepthCurve) Add(hDepthCurve, c.label);
    hDepthWhite = Combo(ID_DEPTH, CtrlX(dp) + dhalf + 12, dp.row, CtrlW(dp) - dhalf - 12);
    for (const Choice& c : kDepthWhite) Add(hDepthWhite, c.label);
    dp.row += kRow;
    Label(dp, L"Normals FOV");
    hNormalsFov = Edit(CtrlX(dp), dp.row, 64, ES_NUMBER, ID_DEPTH);
    Hint(CtrlX(dp) + 76, dp.row + 8, CtrlW(dp) - 76, 18, L"degrees, without Focus Puller");
    dp.row += kRow;
    hDepthNote = Hint(dp.x + kIn, dp.row, dp.w - 2 * kIn, 54);
    dp.row += 56;
    EndGroup(dp);

    Group ex = BeginGroup(L"More files", kRightX, kTop, kColW, I_Doc);
    hPlateOn = CheckBox(L"Clean plate", ex.x + kIn, ex.row + 4, ex.w - 2 * kIn, ID_DEPTH);
    Hint(ex.x + kIn + kIndent, ex.row + 30, ex.w - 2 * kIn - kIndent, 18, L"The same shot without ReShade (ProRes).");
    ex.row += 58;
    hProxyOn = CheckBox(L"Proxy", ex.x + kIn, ex.row + 4, ex.w - 2 * kIn, ID_DEPTH);
    Hint(ex.x + kIn + kIndent, ex.row + 30, ex.w - 2 * kIn - kIndent, 18, L"Half-size H.264 for fast editing.");
    ex.row += 58;
    hPassNote = Hint(ex.x + kIn, ex.row, ex.w - 2 * kIn, 36);
    ex.row += 38;
    EndGroup(ex);

    // ================= 5. Camera data and the log
    g_page = 4;
    Group fp = BeginGroup(L"Camera files", kLeftX, kTop, kColW, I_Video);
    hCamFiles = CheckBox(L"Write the camera files with every export", fp.x + kIn, fp.row + 4, fp.w - 2 * kIn);
    fp.row += kRow;
    hCamFolder = CheckBox(L"In a \"camera\" folder next to the video", fp.x + kIn + kIndent, fp.row + 4,
                          fp.w - 2 * kIn - kIndent);
    fp.row += kRow + 4;
    Hint(fp.x + kIn, fp.row, fp.w - 2 * kIn, 112,
         L"Needs the Focus Puller add-on. Every export then also writes the Rockstar Editor camera, frame for "
         L"frame:\r\n"
         L"•   _camera.jsx  -  After Effects (File > Scripts > Run Script File)\r\n"
         L"•   _camera.py  -  Blender (Scripting > Run Script)\r\n"
         L"•   _camera.csv  -  position, rotation, FOV, focus and aperture\r\n"
         L"The same switch is in Focus Puller > Export.");
    fp.row += 116;
    EndGroup(fp);

    Group dg = BeginGroup(L"Diagnostics", kRightX, kTop, kColW, I_Pulse);
    Label(dg, L"Log level");
    hLog = Combo(0, CtrlX(dg), dg.row, CtrlW(dg) - 124);
    for (const Choice& c : kLog) Add(hLog, c.label);
    Button(L"Open log", CtrlX(dg) + CtrlW(dg) - 112, dg.row, 112, 32, ID_OPENLOG, B::Secondary, I_Doc);
    dg.row += kRow;
    Hint(dg.x + kIn, dg.row, dg.w - 2 * kIn, 36,
         L"Found a bug? Send EVE1G.log (and your EVE1G.ini) with what you did. \"info\" is enough; \"debug\" for "
         L"crashes.");
    dg.row += 38;
    EndGroup(dg);

    // Both columns of a page end on the same line, and the window fits the tallest page.
    int bottom = 0;
    for (int p = 0; p < kPages; ++p) {
        int end[2] = {0, 0}, last[2] = {-1, -1};
        for (size_t i = 0; i < g_cards.size(); ++i) {
            const Card& c = g_cards[i];
            if (c.page != p) continue;
            const int side = c.x == kLeftX ? 0 : 1;
            if (c.y + c.h > end[side]) end[side] = c.y + c.h, last[side] = (int)i;
        }
        const int b = std::max(end[0], end[1]);
        for (int s = 0; s < 2; ++s)
            if (last[s] >= 0) g_cards[last[s]].h = b - g_cards[last[s]].y;
        bottom = std::max(bottom, b);
    }

    // ---------------- bottom bar and sidebar foot (all pages)
    g_page = -1;
    const int footY = bottom + 24;
    g_height = footY + kFootH;
    const int by = footY + (kFootH - 40) / 2;
    hStatus = Text(kLeftX, footY + 18, kWinW - kLeftX - kPad - 460, 40, L"", col::text2, g_small);
    g_ctl[hStatus].bgc = col::foot;
    HWND b = Button(L"Open output folder", kWinW - kPad - 440, by, 188, 40, ID_OPENFOLDER, B::Secondary, I_Folder);
    g_ctl[b].bgc = col::foot;
    b = Button(L"Save", kWinW - kPad - 240, by, 136, 40, ID_SAVE, B::Primary, I_Check);
    g_ctl[b].bgc = col::foot;
    b = Button(L"Close", kWinW - kPad - 92, by, 92, 40, ID_CLOSE, B::Ghost);
    g_ctl[b].bgc = col::foot;
    hGitHub = Button(L"Updates on GitHub", 16, g_height - 60, kSideW - 32, 44, ID_GITHUB, B::Ghost, I_Arrow);
    g_ctl[hGitHub].bgc = col::side;
}

// Shows the controls of one page and hides the others.
void ShowPage(int page) {
    page = std::clamp(page, 0, kPages - 1);
    g_curPage = page;
    for (int p = 0; p < kPages; ++p)
        for (HWND c : g_pages[p]) ShowWindow(c, p == page ? SW_SHOW : SW_HIDE);
    ShowWindow(hFpsCustom, (page == 1 && Sel(hFps) == kFpsCount) ? SW_SHOW : SW_HIDE);
    for (auto& [h, c] : g_ctl)
        if (c.kind == K::Nav) InvalidateRect(h, nullptr, FALSE);
    InvalidateRect(g_wnd, nullptr, FALSE);
}

void Browse() {
    BROWSEINFOW bi{};
    bi.hwndOwner = g_wnd;
    bi.lpszTitle = L"Where should EVE1G save the videos?";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    if (PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi)) {
        wchar_t path[MAX_PATH];
        if (SHGetPathFromIDListW(pidl, path)) SetWindowTextW(hOut, path);
        CoTaskMemFree(pidl);
    }
}

// ------------------------------------------------------------------------------------------------------- drawing
// A list entry like "Edit master  -  ProRes 422 HQ (...)  [recommended]": the name in white, the rest in grey,
// and a small outlined RECOMMENDED tag for the recommended one.
void DrawEntry(HDC dc, RECT r, const std::wstring& s, bool enabled, bool field = false) {
    std::wstring t = s, main, rest;
    bool rec = false;
    const size_t tag = t.find(L"[recommended]");
    if (tag != std::wstring::npos) {
        rec = true;
        t.erase(tag, 13);
    }
    while (!t.empty() && t.back() == L' ') t.pop_back();
    const size_t d = t.find(L"  -  ");
    main = d == std::wstring::npos ? t : t.substr(0, d);
    rest = d == std::wstring::npos ? L"" : t.substr(d + 5);
    while (!rest.empty() && rest.front() == L' ') rest.erase(0, 1);

    if (rec) {
        const std::wstring word = L"RECOMMENDED";
        const int tw = TextW(dc, word, g_tag, S(1)) + S(16);
        if (r.right - r.left > tw + S(110)) {
            const int mid = (r.top + r.bottom) / 2;
            RECT pill{r.right - tw, mid - S(10), r.right, mid + S(10)};
            Round(dc, pill, (float)S(5), CLR_INVALID, enabled ? RGB(96, 96, 102) : col::dim);
            RECT tr = pill;
            tr.left += S(1);   // the letter spacing also follows the last letter
            Txt(dc, word, tr, g_tag, enabled ? col::text : col::dim, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX,
                S(1));
            r.right = pill.left - S(8);
        }
    }
    const int mw = std::min<int>(TextW(dc, main, g_font), r.right - r.left);
    RECT a = r;
    a.right = a.left + mw;
    Txt(dc, main, a, g_font, enabled ? col::text : col::dim);
    // In the closed box the grey description is shown only when it fits whole (the list shows all of it).
    const bool fits = !field || TextW(dc, rest, g_font) <= r.right - a.right - S(10);
    if (!rest.empty() && fits && r.right - a.right > S(30)) {
        RECT b = r;
        b.left = a.right + S(10);
        Txt(dc, rest, b, g_font, enabled ? col::text2 : col::dim);
    }
}

void Chevron(HDC dc, int cx, int cy, COLORREF c) {
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    Gdiplus::Pen pen(GC(c), (float)std::max(1.5, 1.7 * g_scale));
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    const float w = (float)S(4), h = (float)S(2) + 0.5f;
    g.DrawLine(&pen, cx - w, cy - h, (float)cx, cy + h);
    g.DrawLine(&pen, (float)cx, cy + h, cx + w, cy - h);
}

void PaintCombo(HWND h, HDC target) {
    RECT rc;
    GetClientRect(h, &rc);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bm = CreateCompatibleBitmap(target, rc.right, rc.bottom);
    const HGDIOBJ old = SelectObject(dc, bm);
    const Ctl& c = g_ctl[h];
    FillRect(dc, &rc, BrushOf(c.bgc));
    const bool en = IsWindowEnabled(h) != FALSE;
    const bool open = SendMessageW(h, CB_GETDROPPEDSTATE, 0, 0) != 0;
    const bool focus = GetFocus() == h;
    const COLORREF line = !en ? CLR_INVALID : (open || focus) ? col::accent : c.hover ? col::inputHover : CLR_INVALID;
    Round(dc, rc, (float)S(8), col::input, line);
    const int sel = Sel(h);
    if (sel >= 0) {
        const int n = (int)SendMessageW(h, CB_GETLBTEXTLEN, sel, 0);
        std::wstring s(std::max(n, 0), 0);
        if (n > 0) SendMessageW(h, CB_GETLBTEXT, sel, (LPARAM)s.data());
        RECT tr{rc.left + S(12), rc.top, rc.right - S(32), rc.bottom - S(3)};
        DrawEntry(dc, tr, s, en, true);
    }
    Chevron(dc, rc.right - S(17), (rc.top + rc.bottom) / 2, en ? (open ? col::text : col::text2) : col::dim);
    BitBlt(target, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old);
    DeleteObject(bm);
    DeleteDC(dc);
}

// A switch: white track with a black knob when on, grey when off.
void DrawToggle(const DRAWITEMSTRUCT* d, const Ctl& c) {
    HDC dc = d->hDC;
    RECT r = d->rcItem;
    FillRect(dc, &r, BrushOf(c.bgc));
    const bool on = Checked(d->hwndItem);
    const bool en = !(d->itemState & ODS_DISABLED);
    const int tw = S(42), th = S(24);
    RECT t{r.left, r.top + (std::min<int>(r.bottom - r.top, S(24)) - th) / 2, r.left + tw, 0};
    t.bottom = t.top + th;
    const COLORREF track = on ? (en ? (c.hover ? col::accentHi : col::accent) : RGB(84, 84, 90))
                              : (en ? (c.hover ? RGB(70, 70, 75) : col::track) : col::line);
    Round(dc, t, th / 2.0f, track);
    if (d->itemState & ODS_FOCUS) {
        RECT f = t;
        InflateRect(&f, S(3), S(3));
        Round(dc, f, th / 2.0f + S(3), CLR_INVALID, RGB(150, 150, 156), 1.2f);
    }
    const int k = th - S(6);
    RECT knob{on ? t.right - S(3) - k : t.left + S(3), t.top + S(3), 0, 0};
    knob.right = knob.left + k;
    knob.bottom = knob.top + k;
    Round(dc, knob, k / 2.0f, on ? (en ? col::ink : RGB(40, 40, 43)) : (en ? RGB(156, 156, 162) : col::dim));
    wchar_t buf[256];
    GetWindowTextW(d->hwndItem, buf, 256);
    RECT tr{t.right + S(14), r.top, r.right, r.bottom};
    Txt(dc, buf, tr, g_font, en ? col::text : col::dim,
        (r.bottom - r.top > S(30) ? DT_WORDBREAK : DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS) | DT_LEFT | DT_NOPREFIX);
}

// Buttons: rounded rectangles. The main action is white with black text, the others grey or text only.
void DrawButton(const DRAWITEMSTRUCT* d, const Ctl& c) {
    HDC dc = d->hDC;
    RECT r = d->rcItem;
    FillRect(dc, &r, BrushOf(c.bgc));
    const bool en = !(d->itemState & ODS_DISABLED);
    const bool down = (d->itemState & ODS_SELECTED) != 0;
    COLORREF fill = CLR_INVALID, line = CLR_INVALID, text = col::text;
    HFONT font = g_semi;
    switch (c.btn) {
    case B::Primary:
        fill = !en ? col::accentSoft : down ? col::accentLo : c.hover ? col::accentHi : col::accent;
        text = col::ink;
        break;
    case B::Secondary:
        fill = down ? col::button : c.hover ? col::buttonHover : col::button;
        break;
    case B::Ghost:
        fill = c.hover || down ? (c.bgc == col::side ? RGB(26, 26, 28) : col::button) : CLR_INVALID;
        text = c.hover ? col::text : col::text2;
        font = g_font;
        break;
    }
    if (d->itemState & ODS_FOCUS) line = RGB(150, 150, 156);
    if (fill != CLR_INVALID || line != CLR_INVALID) Round(dc, r, (float)S(8), fill, line);
    wchar_t buf[128];
    GetWindowTextW(d->hwndItem, buf, 128);
    const COLORREF fg = en ? text : col::dim;
    const int is = S(16), gap = S(8);
    const bool icon = c.icon >= 0;
    const int total = TextW(dc, buf, font) + (icon ? is + gap : 0);
    // in the sidebar the icon and text line up with the page list; elsewhere they are centred
    const int x0 = c.bgc == col::side ? r.left + S(17) : (r.left + r.right - total) / 2;
    if (icon) Icon(dc, c.icon, x0, (r.top + r.bottom - is) / 2, is, fg);
    RECT tr{c.bgc == col::side ? r.left + S(46) : x0 + (icon ? is + gap : 0), r.top, r.right, r.bottom};
    Txt(dc, buf, tr, font, fg, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

// Sidebar page: an icon and the name. The open page is white.
void DrawNav(const DRAWITEMSTRUCT* d, const Ctl& c) {
    HDC dc = d->hDC;
    RECT r = d->rcItem;
    FillRect(dc, &r, g_brSide);
    const bool sel = c.nav == g_curPage;
    if (sel || c.hover) Round(dc, r, (float)S(10), sel ? col::navSel : RGB(26, 26, 28));
    const COLORREF fg = sel ? col::ink : c.hover ? col::text : col::text2;
    const int is = S(18);
    Icon(dc, c.icon, r.left + S(16), (r.top + r.bottom - is) / 2, is, fg);
    wchar_t buf[64];
    GetWindowTextW(d->hwndItem, buf, 64);
    RECT tr{r.left + S(46), r.top, r.right - S(8), r.bottom};
    Txt(dc, buf, tr, g_semi, fg);
}

void PaintWindow(HDC target) {
    RECT rc;
    GetClientRect(g_wnd, &rc);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bm = CreateCompatibleBitmap(target, rc.right, rc.bottom);
    const HGDIOBJ old = SelectObject(dc, bm);

    FillRect(dc, &rc, g_brBg);
    RECT side{0, 0, S(kSideW), rc.bottom};
    FillRect(dc, &side, g_brSide);
    HBRUSH lb = CreateSolidBrush(col::line);
    RECT edge{S(kSideW) - 1, 0, S(kSideW), rc.bottom};
    FillRect(dc, &edge, lb);

    // brand: EVE1G, then "—— BY TEX1G" and the version
    const int bx = S(28), by = S(30);
    Txt(dc, L"EVE1G", {bx, by, S(kSideW) - S(12), by + S(40)}, g_brand, col::text, DT_LEFT | DT_TOP | DT_SINGLELINE,
        S(1));
    Txt(dc, L"BY TEX1G", {bx + S(1), by + S(44), S(kSideW) - S(12), by + S(60)}, g_tag, col::text2,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE, S(1));
    {
        std::wstring v = W(EVE1G_VERSION), kind;   // "0.11.2-beta" -> "BETA  0.11.2"
        const size_t dash = v.find(L'-');
        if (dash != std::wstring::npos) kind = Upper(v.substr(dash + 1)), v = v.substr(0, dash);
        const std::wstring label = kind.empty() ? v : kind + L"  " + v;
        const int vw = TextW(dc, label, g_tag, S(1)) + S(20);
        RECT pill{bx, by + S(70), bx + vw, by + S(70) + S(22)};
        Round(dc, pill, (float)S(6), CLR_INVALID, RGB(70, 70, 75));
        RECT tr = pill;
        tr.left += S(1);
        Txt(dc, label, tr, g_tag, col::text2, DT_CENTER | DT_VCENTER | DT_SINGLELINE, S(1));
    }
    Txt(dc, L"For the Rockstar Editor only. Never take mods into GTA Online.",
        {bx, rc.bottom - S(122), S(kSideW) - S(24), rc.bottom - S(70)}, g_small, col::hint, DT_LEFT | DT_WORDBREAK);

    // page title: big, wide, uppercase, with a dash after it; the page number on the right
    const PageInfo& pg = kPage[g_curPage];
    const std::wstring title = Upper(pg.title);
    const int ty = S(28);
    Txt(dc, title, {S(kLeftX), ty, rc.right - S(kPad) - S(80), ty + S(50)}, g_pageTitle, col::text,
        DT_LEFT | DT_TOP | DT_SINGLELINE);
    wchar_t num[16];
    swprintf_s(num, L"%02d / %02d", g_curPage + 1, kPages);
    Txt(dc, num, {rc.right - S(kPad) - S(120), ty + S(8), rc.right - S(kPad), ty + S(30)}, g_num, col::hint,
        DT_RIGHT | DT_VCENTER | DT_SINGLELINE, S(1));
    Txt(dc, pg.sub, {S(kLeftX), S(90), rc.right - S(kPad), S(112)}, g_font, col::text2,
        DT_LEFT | DT_TOP | DT_SINGLELINE);

    // cards of this page
    for (const Card& cd : g_cards) {
        if (cd.page != g_curPage) continue;
        RECT r{S(cd.x), S(cd.y), S(cd.x + cd.w), S(cd.y + cd.h)};
        Round(dc, r, (float)S(16), col::card, col::cardLine);
        Icon(dc, cd.icon, r.left + S(kIn), r.top + S(21), S(18), col::text);
        Txt(dc, Upper(cd.title), {r.left + S(kIn) + S(cd.icon >= 0 ? 28 : 0), r.top + S(20), r.right - S(kIn), r.top + S(40)}, g_cardTitle,
            col::text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, S(1));
    }
    // boxes around the text fields
    for (const auto& [h, c] : g_ctl) {
        if (c.kind != K::Edit || !IsWindowVisible(h)) continue;
        const bool en = IsWindowEnabled(h) != FALSE;
        const COLORREF line = !en ? CLR_INVALID : GetFocus() == h ? col::accent : c.hover ? col::inputHover : CLR_INVALID;
        Round(dc, c.frame, (float)S(8), col::input, line);
    }

    // bottom bar: a hairline above the status and the buttons
    RECT top{S(kLeftX), rc.bottom - S(kFootH), rc.right - S(kPad), rc.bottom - S(kFootH) + 1};
    FillRect(dc, &top, lb);
    DeleteObject(lb);

    BitBlt(target, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old);
    DeleteObject(bm);
    DeleteDC(dc);
}

void Repaint(HWND h) {   // the box of a text field is drawn by the window behind it
    const auto it = g_ctl.find(h);
    if (it != g_ctl.end() && it->second.kind == K::Edit) {
        RECT f = it->second.frame;
        InvalidateRect(g_wnd, &f, FALSE);
    }
}

// Hover for buttons, switches and the sidebar.
LRESULT CALLBACK HoverProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    switch (msg) {
    case WM_MOUSEMOVE: {
        Ctl& c = g_ctl[h];
        if (!c.hover) {
            c.hover = true;
            TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, h, 0};
            TrackMouseEvent(&t);
            InvalidateRect(h, nullptr, FALSE);
        }
        break;
    }
    case WM_MOUSELEAVE:
        g_ctl[h].hover = false;
        InvalidateRect(h, nullptr, FALSE);
        break;
    case WM_LBUTTONDBLCLK:   // a quick second click on a switch flips it again
        return DefSubclassProc(h, WM_LBUTTONDOWN, wp, lp);
    }
    return DefSubclassProc(h, msg, wp, lp);
}

LRESULT CALLBACK ComboProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        PaintCombo(h, dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE: {
        Ctl& c = g_ctl[h];
        if (!c.hover) {
            c.hover = true;
            TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, h, 0};
            TrackMouseEvent(&t);
            InvalidateRect(h, nullptr, FALSE);
        }
        break;
    }
    case WM_MOUSELEAVE:
        g_ctl[h].hover = false;
        InvalidateRect(h, nullptr, FALSE);
        break;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ENABLE:
    case CB_SETCURSEL: {
        const LRESULT r = DefSubclassProc(h, msg, wp, lp);
        InvalidateRect(h, nullptr, FALSE);
        return r;
    }
    }
    return DefSubclassProc(h, msg, wp, lp);
}

LRESULT CALLBACK EditProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    switch (msg) {
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ENABLE: {
        const LRESULT r = DefSubclassProc(h, msg, wp, lp);
        Repaint(h);
        return r;
    }
    case WM_MOUSEMOVE: {
        Ctl& c = g_ctl[h];
        if (!c.hover) {
            c.hover = true;
            TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, h, 0};
            TrackMouseEvent(&t);
            Repaint(h);
        }
        break;
    }
    case WM_MOUSELEAVE:
        g_ctl[h].hover = false;
        Repaint(h);
        break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

void FlashStatus(const wchar_t* text) {
    g_ctl[hStatus].fg = col::good;
    SetWindowTextW(hStatus, text);
    InvalidateRect(hStatus, nullptr, TRUE);
    SetTimer(g_wnd, 1, 2600, nullptr);
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND: {
        const int id = LOWORD(wp), code = HIWORD(wp);
        const HWND from = (HWND)lp;
        if (code == BN_CLICKED) {   // switches keep their own state
            const auto t = g_toggle.find(from);
            if (t != g_toggle.end()) {
                t->second = !t->second;
                InvalidateRect(from, nullptr, FALSE);
            }
        }
        if (code == CBN_DROPDOWN || code == CBN_CLOSEUP) InvalidateRect(from, nullptr, FALSE);
        if (id >= ID_NAV && id < ID_NAV + kPages) {
            ShowPage(id - ID_NAV);
        } else if (id == ID_BROWSE) {
            Browse();
        } else if ((id == ID_CODEC || id == ID_FPS || id == ID_SHUTTER || id == ID_CAPTURE) && code == CBN_SELCHANGE) {
            if (id == ID_CODEC) {   // a new preset starts from its recommended quality
                const int c = CodecSel();
                if (c >= 0 && kCodecs[c].quality != Q::None)
                    SetWindowTextW(hQuality, std::to_wstring(kCodecs[c].defQuality).c_str());
            }
            UpdateHints();
        } else if ((id == ID_SAMPLES || id == ID_FPS || id == ID_NAMING) && code == EN_CHANGE) {
            UpdateHints();
        } else if ((id == ID_NAMING || id == ID_CAPTURE || id == ID_DEPTH) && code == BN_CLICKED) {
            UpdateHints();
        } else if (id == ID_DEPTH && code == CBN_SELCHANGE) {
            UpdateHints();
        } else if (id == ID_SAVE) {
            if (Save()) {
                UpdateHints();
                FlashStatus(L"Saved. It applies to your next export.");
            }
        } else if (id == ID_CLOSE) {
            DestroyWindow(h);
        } else if (id == ID_GITHUB) {
            ShellExecuteW(h, L"open", kGitHub, nullptr, nullptr, SW_SHOWNORMAL);
        } else if (id == ID_VOUKODER) {
            const std::wstring dir = W(g_dir);
            const std::wstring exe = dir + L"EVE1G-Encoder.exe";
            SHELLEXECUTEINFOW sei{sizeof(sei)};
            sei.fMask = SEE_MASK_NOCLOSEPROCESS;
            sei.hwnd = h;
            sei.lpFile = exe.c_str();
            sei.lpDirectory = dir.c_str();
            sei.nShow = SW_SHOWNORMAL;
            if (ShellExecuteExW(&sei) && sei.hProcess) {
                EnableWindow(h, FALSE);
                WaitForSingleObject(sei.hProcess, INFINITE);
                CloseHandle(sei.hProcess);
                EnableWindow(h, TRUE);
                SetForegroundWindow(h);
                Load();   // shows "Custom" unless the Voukoder choice matches a preset
            } else {
                MessageBoxW(h, L"EVE1G-Encoder.exe was not found next to this program.", kApp, MB_ICONERROR);
            }
        } else if (id == ID_OPENFOLDER) {
            ShellExecuteW(h, L"open", W(OutputFolder()).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        } else if (id == ID_TESTFORMATS) {
            HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
            SetWindowTextW(hFormats, L"Testing every format at 4K... (about a minute)");
            UpdateWindow(hFormats);
            EnableWindow(h, FALSE);
            const int ok = ProbeAll();
            EnableWindow(h, TRUE);
            SetCursor(old);
            PopulateCodecs();
            UpdateHints();
            MessageBoxW(h, (std::to_wstring(ok) + L" of " + std::to_wstring(kCustom) +
                            L" formats work on this PC. The others are hidden from the list.").c_str(),
                        kApp, MB_ICONINFORMATION);
        } else if (id == ID_OPENLOG) {
            ShellExecuteW(h, L"open", L"notepad.exe", W(LogPath()).c_str(), nullptr, SW_SHOWNORMAL);
        }
        return 0;
    }
    case WM_TIMER:
        if (wp == 1) {
            KillTimer(h, 1);
            UpdateStatus();
        }
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE && hStatus) UpdateStatus();   // refresh after an export
        break;
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT* m = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
        if (m->CtlType == ODT_COMBOBOX) {
            m->itemHeight = m->itemID == (UINT)-1 ? S(32) - 6 : S(34);
            return TRUE;
        }
        break;
    }
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* d = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
        if (d->CtlType == ODT_COMBOBOX) {
            if (d->itemState & ODS_COMBOBOXEDIT) {   // the closed box: drawn whole by PaintCombo
                InvalidateRect(d->hwndItem, nullptr, FALSE);
                return TRUE;
            }
            if (d->itemID == (UINT)-1) return TRUE;
            const bool sel = (d->itemState & ODS_SELECTED) != 0;
            RECT r = d->rcItem;
            FillRect(d->hDC, &r, sel ? g_brSoft : g_brInput);
            const int n = (int)SendMessageW(d->hwndItem, CB_GETLBTEXTLEN, d->itemID, 0);
            std::wstring s(std::max(n, 0), 0);
            if (n > 0) SendMessageW(d->hwndItem, CB_GETLBTEXT, d->itemID, (LPARAM)s.data());
            RECT tr{r.left + S(12), r.top, r.right - S(10), r.bottom};
            DrawEntry(d->hDC, tr, s, true);
            return TRUE;
        }
        if (d->CtlType == ODT_BUTTON) {
            const auto it = g_ctl.find(d->hwndItem);
            if (it == g_ctl.end()) break;
            if (it->second.kind == K::Toggle) DrawToggle(d, it->second);
            else if (it->second.kind == K::Nav) DrawNav(d, it->second);
            else DrawButton(d, it->second);
            return TRUE;
        }
        break;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        const auto it = g_ctl.find((HWND)lp);
        if (it == g_ctl.end()) break;
        if (it->second.kind == K::Edit) {   // a disabled or read-only field
            SetTextColor(dc, col::dim);
            SetBkColor(dc, col::input);
            return (LRESULT)g_brInput;
        }
        SetTextColor(dc, it->second.fg);
        SetBkColor(dc, it->second.bgc);
        return (LRESULT)BrushOf(it->second.bgc);
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC dc = (HDC)wp;
        SetTextColor(dc, col::text);
        SetBkColor(dc, col::input);
        return (LRESULT)g_brInput;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        PaintWindow(dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

bool HasFont(const wchar_t* face) {
    HDC dc = GetDC(nullptr);
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcsncpy(lf.lfFaceName, face, LF_FACESIZE - 1);
    bool found = false;
    EnumFontFamiliesExW(
        dc, &lf, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM p) -> int { return *(bool*)p = true, 0; },
        (LPARAM)&found, 0);
    ReleaseDC(nullptr, dc);
    return found;
}

HFONT Font(int px, int weight, const wchar_t* face = L"Segoe UI") {
    return CreateFontW(-S(px), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    SetProcessDPIAware();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    {
        HDC dc = GetDC(nullptr);
        g_scale = GetDeviceCaps(dc, LOGPIXELSY) / 96.0;
        ReleaseDC(nullptr, dc);
    }

    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    g_dir = path;
    g_dir = g_dir.substr(0, g_dir.find_last_of("\\/") + 1);

    // EVE1G-Settings.exe --probe : test every format, write formats.ini, no window (used by build.bat)
    if (wcsstr(GetCommandLineW(), L"--probe")) {
        const int ok = ProbeAll();
        CoUninitialize();
        return ok;
    }

    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR gdiplus = 0;
    Gdiplus::GdiplusStartup(&gdiplus, &gsi, nullptr);

    // Headings in Bahnschrift (built into Windows 10 / 11), Segoe UI if it is missing.
    const bool bahn = HasFont(L"Bahnschrift"), bahnSemi = HasFont(L"Bahnschrift SemiBold");
    const wchar_t* display = bahn ? L"Bahnschrift" : L"Segoe UI";
    g_font = Font(14, FW_NORMAL);
    g_small = Font(13, FW_NORMAL);
    g_semi = Font(14, FW_SEMIBOLD);
    g_cardTitle = bahnSemi ? Font(14, FW_NORMAL, L"Bahnschrift SemiBold") : Font(14, FW_BOLD, display);
    g_pageTitle = Font(40, FW_BOLD, display);
    g_brand = Font(32, FW_BOLD, display);
    g_tag = bahnSemi ? Font(11, FW_NORMAL, L"Bahnschrift SemiBold") : Font(11, FW_SEMIBOLD, display);
    g_num = Font(13, FW_NORMAL, display);
    g_brBg = CreateSolidBrush(col::bg);
    g_brSide = CreateSolidBrush(col::side);
    g_brFoot = CreateSolidBrush(col::foot);
    g_brCard = CreateSolidBrush(col::card);
    g_brInput = CreateSolidBrush(col::input);
    g_brSoft = CreateSolidBrush(col::accentSoft);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"EVE1GSettings";
    RegisterClassExW(&wc);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    const std::wstring title = L"EVE1G Settings  -  " + VersionText();
    g_wnd = CreateWindowExW(0, wc.lpszClassName, title.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT, S(kWinW), S(600),
                            nullptr, nullptr, inst, nullptr);
    {
        // dark title bar (Windows 10 2004+ / 11), coloured like the sidebar on Windows 11
        BOOL dark = TRUE;
        if (FAILED(DwmSetWindowAttribute(g_wnd, 20, &dark, sizeof(dark))))
            DwmSetWindowAttribute(g_wnd, 19, &dark, sizeof(dark));
        COLORREF caption = col::side, captionText = col::text;
        DwmSetWindowAttribute(g_wnd, 35, &caption, sizeof(caption));
        DwmSetWindowAttribute(g_wnd, 36, &captionText, sizeof(captionText));
    }
    Build();
    PopulateCodecs();
    RECT want{0, 0, S(kWinW), S(g_height)};
    AdjustWindowRect(&want, style, FALSE);
    SetWindowPos(g_wnd, nullptr, 0, 0, want.right - want.left, want.bottom - want.top, SWP_NOMOVE | SWP_NOZORDER);
    Load();
    {
        // --page=N opens on a page (1 = Output ... 5 = Camera data)
        const wchar_t* pg = wcsstr(GetCommandLineW(), L"--page=");
        ShowPage(pg ? _wtoi(pg + 7) - 1 : 0);
    }
    ShowWindow(g_wnd, show);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        if (!IsDialogMessageW(g_wnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    Gdiplus::GdiplusShutdown(gdiplus);
    CoUninitialize();
    return 0;
}
