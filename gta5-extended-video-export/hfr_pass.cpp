// EVE1G HFR export - see hfr_pass.h
#include "hfr_pass.h"

#include "config.h"
#include "encoder.h"
#include "logger.h"
#include "util.h"

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

namespace hfr {
namespace {

enum class Format { ProRes, Png };

struct Job {
    uint64_t index = 0;
    std::vector<uint8_t> rgba; // width * height * 4
};

constexpr size_t kMaxQueued = 4; // frames waiting for the writer (each 4K frame is ~33 MB)

std::mutex mx; // guards everything below
bool active = false;
Format format = Format::ProRes;
int every = 1;
std::string folder, stem, target;
uint32_t outW = 0, outH = 0;
double fps = 0.0;

// render thread
ComPtr<ID3D11Texture2D> staging;
D3D11_TEXTURE2D_DESC stagingDesc{};
uint64_t counter = 0;
bool toldMsaa = false, toldSize = false, toldFormat = false;

uint64_t frames = 0, written = 0, failed = 0, skipped = 0;
std::string firstError;

// writer
std::deque<Job> queue;
std::condition_variable cvPush, cvPop;
bool stop = false;
std::thread worker;
std::unique_ptr<Encoder::DepthVideo> video;

std::string FramePath(uint64_t index) {
    char n[32];
    snprintf(n, sizeof(n), "_hfr_%07llu.png", static_cast<unsigned long long>(index));
    return folder + "\\" + stem + n;
}

bool WriteFrame(const Job& job, std::string& error) {
    if (format == Format::ProRes) {
        if (!video || !video->isOpen() || !video->write(job.rgba.data(), static_cast<int>(outW * 4))) {
            error = "Voukoder refused an HFR frame";
            return false;
        }
        return true;
    }
    DirectX::Image img{};
    img.width = outW;
    img.height = outH;
    img.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    img.rowPitch = outW * 4;
    img.slicePitch = img.rowPitch * outH;
    img.pixels = const_cast<uint8_t*>(job.rgba.data());
    const std::wstring path = utf8_decode(FramePath(job.index));
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
        const bool ok = WriteFrame(job, error);
        std::lock_guard<std::mutex> lock(mx);
        if (ok) {
            ++written;
        } else {
            ++failed;
            if (firstError.empty()) {
                firstError = error;
                LOG(LL_ERR, "EVE1G HFR: frame ", job.index, " not written: ", error);
            }
        }
    }
    if (SUCCEEDED(co)) {
        CoUninitialize();
    }
}

inline uint8_t Half8(uint16_t h) { // half float -> 0..255, clamped (HDR sources are rare here)
    const uint32_t s = h >> 15, e = (h >> 10) & 0x1F, m = h & 0x3FF;
    float f;
    if (e == 0) {
        f = std::ldexp(static_cast<float>(m), -24);
    } else if (e == 31) {
        f = 1.0f;
    } else {
        f = std::ldexp(static_cast<float>(m | 0x400), static_cast<int>(e) - 25);
    }
    if (s) {
        f = 0.0f;
    }
    return static_cast<uint8_t>(std::lround(std::clamp(f, 0.0f, 1.0f) * 255.0f));
}

// One row of the source format -> RGBA8. False for a format this does not know.
bool ConvertRow(DXGI_FORMAT f, const uint8_t* src, uint8_t* dst, uint32_t w) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        for (uint32_t x = 0; x < w; ++x) {
            dst[4 * x + 0] = src[4 * x + 0];
            dst[4 * x + 1] = src[4 * x + 1];
            dst[4 * x + 2] = src[4 * x + 2];
            dst[4 * x + 3] = 255;
        }
        return true;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        for (uint32_t x = 0; x < w; ++x) {
            dst[4 * x + 0] = src[4 * x + 2];
            dst[4 * x + 1] = src[4 * x + 1];
            dst[4 * x + 2] = src[4 * x + 0];
            dst[4 * x + 3] = 255;
        }
        return true;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: {
        const uint32_t* p = reinterpret_cast<const uint32_t*>(src);
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t v = p[x];
            dst[4 * x + 0] = static_cast<uint8_t>((v & 0x3FF) >> 2);
            dst[4 * x + 1] = static_cast<uint8_t>(((v >> 10) & 0x3FF) >> 2);
            dst[4 * x + 2] = static_cast<uint8_t>(((v >> 20) & 0x3FF) >> 2);
            dst[4 * x + 3] = 255;
        }
        return true;
    }
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: {
        const uint16_t* p = reinterpret_cast<const uint16_t*>(src);
        for (uint32_t x = 0; x < w; ++x) {
            dst[4 * x + 0] = Half8(p[4 * x + 0]);
            dst[4 * x + 1] = Half8(p[4 * x + 1]);
            dst[4 * x + 2] = Half8(p[4 * x + 2]);
            dst[4 * x + 3] = 255;
        }
        return true;
    }
    default:
        return false;
    }
}

std::string DescribeLocked() {
    if (!config::hfr_enabled) {
        return "off";
    }
    char b[200];
    if (every == 1) {
        snprintf(b, sizeof(b), "%s at %.3f fps (every game frame)",
                 format == Format::ProRes ? "ProRes 422 HQ video" : "PNG sequence", fps);
    } else {
        snprintf(b, sizeof(b), "%s at %.3f fps (every %d. game frame)",
                 format == Format::ProRes ? "ProRes 422 HQ video" : "PNG sequence", fps, every);
    }
    return b;
}

} // namespace

void Begin(const std::string& base, uint32_t width, uint32_t height, uint32_t fps_num, uint32_t fps_den) {
    End();
    std::lock_guard<std::mutex> lock(mx);
    active = config::hfr_enabled && width > 0 && height > 0 && fps_num > 0 && fps_den > 0;
    if (!active) {
        return;
    }
    format = config::hfr_format == "png" ? Format::Png : Format::ProRes;
    every = std::max(1, config::hfr_every);
    outW = width;
    outH = height;
    frames = written = failed = skipped = 0;
    counter = 0;
    firstError.clear();
    queue.clear();
    stop = false;
    toldMsaa = toldSize = toldFormat = false;

    // game frames per second of video = fps * (samples + 1); keep every Nth
    const uint64_t num = static_cast<uint64_t>(fps_num) * (static_cast<uint64_t>(config::motion_blur_samples) + 1);
    const uint64_t den = static_cast<uint64_t>(fps_den) * static_cast<uint64_t>(every);
    fps = static_cast<double>(num) / static_cast<double>(den);

    const std::filesystem::path p = std::filesystem::u8path(base);
    {
        const auto s = p.filename().u8string();
        stem = std::string(s.begin(), s.end());
    }
    char tag[32];
    snprintf(tag, sizeof(tag), "_hfr%.0f", fps);
    if (format == Format::ProRes) {
        video = std::make_unique<Encoder::DepthVideo>();
        target = base + tag + ".mov";
        if (!video->open(utf8_decode(target), width, height, static_cast<uint32_t>(num), static_cast<uint32_t>(den),
                         "prores_ks", "_pixelFormat=yuv422p10le|profile=3", "rgba", L"EVE1G HFR")) {
            LOG(LL_WRN, "EVE1G HFR: the ProRes file could not be opened - writing a PNG sequence instead");
            video.reset();
            format = Format::Png;
        }
    }
    if (format == Format::Png) {
        folder = base + tag;
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::u8path(folder), ec);
        target = folder + "\\" + stem + "_hfr_#######.png";
    }
    worker = std::thread(WorkerMain);
    LOG(LL_NON, "EVE1G HFR: ", DescribeLocked(), " -> ", target);
}

void OnGameFrame(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* source) {
    uint32_t w, h;
    {
        std::lock_guard<std::mutex> lock(mx);
        if (!active || stop) {
            return;
        }
        w = outW;
        h = outH;
    }
    if ((counter++ % static_cast<uint64_t>(every)) != 0 || !device || !context || !source) {
        return;
    }
    D3D11_TEXTURE2D_DESC d;
    source->GetDesc(&d);
    if (d.SampleDesc.Count > 1) {
        if (!toldMsaa) {
            toldMsaa = true;
            LOG(LL_ERR, "EVE1G HFR: the captured image is multisampled - HFR frames skipped");
        }
        std::lock_guard<std::mutex> lock(mx);
        ++skipped;
        return;
    }
    if (d.Width != w || d.Height != h) {
        if (!toldSize) {
            toldSize = true;
            LOG(LL_ERR, "EVE1G HFR: the captured image is ", d.Width, "x", d.Height, " but the video is ", w, "x", h,
                " - HFR frames skipped until they match");
        }
        std::lock_guard<std::mutex> lock(mx);
        ++skipped;
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
        if (FAILED(device->CreateTexture2D(&sd, nullptr, staging.ReleaseAndGetAddressOf()))) {
            staging = nullptr;
            return;
        }
        stagingDesc = sd;
    }
    context->CopyResource(staging.Get(), source);

    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) {
        std::lock_guard<std::mutex> lock(mx);
        ++skipped;
        return;
    }
    bool ok = true;
    for (uint32_t y = 0; y < h && ok; ++y) {
        ok = ConvertRow(d.Format, static_cast<const uint8_t*>(map.pData) + static_cast<size_t>(y) * map.RowPitch,
                        rgba.data() + static_cast<size_t>(y) * w * 4, w);
    }
    context->Unmap(staging.Get(), 0);
    if (!ok) {
        if (!toldFormat) {
            toldFormat = true;
            LOG(LL_ERR, "EVE1G HFR: unsupported image format ", conv_dxgi_format_to_string(d.Format),
                " - HFR frames skipped");
        }
        std::lock_guard<std::mutex> lock(mx);
        ++skipped;
        return;
    }

    std::unique_lock<std::mutex> lock(mx);
    cvPush.wait(lock, [] { return queue.size() < kMaxQueued || stop; });
    if (stop || !active) {
        return;
    }
    Job job;
    job.index = frames++;
    job.rgba = std::move(rgba);
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
    LOG(LL_NON, "EVE1G HFR: ", written, " frame(s) written, ", skipped, " skipped, ", failed, " failed -> ", target);
    active = false;
}

std::string Describe() {
    std::lock_guard<std::mutex> lock(mx);
    if (!config::hfr_enabled) {
        return "off";
    }
    if (target.empty()) {
        return "on, but not started";
    }
    return DescribeLocked() + "\r\n                 -> " + target;
}

std::string Result() {
    std::lock_guard<std::mutex> lock(mx);
    if (!config::hfr_enabled) {
        return {};
    }
    char b[200];
    snprintf(b, sizeof(b), "%llu written, %llu skipped, %llu failed", static_cast<unsigned long long>(written),
             static_cast<unsigned long long>(skipped), static_cast<unsigned long long>(failed));
    return b;
}

} // namespace hfr
