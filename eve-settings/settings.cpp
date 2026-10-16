// EVE1G Settings - the settings window for EVE1G.ini and preset.json.
// Lives in plugins\EVE1G next to those files. Changes apply to the next export (EVE1G re-reads them every time).
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <CommCtrl.h>
#include <ShlObj.h>
#include <shellapi.h>

#include <INIReader.h>
#include <wrl/client.h>
#include "VoukoderTypeLib_h.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

const wchar_t* kApp = L"EVE1G";

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
    {L"Clean  -  before the game UI, ReShade applied  [recommended]", "clean"},
    {L"Present  -  what is on screen (progress bar + cursor)", "present"},
    {L"Encode  -  Rockstar's export image, no ReShade", "encode"},
    {L"Effects  -  experimental", "effects"},
    {L"Legacy  -  original EVE (black with the Upscaler)", "legacy"},
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
    {L"PNG 16-bit sequence  -  lossless, for AE / Premiere / Resolve  [recommended]", "png16"},
    {L"ProRes 4444 video  -  one .mov next to the video (10-bit)", "prores"},
    {L"OpenEXR float sequence  -  real distance in metres (Nuke / Fusion)", "exr"},
};
const Choice kMaster[] = {
    {L"8-bit  -  as original EVE  [default]", "false"},
    {L"Auto  -  16-bit into 10-bit / lossless formats  (new, testing)", "auto"},
    {L"16-bit always  -  smoothest gradients  (new, testing)", "true"},
};
const Choice kOsdSize[] = {
    {L"Normal  (grows with the screen: 2x at 4K)", "1"},
    {L"Large", "1.5"},
    {L"Extra large", "2"},
};
const Choice kDepthCurve[] = {{L"Log - more detail close up", "log"}, {L"Linear", "linear"}};
const Choice kDepthWhite[] = {{L"White = near", "near"}, {L"White = far", "far"}};

// ------------------------------------------------------------------------------------------------------------ state
std::string g_dir;   // folder of this exe, with trailing backslash
HWND g_wnd;
HFONT g_font, g_bold, g_title;
double g_scale = 1.0;
HWND hOut, hProject, hShot, hTake, hSidecar, hPreview;
HWND hCodec, hQuality, hQualityLabel, hBitrate, hBitrateLabel, hNote, hFormats;
HWND hFps, hFpsCustom, hSamples, hShutter, hInfo, hRealCam, hRolling;
HWND hCapture, hReshade, hLivePreview, hHideUi, hProgress, hExr, hLog;
HWND hDepthOn, hDepthFormat, hDepthNear, hDepthFar, hDepthCurve, hDepthWhite, hDepthNote;
HWND hHfrOn, hHfrFormat, hHfrEvery, hHfrNote;
HWND hNormalsOn, hNormalsFov, hPlateOn, hProxyOn, hIdOn, hPassNote, hMaster, hNotify, hCaptureNote, hOsdSize;
HWND hCamFiles, hCamFolder;
HWND hStatus;
std::vector<HWND> g_groups;
std::vector<HWND> g_hints;

enum : int {
    ID_BROWSE = 100, ID_FPS, ID_SAMPLES, ID_SHUTTER, ID_CODEC, ID_SAVE, ID_CLOSE, ID_VOUKODER, ID_OPENFOLDER,
    ID_OPENLOG, ID_NAMING, ID_CAPTURE, ID_TESTFORMATS, ID_DEPTH, ID_TABS
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
bool Checked(HWND h) { return SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED; }
void Check(HWND h, bool on) { SendMessageW(h, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0); }

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

// ------------------------------------------------------------------------------------------------------------- layout
// All positions are in 96-dpi units and scaled, so the window looks the same at 125 % / 150 % Windows scaling.
int S(int v) { return (int)std::lround(v * g_scale); }

constexpr int kPad = 14;                 // window margin
constexpr int kColW = 470;               // column width
constexpr int kGap = 14;                 // between columns
constexpr int kLeftX = kPad, kRightX = kPad + kColW + kGap;
constexpr int kLabelW = 118;             // label column inside a group
constexpr int kRow = 32;
constexpr int kWinW = kPad * 2 + kColW * 2 + kGap;

// Tabs: every control made while g_page >= 0 belongs to that tab.
constexpr int kPages = 4;
const wchar_t* kPageNames[kPages] = {L"  Output  ", L"  Motion + Camera  ", L"  Passes  ", L"  Workflow  "};
int g_page = -1, g_curPage = 0;
std::vector<HWND> g_pages[kPages];
HWND hTabs;

HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id = 0, DWORD ex = 0,
          HFONT font = nullptr) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g_wnd,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)(font ? font : g_font), TRUE);
    if (g_page >= 0) g_pages[g_page].push_back(c);   // shown only on its tab
    return c;
}

// A titled box. Controls are placed inside it by the caller; returns the y of its first row.
struct Group {
    int x, y, w;
    int row;   // current row y
    HWND box;
};
Group BeginGroup(const wchar_t* title, int x, int y, int w) {
    Group g{x, y, w, y + 26, nullptr};
    g.box = Make(WC_BUTTONW, title, BS_GROUPBOX, x, y, w, 40, 0, 0, g_bold);
    g_groups.push_back(g.box);
    return g;
}
int EndGroup(Group& g) {   // sizes the box to its content, returns the y below it
    const int h = g.row - g.y + 8;
    SetWindowPos(g.box, nullptr, 0, 0, S(g.w), S(h), SWP_NOMOVE | SWP_NOZORDER);
    return g.y + h;
}
HWND Label(Group& g, const wchar_t* text) {
    return Make(L"STATIC", text, SS_LEFT, g.x + 12, g.row + 4, kLabelW - 8, 20);
}
int CtrlX(const Group& g) { return g.x + 12 + kLabelW; }
int CtrlW(const Group& g) { return g.w - 24 - kLabelW; }
HWND Hint(int x, int y, int w, int h, const wchar_t* text = L"") {
    HWND c = Make(L"STATIC", text, SS_LEFT, x, y, w, h);
    g_hints.push_back(c);
    return c;
}
HWND Combo(int id, int x, int y, int w) {
    return Make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, x, y, w, 400, id);
}
HWND Edit(int x, int y, int w, DWORD style = 0, int id = 0) {
    return Make(WC_EDITW, L"", ES_AUTOHSCROLL | WS_TABSTOP | style, x, y, w, 24, id, WS_EX_CLIENTEDGE);
}
HWND CheckBox(const wchar_t* text, int x, int y, int w, int id = 0) {
    return Make(WC_BUTTONW, text, BS_AUTOCHECKBOX | WS_TABSTOP, x, y, w, 22, id);
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
        EnableWindow(hQualityLabel, q);
        SetWindowTextW(hQualityLabel, k.quality == Q::Cq ? L"Quality (CQ)" : L"Quality (CRF)");
        EnableWindow(hBitrate, k.bitrate);
        EnableWindow(hBitrateLabel, k.bitrate);
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
    swprintf_s(buf, L"Each video frame = %d game frame(s), %d blended. The game runs at %.0f fps.\r\n%s", samples + 1,
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
    const bool exr = df == 2;
    EnableWindow(hDepthFormat, depthOn || normalsOn);
    for (HWND c : {hDepthNear, hDepthFar, hDepthCurve, hDepthWhite}) EnableWindow(c, depthOn && !exr);
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
            if (depthOn) note += L" " + W(name) + (df == 1 ? L"_depth.mov" : L"_depth\\*.png");
            if (normalsOn) note += L"  " + W(name) + (df == 1 ? L"_normals.mov" : L"_normals\\*.png");
        }
        note += L"\r\nNeeds EVE1G_Depth.fx in reshade-shaders\\Shaders.";
    }
    SetWindowTextW(hDepthNote, note.c_str());

    {
        const bool cleanReshade = clean && Checked(hReshade);
        std::wstring pn;
        if (Checked(hPlateOn)) {
            pn += cleanReshade ? L"Plate: " + W(name) + L"_plate.mov   "
                               : L"Plate needs capture Clean with ReShade applied (Motion + Camera tab).   ";
        }
        if (Checked(hProxyOn)) pn += L"Proxy: " + W(name) + L"_proxy.mp4 (half size)   ";
        if (Checked(hIdOn)) pn += L"ID: " + W(name) + L"_id\\*.png + _id_legend.txt (no MSAA)";
        if (pn.empty()) pn = L"Each one is written next to the video, frame for frame. The proxy relinks to the master "
                             L"in Premiere / Resolve by name.";
        SetWindowTextW(hPassNote, pn.c_str());
    }
    {
        static const wchar_t* capNotes[] = {
            L"The frame is taken before the game draws its UI and your ReShade preset is rendered on it. Needed for "
            L"the clean plate and the on-screen progress.",
            L"Reads the finished screen - progress bar and cursor end up in the video.",
            L"Rockstar's own export image: no ReShade, no DLSS 5 effects drawn at present.",
            L"Reads where ReShade renders its effects. Experimental.",
            L"Original EVE capture, kept for comparison. Black with the GTA V Upscaler.",
        };
        const int ci = std::clamp(Sel(hCapture), 0, 4);
        SetWindowTextW(hCaptureNote, capNotes[ci]);
    }

    const bool hfrOn = Checked(hHfrOn);
    EnableWindow(hHfrFormat, hfrOn);
    EnableWindow(hHfrEvery, hfrOn);
    std::wstring hn;
    if (!hfrOn) {
        hn = L"Off. When on, every game frame is also saved unblended - real high-frame-rate footage for slow "
             L"motion, speed ramps or your own blur in post.";
    } else {
        const int every = kHfrEvery[std::max(0, Sel(hHfrEvery))].n;
        const double hfps = fps * (samples + 1) / every;
        wchar_t b[256];
        swprintf_s(b, L"%.0f fps, no audio:  %s%s   (big files - fast drive recommended)", hfps, W(name).c_str(),
                   Sel(hHfrFormat) == 1 ? L"_hfr...\\*.png" : (L"_hfr" + std::to_wstring((int)std::lround(hfps)) + L".mov").c_str());
        hn = b;
    }
    SetWindowTextW(hHfrNote, hn.c_str());
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
    SetWindowTextW(hStatus, s.c_str());
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
    std::wstring line = tested ? L"Showing the " + std::to_wstring(shown) + L" of " + std::to_wstring(kCustom) +
                                     L" formats that work on this PC (tested " +
                                     W(formats.GetString("formats", "tested", "")) + L")."
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
    Check(hRealCam, ini.GetBoolean("EXPORT", "blend_linear", false));
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
    Check(hHideUi, ini.GetBoolean("EXPORT", "hide_ui", true));
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
    Check(hExr, ini.GetBoolean("EXPORT", "export_openexr", false));

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
        SendMessageW(hDepthCurve, CB_SETCURSEL, Trim(ini.GetString("DEPTH", "curve", "log")) == "linear" ? 1 : 0, 0);
        SendMessageW(hDepthWhite, CB_SETCURSEL, Trim(ini.GetString("DEPTH", "white", "near")) == "far" ? 1 : 0, 0);
        Check(hNormalsOn, ini.GetBoolean("PASSES", "normals", false));
        snprintf(b, sizeof(b), "%g", ini.GetReal("PASSES", "normals_fov", 50.0));
        SetWindowTextW(hNormalsFov, W(b).c_str());
        Check(hPlateOn, ini.GetBoolean("PASSES", "clean_plate", false));
        Check(hProxyOn, ini.GetBoolean("PASSES", "proxy", false));
        Check(hIdOn, ini.GetBoolean("PASSES", "id_pass", false));
        Check(hNotify, ini.GetBoolean("EXPORT", "notify_done", false));
        {
            const std::string m = Trim(ini.GetString("EXPORT", "master_16bit", "false"));
            SendMessageW(hMaster, CB_SETCURSEL, (m == "true" || m == "1" || m == "on") ? 2
                                                : (m == "auto") ? 1 : 0, 0);
        }
        Check(hHfrOn, ini.GetBoolean("HFR", "enabled", false));
        SendMessageW(hHfrFormat, CB_SETCURSEL, Trim(ini.GetString("HFR", "format", "prores")) == "png" ? 1 : 0, 0);
        const long ev = ini.GetInteger("HFR", "every", 1);
        SendMessageW(hHfrEvery, CB_SETCURSEL, ev >= 4 ? 2 : ev >= 2 ? 1 : 0, 0);
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
        const bool real = Checked(hRealCam);
        const int ri = std::max(0, Sel(hRolling));
        char ms[16];
        snprintf(ms, sizeof(ms), "%.0f", kRolling[ri].ms);
        ini += "; Real camera: blend in linear light like a sensor (bright lights streak) and fade the shutter in / out\r\n";
        ini += ";   like a rotary shutter. false = the old display-image blend with a hard shutter.\r\n";
        ini += "blend_linear = " + b(real) + "\r\n";
        ini += "shutter_soft = " + std::string(real ? "0.15" : "0") + "\r\n";
        ini += "; Rolling shutter: ms the sensor takes to read the image top to bottom (0 = global shutter, like a\r\n";
        ini += ";   cinema camera; 8-25 = mirrorless / phone - fast pans lean). Needs motion blur samples.\r\n";
        ini += "rolling_shutter_ms = " + std::string(ms) + "\r\n";
    }
    ini += "export_openexr = " + b(Checked(hExr)) + "\r\n\r\n";
    ini += "; clean   = frame taken before the game UI, your ReShade preset applied (no progress bar, no cursor)\r\n";
    ini += "; present = what is on screen (includes the progress bar and cursor)\r\n";
    ini += "; encode  = Rockstar's export image, no ReShade\r\n";
    ini += "; effects = experimental;  legacy = original EVE (black with the GTA V Upscaler)\r\n";
    ini += "capture_source = " + std::string(kCapture[Sel(hCapture)].value) + "\r\n";
    ini += "; clean: apply your ReShade preset (DOF, grade) to the video\r\n";
    ini += "clean_reshade = " + b(Checked(hReshade)) + "\r\n";
    ini += "; clean: true = the screen shows the finished frame while exporting (DOF visible for focus pulls,\r\n";
    ini += ";   no progress bar); false = the screen shows the game with its UI but without ReShade\r\n";
    ini += "clean_preview = " + b(Checked(hLivePreview)) + "\r\n";
    ini += "; Hide the game UI (export progress bar, buttons, spinner) while exporting\r\n";
    ini += "hide_ui = " + b(Checked(hHideUi)) + "\r\n";
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
        ini += "; png16 = 16-bit PNG sequence,  prores = ProRes 4444 .mov,  exr = float OpenEXR sequence (metres)\r\n";
        ini += "format = " + std::string(kDepthFormat[std::max(0, Sel(hDepthFormat))].value) + "\r\n";
        ini += "; png16 / prores: distance range in metres, curve (log / linear) and which end is white (near / far)\r\n";
        ini += "near = " + std::string(nb) + "\r\nfar = " + std::string(fb) + "\r\n";
        ini += "curve = " + std::string(kDepthCurve[std::max(0, Sel(hDepthCurve))].value) + "\r\n";
        ini += "white = " + std::string(kDepthWhite[std::max(0, Sel(hDepthWhite))].value) + "\r\n\r\n";
        ini += "[HFR]\r\n";
        ini += "; HFR export: every game frame also saved unblended (fps x (samples + 1)), no audio\r\n";
        ini += "enabled = " + b(Checked(hHfrOn)) + "\r\n";
        ini += "; prores = ProRes 422 HQ .mov,  png = PNG sequence\r\n";
        ini += "format = " + std::string(kHfrFormat[std::max(0, Sel(hHfrFormat))].value) + "\r\n";
        ini += "; keep every Nth game frame (1 = all)\r\n";
        ini += "every = " + std::to_string(kHfrEvery[std::max(0, Sel(hHfrEvery))].n) + "\r\n\r\n";
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
        ini += "; id_pass = EXPERIMENTAL colour matte from the game's stencil buffer, PNG sequence + legend\r\n";
        ini += "id_pass = " + b(Checked(hIdOn)) + "\r\n";
    }
    {
        std::ofstream f(IniPath(), std::ios::binary | std::ios::trunc);
        if (!f) {
            MessageBoxW(g_wnd, L"Could not write EVE1G.ini. Is FiveM's folder read-only?", kApp, MB_ICONERROR);
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
int g_height = 0;

void Build() {
    // title
    g_page = -1;
    Make(L"STATIC", L"EVE1G", SS_LEFT, kPad, 10, 88, 30, 0, 0, g_title);
    Hint(kPad + 92, 20, 640, 20, L"v0.9.0-beta by TEX1G  •  Rockstar Editor export with ReShade + motion blur, without the UI");
    hTabs = Make(WC_TABCONTROLW, L"", WS_TABSTOP | TCS_FOCUSNEVER, kPad, 48, kWinW - kPad * 2, 28, ID_TABS);
    for (int i = 0; i < kPages; ++i) {
        TCITEMW it{};
        it.mask = TCIF_TEXT;
        it.pszText = const_cast<wchar_t*>(kPageNames[i]);
        SendMessageW(hTabs, TCM_INSERTITEMW, i, (LPARAM)&it);
    }
    const int top = 86;
    int pageBottom[kPages] = {};

    // ================= 1. Output: where and in what format
    g_page = 0;
    Group out = BeginGroup(L"Output and naming", kLeftX, top, kColW);
    Label(out, L"Folder");
    hOut = Edit(CtrlX(out), out.row, CtrlW(out) - 90, 0, ID_NAMING);
    Make(WC_BUTTONW, L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, CtrlX(out) + CtrlW(out) - 84, out.row - 1, 84, 26,
         ID_BROWSE);
    out.row += kRow;
    Label(out, L"Project");
    hProject = Edit(CtrlX(out), out.row, 150, 0, ID_NAMING);
    Make(L"STATIC", L"Shot", SS_RIGHT, CtrlX(out) + 156, out.row + 4, 40, 20);
    hShot = Edit(CtrlX(out) + 202, out.row, CtrlW(out) - 202, 0, ID_NAMING);
    out.row += kRow;
    Label(out, L"Take");
    hTake = Edit(CtrlX(out), out.row, 50, ES_NUMBER, ID_NAMING);
    Hint(CtrlX(out) + 58, out.row + 4, 110, 20, L"0 = next free");
    hSidecar = CheckBox(L"Settings .txt beside each video", CtrlX(out) + 160, out.row + 1, CtrlW(out) - 160, ID_NAMING);
    out.row += kRow;
    hPreview = Hint(out.x + 12, out.row, out.w - 24, 52);
    out.row += 54;
    int leftY = EndGroup(out);

    Group enc = BeginGroup(L"Video format (Voukoder)", kRightX, top, kColW);
    Label(enc, L"Preset");
    hCodec = Combo(ID_CODEC, CtrlX(enc), enc.row, CtrlW(enc));
    enc.row += kRow;
    hQualityLabel = Label(enc, L"Quality (CRF)");
    hQuality = Edit(CtrlX(enc), enc.row, 50, ES_NUMBER);
    hBitrateLabel = Make(L"STATIC", L"Bitrate Mbit/s (0 = quality only)", SS_LEFT, CtrlX(enc) + 62, enc.row + 4, 200, 20);
    hBitrate = Edit(CtrlX(enc) + CtrlW(enc) - 60, enc.row, 60, ES_NUMBER);
    enc.row += kRow;
    Label(enc, L"Precision");
    hMaster = Combo(ID_CAPTURE, CtrlX(enc), enc.row, CtrlW(enc));
    for (const Choice& c : kMaster) Add(hMaster, c.label);
    enc.row += kRow;
    hNote = Hint(enc.x + 12, enc.row, enc.w - 24, 52);
    enc.row += 54;
    hFormats = Hint(enc.x + 12, enc.row, enc.w - 24, 20);
    enc.row += 24;
    Make(WC_BUTTONW, L"Advanced Voukoder settings...", BS_PUSHBUTTON | WS_TABSTOP, enc.x + 12, enc.row, 210, 28,
         ID_VOUKODER);
    Make(WC_BUTTONW, L"Test formats again", BS_PUSHBUTTON | WS_TABSTOP, enc.x + enc.w - 12 - 150, enc.row, 150, 28,
         ID_TESTFORMATS);
    enc.row += 34;
    int rightY = EndGroup(enc);
    pageBottom[0] = std::max(leftY, rightY);

    // ================= 2. Motion & camera: how the frames look
    g_page = 1;
    Group tm = BeginGroup(L"Frame rate and motion blur", kLeftX, top, kColW);
    Label(tm, L"Frame rate");
    hFps = Combo(ID_FPS, CtrlX(tm), tm.row, 110);
    for (const Fps& f : kFps) Add(hFps, f.label);
    Add(hFps, L"Custom...");
    hFpsCustom = Edit(CtrlX(tm) + 118, tm.row, 100, 0, ID_FPS);
    tm.row += kRow;
    Label(tm, L"Blur samples");
    hSamples = Edit(CtrlX(tm), tm.row, 50, ES_NUMBER, ID_SAMPLES);
    Hint(CtrlX(tm) + 58, tm.row + 4, CtrlW(tm) - 58, 20, L"0 = off,  12 cinematic,  39 = 960 fps at 24 (cleanest)");
    tm.row += kRow;
    Label(tm, L"Shutter angle");
    hShutter = Combo(ID_SHUTTER, CtrlX(tm), tm.row, 200);
    for (const Shutter& s : kShutter) Add(hShutter, s.label);
    tm.row += kRow;
    hRealCam = CheckBox(L"Real camera blur - light-correct, soft shutter edges", tm.x + 12, tm.row, tm.w - 24);
    tm.row += 26;
    Label(tm, L"Rolling shutter");
    hRolling = Combo(0, CtrlX(tm), tm.row, CtrlW(tm));
    for (const Rolling& r : kRolling) Add(hRolling, r.label);
    tm.row += kRow;
    hInfo = Hint(tm.x + 12, tm.row, tm.w - 24, 36);
    tm.row += 38;
    leftY = EndGroup(tm);

    Group cap = BeginGroup(L"Capture and ReShade", kRightX, top, kColW);
    Label(cap, L"Capture");
    hCapture = Combo(ID_CAPTURE, CtrlX(cap), cap.row, CtrlW(cap));
    for (const Choice& c : kCapture) Add(hCapture, c.label);
    cap.row += kRow;
    hReshade = CheckBox(L"Apply my ReShade preset to the video (DOF, grade)", cap.x + 12, cap.row, cap.w - 24, ID_CAPTURE);
    cap.row += 26;
    hLivePreview = CheckBox(L"Show the finished frame on screen while exporting (focus pulls)", cap.x + 12, cap.row,
                            cap.w - 24, ID_CAPTURE);
    cap.row += 30;
    hCaptureNote = Hint(cap.x + 12, cap.row, cap.w - 24, 54);
    cap.row += 56;
    rightY = EndGroup(cap);
    pageBottom[1] = std::max(leftY, rightY);

    // ================= 3. Passes: extra files written with every video
    g_page = 2;
    Group dp = BeginGroup(L"Depth and normals", kLeftX, top, kColW);
    hDepthOn = CheckBox(L"Depth map with every video", dp.x + 12, dp.row, dp.w - 24, ID_DEPTH);
    dp.row += 26;
    hNormalsOn = CheckBox(L"Surface normals from the depth (relighting, AO in post)", dp.x + 12, dp.row, dp.w - 24,
                          ID_DEPTH);
    dp.row += 28;
    Label(dp, L"Format");
    hDepthFormat = Combo(ID_DEPTH, CtrlX(dp), dp.row, CtrlW(dp));
    for (const Choice& c : kDepthFormat) Add(hDepthFormat, c.label);
    dp.row += kRow;
    Label(dp, L"Range (m)");
    hDepthNear = Edit(CtrlX(dp), dp.row, 60, 0, ID_DEPTH);
    Make(L"STATIC", L"to", SS_CENTER, CtrlX(dp) + 62, dp.row + 4, 24, 20);
    hDepthFar = Edit(CtrlX(dp) + 88, dp.row, 60, 0, ID_DEPTH);
    Hint(CtrlX(dp) + 154, dp.row + 4, CtrlW(dp) - 154, 20, L"metres, near to far");
    dp.row += kRow;
    Label(dp, L"Look");
    hDepthCurve = Combo(ID_DEPTH, CtrlX(dp), dp.row, 170);
    for (const Choice& c : kDepthCurve) Add(hDepthCurve, c.label);
    hDepthWhite = Combo(ID_DEPTH, CtrlX(dp) + 176, dp.row, CtrlW(dp) - 176);
    for (const Choice& c : kDepthWhite) Add(hDepthWhite, c.label);
    dp.row += kRow;
    Label(dp, L"Normals FOV");
    hNormalsFov = Edit(CtrlX(dp), dp.row, 50, ES_NUMBER, ID_DEPTH);
    Hint(CtrlX(dp) + 58, dp.row + 4, CtrlW(dp) - 58, 20, L"degrees - only used without Focus Puller");
    dp.row += kRow;
    hDepthNote = Hint(dp.x + 12, dp.row, dp.w - 24, 52);
    dp.row += 54;
    leftY = EndGroup(dp);

    Group ex = BeginGroup(L"More files with every video", kRightX, top, kColW);
    hPlateOn = CheckBox(L"Clean plate - the same shot without ReShade (ProRes)", ex.x + 12, ex.row, ex.w - 24, ID_DEPTH);
    ex.row += 26;
    hProxyOn = CheckBox(L"Proxy - half size H.264 for fast editing", ex.x + 12, ex.row, ex.w - 24, ID_DEPTH);
    ex.row += 26;
    hIdOn = CheckBox(L"ID pass - experimental colour matte from the game's stencil", ex.x + 12, ex.row, ex.w - 24,
                     ID_DEPTH);
    ex.row += 28;
    hPassNote = Hint(ex.x + 12, ex.row, ex.w - 24, 52);
    ex.row += 54;
    rightY = EndGroup(ex) + 10;

    Group hf = BeginGroup(L"HFR export (high frame rate)", kRightX, rightY, kColW);
    hHfrOn = CheckBox(L"Also save every game frame, unblended (e.g. 960 fps)", hf.x + 12, hf.row, hf.w - 24, ID_DEPTH);
    hf.row += 28;
    Label(hf, L"Format");
    hHfrFormat = Combo(ID_DEPTH, CtrlX(hf), hf.row, CtrlW(hf));
    for (const Choice& c : kHfrFormat) Add(hHfrFormat, c.label);
    hf.row += kRow;
    Label(hf, L"Keep");
    hHfrEvery = Combo(ID_DEPTH, CtrlX(hf), hf.row, CtrlW(hf));
    for (const Every& e : kHfrEvery) Add(hHfrEvery, e.label);
    hf.row += kRow;
    hHfrNote = Hint(hf.x + 12, hf.row, hf.w - 24, 36);
    hf.row += 38;
    rightY = EndGroup(hf);
    pageBottom[2] = std::max(leftY, rightY);

    // ================= 4. Workflow: while and after exporting, the camera data, diagnostics
    g_page = 3;
    Group wk = BeginGroup(L"While and after exporting", kLeftX, top, kColW);
    hHideUi = CheckBox(L"Hide the game UI while exporting", wk.x + 12, wk.row, wk.w - 24);
    wk.row += 26;
    hProgress = CheckBox(L"Show export progress on screen (%, frames, motion blur, time left) - never in the video",
                         wk.x + 12, wk.row, wk.w - 24);
    wk.row += 26;
    Make(L"STATIC", L"Progress text size:", SS_LEFT, wk.x + 32, wk.row + 3, 140, 20);
    hOsdSize = Combo(0, wk.x + 176, wk.row, 300);
    for (const Choice& c : kOsdSize) Add(hOsdSize, c.label);
    wk.row += 30;
    hNotify = CheckBox(L"When an export finishes: sound, taskbar flash and an on-screen note", wk.x + 12, wk.row,
                       wk.w - 24);
    wk.row += 30;
    Hint(wk.x + 12, wk.row, wk.w - 24, 52,
         L"The progress and the finished note are drawn by ReShade after EVE1G took the frame, so they never reach "
         L"the video. Every export also lists its passes on screen (\"also writing: depth + plate ...\").");
    wk.row += 54;
    leftY = EndGroup(wk) + 10;

    Group dg = BeginGroup(L"Diagnostics", kLeftX, leftY, kColW);
    Label(dg, L"Log level");
    hLog = Combo(0, CtrlX(dg), dg.row, 110);
    for (const Choice& c : kLog) Add(hLog, c.label);
    Make(WC_BUTTONW, L"Open log", BS_PUSHBUTTON | WS_TABSTOP, CtrlX(dg) + 120, dg.row - 1, 90, 26, ID_OPENLOG);
    dg.row += kRow;
    hExr = CheckBox(L"Legacy OpenEXR export of original EVE (colour + game depth)", dg.x + 12, dg.row, dg.w - 24);
    dg.row += 30;
    leftY = EndGroup(dg);

    Group fp = BeginGroup(L"Camera data for After Effects / Blender (Focus Puller)", kRightX, top, kColW);
    hCamFiles = CheckBox(L"Write the camera files with every export  (off = only the video)", fp.x + 12, fp.row,
                         fp.w - 24);
    fp.row += 26;
    hCamFolder = CheckBox(L"Put them in a \"camera\" folder next to the video", fp.x + 32, fp.row, fp.w - 44);
    fp.row += 30;
    Hint(fp.x + 12, fp.row, fp.w - 24, 130,
         L"With the Focus Puller ReShade add-on, an EVE1G export can also write the Rockstar Editor camera, frame for "
         L"frame, into a \"camera\" folder next to the video:\r\n"
         L"  •  <video>_camera.jsx  -  After Effects (File > Scripts > Run Script File)\r\n"
         L"  •  <video>_camera.py  -  Blender (Scripting > Run Script; export FBX for Nuke / C4D)\r\n"
         L"  •  <video>_camera.csv  -  position, rotation, FOV, focus, aperture per frame\r\n"
         L"The same switch is in Focus Puller > Export (ReShade menu), where you also pick which of the three files. "
         L"Needs Focus Puller. The normals pass takes the field of view from it too.");
    fp.row += 132;
    rightY = EndGroup(fp);
    pageBottom[3] = std::max(leftY, rightY);

    // ---------------- status + buttons (all pages)
    g_page = -1;
    int bottom = 0;
    for (int b : pageBottom) bottom = std::max(bottom, b);
    int y = bottom + 12;
    hStatus = Make(L"STATIC", L"", SS_LEFT, kPad, y, kWinW - kPad * 2, 36);
    y += 42;
    Make(WC_BUTTONW, L"Open output folder", BS_PUSHBUTTON | WS_TABSTOP, kPad, y, 140, 30, ID_OPENFOLDER);
    Make(WC_BUTTONW, L"Save", BS_DEFPUSHBUTTON | WS_TABSTOP, kWinW - kPad - 190, y, 92, 30, ID_SAVE);
    Make(WC_BUTTONW, L"Close", BS_PUSHBUTTON | WS_TABSTOP, kWinW - kPad - 92, y, 92, 30, ID_CLOSE);
    g_height = y + 30 + kPad;
}

// Shows the controls of one tab and hides the others.
void ShowPage(int page) {
    page = std::clamp(page, 0, kPages - 1);
    for (int p = 0; p < kPages; ++p)
        for (HWND c : g_pages[p]) ShowWindow(c, p == page ? SW_SHOW : SW_HIDE);
    SendMessageW(hTabs, TCM_SETCURSEL, page, 0);
    g_curPage = page;
    ShowWindow(hFpsCustom, (page == 1 && Sel(hFps) == kFpsCount) ? SW_SHOW : SW_HIDE);
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

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND: {
        const int id = LOWORD(wp), code = HIWORD(wp);
        if (id == ID_BROWSE) {
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
                MessageBoxW(h, L"Saved. It applies to your next export.", kApp, MB_ICONINFORMATION);
            }
        } else if (id == ID_CLOSE) {
            DestroyWindow(h);
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
    case WM_NOTIFY: {
        const NMHDR* n = reinterpret_cast<const NMHDR*>(lp);
        if (n && n->hwndFrom == hTabs && n->code == TCN_SELCHANGE) {
            ShowPage((int)SendMessageW(hTabs, TCM_GETCURSEL, 0, 0));
        }
        break;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE && hStatus) UpdateStatus();   // refresh after an export
        break;
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, TRANSPARENT);
        const HWND c = (HWND)lp;
        if (std::find(g_hints.begin(), g_hints.end(), c) != g_hints.end()) SetTextColor(dc, RGB(100, 100, 100));
        if (c == hStatus) SetTextColor(dc, RGB(0, 110, 60));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    SetProcessDPIAware();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_TAB_CLASSES};
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

    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW bold = ncm.lfMessageFont;
    bold.lfWeight = FW_SEMIBOLD;
    g_bold = CreateFontIndirectW(&bold);
    LOGFONTW title = ncm.lfMessageFont;
    title.lfWeight = FW_BOLD;
    title.lfHeight = -S(22);
    g_title = CreateFontIndirectW(&title);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"EVE1GSettings";
    RegisterClassExW(&wc);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    g_wnd = CreateWindowExW(0, wc.lpszClassName, L"EVE1G Settings", style, CW_USEDEFAULT, CW_USEDEFAULT, S(kWinW),
                            S(600), nullptr, nullptr, inst, nullptr);
    Build();
    PopulateCodecs();
    RECT want{0, 0, S(kWinW), S(g_height)};
    AdjustWindowRect(&want, style, FALSE);
    SetWindowPos(g_wnd, nullptr, 0, 0, want.right - want.left, want.bottom - want.top, SWP_NOMOVE | SWP_NOZORDER);
    Load();
    {
        // --page=N opens on a tab (1 = Output ... 4 = Workflow)
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
    CoUninitialize();
    return 0;
}
