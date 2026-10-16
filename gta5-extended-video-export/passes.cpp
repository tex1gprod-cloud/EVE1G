// EVE1G extra passes - see passes.h
#include "passes.h"

#include "config.h"
#include "encoder.h"
#include "logger.h"
#include "util.h"

#include <DirectXTex.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace passes {
namespace {

// ---- a side file written on its own thread ---------------------------------------------------------------------------
// The render thread hands over a copy of the frame and goes on; the writer encodes it. At most kMax frames wait.
class Writer {
  public:
    using WriteFn = std::function<bool(uint64_t index, const std::vector<uint8_t>& data, std::string& error)>;

    void start(const char* name, WriteFn fn, bool com) {
        stopAll();
        tag = name;
        write = std::move(fn);
        written = failed = queued = 0;
        firstError.clear();
        stop = false;
        thread = std::thread([this, com] { run(com); });
    }

    // false if the writer is not running
    bool push(std::vector<uint8_t>&& data) {
        std::unique_lock<std::mutex> lock(mx);
        if (!thread.joinable() || stop) {
            return false;
        }
        cvPush.wait(lock, [this] { return q.size() < kMax || stop; });
        if (stop) {
            return false;
        }
        q.push_back({queued++, std::move(data)});
        cvPop.notify_one();
        return true;
    }

    void stopAll() {
        std::thread t;
        {
            std::lock_guard<std::mutex> lock(mx);
            if (!thread.joinable()) {
                return;
            }
            stop = true;
            cvPop.notify_all();
            cvPush.notify_all();
            t = std::move(thread);
        }
        t.join();
    }

    bool running() {
        std::lock_guard<std::mutex> lock(mx);
        return thread.joinable() && !stop;
    }

    std::string result() {
        std::lock_guard<std::mutex> lock(mx);
        char b[160];
        snprintf(b, sizeof(b), "%llu written, %llu failed", static_cast<unsigned long long>(written),
                 static_cast<unsigned long long>(failed));
        return firstError.empty() ? std::string(b) : std::string(b) + " (" + firstError + ")";
    }

    uint64_t writtenCount() {
        std::lock_guard<std::mutex> lock(mx);
        return written;
    }

  private:
    static constexpr size_t kMax = 3;
    struct Item {
        uint64_t index;
        std::vector<uint8_t> data;
    };

    void run(bool com) {
        const HRESULT co = com ? CoInitializeEx(nullptr, COINIT_MULTITHREADED) : E_FAIL;
        for (;;) {
            Item item;
            {
                std::unique_lock<std::mutex> lock(mx);
                cvPop.wait(lock, [this] { return stop || !q.empty(); });
                if (q.empty()) {
                    break; // stopped and drained
                }
                item = std::move(q.front());
                q.pop_front();
                cvPush.notify_all();
            }
            std::string error;
            const bool ok = write(item.index, item.data, error);
            std::lock_guard<std::mutex> lock(mx);
            if (ok) {
                ++written;
            } else {
                ++failed;
                if (firstError.empty()) {
                    firstError = error;
                    LOG(LL_ERR, "EVE1G ", tag, ": frame ", item.index, " not written: ", error);
                }
            }
        }
        if (SUCCEEDED(co)) {
            CoUninitialize();
        }
    }

    std::mutex mx;
    std::condition_variable cvPush, cvPop;
    std::deque<Item> q;
    std::thread thread;
    bool stop = false;
    WriteFn write;
    std::string tag;
    uint64_t written = 0, failed = 0, queued = 0;
    std::string firstError;
};

std::mutex mx; // guards the settings / names below
bool plateOn = false, proxyOn = false, idOn = false;
bool is16 = false;
uint32_t outW = 0, outH = 0, proxyW = 0, proxyH = 0;
std::string plateFile, proxyFile, idFolder, idStem, legendFile;
std::string plateNote, proxyNote, idNote;

Writer plateWriter, proxyWriter, idWriter;
std::unique_ptr<Encoder::DepthVideo> plateVideo, proxyVideo;

// ID pass, render thread
ComPtr<ID3D11Texture2D> idStaging;
D3D11_TEXTURE2D_DESC idDesc{};
bool idHave = false;
bool toldIdFormat = false, toldIdMissing = false, toldIdMsaa = false;
std::array<uint64_t, 256> idCount{}; // pixels per stencil value (whole export, for the legend)
std::vector<uint8_t> idLast;

// one clear colour per stencil value: hue by golden ratio, 0 = black
std::array<uint32_t, 256> MakePalette() {
    std::array<uint32_t, 256> p{};
    for (int v = 1; v < 256; ++v) {
        const float h = std::fmod(v * 0.61803398875f, 1.0f) * 6.0f;
        const float s = (v & 8) ? 0.65f : 1.0f, l = (v & 16) ? 0.6f : 1.0f;
        const int i = static_cast<int>(h);
        const float f = h - i;
        float r = 0, g = 0, b = 0;
        const float q = 1.0f - s * f, t = 1.0f - s * (1.0f - f), m = 1.0f - s;
        switch (i % 6) {
        case 0: r = 1; g = t; b = m; break;
        case 1: r = q; g = 1; b = m; break;
        case 2: r = m; g = 1; b = t; break;
        case 3: r = m; g = q; b = 1; break;
        case 4: r = t; g = m; b = 1; break;
        default: r = 1; g = m; b = q; break;
        }
        const auto c = [l](float x) { return static_cast<uint32_t>(std::lround(std::clamp(x * l, 0.0f, 1.0f) * 255)); };
        p[v] = c(r) | (c(g) << 8) | (c(b) << 16) | 0xFF000000u; // RGBA in memory
    }
    p[0] = 0xFF000000u;
    return p;
}
const std::array<uint32_t, 256> kPalette = MakePalette();

std::string Stem(const std::string& base) {
    const auto s = std::filesystem::u8path(base).filename().u8string();
    return std::string(s.begin(), s.end());
}

bool WritePng(const std::string& path, const void* pixels, uint32_t w, uint32_t h, DXGI_FORMAT f, uint32_t bpp,
              std::string& error) {
    DirectX::Image img{};
    img.width = w;
    img.height = h;
    img.format = f;
    img.rowPitch = static_cast<size_t>(w) * bpp;
    img.slicePitch = img.rowPitch * h;
    img.pixels = static_cast<uint8_t*>(const_cast<void*>(pixels));
    const HRESULT hr = DirectX::SaveToWICFile(img, DirectX::WIC_FLAGS_NONE,
                                              DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG), utf8_decode(path).c_str());
    if (FAILED(hr)) {
        char b[64];
        snprintf(b, sizeof(b), "PNG write failed (0x%08X)", static_cast<unsigned>(hr));
        error = b;
        return false;
    }
    return true;
}

// stencil byte of one pixel of a depth-stencil texture
bool StencilLayout(DXGI_FORMAT f, uint32_t& bytesPerPixel, uint32_t& offset) {
    switch (f) {
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        bytesPerPixel = 8;
        offset = 4;
        return true;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        bytesPerPixel = 4;
        offset = 3;
        return true;
    default:
        return false;
    }
}

void WriteLegend() {
    if (legendFile.empty()) {
        return;
    }
    std::ofstream f(std::filesystem::u8path(legendFile), std::ios::binary | std::ios::trunc);
    f << "EVE1G ID pass (experimental) - the game's stencil value of each pixel, one colour per value\r\n"
      << "Use a colour key (After Effects: Keylight / Linear Color Key with matching tolerance 0) on a colour below.\r\n"
      << "The low 3 bits are usually the kind of surface the game marks (world, characters, vehicles, sky...),\r\n"
      << "the higher bits are flags. What each value means is not documented - check it on your shot.\r\n\r\n"
      << "value  low3  colour (R G B)      share of pixels\r\n";
    uint64_t total = 0;
    for (const uint64_t c : idCount) total += c;
    for (int v = 0; v < 256; ++v) {
        if (!idCount[v]) continue;
        const uint32_t c = kPalette[v];
        char line[160];
        snprintf(line, sizeof(line), "%5d  %4d  %3u %3u %3u   #%02X%02X%02X   %6.2f %%\r\n", v, v & 7, c & 255,
                 (c >> 8) & 255, (c >> 16) & 255, c & 255, (c >> 8) & 255, (c >> 16) & 255,
                 total ? 100.0 * static_cast<double>(idCount[v]) / static_cast<double>(total) : 0.0);
        f << line;
    }
}

} // namespace

void Begin(const std::string& base, uint32_t width, uint32_t height, uint32_t fps_num, uint32_t fps_den, bool master16) {
    End();
    std::lock_guard<std::mutex> lock(mx);
    outW = width;
    outH = height;
    is16 = master16;
    plateOn = proxyOn = idOn = false;
    plateNote = proxyNote = idNote = {};
    plateFile = proxyFile = idFolder = legendFile = {};
    idHave = false;
    idCount.fill(0);
    idLast.clear();
    toldIdFormat = toldIdMissing = toldIdMsaa = false;
    if (!width || !height) {
        return;
    }
    const char* pix = master16 ? "rgba64le" : "rgba";

    if (config::plate_enabled) {
        if (config::capture_source != config::CaptureSource::Clean) {
            plateNote = "needs capture = clean (the frame before ReShade) - not written";
            LOG(LL_WRN, "EVE1G clean plate: ", plateNote);
        } else if (!config::clean_apply_reshade) {
            plateNote = "ReShade is not applied to the video anyway - not written";
            LOG(LL_WRN, "EVE1G clean plate: ", plateNote);
        } else {
            plateFile = base + "_plate.mov";
            plateVideo = std::make_unique<Encoder::DepthVideo>();
            if (plateVideo->open(utf8_decode(plateFile), width, height, fps_num, fps_den, "prores_ks",
                                 "_pixelFormat=yuv422p10le|profile=3", pix, L"EVE1G clean plate", "mov")) {
                plateOn = true;
                Encoder::DepthVideo* v = plateVideo.get();
                const uint32_t rowBytes = width * (master16 ? 8u : 4u);
                plateWriter.start(
                    "clean plate",
                    [v, rowBytes](uint64_t, const std::vector<uint8_t>& d, std::string& e) {
                        if (!v->write(d.data(), static_cast<int>(rowBytes))) {
                            e = "Voukoder refused a plate frame";
                            return false;
                        }
                        return true;
                    },
                    false);
                LOG(LL_NON, "EVE1G clean plate (no ReShade) -> ", plateFile);
            } else {
                plateVideo.reset();
                plateNote = "Voukoder could not open ProRes for the plate";
                LOG(LL_ERR, "EVE1G clean plate: ", plateNote);
            }
        }
    }

    if (config::proxy_enabled) {
        proxyW = (width / 2) & ~1u;
        proxyH = (height / 2) & ~1u;
        proxyFile = base + "_proxy.mp4";
        proxyVideo = std::make_unique<Encoder::DepthVideo>();
        bool ok = proxyVideo->open(utf8_decode(proxyFile), proxyW, proxyH, fps_num, fps_den, "libx264",
                                   "_pixelFormat=yuv420p|rc=crf|preset=veryfast|crf=20", "rgba", L"EVE1G proxy", "mp4");
        if (!ok) {
            ok = proxyVideo->open(utf8_decode(proxyFile), proxyW, proxyH, fps_num, fps_den, "h264_nvenc",
                                  "_pixelFormat=yuv420p|preset=p4|rc=vbr|cq=23|b=0", "rgba", L"EVE1G proxy", "mp4");
        }
        if (ok) {
            proxyOn = true;
            Encoder::DepthVideo* v = proxyVideo.get();
            const uint32_t rowBytes = proxyW * 4;
            proxyWriter.start(
                "proxy",
                [v, rowBytes](uint64_t, const std::vector<uint8_t>& d, std::string& e) {
                    if (!v->write(d.data(), static_cast<int>(rowBytes))) {
                        e = "Voukoder refused a proxy frame";
                        return false;
                    }
                    return true;
                },
                false);
            LOG(LL_NON, "EVE1G proxy ", proxyW, "x", proxyH, " H.264 -> ", proxyFile);
        } else {
            proxyVideo.reset();
            proxyNote = "no H.264 encoder could be opened";
            LOG(LL_ERR, "EVE1G proxy: ", proxyNote);
        }
    }

    if (config::id_enabled) {
        idFolder = base + "_id";
        idStem = Stem(base);
        legendFile = base + "_id_legend.txt";
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::u8path(idFolder), ec);
        idOn = true;
        const std::string folder = idFolder, stem = idStem;
        const uint32_t w = width, h = height;
        idWriter.start(
            "id pass",
            [folder, stem, w, h](uint64_t index, const std::vector<uint8_t>& d, std::string& e) {
                char n[32];
                snprintf(n, sizeof(n), "_id_%06llu.png", static_cast<unsigned long long>(index));
                return WritePng(folder + "\\" + stem + n, d.data(), w, h, DXGI_FORMAT_R8G8B8A8_UNORM, 4, e);
            },
            true);
        LOG(LL_NON, "EVE1G ID pass (experimental, stencil) -> ", idFolder);
    }
}

bool PlateWanted() {
    std::lock_guard<std::mutex> lock(mx);
    return plateOn;
}

void OnMasterFrame(const void* pixels, uint32_t rowPitch) {
    uint32_t pw, ph;
    bool b16;
    {
        std::lock_guard<std::mutex> lock(mx);
        if (!proxyOn || !pixels) {
            return;
        }
        pw = proxyW;
        ph = proxyH;
        b16 = is16;
    }
    // 2 x 2 box filter, RGBA8 out (16-bit masters: the high bytes)
    std::vector<uint8_t> out(static_cast<size_t>(pw) * ph * 4);
    const auto* src = static_cast<const uint8_t*>(pixels);
    for (uint32_t y = 0; y < ph; ++y) {
        const uint8_t* r0 = src + static_cast<size_t>(2 * y) * rowPitch;
        const uint8_t* r1 = r0 + rowPitch;
        uint8_t* o = out.data() + static_cast<size_t>(y) * pw * 4;
        if (b16) {
            const auto* a = reinterpret_cast<const uint16_t*>(r0);
            const auto* b = reinterpret_cast<const uint16_t*>(r1);
            for (uint32_t x = 0; x < pw; ++x) {
                const size_t i = static_cast<size_t>(x) * 8;
                for (int c = 0; c < 4; ++c) {
                    const uint32_t s = a[i + c] + a[i + 4 + c] + b[i + c] + b[i + 4 + c];
                    o[x * 4 + c] = static_cast<uint8_t>((s + 514) / 1028); // /4 then 65535 -> 255
                }
            }
        } else {
            for (uint32_t x = 0; x < pw; ++x) {
                const size_t i = static_cast<size_t>(x) * 8;
                for (int c = 0; c < 4; ++c) {
                    o[x * 4 + c] = static_cast<uint8_t>((r0[i + c] + r0[i + 4 + c] + r1[i + c] + r1[i + 4 + c] + 2) / 4);
                }
            }
        }
    }
    proxyWriter.push(std::move(out));
}

void OnPlateFrame(const void* pixels, uint32_t rowPitch) {
    uint32_t w, h;
    bool b16;
    {
        std::lock_guard<std::mutex> lock(mx);
        if (!plateOn || !pixels) {
            return;
        }
        w = outW;
        h = outH;
        b16 = is16;
    }
    const size_t row = static_cast<size_t>(w) * (b16 ? 8 : 4);
    std::vector<uint8_t> out(row * h);
    for (uint32_t y = 0; y < h; ++y) {
        memcpy(out.data() + y * row, static_cast<const uint8_t*>(pixels) + static_cast<size_t>(y) * rowPitch, row);
    }
    plateWriter.push(std::move(out));
}

void OnSample(ID3D11DeviceContext* context, ID3D11Texture2D* depthStencil, int sample, int perFrame) {
    {
        std::lock_guard<std::mutex> lock(mx);
        if (!idOn) {
            return;
        }
    }
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
    idHave = false;
    if (!depthStencil) {
        if (!toldIdMissing) {
            toldIdMissing = true;
            LOG(LL_WRN, "EVE1G ID pass: the game's depth-stencil buffer was not seen (EVE1G loaded after the game "
                        "made it) - the ID frames stay black");
        }
        return;
    }
    D3D11_TEXTURE2D_DESC d;
    depthStencil->GetDesc(&d);
    uint32_t bpp, off;
    if (!StencilLayout(d.Format, bpp, off)) {
        if (!toldIdFormat) {
            toldIdFormat = true;
            LOG(LL_WRN, "EVE1G ID pass: the depth-stencil buffer is ", conv_dxgi_format_to_string(d.Format),
                " - no stencil to read");
        }
        return;
    }
    if (d.SampleDesc.Count > 1) {
        if (!toldIdMsaa) {
            toldIdMsaa = true;
            LOG(LL_WRN, "EVE1G ID pass: MSAA depth buffers cannot be read - turn MSAA off in the game for the ID pass");
        }
        return;
    }
    if (!idStaging || idDesc.Width != d.Width || idDesc.Height != d.Height || idDesc.Format != d.Format) {
        D3D11_TEXTURE2D_DESC sd = d;
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        ComPtr<ID3D11Device> device;
        depthStencil->GetDevice(device.GetAddressOf());
        if (!device || FAILED(device->CreateTexture2D(&sd, nullptr, idStaging.ReleaseAndGetAddressOf()))) {
            idStaging = nullptr;
            if (!toldIdFormat) {
                toldIdFormat = true;
                LOG(LL_WRN, "EVE1G ID pass: could not create a readable copy of the depth-stencil buffer");
            }
            return;
        }
        idDesc = sd;
    }
    context->CopySubresourceRegion(idStaging.Get(), 0, 0, 0, 0, depthStencil, 0, nullptr);
    idHave = true;
}

void OnFrameEmitted(ID3D11DeviceContext* context) {
    uint32_t w, h;
    {
        std::lock_guard<std::mutex> lock(mx);
        if (!idOn) {
            return;
        }
        w = outW;
        h = outH;
    }
    std::vector<uint8_t> out;
    uint32_t bpp = 0, off = 0;
    if (idHave && idStaging && context && StencilLayout(idDesc.Format, bpp, off)) {
        D3D11_MAPPED_SUBRESOURCE map{};
        if (SUCCEEDED(context->Map(idStaging.Get(), 0, D3D11_MAP_READ, 0, &map))) {
            out.resize(static_cast<size_t>(w) * h * 4);
            const uint32_t sw = idDesc.Width, sh = idDesc.Height;
            auto* o32 = reinterpret_cast<uint32_t*>(out.data());
            for (uint32_t y = 0; y < h; ++y) {
                const uint32_t sy = static_cast<uint32_t>((static_cast<uint64_t>(y) * sh) / h);
                const uint8_t* row = static_cast<const uint8_t*>(map.pData) + static_cast<size_t>(sy) * map.RowPitch;
                for (uint32_t x = 0; x < w; ++x) {
                    const uint32_t sx = static_cast<uint32_t>((static_cast<uint64_t>(x) * sw) / w);
                    const uint8_t v = row[static_cast<size_t>(sx) * bpp + off];
                    ++idCount[v];
                    o32[static_cast<size_t>(y) * w + x] = kPalette[v];
                }
            }
            context->Unmap(idStaging.Get(), 0);
        }
    }
    idHave = false;
    if (out.empty()) {
        out = idLast.empty() ? std::vector<uint8_t>(static_cast<size_t>(w) * h * 4, 0) : idLast;
    } else {
        idLast = out;
    }
    idWriter.push(std::move(out));
}

void End() {
    plateWriter.stopAll();
    proxyWriter.stopAll();
    idWriter.stopAll();
    std::lock_guard<std::mutex> lock(mx);
    if (plateVideo) {
        plateVideo->close();
        plateVideo.reset();
        LOG(LL_NON, "EVE1G clean plate: ", plateWriter.result(), " -> ", plateFile);
    }
    if (proxyVideo) {
        proxyVideo->close();
        proxyVideo.reset();
        LOG(LL_NON, "EVE1G proxy: ", proxyWriter.result(), " -> ", proxyFile);
    }
    if (idOn) {
        WriteLegend();
        LOG(LL_NON, "EVE1G ID pass: ", idWriter.result(), " -> ", idFolder);
    }
    idLast.clear();
    idLast.shrink_to_fit();
    idStaging = nullptr;
}

std::string Describe() {
    std::lock_guard<std::mutex> lock(mx);
    std::string s;
    s += "Clean plate    : ";
    s += plateOn ? "ProRes 422 HQ, no ReShade -> " + plateFile
                 : (config::plate_enabled ? "on, " + plateNote : std::string("off"));
    s += "\r\nProxy          : ";
    s += proxyOn ? "H.264 " + std::to_string(proxyW) + " x " + std::to_string(proxyH) + " -> " + proxyFile
                 : (config::proxy_enabled ? "on, " + proxyNote : std::string("off"));
    s += "\r\nID pass        : ";
    s += idOn ? "experimental stencil colours, PNG -> " + idFolder : std::string("off");
    s += "\r\nMaster         : ";
    s += is16 ? "16-bit (rgba64le) into the encoder" : "8-bit (rgba) into the encoder";
    return s;
}

std::string Result() {
    std::lock_guard<std::mutex> lock(mx);
    std::string s;
    if (plateFile.size()) s += "Clean plate    : " + plateWriter.result() + "\r\n";
    if (proxyFile.size()) s += "Proxy          : " + proxyWriter.result() + "\r\n";
    if (idFolder.size()) s += "ID frames      : " + idWriter.result() + "\r\n";
    return s;
}

std::string ActiveList() {
    std::lock_guard<std::mutex> lock(mx);
    std::string s;
    auto add = [&s](const char* n) {
        if (!s.empty()) s += " + ";
        s += n;
    };
    if (plateOn) add("clean plate");
    if (proxyOn) add("proxy");
    if (idOn) add("ID");
    return s;
}

} // namespace passes
