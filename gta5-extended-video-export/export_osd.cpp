// EVE1G export progress on screen. See export_osd.h.
#include "export_osd.h"

// ImGui 1.90 (matches the ReShade headers in ../reshade/include); only this file uses it.
#define ImTextureID ImU64
#include <imgui.h>

#include "config.h"
#include "passes.h"
#include "logger.h"
#include "re_core.h"
#include "reshade.hpp"

#include <Psapi.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

extern "C" __declspec(dllexport) float EVE1G_ExportTimeSeconds(); // script.cpp: -1 = no export

namespace {

bool g_registered = false;
recore::addresses g_addr;
std::atomic<int> g_resolved{0}; // 0 not tried, 1 found, 2 not found
std::atomic<float> g_total_s{0.0f};
std::atomic<double> g_start{0.0};
double g_last_total_read = 0.0;
bool g_was_exporting = false;
std::mutex g_doneMx;
std::string g_doneFile;   // last finished export (for the "finished" note)
double g_doneAt = -1.0;   // when it finished
std::string g_extras;     // extra files of the running export ("depth + plate ...")
std::string g_file;       // the running export's file name
std::string g_format;     // "3840x2160  24 fps  prores_ks  16-bit"
std::string g_blur;       // "motion blur 16 samples / frame  shutter 180"
int g_subframes = 1;      // game frames per video frame (motion blur samples + 1)

double Now() {
    static LARGE_INTEGER freq = {};
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(freq.QuadPart);
}

void Resolve() {
    if (g_resolved.load() != 0) {
        return;
    }
    MODULEINFO mi = {};
    if (!GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(nullptr), &mi, sizeof(mi))) {
        g_resolved = 2;
        return;
    }
    recore::image img;
    img.base = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
    img.size = mi.SizeOfImage;
    recore::resolve(img, g_addr, false);
    g_resolved = g_addr.controller ? 1 : 2;
    if (g_addr.controller) {
        LOG(LL_NON, "EVE1G: export progress - found the Rockstar Editor's project timeline");
    } else {
        LOG(LL_NON, "EVE1G: export progress - project length not found on this game build, the on-screen display "
                    "shows rendered time only");
    }
}

void ReadTotal() {
    if (g_resolved.load() != 1) {
        return;
    }
    const float ms = recore::total_ms(g_addr);
    if (ms > 100.0f) {
        g_total_s = ms / 1000.0f;
    }
}

void FmtClock(char* b, size_t n, double s) {
    if (!(s >= 0.0) || s > 360000.0) {
        snprintf(b, n, "--:--");
        return;
    }
    const int t = static_cast<int>(s + 0.5);
    if (t >= 3600) {
        snprintf(b, n, "%d:%02d:%02d", t / 3600, (t / 60) % 60, t % 60);
    } else {
        snprintf(b, n, "%02d:%02d", t / 60, t % 60);
    }
}

bool ShownForThisCapture() {
    switch (config::capture_source) {
    case config::CaptureSource::Clean:
    case config::CaptureSource::Encode:
    case config::CaptureSource::Effects:
        return true;
    default:
        return false; // present / legacy read the finished screen - anything drawn there would be in the video
    }
}

// Size: grows with the screen (2x at 4K), times osd_scale from EVE1G.ini.
void ScaleText() {
    const float h = ImGui::GetIO().DisplaySize.y;
    const float s = std::clamp((h > 0.0f ? h / 1080.0f : 1.0f) * config::osd_scale, 0.75f, 5.0f);
    ImGui::SetWindowFontScale(s);
}

// Dark box behind everything drawn since BeginGroup(), so the text reads on any scene.
// Drawn on the background list: it renders under the OSD window, never over the text.
void EndBox() {
    ImGui::EndGroup();
    const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
    const float pad = ImGui::GetFontSize() * 0.5f;
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(mn.x - pad, mn.y - pad), ImVec2(mx.x + pad, mx.y + pad),
                                                  IM_COL32(0, 0, 0, 210), pad);
}

void Draw(reshade::api::effect_runtime*) {
    if (!config::progress_osd) {
        return;
    }
    ScaleText();
    const float t = EVE1G_ExportTimeSeconds();
    if (!(t >= 0.0f)) {
        // not exporting: the "finished" note for a few seconds (never in a video - no export is running)
        std::lock_guard<std::mutex> lock(g_doneMx);
        if (g_doneAt > 0.0 && config::notify_done && Now() - g_doneAt < 8.0) {
            ScaleText();
            ImGui::BeginGroup();
            ImGui::TextColored(ImVec4(0.45f, 1.0f, 0.55f, 1.0f), "EVE1G  EXPORT FINISHED");
            ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.9f, 1.0f), "%s", g_doneFile.c_str());
            EndBox();
        }
        return;
    }
    if (!ShownForThisCapture()) {
        return;
    }
    const auto fps = config::fps;
    const double rate = (fps.first > 0 && fps.second > 0) ? static_cast<double>(fps.first) / fps.second : 30.0;
    const float total = g_total_s.load();
    const double elapsed = (std::max)(0.0, Now() - g_start.load());
    const long long frame = static_cast<long long>(std::floor(t * rate + 1e-4));
    const double speed = elapsed > 0.5 ? frame / elapsed : 0.0; // video frames per second of real time

    const ImVec4 red(1.00f, 0.40f, 0.40f, 1.0f), dim(0.70f, 0.70f, 0.75f, 1.0f), white(1.0f, 1.0f, 1.0f, 1.0f);
    const ImVec4 cyan(0.55f, 0.85f, 1.0f, 1.0f);
    char line[160], a[32], b[32], c[32];
    const long long game_frames = frame * g_subframes;
    ImGui::BeginGroup();
    if (total > 0.05f) {
        const float p = std::clamp(t / total, 0.0f, 1.0f);
        const long long frames = static_cast<long long>(std::llround(total * rate));
        ImGui::TextColored(red, "EVE1G  EXPORTING  %d%%", static_cast<int>(std::floor(p * 100.0f)));
        snprintf(line, sizeof(line), "frame %lld / %lld", (std::min)(frame, frames), frames);
        ImGui::ProgressBar(p, ImVec2(ImGui::GetFontSize() * 20.0f, 0.0f), line);
        FmtClock(a, sizeof(a), elapsed);
        FmtClock(b, sizeof(b), p > 0.005f ? elapsed * (1.0 - p) / p : -1.0);
        FmtClock(c, sizeof(c), total);
        ImGui::TextColored(white, "elapsed %s   left ~%s   video %s", a, b, c);
    } else {
        FmtClock(a, sizeof(a), t);
        FmtClock(b, sizeof(b), elapsed);
        ImGui::TextColored(red, "EVE1G  EXPORTING");
        ImGui::TextColored(white, "%s of video rendered  (frame %lld)", a, frame);
        ImGui::TextColored(white, "elapsed %s", b);
    }
    ImGui::TextColored(dim, "speed %.2f video fps  (%.1f game fps)", speed, speed * g_subframes);
    {
        std::lock_guard<std::mutex> lock(g_doneMx);
        if (!g_file.empty()) {
            ImGui::TextColored(cyan, "%s", g_file.c_str());
        }
        if (!g_format.empty()) {
            ImGui::TextColored(dim, "%s", g_format.c_str());
        }
        if (!g_blur.empty()) {
            ImGui::TextColored(dim, "%s", g_blur.c_str());
        }
        if (g_subframes > 1) {
            ImGui::TextColored(dim, "game frames rendered: %lld", game_frames);
        }
        if (!g_extras.empty()) {
            ImGui::TextColored(dim, "also writing: %s", g_extras.c_str());
        }
    }
    ImGui::TextColored(dim, "only on your screen - not in the video");
    EndBox();
}

} // namespace

void exportosd::Register() {
    HMODULE rs = reshade::internal::get_reshade_module_handle();
    if (!rs) {
        return;
    }
    const auto get_table =
        reinterpret_cast<const imgui_function_table* (*)(uint32_t)>(GetProcAddress(rs, "ReShadeGetImGuiFunctionTable"));
    const imgui_function_table* table = get_table ? get_table(IMGUI_VERSION_NUM) : nullptr;
    if (!table) {
        LOG(LL_NON, "EVE1G: this ReShade build offers no overlay to add-ons - export progress is not shown on screen");
        return;
    }
    imgui_function_table_instance() = table;
    reshade::register_overlay("OSD", &Draw);
    g_registered = true;
}

void exportosd::Unregister() {
    if (g_registered) {
        reshade::unregister_overlay("OSD", &Draw);
        g_registered = false;
    }
}

void exportosd::OnExportBegin(const std::string& fileName, uint32_t width, uint32_t height, bool master16) {
    {
        const auto fps = config::fps;
        const double rate = (fps.first > 0 && fps.second > 0) ? static_cast<double>(fps.first) / fps.second : 30.0;
        const std::string enc(config::encoder_config.video.encoder,
                              strnlen(config::encoder_config.video.encoder, sizeof(config::encoder_config.video.encoder)));
        char fps_txt[32];
        if (std::fabs(rate - std::round(rate)) < 1e-3) {
            snprintf(fps_txt, sizeof(fps_txt), "%.0f", rate);
        } else {
            snprintf(fps_txt, sizeof(fps_txt), "%.3f", rate);
        }
        char fmt[200];
        snprintf(fmt, sizeof(fmt), "%ux%u   %s fps   %s%s", width, height, fps_txt, enc.empty() ? "?" : enc.c_str(),
                 master16 ? "   16-bit master" : "");
        const int samples = config::motion_blur_samples;
        char blur[200];
        if (samples > 0) {
            snprintf(blur, sizeof(blur), "motion blur  %d samples / frame   shutter %d deg%s%s%s", samples,
                     static_cast<int>(std::lround(config::motion_blur_strength * 360.0f)),
                     config::blend_linear ? "   linear blend" : "", config::shutter_soft > 0.0f ? "   soft shutter" : "",
                     config::rolling_shutter_ms > 0.0f ? "   rolling shutter" : "");
        } else {
            snprintf(blur, sizeof(blur), "motion blur off");
        }
        std::lock_guard<std::mutex> lock(g_doneMx);
        g_file = fileName;
        g_format = fmt;
        g_blur = blur;
        g_subframes = samples > 0 ? samples + 1 : 1;
    }
    {
        std::string extras;
        auto add = [&extras](const char* n) {
            if (!extras.empty()) extras += " + ";
            extras += n;
        };
        if (config::depth_enabled) add("depth");
        if (config::normals_enabled) add("normals");
        if (config::hfr_enabled) add("HFR");
        const std::string p = passes::ActiveList();
        std::lock_guard<std::mutex> lock(g_doneMx);
        g_extras = extras;
        if (!p.empty()) {
            g_extras += (g_extras.empty() ? "" : " + ") + p;
        }
        g_doneAt = -1.0;
    }
    g_start = Now();
    g_total_s = 0.0f;
    Resolve();
    ReadTotal();
    const float total = g_total_s.load();
    if (total > 0.0f) {
        LOG(LL_NON, "EVE1G: exporting ", total, " s of project");
    }
}

void exportosd::OnExportEnd(const std::string& fileName) {
    std::lock_guard<std::mutex> lock(g_doneMx);
    g_doneFile = fileName;
    g_doneAt = Now();
}

void exportosd::OnPresent(bool exporting) {
    if (exporting && !g_was_exporting && g_start.load() == 0.0) {
        g_start = Now();
    }
    g_was_exporting = exporting;
    if (!exporting) {
        return;
    }
    const double now = Now();
    if (now - g_last_total_read > 2.0) {
        g_last_total_read = now;
        ReadTotal();
    }
}
