#pragma warning(push)
#pragma warning(disable : 4244)
#pragma warning(disable : 26812)
#include "encoder.h"
#include "logger.h"
#include "util.h"

#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfOutputFile.h>
#include <OpenEXR/ImfRgba.h>
#include <OpenEXR/ImfRgbaFile.h>
#include <filesystem>
#include <xutility>
#pragma warning(pop)
#include <atomic>
#include <cctype>
#include <iterator>
#include <objbase.h>
#include <string>
#include <type_traits>
#include <winreg.h>

#pragma comment(lib, "advapi32.lib")

// ------------------------------------------------------------------------------------------------------------------
// EVE1G: keep Voukoder off the GPU when the chosen encoder does not need it.
//
// Every time a context opens, Voukoder (ffmpeg inside it) loads NVIDIA's encoder and CUDA, and makes test D3D11
// devices, to see which hardware encoders exist - even for ProRes or x264, which run on the CPU only. That start-up
// runs on the export thread while the game, the GTA V Upscaler and ReShade keep drawing frames on the same GPU, and
// the NVIDIA driver crashes in the middle of it (access violation reading 0x10 in nvwgf2umx.dll; every crash dump had
// Voukoder inside nvcuda/nvEncodeAPI at that moment).
//
// Voukoder's imports of LoadLibrary* and of the D3D/DXGI device functions are redirected (import table patch, only
// inside voukoder.dll). While a CPU encoder is being opened, the GPU libraries simply "aren't there" for Voukoder; it
// skips its hardware checks and opens the CPU encoder. A NVENC / AMF / QSV / Media Foundation choice keeps everything.
// ------------------------------------------------------------------------------------------------------------------
namespace {

enum : uint32_t { VK_BLOCK_NVIDIA = 1, VK_BLOCK_AMD = 2, VK_BLOCK_INTEL = 4, VK_BLOCK_GPU_API = 8 };

std::atomic<uint32_t> g_vkBlock{0};
std::atomic<uint32_t> g_vkBlockedLoads{0};
std::atomic<uint32_t> g_vkBlockedDevices{0};
std::atomic<bool> g_vkPatched{false};

using LoadLibraryA_t = HMODULE(WINAPI*)(LPCSTR);
using LoadLibraryW_t = HMODULE(WINAPI*)(LPCWSTR);
using LoadLibraryExA_t = HMODULE(WINAPI*)(LPCSTR, HANDLE, DWORD);
using LoadLibraryExW_t = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
using D3D11CreateDevice_t = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*,
                                             UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
using D3D11CreateDeviceAndSwapChain_t = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                                         const D3D_FEATURE_LEVEL*, UINT, UINT,
                                                         const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**,
                                                         D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
using CreateDXGIFactory_t = HRESULT(WINAPI*)(REFIID, void**);
using CreateDXGIFactory2_t = HRESULT(WINAPI*)(UINT, REFIID, void**);
using D3D12CreateDevice_t = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
using Direct3DCreate9Ex_t = HRESULT(WINAPI*)(UINT, void**);
using Direct3DCreate9_t = void*(WINAPI*)(UINT);

LoadLibraryA_t o_LoadLibraryA = nullptr;
LoadLibraryW_t o_LoadLibraryW = nullptr;
LoadLibraryExA_t o_LoadLibraryExA = nullptr;
LoadLibraryExW_t o_LoadLibraryExW = nullptr;
D3D11CreateDevice_t o_D3D11CreateDevice = nullptr;
D3D11CreateDeviceAndSwapChain_t o_D3D11CreateDeviceAndSwapChain = nullptr;
CreateDXGIFactory_t o_CreateDXGIFactory = nullptr;
CreateDXGIFactory_t o_CreateDXGIFactory1 = nullptr;
CreateDXGIFactory2_t o_CreateDXGIFactory2 = nullptr;
D3D12CreateDevice_t o_D3D12CreateDevice = nullptr;
Direct3DCreate9Ex_t o_Direct3DCreate9Ex = nullptr;
Direct3DCreate9_t o_Direct3DCreate9 = nullptr;

constexpr HRESULT kNoGpu = static_cast<HRESULT>(0x887A0004L); // DXGI_ERROR_UNSUPPORTED

// Which group a library belongs to (0 = not a GPU library). Compares the file name only, case-insensitive.
template <typename C> uint32_t GpuLibraryGroup(const C* path) {
    if (!path) {
        return 0;
    }
    const C* name = path;
    for (const C* p = path; *p; ++p) {
        if (*p == C('\\') || *p == C('/')) {
            name = p + 1;
        }
    }
    char lower[64] = {};
    size_t n = 0;
    for (; name[n] && n < sizeof(lower) - 1; ++n) {
        const C c = name[n];
        if (static_cast<unsigned>(static_cast<std::make_unsigned_t<C>>(c)) > 127u) {
            return 0;
        }
        lower[n] = static_cast<char>((c >= C('A') && c <= C('Z')) ? c - C('A') + C('a') : c);
    }
    if (name[n]) {
        return 0; // longer than any name below
    }
    std::string s(lower, n);
    if (s.size() < 4 || s.compare(s.size() - 4, 4, ".dll") != 0) {
        s += ".dll";
    }
    static const char* const nvidia[] = {"nvcuda.dll", "nvencodeapi64.dll", "nvencodeapi.dll", "nvcuvid.dll"};
    static const char* const amd[] = {"amfrt64.dll", "amfrt32.dll"};
    static const char* const intel[] = {"libmfxhw64.dll", "libmfx64.dll", "libvpl.dll", "libmfx64-gen.dll",
                                        "libmfx-gen.dll"};
    static const char* const gpu_api[] = {"d3d11.dll", "dxgi.dll",   "d3d12.dll",   "d3d9.dll",
                                          "opencl.dll", "vulkan-1.dll", "dxva2.dll"};
    for (const char* x : nvidia) {
        if (s == x) return VK_BLOCK_NVIDIA;
    }
    for (const char* x : amd) {
        if (s == x) return VK_BLOCK_AMD;
    }
    for (const char* x : intel) {
        if (s == x) return VK_BLOCK_INTEL;
    }
    for (const char* x : gpu_api) {
        if (s == x) return VK_BLOCK_GPU_API;
    }
    return 0;
}

template <typename C> bool RefuseLibrary(const C* path) {
    const uint32_t group = GpuLibraryGroup(path);
    if (group != 0 && (g_vkBlock.load() & group) != 0) {
        g_vkBlockedLoads.fetch_add(1);
        SetLastError(ERROR_MOD_NOT_FOUND);
        return true;
    }
    return false;
}

bool RefuseDevice() {
    if ((g_vkBlock.load() & VK_BLOCK_GPU_API) != 0) {
        g_vkBlockedDevices.fetch_add(1);
        return true;
    }
    return false;
}

HMODULE WINAPI vk_LoadLibraryA(LPCSTR name) {
    return RefuseLibrary(name) ? nullptr : o_LoadLibraryA(name);
}
HMODULE WINAPI vk_LoadLibraryW(LPCWSTR name) {
    return RefuseLibrary(name) ? nullptr : o_LoadLibraryW(name);
}
HMODULE WINAPI vk_LoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags) {
    return RefuseLibrary(name) ? nullptr : o_LoadLibraryExA(name, file, flags);
}
HMODULE WINAPI vk_LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags) {
    return RefuseLibrary(name) ? nullptr : o_LoadLibraryExW(name, file, flags);
}
HRESULT WINAPI vk_D3D11CreateDevice(IDXGIAdapter* a, D3D_DRIVER_TYPE t, HMODULE s, UINT f, const D3D_FEATURE_LEVEL* fl,
                                    UINT n, UINT v, ID3D11Device** d, D3D_FEATURE_LEVEL* ofl,
                                    ID3D11DeviceContext** c) {
    if (RefuseDevice()) {
        if (d) *d = nullptr;
        if (c) *c = nullptr;
        return kNoGpu;
    }
    return o_D3D11CreateDevice(a, t, s, f, fl, n, v, d, ofl, c);
}
HRESULT WINAPI vk_D3D11CreateDeviceAndSwapChain(IDXGIAdapter* a, D3D_DRIVER_TYPE t, HMODULE s, UINT f,
                                                const D3D_FEATURE_LEVEL* fl, UINT n, UINT v,
                                                const DXGI_SWAP_CHAIN_DESC* sd, IDXGISwapChain** sc,
                                                ID3D11Device** d, D3D_FEATURE_LEVEL* ofl, ID3D11DeviceContext** c) {
    if (RefuseDevice()) {
        if (sc) *sc = nullptr;
        if (d) *d = nullptr;
        if (c) *c = nullptr;
        return kNoGpu;
    }
    return o_D3D11CreateDeviceAndSwapChain(a, t, s, f, fl, n, v, sd, sc, d, ofl, c);
}
HRESULT WINAPI vk_CreateDXGIFactory(REFIID riid, void** out) {
    if (RefuseDevice()) {
        if (out) *out = nullptr;
        return kNoGpu;
    }
    return o_CreateDXGIFactory(riid, out);
}
HRESULT WINAPI vk_CreateDXGIFactory1(REFIID riid, void** out) {
    if (RefuseDevice()) {
        if (out) *out = nullptr;
        return kNoGpu;
    }
    return o_CreateDXGIFactory1(riid, out);
}
HRESULT WINAPI vk_CreateDXGIFactory2(UINT flags, REFIID riid, void** out) {
    if (RefuseDevice()) {
        if (out) *out = nullptr;
        return kNoGpu;
    }
    return o_CreateDXGIFactory2(flags, riid, out);
}
HRESULT WINAPI vk_D3D12CreateDevice(IUnknown* a, D3D_FEATURE_LEVEL fl, REFIID riid, void** out) {
    if (RefuseDevice()) {
        if (out) *out = nullptr;
        return kNoGpu;
    }
    return o_D3D12CreateDevice(a, fl, riid, out);
}
HRESULT WINAPI vk_Direct3DCreate9Ex(UINT sdk, void** out) {
    if (RefuseDevice()) {
        if (out) *out = nullptr;
        return kNoGpu;
    }
    return o_Direct3DCreate9Ex(sdk, out);
}
void* WINAPI vk_Direct3DCreate9(UINT sdk) {
    return RefuseDevice() ? nullptr : o_Direct3DCreate9(sdk);
}

struct ImportRedirect {
    const char* name;
    void* replacement;
    void** original;
};

const ImportRedirect kRedirects[] = {
    {"LoadLibraryA", reinterpret_cast<void*>(&vk_LoadLibraryA), reinterpret_cast<void**>(&o_LoadLibraryA)},
    {"LoadLibraryW", reinterpret_cast<void*>(&vk_LoadLibraryW), reinterpret_cast<void**>(&o_LoadLibraryW)},
    {"LoadLibraryExA", reinterpret_cast<void*>(&vk_LoadLibraryExA), reinterpret_cast<void**>(&o_LoadLibraryExA)},
    {"LoadLibraryExW", reinterpret_cast<void*>(&vk_LoadLibraryExW), reinterpret_cast<void**>(&o_LoadLibraryExW)},
    {"D3D11CreateDevice", reinterpret_cast<void*>(&vk_D3D11CreateDevice),
     reinterpret_cast<void**>(&o_D3D11CreateDevice)},
    {"D3D11CreateDeviceAndSwapChain", reinterpret_cast<void*>(&vk_D3D11CreateDeviceAndSwapChain),
     reinterpret_cast<void**>(&o_D3D11CreateDeviceAndSwapChain)},
    {"CreateDXGIFactory", reinterpret_cast<void*>(&vk_CreateDXGIFactory),
     reinterpret_cast<void**>(&o_CreateDXGIFactory)},
    {"CreateDXGIFactory1", reinterpret_cast<void*>(&vk_CreateDXGIFactory1),
     reinterpret_cast<void**>(&o_CreateDXGIFactory1)},
    {"CreateDXGIFactory2", reinterpret_cast<void*>(&vk_CreateDXGIFactory2),
     reinterpret_cast<void**>(&o_CreateDXGIFactory2)},
    {"D3D12CreateDevice", reinterpret_cast<void*>(&vk_D3D12CreateDevice),
     reinterpret_cast<void**>(&o_D3D12CreateDevice)},
    {"Direct3DCreate9Ex", reinterpret_cast<void*>(&vk_Direct3DCreate9Ex),
     reinterpret_cast<void**>(&o_Direct3DCreate9Ex)},
    {"Direct3DCreate9", reinterpret_cast<void*>(&vk_Direct3DCreate9), reinterpret_cast<void**>(&o_Direct3DCreate9)},
};

// The real function for a redirected import (used when the import is still an unresolved delay-load thunk).
void* RealFunction(const char* dll, const char* name) {
    HMODULE m = GetModuleHandleA(dll);
    if (!m) {
        m = LoadLibraryA(dll);
    }
    return m ? reinterpret_cast<void*>(GetProcAddress(m, name)) : nullptr;
}

const char* HomeDll(const char* name) {
    if (strncmp(name, "LoadLibrary", 11) == 0) return "kernel32.dll";
    if (strncmp(name, "D3D11", 5) == 0) return "d3d11.dll";
    if (strncmp(name, "CreateDXGI", 10) == 0) return "dxgi.dll";
    if (strncmp(name, "D3D12", 5) == 0) return "d3d12.dll";
    return "d3d9.dll";
}

bool WriteSlot(void** slot, void* value) {
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        return false;
    }
    *slot = value;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

// Point one import slot of voukoder.dll at our function. `resolved` = the slot holds the real function already
// (normal import); otherwise it is a delay-load thunk and the real function is looked up by name.
int RedirectSlot(const char* name, void** slot, bool resolved) {
    for (const ImportRedirect& r : kRedirects) {
        if (strcmp(name, r.name) != 0) {
            continue;
        }
        if (*slot == r.replacement) {
            return 0;
        }
        void* real = resolved ? *slot : RealFunction(HomeDll(name), name);
        if (!real) {
            return 0;
        }
        if (*r.original == nullptr) {
            *r.original = real;
        }
        return WriteSlot(slot, r.replacement) ? 1 : 0;
    }
    return 0;
}

int PatchVoukoderImports(HMODULE module) {
    auto* base = reinterpret_cast<uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    int patched = 0;

    const IMAGE_DATA_DIRECTORY& imp = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imp.VirtualAddress && imp.Size) {
        for (auto* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + imp.VirtualAddress); d->Name; ++d) {
            if (!d->OriginalFirstThunk || !d->FirstThunk) continue;
            auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->OriginalFirstThunk);
            auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->FirstThunk);
            for (; names->u1.AddressOfData; ++names, ++slots) {
                if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
                const auto* ibn = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
                patched += RedirectSlot(reinterpret_cast<const char*>(ibn->Name),
                                        reinterpret_cast<void**>(&slots->u1.Function), true);
            }
        }
    }

    const IMAGE_DATA_DIRECTORY& dimp = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (dimp.VirtualAddress && dimp.Size) {
        for (auto* d = reinterpret_cast<IMAGE_DELAYLOAD_DESCRIPTOR*>(base + dimp.VirtualAddress); d->DllNameRVA;
             ++d) {
            if (!d->ImportNameTableRVA || !d->ImportAddressTableRVA) continue;
            auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->ImportNameTableRVA);
            auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->ImportAddressTableRVA);
            for (; names->u1.AddressOfData; ++names, ++slots) {
                if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
                const auto* ibn = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
                // a delay-load slot that already points inside voukoder.dll is the unresolved thunk
                const auto target = reinterpret_cast<uintptr_t>(reinterpret_cast<void*>(slots->u1.Function));
                const bool resolved = target < reinterpret_cast<uintptr_t>(base) ||
                                      target >= reinterpret_cast<uintptr_t>(base) + nt->OptionalHeader.SizeOfImage;
                patched += RedirectSlot(reinterpret_cast<const char*>(ibn->Name),
                                        reinterpret_cast<void**>(&slots->u1.Function), resolved);
            }
        }
    }
    return patched;
}

// voukoder.dll as registered for CLSID_CoVoukoder (CoCreateInstance loads exactly this file).
std::wstring VoukoderPath() {
    wchar_t clsid[64] = {};
    if (StringFromGUID2(CLSID_CoVoukoder, clsid, static_cast<int>(std::size(clsid))) == 0) {
        return {};
    }
    const std::wstring key = std::wstring(L"CLSID\\") + clsid + L"\\InprocServer32";
    wchar_t path[MAX_PATH * 2] = {};
    DWORD size = sizeof(path);
    if (RegGetValueW(HKEY_CLASSES_ROOT, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, path, &size) != ERROR_SUCCESS) {
        return {};
    }
    return path;
}

// Load voukoder.dll ourselves (before COM does) and patch its imports, so its very first hardware check already
// goes through us. Only done once; the DLL stays loaded for the rest of the session.
void PrepareVoukoder() {
    if (g_vkPatched.load()) {
        return;
    }
    HMODULE module = GetModuleHandleW(L"voukoder.dll");
    if (!module) {
        const std::wstring path = VoukoderPath();
        if (!path.empty()) {
            module = LoadLibraryW(path.c_str());
        }
    }
    if (!module) {
        LOG(LL_NON, "EVE1G: voukoder.dll not found before opening - its GPU checks cannot be held back");
        return;
    }
    const int n = PatchVoukoderImports(module);
    g_vkPatched = true;
    LOG(LL_NON, "EVE1G: Voukoder import redirects installed: ", n);
}

// Which GPU groups the chosen video encoder may use; everything else is hidden from Voukoder while it opens.
uint32_t BlockMaskFor(const char* encoder) {
    std::string e = encoder ? encoder : "";
    for (char& c : e) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (e.find("_mf") != std::string::npos) {
        return 0; // Media Foundation picks its own hardware: leave everything visible
    }
    uint32_t block = VK_BLOCK_NVIDIA | VK_BLOCK_AMD | VK_BLOCK_INTEL | VK_BLOCK_GPU_API;
    bool hardware = false;
    if (e.find("nvenc") != std::string::npos || e.find("cuda") != std::string::npos) {
        block &= ~VK_BLOCK_NVIDIA;
        hardware = true;
    }
    if (e.find("amf") != std::string::npos) {
        block &= ~VK_BLOCK_AMD;
        hardware = true;
    }
    if (e.find("qsv") != std::string::npos) {
        block &= ~VK_BLOCK_INTEL;
        hardware = true;
    }
    if (hardware) {
        block &= ~VK_BLOCK_GPU_API;
    }
    return block;
}

// For a second Voukoder file opened while the main one runs (the depth pass): hide only what NEITHER encoder needs,
// so a GPU main encoder (NVENC) keeps its GPU while a CPU depth encoder is opened next to it.
void HoldVoukoderOffGpuAlso(const char* encoder) {
    PrepareVoukoder();
    g_vkBlock = g_vkPatched.load() ? (g_vkBlock.load() & BlockMaskFor(encoder)) : 0;
}

// Set before each Voukoder open. It stays in force until the next open: Voukoder can still reach for the GPU after
// Open() returns, and while a CPU encoder runs it never needs it.
void HoldVoukoderOffGpu(const char* encoder) {
    PrepareVoukoder();
    g_vkBlockedLoads = 0;
    g_vkBlockedDevices = 0;
    g_vkBlock = g_vkPatched.load() ? BlockMaskFor(encoder) : 0;
}

} // namespace


namespace Encoder {

Session::Session() : videoFrameQueue(128), exrImageQueue(16) {
    PRE();
    LOG(LL_NFO, "Opening session: ", reinterpret_cast<uint64_t>(this));
    POST();
}

Session::~Session() {
    PRE();
    LOG(LL_NFO, "Closing session: ", reinterpret_cast<uint64_t>(this));
    this->isCapturing = false;
    LOG_CALL(LL_DBG, this->finishVideo());
    LOG_CALL(LL_DBG, this->finishAudio());
    LOG_CALL(LL_DBG, this->endSession());
    this->isBeingDeleted = true;
    POST();
}

HRESULT Session::createContext(const VKENCODERCONFIG& config, const std::wstring& in_filename, uint32_t in_width,
                               uint32_t in_height, const std::string& input_pixel_fmt, uint32_t fps_num,
                               uint32_t fps_den, uint32_t input_channels, uint32_t input_sample_rate,
                               const std::string& input_sample_format, uint32_t input_align, bool in_export_open_exr,
                               uint32_t in_open_exr_width, uint32_t in_open_exr_height) {
    PRE();

    this->width = static_cast<int32_t>(in_width);
    this->height = static_cast<int32_t>(in_height);
    open_exr_width = static_cast<int32_t>(in_open_exr_width);
    open_exr_height = static_cast<int32_t>(in_open_exr_height);

    ASSERT_RUNTIME(in_filename.length() < sizeof(VKENCODERINFO::filename) / sizeof(wchar_t), "File name too long.");
    ASSERT_RUNTIME(input_channels == 1 || input_channels == 2 || input_channels == 6,
                   "Invalid number of audio channels, only 1, 2 and 6 (5.1) are supported.");

    Microsoft::WRL::ComPtr<IVoukoder> m_pVoukoder = nullptr;

    // EVE1G: a CPU encoder (ProRes, x264...) must not make Voukoder start NVENC/CUDA/D3D on the GPU the game is
    // drawing with - that start-up is what crashed the NVIDIA driver at the start of exports
    HoldVoukoderOffGpu(config.video.encoder);

    REQUIRE(CoCreateInstance(CLSID_CoVoukoder, NULL, CLSCTX_INPROC_SERVER, IID_IVoukoder,
                             reinterpret_cast<void**>(m_pVoukoder.GetAddressOf())),
            "Failed to create an instance of IVoukoder interface.");

    REQUIRE(m_pVoukoder->SetConfig(config), "Failed to set Voukoder config");

    VKENCODERINFO vkInfo{
        .application = L"EVE1G",
        .video{
            .enabled = true,
            .width = static_cast<int>(in_width),
            .height = static_cast<int>(in_height),
            .timebase = {static_cast<int>(fps_den), static_cast<int>(fps_num)},
            .aspectratio{1, 1},
            .fieldorder = FieldOrder::Progressive, // TODO
        },
        .audio{
            .enabled = true,
            .samplerate = static_cast<int>(input_sample_rate),
            .channellayout =
                [input_channels] {
                    switch (input_channels) {
                    case 1:
                        return ChannelLayout::Mono;
                    case 2:
                        return ChannelLayout::Stereo;
                    default:
                        return ChannelLayout::FivePointOne;
                    }
                }(),
            .numberChannels = static_cast<int>(input_channels),
        },
    };
    in_filename.copy(vkInfo.filename, std::size(vkInfo.filename));
    this->exportExr = in_export_open_exr;
    if (this->exportExr) {
        this->exrOutputPath = utf8_encode(in_filename) + ".OpenEXR";
        std::filesystem::create_directories(this->exrOutputPath);
    }

    REQUIRE(m_pVoukoder->Open(vkInfo), "Failed to open Voukoder context.");
    LOG(LL_NFO, "Voukoder context opened.");
    if (g_vkBlock.load() != 0) {
        LOG(LL_NON, "EVE1G: ", config.video.encoder, " runs on the CPU - Voukoder kept off the GPU (",
            g_vkBlockedLoads.load(), " GPU libraries and ", g_vkBlockedDevices.load(), " GPU devices refused)");
    }

    videoFrame = {
        .buffer = new byte*[1], // We'll set this in writeVideoFrame() function
        .rowsize = new int[1],  // We'll set this in writeVideoFrame() function
        .planes = 1,
        .width = static_cast<int>(in_width),
        .height = static_cast<int>(in_height),
        .pass = 1,
    };
    input_pixel_fmt.copy(videoFrame.format, std::size(videoFrame.format));

    audioChunk = {
        .buffer = new byte*[1], // We'll set this in writeAudioFrame() function
        .samples = 0,           // We'll set this in writeAudioFrame() function
        .blockSize = static_cast<int>(input_align),
        .planes = 1,
        .sampleRate = static_cast<int>(input_sample_rate),
        .layout = vkInfo.audio.channellayout,
    };
    input_sample_format.copy(audioChunk.format, std::size(audioChunk.format));

    pVoukoder = std::move(m_pVoukoder);

    this->isCapturing = true;

    POST();
    return S_OK;
}

HRESULT Session::enqueueEXRImage(const Microsoft::WRL::ComPtr<ID3D11DeviceContext>& pDeviceContext,
                                 const Microsoft::WRL::ComPtr<ID3D11Texture2D>& cRGB,
                                 const Microsoft::WRL::ComPtr<ID3D11Texture2D>& cDepth) {
    PRE();

    if (this->isBeingDeleted) {
        POST();
        return E_FAIL;
    }

    D3D11_MAPPED_SUBRESOURCE mHDR = {nullptr};
    D3D11_MAPPED_SUBRESOURCE mDepth = {nullptr};
    D3D11_MAPPED_SUBRESOURCE mStencil = {nullptr};

    if (cRGB) {
        REQUIRE(pDeviceContext->Map(cRGB.Get(), 0, D3D11_MAP::D3D11_MAP_READ, 0, &mHDR), "Failed to map HDR texture");
    }

    if (cDepth) {
        REQUIRE(pDeviceContext->Map(cDepth.Get(), 0, D3D11_MAP::D3D11_MAP_READ, 0, &mDepth),
                "Failed to map depth texture");
    }

    struct RGBA {
        half R;
        half G;
        half B;
        half A;
    };

    struct Depth {
        float depth;
    };

    Imf::Header header(open_exr_width, open_exr_height);
    Imf::FrameBuffer framebuffer;

    if (mHDR.pData != nullptr) {
        LOG_CALL(LL_DBG, header.channels().insert("R", Imf::Channel(Imf::HALF)));
        LOG_CALL(LL_DBG, header.channels().insert("G", Imf::Channel(Imf::HALF)));
        LOG_CALL(LL_DBG, header.channels().insert("B", Imf::Channel(Imf::HALF)));
        LOG_CALL(LL_DBG, header.channels().insert("SubsurfaceScatter", Imf::Channel(Imf::HALF)));
        const auto mHDRArray = static_cast<RGBA*>(mHDR.pData);

        LOG_CALL(LL_DBG, framebuffer.insert("R", Imf::Slice(Imf::HALF, reinterpret_cast<char*>(&mHDRArray[0].R),
                                                            sizeof(RGBA), sizeof(RGBA) * open_exr_width)));

        LOG_CALL(LL_DBG, framebuffer.insert("G", Imf::Slice(Imf::HALF, reinterpret_cast<char*>(&mHDRArray[0].G),
                                                            sizeof(RGBA), sizeof(RGBA) * open_exr_width)));

        LOG_CALL(LL_DBG, framebuffer.insert("B", Imf::Slice(Imf::HALF, reinterpret_cast<char*>(&mHDRArray[0].B),
                                                            sizeof(RGBA), sizeof(RGBA) * open_exr_width)));

        LOG_CALL(LL_DBG,
                 framebuffer.insert("SubsurfaceScatter", Imf::Slice(Imf::HALF, reinterpret_cast<char*>(&mHDRArray[0].A),
                                                                    sizeof(RGBA), sizeof(RGBA) * open_exr_width)));
    }

    if (mDepth.pData != nullptr) {
        LOG_CALL(LL_DBG, header.channels().insert("depth.Z", Imf::Channel(Imf::FLOAT)));
        const auto mDSArray = static_cast<Depth*>(mDepth.pData);

        LOG_CALL(LL_DBG,
                 framebuffer.insert("depth.Z", Imf::Slice(Imf::FLOAT, reinterpret_cast<char*>(&mDSArray[0].depth),
                                                          sizeof(Depth), sizeof(Depth) * open_exr_width)));
    }

    std::stringstream sstream;
    sstream << std::setw(5) << std::setfill('0') << this->exrPTS++;
    Imf::OutputFile file((this->exrOutputPath + "\\frame." + sstream.str() + ".exr").c_str(), header);
    LOG_CALL(LL_DBG, file.setFrameBuffer(framebuffer));
    LOG_CALL(LL_DBG, file.writePixels(open_exr_height));
    //}

    if (cRGB) {
        LOG_CALL(LL_DBG, pDeviceContext->Unmap(cRGB.Get(), 0));
    }

    if (cDepth) {
        LOG_CALL(LL_DBG, pDeviceContext->Unmap(cDepth.Get(), 0));
    }

    POST();
    return S_OK;
}

HRESULT Session::enqueueVideoFrame(const D3D11_MAPPED_SUBRESOURCE& subresource) {
    PRE();

    if (this->isBeingDeleted) {
        POST();
        return E_FAIL;
    }

    LOG(LL_NFO, "Encoding frame: ", this->videoPTS);
    REQUIRE(this->writeVideoFrame(static_cast<BYTE*>(subresource.pData), static_cast<int32_t>(subresource.DepthPitch),
                                  subresource.RowPitch, this->videoPTS++),
            "Failed to write video frame.");

    POST();
    return S_OK;
}

HRESULT Session::writeVideoFrame(BYTE* pData, const int32_t length, const int rowPitch, const LONGLONG sampleTime) {
    PRE();
    if (this->isBeingDeleted) {
        POST();
        return E_FAIL;
    }

    if (!pVoukoder) {   // EVE1G: no video file open (yet) - nothing to write into
        POST();
        return S_OK;
    }
    videoFrame.buffer[0] = pData;
    videoFrame.rowsize[0] = rowPitch;

    REQUIRE(pVoukoder->SendVideoFrame(videoFrame), "Failed to send video frame to Voukoder.");

    POST();
    return S_OK;
}

HRESULT Session::writeAudioFrame(BYTE* pData, const int32_t length, LONGLONG sampleTime) {
    PRE();

    if (this->isBeingDeleted) {
        POST();
        return E_FAIL;
    }

    if (!pVoukoder) {   // EVE1G: no video file open (yet) - nothing to write into
        POST();
        return S_OK;
    }
    audioChunk.buffer[0] = pData;
    audioChunk.samples = length;

    REQUIRE(pVoukoder->SendAudioSampleChunk(audioChunk), "Failed to send audio chunk to Voukoder.");

    POST();
    return S_OK;
}

HRESULT Session::finishVideo() {
    PRE();
    std::lock_guard<std::mutex> guard(this->mxFinish);

    if (!this->isVideoFinished) {
        if (thread_exr_encoder.joinable()) {
            // Write end of the stream object with a nullptr
            this->exrImageQueue.enqueue(exr_queue_item());
            std::unique_lock<std::mutex> lock(this->mxEXREncodingThread);
            while (!this->isEXREncodingThreadFinished) {
                this->cvEXREncodingThreadFinished.wait(lock);
            }

            thread_exr_encoder.join();
        }

        delete videoFrame.buffer;
        videoFrame.buffer = nullptr;
        delete videoFrame.rowsize;
        videoFrame.rowsize = nullptr;

        this->isVideoFinished = true;
    }

    POST();
    return S_OK;
}

HRESULT Session::finishAudio() {
    PRE();
    std::lock_guard<std::mutex> guard(this->mxFinish);

    if (!this->isAudioFinished) {
        delete this->audioChunk.buffer;
        this->audioChunk.buffer = nullptr;
    }

    this->isAudioFinished = true;
    POST();
    return S_OK;
}

HRESULT Session::endSession() {
    PRE();
    std::lock_guard<std::mutex> lock(this->mxEndSession);

    if (this->isSessionFinished || this->isBeingDeleted) {
        POST();
        return S_OK;
    }

    while (true) {
        std::lock_guard<std::mutex> guard(this->mxFinish);
        if (this->isVideoFinished && this->isAudioFinished) {
            break;
        }
    }

    this->isCapturing = false;
    LOG(LL_NFO, "Ending session...");

    if (pVoukoder) {
        LOG_IF_FAILED(pVoukoder->Close(true), "Failed to close Voukoder context.");
    } else {
        // EVE1G: the export ended before the game started writing (no video file was ever opened).
        LOG(LL_WRN, "EVE1G: the export ended before any video was written - nothing to close.");
    }

    this->isSessionFinished = true;
    this->cvEndSession.notify_all();
    LOG(LL_NFO, "Done.");

    POST();
    return S_OK;
}
// ------------------------------------------------------------------------------------------------- depth video
DepthVideo::~DepthVideo() { close(); }

bool DepthVideo::open(const std::wstring& file, uint32_t width, uint32_t height, uint32_t fps_num, uint32_t fps_den,
                      const char* encoder, const char* options, const char* pixel_format, const wchar_t* application,
                      const char* container) {
    close();
    try {
        VKENCODERCONFIG config{};
        config.version = 1;
        strncpy_s(config.video.encoder, encoder, _TRUNCATE);
        strncpy_s(config.video.options, options, _TRUNCATE);
        strncpy_s(config.audio.encoder, "pcm_s16le", _TRUNCATE);
        strncpy_s(config.audio.options, "_sampleFormat=s16", _TRUNCATE);
        strncpy_s(config.format.container, container, _TRUNCATE);
        config.format.faststart = strcmp(container, "mp4") == 0;

        ASSERT_RUNTIME(file.length() < sizeof(VKENCODERINFO::filename) / sizeof(wchar_t), "File name too long.");
        HoldVoukoderOffGpuAlso(config.video.encoder);
        Microsoft::WRL::ComPtr<IVoukoder> vk;
        REQUIRE(CoCreateInstance(CLSID_CoVoukoder, NULL, CLSCTX_INPROC_SERVER, IID_IVoukoder,
                                 reinterpret_cast<void**>(vk.GetAddressOf())),
                "Failed to create a Voukoder instance for the depth pass.");
        REQUIRE(vk->SetConfig(config), "Failed to set the depth pass Voukoder config");
        VKENCODERINFO info{
            .video{
                .enabled = true,
                .width = static_cast<int>(width),
                .height = static_cast<int>(height),
                .timebase = {static_cast<int>(fps_den), static_cast<int>(fps_num)},
                .aspectratio{1, 1},
                .fieldorder = FieldOrder::Progressive,
            },
            .audio{
                .enabled = false,
                .samplerate = 48000,
                .channellayout = ChannelLayout::Stereo,
                .numberChannels = 2,
            },
        };
        file.copy(info.filename, std::size(info.filename));
        wcsncpy_s(info.application, application, _TRUNCATE);
        REQUIRE(vk->Open(info), "Voukoder could not open the side video (depth / HFR).");
        frame = {};
        frame.buffer = plane;
        frame.rowsize = rowsize;
        frame.planes = 1;
        frame.width = static_cast<int>(width);
        frame.height = static_cast<int>(height);
        frame.pass = 1;
        std::string(pixel_format).copy(frame.format, std::size(frame.format));
        pVoukoder = std::move(vk);
        return true;
    } catch (std::exception& ex) {
        LOG(LL_ERR, "EVE1G depth: ", ex.what());
        pVoukoder = nullptr;
        return false;
    }
}

bool DepthVideo::write(const void* pixels, int rowBytes) {
    if (!pVoukoder) {
        return false;
    }
    plane[0] = static_cast<byte*>(const_cast<void*>(pixels));
    rowsize[0] = rowBytes;
    return SUCCEEDED(pVoukoder->SendVideoFrame(frame));
}

void DepthVideo::close() {
    if (pVoukoder) {
        LOG_IF_FAILED(pVoukoder->Close(true), "Failed to close the depth pass video.");
        pVoukoder = nullptr;
    }
}
} // namespace Encoder