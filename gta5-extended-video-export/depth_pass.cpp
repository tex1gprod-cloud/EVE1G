// EVE1G depth pass - see depth_pass.h
#include "depth_pass.h"

#include "config.h"
#include "encoder.h"
#include "logger.h"
#include "util.h"

#pragma warning(push)
#pragma warning(disable : 4244)
#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfOutputFile.h>
#pragma warning(pop)

#include <Psapi.h>

#include <DirectXTex.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace depth {
namespace {

enum class Format { Png16, ProRes, Exr };

struct Settings {
    bool enabled = false;  // the depth map
    bool normals = false;  // surface normals from the same depth
    Format format = Format::Png16;
    float nearM = 0.5f;
    float farM = 200.0f;
    bool logCurve = true;
    bool whiteNear = true;
    bool autoRange = true; // near/far from the first frame of the clip
};

struct Job {
    uint64_t index = 0;
    std::vector<float> metres;
    float fovV = 50.0f; // vertical field of view of this frame (degrees), for the normals
};

constexpr const char* kEffect = "EVE1G_Depth.fx";
constexpr const char* kTechnique = "EVE1G_Depth";
constexpr const char* kTexture = "EVE1G_DepthTex";
constexpr size_t kMaxQueued = 4; // frames waiting for the writer (each 4K frame is ~33 MB)

std::mutex mx; // guards everything below
Settings cfg;
bool active = false;   // this export writes a depth pass
std::string folder;    // png16 / exr: the sequence folder
std::string stem;      // file name stem of the sequence files
std::string target;    // what is written, for the log and sidecar
uint32_t outW = 0, outH = 0;
float farPlane = 1000.0f; // RESHADE_DEPTH_LINEARIZATION_FAR_PLANE: linear depth 1.0 = this many metres
bool reversedZ = true;    // RESHADE_DEPTH_INPUT_IS_REVERSED (GTA V: reversed)
bool rangeSet = false;    // auto range worked out for this clip

// render thread
bool armed = false;
reshade::api::effect_technique technique{0};
bool techniqueWasOn = false;
bool toldNoEffect = false, toldNoTexture = false, toldFormat = false;
ComPtr<ID3D11Texture2D> staging;
D3D11_TEXTURE2D_DESC stagingDesc{};
bool haveSample = false;
// The game's own depth-stencil buffer: read first, because it always holds the frame being exported. ReShade's depth
// texture can stay on an older frame during an export (its depth-buffer choice / "copy before clear" sees a different
// pattern of draws while the editor exports) - then every depth frame came out the same.
ComPtr<ID3D11Texture2D> gameStaging;
D3D11_TEXTURE2D_DESC gameDesc{};
int gameKind = 0;      // 1 = 32-bit float depth, 2 = 24-bit unorm depth
uint32_t gameBpp = 0;  // bytes per texel of the game's depth-stencil
int sampleSource = 0;  // this sample: 1 = game depth buffer, 2 = ReShade's EVE1G_Depth texture
bool toldSource = false;
uint64_t lastHash = 0;
int sameCount = 0;
bool toldStale = false;
std::vector<float> last; // last depth frame, repeated when a frame had no depth

uint64_t frames = 0, repeated = 0, written = 0, failed = 0;
std::string firstError;

// writer
std::deque<Job> queue;
std::condition_variable cvPush, cvPop;
bool stop = false;
std::thread worker;
std::unique_ptr<Encoder::DepthVideo> video;
std::unique_ptr<Encoder::DepthVideo> normalsVideo; // normals as ProRes 4444
std::string normalsFolder, normalsTarget;
bool fovFromFocusPuller = false;

const char* FormatName(Format f) {
    switch (f) {
    case Format::ProRes: return "ProRes 4444 video (10-bit)";
    case Format::Exr: return "OpenEXR float sequence (Z = metres)";
    default: return "PNG 16-bit grayscale sequence";
    }
}

Settings ReadSettings() {
    Settings s;
    s.enabled = config::depth_enabled;
    s.normals = config::normals_enabled;
    const std::string f = config::depth_format;
    s.format = f == "prores" ? Format::ProRes : f == "exr" ? Format::Exr : Format::Png16;
    s.nearM = std::max(0.01f, config::depth_near);
    s.farM = std::max(s.nearM * 1.01f, config::depth_far);
    s.logCurve = config::depth_log_curve;
    s.whiteNear = config::depth_white_near;
    s.autoRange = config::depth_auto_range;
    return s;
}

std::string DescribeLocked() {
    if (!cfg.enabled) {
        return cfg.normals ? "off (normals only)" : "off";
    }
    char b[256];
    if (cfg.format == Format::Exr) {
        snprintf(b, sizeof(b), "%s", FormatName(cfg.format));
    } else {
        if (cfg.autoRange && !rangeSet) {
            snprintf(b, sizeof(b), "%s, auto range (from the first frame), %s, white = %s", FormatName(cfg.format),
                     cfg.logCurve ? "log" : "linear", cfg.whiteNear ? "near" : "far");
        } else {
            snprintf(b, sizeof(b), "%s, %.2f m to %.1f m%s, %s, white = %s", FormatName(cfg.format), cfg.nearM, cfg.farM,
                     cfg.autoRange ? " (auto)" : "", cfg.logCurve ? "log" : "linear", cfg.whiteNear ? "near" : "far");
        }
    }
    return b;
}

// metres -> 0..65535, with the chosen range, curve and polarity
inline uint16_t ToGray(float m) {
    float t;
    if (!(m > cfg.nearM)) {
        t = 0.0f;
    } else if (m >= cfg.farM) {
        t = 1.0f;
    } else if (cfg.logCurve) {
        t = std::log(m / cfg.nearM) / std::log(cfg.farM / cfg.nearM);
    } else {
        t = (m - cfg.nearM) / (cfg.farM - cfg.nearM);
    }
    if (cfg.whiteNear) {
        t = 1.0f - t;
    }
    return static_cast<uint16_t>(std::lround(std::clamp(t, 0.0f, 1.0f) * 65535.0f));
}

std::string FramePath(uint64_t index, const char* ext) {
    char n[32];
    snprintf(n, sizeof(n), "_depth_%06llu.", static_cast<unsigned long long>(index));
    return folder + "\\" + stem + n + ext;
}

// View-space surface normals from the depth: x right, y up, z towards the camera (the usual normal map look:
// facing the camera = blue). Each pixel's 3D position comes from its depth and the field of view; the normal is the
// cross product of the neighbour differences, taking the smaller step on each axis so edges stay clean.
void ComputeNormals(const std::vector<float>& m, uint32_t w, uint32_t h, float fovV, float sky,
                    std::vector<float>& n) {
    n.resize(static_cast<size_t>(w) * h * 3);
    const float tanV = std::tan(fovV * 0.5f * 3.14159265f / 180.0f);
    const float tanH = tanV * static_cast<float>(w) / static_cast<float>(h);
    auto pos = [&](uint32_t x, uint32_t y, float out[3]) {
        const float d = m[static_cast<size_t>(y) * w + x];
        out[0] = ((x + 0.5f) / w * 2.0f - 1.0f) * tanH * d;
        out[1] = (1.0f - (y + 0.5f) / h * 2.0f) * tanV * d;
        out[2] = -d;
    };
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const size_t i = static_cast<size_t>(y) * w + x;
            float* o = n.data() + i * 3;
            const float d = m[i];
            if (!(d > 0.0f) || d >= sky) {
                o[0] = 0.0f; o[1] = 0.0f; o[2] = 1.0f;
                continue;
            }
            float c[3], a[3], b[3], dx[3], dy[3];
            pos(x, y, c);
            // x: forward or backward difference, whichever changes less in depth
            const bool hasR = x + 1 < w, hasL = x > 0;
            bool useR = hasR && (!hasL || std::fabs(m[i + 1] - d) <= std::fabs(d - m[i - 1]));
            if (useR) { pos(x + 1, y, a); for (int k = 0; k < 3; ++k) dx[k] = a[k] - c[k]; }
            else if (hasL) { pos(x - 1, y, a); for (int k = 0; k < 3; ++k) dx[k] = c[k] - a[k]; }
            else { dx[0] = 1; dx[1] = 0; dx[2] = 0; }
            const bool hasD = y + 1 < h, hasU = y > 0;
            bool useD = hasD && (!hasU || std::fabs(m[i + w] - d) <= std::fabs(d - m[i - w]));
            if (useD) { pos(x, y + 1, b); for (int k = 0; k < 3; ++k) dy[k] = b[k] - c[k]; }
            else if (hasU) { pos(x, y - 1, b); for (int k = 0; k < 3; ++k) dy[k] = c[k] - b[k]; }
            else { dy[0] = 0; dy[1] = -1; dy[2] = 0; }
            // cross(dy, dx): +z for a surface facing the camera
            float nx = dy[1] * dx[2] - dy[2] * dx[1];
            float ny = dy[2] * dx[0] - dy[0] * dx[2];
            float nz = dy[0] * dx[1] - dy[1] * dx[0];
            const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (len > 1e-20f) { nx /= len; ny /= len; nz /= len; } else { nx = 0; ny = 0; nz = 1; }
            o[0] = nx; o[1] = ny; o[2] = nz;
        }
    }
}

std::string NormalsPath(uint64_t index) {
    char n[32];
    snprintf(n, sizeof(n), "_normals_%06llu.png", static_cast<unsigned long long>(index));
    return normalsFolder + "\\" + stem + n;
}

bool WriteNormals(const Job& job, const std::vector<float>& n, std::vector<uint16_t>& rgba, std::string& error) {
    rgba.resize(static_cast<size_t>(outW) * outH * 4);
    for (size_t i = 0, e = static_cast<size_t>(outW) * outH; i < e; ++i) {
        for (int k = 0; k < 3; ++k) {
            rgba[i * 4 + k] = static_cast<uint16_t>(std::lround(std::clamp(n[i * 3 + k] * 0.5f + 0.5f, 0.0f, 1.0f) * 65535.0f));
        }
        rgba[i * 4 + 3] = 65535;
    }
    if (normalsVideo && normalsVideo->isOpen()) {
        if (!normalsVideo->write(rgba.data(), static_cast<int>(outW * 8))) {
            error = "Voukoder refused a normals frame";
            return false;
        }
        return true;
    }
    DirectX::Image img{};
    img.width = outW;
    img.height = outH;
    img.format = DXGI_FORMAT_R16G16B16A16_UNORM;
    img.rowPitch = static_cast<size_t>(outW) * 8;
    img.slicePitch = img.rowPitch * outH;
    img.pixels = reinterpret_cast<uint8_t*>(rgba.data());
    const HRESULT hr = DirectX::SaveToWICFile(img, DirectX::WIC_FLAGS_NONE, DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG),
                                              utf8_decode(NormalsPath(job.index)).c_str());
    if (FAILED(hr)) {
        char b[64];
        snprintf(b, sizeof(b), "normals PNG write failed (0x%08X)", static_cast<unsigned>(hr));
        error = b;
        return false;
    }
    return true;
}

bool WriteDepth(const Job& job, std::vector<uint16_t>& gray, const std::vector<float>* normals, std::string& error) {
    if (cfg.format == Format::Exr) {
        try {
            Imf::Header header(static_cast<int>(outW), static_cast<int>(outH));
            Imf::FrameBuffer fb;
            if (cfg.enabled) {
                header.channels().insert("Z", Imf::Channel(Imf::FLOAT));
                fb.insert("Z", Imf::Slice(Imf::FLOAT, reinterpret_cast<char*>(const_cast<float*>(job.metres.data())),
                                          sizeof(float), sizeof(float) * outW));
            }
            if (normals) { // normals in the same file: N.X, N.Y, N.Z (-1..1, view space)
                const char* names[3] = {"N.X", "N.Y", "N.Z"};
                for (int k = 0; k < 3; ++k) {
                    header.channels().insert(names[k], Imf::Channel(Imf::FLOAT));
                    fb.insert(names[k], Imf::Slice(Imf::FLOAT, reinterpret_cast<char*>(const_cast<float*>(normals->data() + k)),
                                                   sizeof(float) * 3, sizeof(float) * 3 * outW));
                }
            }
            Imf::OutputFile file(FramePath(job.index, "exr").c_str(), header);
            file.setFrameBuffer(fb);
            file.writePixels(static_cast<int>(outH));
            return true;
        } catch (std::exception& ex) {
            error = ex.what();
            return false;
        }
    }
    if (!cfg.enabled) {
        return true;
    }
    gray.resize(job.metres.size());
    for (size_t i = 0; i < job.metres.size(); ++i) {
        gray[i] = ToGray(job.metres[i]);
    }
    if (cfg.format == Format::ProRes && video && video->isOpen()) {
        if (!video->write(gray.data(), static_cast<int>(outW * sizeof(uint16_t)))) {
            error = "Voukoder refused a depth frame";
            return false;
        }
        return true;
    }
    DirectX::Image img{};
    img.width = outW;
    img.height = outH;
    img.format = DXGI_FORMAT_R16_UNORM;
    img.rowPitch = outW * sizeof(uint16_t);
    img.slicePitch = img.rowPitch * outH;
    img.pixels = reinterpret_cast<uint8_t*>(gray.data());
    const std::wstring path = utf8_decode(FramePath(job.index, "png"));
    const HRESULT hr =
        DirectX::SaveToWICFile(img, DirectX::WIC_FLAGS_NONE, DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG), path.c_str());
    if (FAILED(hr)) {
        char b[64];
        snprintf(b, sizeof(b), "PNG write failed (0x%08X)", static_cast<unsigned>(hr));
        error = b;
        return false;
    }
    return true;
}

void WorkerMain() {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED); // WIC (PNG) needs COM on this thread
    std::vector<uint16_t> gray, rgba;
    std::vector<float> normals;
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mx);
            cvPop.wait(lock, [] { return stop || !queue.empty(); });
            if (queue.empty()) {
                break; // stopped and drained
            }
            job = std::move(queue.front());
            queue.pop_front();
            cvPush.notify_all();
        }
        std::string error;
        // settings and paths do not change while the worker runs
        bool ok = true;
        if (cfg.normals) {
            ComputeNormals(job.metres, outW, outH, job.fovV, farPlane * 0.999f, normals);
        }
        if (cfg.format == Format::Exr) {
            ok = WriteDepth(job, gray, cfg.normals ? &normals : nullptr, error);
        } else {
            ok = WriteDepth(job, gray, nullptr, error);
            if (cfg.normals) {
                std::string e2;
                if (!WriteNormals(job, normals, rgba, e2)) {
                    ok = false;
                    if (error.empty()) error = e2;
                }
            }
        }
        std::lock_guard<std::mutex> lock(mx);
        if (ok) {
            ++written;
        } else {
            ++failed;
            if (firstError.empty()) {
                firstError = error;
                LOG(LL_ERR, "EVE1G depth: frame ", job.index, " not written: ", error);
            }
        }
    }
    if (SUCCEEDED(co)) {
        CoUninitialize();
    }
}

// Vertical field of view of the current frame: from Focus Puller (it reads the Rockstar Editor camera) when it is
// loaded, else [PASSES] normals_fov.
float CameraFov() {
    using FovFn = float (*)();
    static FovFn fn = nullptr;
    static bool looked = false;
    if (!looked) {
        looked = true;
        HMODULE mods[512];
        DWORD needed = 0;
        if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
            const DWORD n = std::min<DWORD>(needed / sizeof(HMODULE), 512);
            for (DWORD i = 0; i < n && !fn; ++i) {
                fn = reinterpret_cast<FovFn>(GetProcAddress(mods[i], "FocusPuller_CameraFovV"));
            }
        }
        LOG(LL_NON, "EVE1G normals: field of view ", fn ? "from Focus Puller (per frame)" : "fixed (normals_fov in [PASSES])");
    }
    const float v = fn ? fn() : 0.0f;
    if (v > 1.0f && v < 170.0f) {
        fovFromFocusPuller = true;
        return v;
    }
    return config::normals_fov;
}

} // namespace

void Begin(const std::string& base, uint32_t width, uint32_t height, uint32_t fps_num, uint32_t fps_den) {
    End();
    std::lock_guard<std::mutex> lock(mx);
    cfg = ReadSettings();
    active = (cfg.enabled || cfg.normals) && width > 0 && height > 0;
    fovFromFocusPuller = false;
    if (!active) {
        return;
    }
    outW = width;
    outH = height;
    frames = repeated = written = failed = 0;
    firstError.clear();
    haveSample = false;
    rangeSet = false;
    toldSource = false;
    toldStale = false;
    sameCount = 0;
    lastHash = 0;
    if (cfg.format == Format::ProRes) {
        // ProRes depth froze the game when its file opened (second Voukoder file during an export) - PNG until fixed
        LOG(LL_WRN, "EVE1G depth: ProRes depth is not available yet - writing a 16-bit PNG sequence instead");
        cfg.format = Format::Png16;
    }
    last.clear();
    queue.clear();
    stop = false;

    const std::filesystem::path p = std::filesystem::u8path(base);
    {
        const auto s = p.filename().u8string();
        stem = std::string(s.begin(), s.end());
    }
    if (cfg.format == Format::ProRes && cfg.enabled) {
        video = std::make_unique<Encoder::DepthVideo>();
        target = base + "_depth.mov";
        if (!video->open(utf8_decode(target), width, height, fps_num, fps_den)) {
            LOG(LL_WRN, "EVE1G depth: the ProRes depth video could not be opened - writing a PNG sequence instead");
            video.reset();
            cfg.format = Format::Png16;
        }
    }
    if (cfg.normals && cfg.format != Format::Exr) {
        if (cfg.format == Format::ProRes) {
            normalsVideo = std::make_unique<Encoder::DepthVideo>();
            normalsTarget = base + "_normals.mov";
            if (!normalsVideo->open(utf8_decode(normalsTarget), width, height, fps_num, fps_den, "prores_ks",
                                    "_pixelFormat=yuv444p10le|profile=4", "rgba64le", L"EVE1G normals")) {
                LOG(LL_WRN, "EVE1G normals: the ProRes video could not be opened - writing a PNG sequence instead");
                normalsVideo.reset();
            }
        }
        if (!normalsVideo) {
            normalsFolder = base + "_normals";
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::u8path(normalsFolder), ec);
            normalsTarget = normalsFolder + "\\" + stem + "_normals_######.png";
        }
        LOG(LL_NON, "EVE1G normals -> ", normalsTarget);
    } else if (cfg.normals) {
        normalsTarget = "N.X / N.Y / N.Z channels of the EXR files";
    }
    if (cfg.format == Format::Exr || (cfg.enabled && cfg.format != Format::ProRes)) {
        folder = base + (cfg.format == Format::Exr ? "_depth_exr" : "_depth");
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::u8path(folder), ec);
        target = folder + "\\" + stem + "_depth_######." + (cfg.format == Format::Exr ? "exr" : "png");
    }
    worker = std::thread(WorkerMain);
    LOG(LL_NON, "EVE1G depth: ", DescribeLocked(), " -> ", target);
}

void OnPresent(reshade::api::effect_runtime* runtime, bool exporting) {
    if (!runtime) {
        return;
    }
    bool want;
    {
        std::lock_guard<std::mutex> lock(mx);
        want = active && exporting;
    }
    if (want && !armed) {
        armed = true;
        technique = runtime->find_technique(kEffect, kTechnique);
        if (technique.handle == 0) {
            if (!toldNoEffect) {
                toldNoEffect = true;
                LOG(LL_ERR, "EVE1G depth: ReShade has not loaded ", kEffect,
                    " - put it in reshade-shaders\\Shaders and restart the game. The depth pass repeats the last "
                    "depth it had (or stays empty) until then.");
            }
        } else {
            techniqueWasOn = runtime->get_technique_state(technique);
            if (!techniqueWasOn) {
                runtime->set_technique_state(technique, true);
            }
        }
        char v[64] = {};
        size_t n = sizeof(v);
        float fp = 1000.0f;
        if (runtime->get_preprocessor_definition("RESHADE_DEPTH_LINEARIZATION_FAR_PLANE", v, &n) && v[0]) {
            fp = static_cast<float>(atof(v));
        }
        n = sizeof(v);
        v[0] = 0;
        bool rev = true;
        if (runtime->get_preprocessor_definition("RESHADE_DEPTH_INPUT_IS_REVERSED", v, &n) && v[0]) {
            rev = atoi(v) != 0;
        }
        std::lock_guard<std::mutex> lock(mx);
        farPlane = fp > 1.0f ? fp : 1000.0f;
        reversedZ = rev;
        LOG(LL_NON, "EVE1G depth: technique ", technique.handle ? "on" : "MISSING", ", ReShade far plane ", farPlane,
            " m");
    } else if (!want && armed) {
        if (technique.handle != 0 && !techniqueWasOn) {
            runtime->set_technique_state(technique, false); // back as it was
        }
        technique = {0};
        armed = false;
    }
}

void OnSample(reshade::api::effect_runtime* runtime, ID3D11DeviceContext* context, int sample, int perFrame,
              ID3D11Texture2D* gameDepth) {
    {
        std::lock_guard<std::mutex> lock(mx);
        if (!active) {
            return;
        }
    }
    // one sample per video frame: the middle of the samples the shutter blends
    const int samples = std::max(0, perFrame - 1);
    int wanted = 0;
    if (samples > 0) {
        const float strength = std::clamp(config::motion_blur_strength, 0.0f, 1.0f);
        const int first = std::clamp(static_cast<int>(std::ceil((1.0f - strength) * samples - 1e-4f)), 0, samples);
        wanted = (first + samples) / 2;
    }
    if (sample != wanted || !context) {
        return;
    }
    haveSample = false;
    sampleSource = 0;
    // 1st choice: the game's own depth-stencil buffer - always the frame being exported
    if (gameDepth) {
        D3D11_TEXTURE2D_DESC gd;
        gameDepth->GetDesc(&gd);
        int kind = 0;
        uint32_t bpp = 0;
        switch (gd.Format) {
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: kind = 1; bpp = 8; break;
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32_FLOAT: kind = 1; bpp = 4; break;
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT: kind = 2; bpp = 4; break;
        default: break;
        }
        if (kind != 0 && gd.SampleDesc.Count == 1) {
            if (!gameStaging || gameDesc.Width != gd.Width || gameDesc.Height != gd.Height || gameDesc.Format != gd.Format) {
                D3D11_TEXTURE2D_DESC sd = gd;
                sd.MipLevels = 1;
                sd.ArraySize = 1;
                sd.Usage = D3D11_USAGE_STAGING;
                sd.BindFlags = 0;
                sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                sd.MiscFlags = 0;
                ComPtr<ID3D11Device> device;
                gameDepth->GetDevice(device.GetAddressOf());
                if (!device || FAILED(device->CreateTexture2D(&sd, nullptr, gameStaging.ReleaseAndGetAddressOf()))) {
                    gameStaging = nullptr;
                } else {
                    gameDesc = sd;
                }
            }
            if (gameStaging) {
                context->CopySubresourceRegion(gameStaging.Get(), 0, 0, 0, 0, gameDepth, 0, nullptr);
                gameKind = kind;
                gameBpp = bpp;
                sampleSource = 1;
                haveSample = true;
                if (!toldSource) {
                    toldSource = true;
                    LOG(LL_NON, "EVE1G depth: reading the game's depth buffer (", gd.Width, "x", gd.Height, " ",
                        conv_dxgi_format_to_string(gd.Format), ")");
                }
                return;
            }
        }
    }
    // 2nd choice: ReShade's depth through EVE1G_Depth.fx
    if (!runtime) {
        return;
    }
    if (!toldSource) {
        toldSource = true;
        LOG(LL_NON, "EVE1G depth: reading ReShade's depth (EVE1G_Depth.fx) - the game's depth buffer is not readable");
    }
    const reshade::api::effect_texture_variable var = runtime->find_texture_variable(kEffect, kTexture);
    reshade::api::resource_view srv{0}, srvSrgb{0};
    if (var.handle != 0) {
        runtime->get_texture_binding(var, &srv, &srvSrgb);
    }
    if (srv.handle == 0) {
        if (!toldNoTexture) {
            toldNoTexture = true;
            LOG(LL_WRN, "EVE1G depth: no depth texture from ReShade this frame (", kEffect, " not loaded?)");
        }
        return;
    }
    auto* resource = reinterpret_cast<ID3D11Resource*>(runtime->get_device()->get_resource_from_view(srv).handle);
    ComPtr<ID3D11Texture2D> tex;
    if (!resource || FAILED(resource->QueryInterface(IID_PPV_ARGS(tex.GetAddressOf())))) {
        return;
    }
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    if (d.Format != DXGI_FORMAT_R32_FLOAT && d.Format != DXGI_FORMAT_R32_TYPELESS) {
        if (!toldFormat) {
            toldFormat = true;
            LOG(LL_ERR, "EVE1G depth: the depth texture is ", conv_dxgi_format_to_string(d.Format),
                ", expected R32_FLOAT - update ", kEffect);
        }
        return;
    }
    if (!staging || stagingDesc.Width != d.Width || stagingDesc.Height != d.Height || stagingDesc.Format != d.Format) {
        D3D11_TEXTURE2D_DESC sd = d;
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.SampleDesc.Count = 1;
        sd.SampleDesc.Quality = 0;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        ComPtr<ID3D11Device> device;
        tex->GetDevice(device.GetAddressOf());
        if (!device || FAILED(device->CreateTexture2D(&sd, nullptr, staging.ReleaseAndGetAddressOf()))) {
            staging = nullptr;
            return;
        }
        stagingDesc = sd;
    }
    context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, tex.Get(), 0, nullptr);
    sampleSource = 2;
    haveSample = true;
}

void OnFrameEmitted(ID3D11DeviceContext* context) {
    uint32_t w, h;
    float fp;
    bool rev;
    {
        std::lock_guard<std::mutex> lock(mx);
        if (!active || stop) {
            return;
        }
        w = outW;
        h = outH;
        fp = farPlane;
        rev = reversedZ;
    }
    std::vector<float> metres;
    bool ok = false;
    if (haveSample && sampleSource == 1 && gameStaging && context) {
        // the game's depth: linearised exactly like ReShade.fxh (same far plane / reversed settings)
        D3D11_MAPPED_SUBRESOURCE map{};
        if (SUCCEEDED(context->Map(gameStaging.Get(), 0, D3D11_MAP_READ, 0, &map))) {
            metres.resize(static_cast<size_t>(w) * h);
            const uint32_t sw = gameDesc.Width, sh = gameDesc.Height;
            for (uint32_t y = 0; y < h; ++y) {
                const uint32_t sy = sh == h ? y : static_cast<uint32_t>((static_cast<uint64_t>(y) * sh) / h);
                const uint8_t* row = static_cast<const uint8_t*>(map.pData) + static_cast<size_t>(sy) * map.RowPitch;
                float* out = metres.data() + static_cast<size_t>(y) * w;
                for (uint32_t x = 0; x < w; ++x) {
                    const uint32_t sx = sw == w ? x : static_cast<uint32_t>((static_cast<uint64_t>(x) * sw) / w);
                    const uint8_t* t = row + static_cast<size_t>(sx) * gameBpp;
                    float d;
                    if (gameKind == 1) {
                        std::memcpy(&d, t, sizeof(float));
                    } else {
                        uint32_t u;
                        std::memcpy(&u, t, sizeof(u));
                        d = static_cast<float>(u & 0xFFFFFFu) / 16777215.0f;
                    }
                    if (rev) {
                        d = 1.0f - d;
                    }
                    float lin = d / (fp - d * (fp - 1.0f));
                    lin = std::clamp(std::isfinite(lin) ? lin : 1.0f, 0.0f, 1.0f);
                    out[x] = lin * fp;
                }
            }
            context->Unmap(gameStaging.Get(), 0);
            ok = true;
        }
    } else if (haveSample && staging && context) {
        D3D11_MAPPED_SUBRESOURCE map{};
        if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) {
            metres.resize(static_cast<size_t>(w) * h);
            const uint32_t sw = stagingDesc.Width, sh = stagingDesc.Height;
            for (uint32_t y = 0; y < h; ++y) {
                const uint32_t sy = sh == h ? y : static_cast<uint32_t>((static_cast<uint64_t>(y) * sh) / h);
                const float* row = reinterpret_cast<const float*>(static_cast<const uint8_t*>(map.pData) +
                                                                   static_cast<size_t>(sy) * map.RowPitch);
                float* out = metres.data() + static_cast<size_t>(y) * w;
                if (sw == w) {
                    for (uint32_t x = 0; x < w; ++x) out[x] = row[x] * fp;
                } else {
                    for (uint32_t x = 0; x < w; ++x)
                        out[x] = row[static_cast<uint32_t>((static_cast<uint64_t>(x) * sw) / w)] * fp;
                }
            }
            context->Unmap(staging.Get(), 0);
            ok = true;
        }
    }
    haveSample = false;
    if (ok) {
        // A depth that never changes while the picture does means the source is stale: say so once.
        uint64_t hsh = 1469598103934665603ull;
        for (size_t i = 0; i < metres.size(); i += 997) {
            uint32_t u;
            std::memcpy(&u, &metres[i], sizeof(u));
            hsh = (hsh ^ u) * 1099511628211ull;
        }
        sameCount = (hsh == lastHash) ? sameCount + 1 : 0;
        lastHash = hsh;
        if (sameCount == 5 && !toldStale) {
            toldStale = true;
            LOG(LL_WRN, "EVE1G depth: the depth has not changed for 6 frames - if the shot moves, the depth source is "
                        "stale (", sampleSource == 1 ? "game depth buffer" : "ReShade depth", ")");
        }
    }
    if (ok && cfg.autoRange && !rangeSet && cfg.enabled) {
        // auto range: near/far from this first frame (sky left out), kept for the whole clip so nothing flickers
        std::vector<float> v;
        v.reserve(metres.size() / 7 + 1);
        for (size_t i = 0; i < metres.size(); i += 7) {
            const float m = metres[i];
            if (m > 0.0f && m < fp * 0.98f) {
                v.push_back(m);
            }
        }
        if (v.size() > 100) {
            std::sort(v.begin(), v.end());
            float n = v[static_cast<size_t>(v.size() * 0.005)];
            float f = v[std::min(v.size() - 1, static_cast<size_t>(v.size() * 0.995))];
            n = std::max(0.05f, n * 0.9f);
            f = std::max(f * 1.1f, n * 2.0f);
            std::lock_guard<std::mutex> lock(mx);
            cfg.nearM = n;
            cfg.farM = f;
            rangeSet = true;
            LOG(LL_NON, "EVE1G depth: auto range ", n, " m to ", f, " m (from the first frame, kept for the clip)");
        }
    }
    if (!ok) {
        // keep the depth sequence frame-for-frame with the video: repeat the last depth (or far, before the first)
        metres = last.empty() ? std::vector<float>(static_cast<size_t>(w) * h, fp) : last;
    }

    std::unique_lock<std::mutex> lock(mx);
    if (!ok) {
        ++repeated;
    }
    if (ok) {
        last = metres;
    }
    cvPush.wait(lock, [] { return queue.size() < kMaxQueued || stop; });
    if (stop || !active) {
        return;
    }
    Job job;
    job.index = frames++;
    job.metres = std::move(metres);
    if (cfg.normals) {
        lock.unlock();
        job.fovV = CameraFov();
        lock.lock();
        if (stop || !active) {
            return;
        }
    }
    queue.push_back(std::move(job));
    cvPop.notify_one();
}

void End() {
    std::thread t;
    {
        std::lock_guard<std::mutex> lock(mx);
        if (!worker.joinable()) {
            active = false;
            return;
        }
        stop = true; // the worker drains what is queued, then stops
        cvPop.notify_all();
        cvPush.notify_all();
        t = std::move(worker);
    }
    t.join();
    std::lock_guard<std::mutex> lock(mx);
    if (video) {
        video->close();
        video.reset();
    }
    if (normalsVideo) {
        normalsVideo->close();
        normalsVideo.reset();
    }
    if (cfg.normals) {
        LOG(LL_NON, "EVE1G normals -> ", normalsTarget, fovFromFocusPuller ? " (FOV from Focus Puller)" : " (fixed FOV)");
    }
    LOG(LL_NON, "EVE1G depth: ", written, " depth frame(s) written, ", repeated, " repeated (no depth that frame), ",
        failed, " failed -> ", target);
    active = false;
    last.clear();
    last.shrink_to_fit();
}

std::string Describe() {
    std::lock_guard<std::mutex> lock(mx);
    if (!active) {
        return cfg.enabled ? "on, but not started" : "off";
    }
    std::string s = DescribeLocked();
    if (!target.empty()) {
        s += "\r\n                 -> " + target;
    }
    if (cfg.normals) {
        s += "\r\nNormals        : view space (x right, y up, z to camera), FOV " +
             std::string("from Focus Puller when it runs, else ") + std::to_string(static_cast<int>(config::normals_fov)) + " deg" +
             "\r\n                 -> " + normalsTarget;
    }
    return s;
}

std::string Result() {
    std::lock_guard<std::mutex> lock(mx);
    if (!cfg.enabled && !cfg.normals) {
        return {};
    }
    char b[200];
    snprintf(b, sizeof(b), "%llu written, %llu repeated (no depth that frame), %llu failed",
             static_cast<unsigned long long>(written), static_cast<unsigned long long>(repeated),
             static_cast<unsigned long long>(failed));
    return b;
}

} // namespace depth
