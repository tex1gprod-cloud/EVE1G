// script.cpp : Defines the exported functions for the DLL application.

#pragma comment(lib, "d3d11.lib")

#include "script.h"
#include "MFUtility.h"
#include "depth_pass.h"
#include "export_osd.h"
#include "hfr_pass.h"
#include "passes.h"
#include "encoder.h"
#include "hookdefs.h"
#include "logger.h"
#include "reshade.hpp"
#include "stdafx.h"
#include "util.h"
#include "yara-helper.h"
#include <d3d11_1.h>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mferror.h>

#include "yara-patterns.h"
#include <DirectXTex.h>
#include <cwctype>
#include <atomic>
#include <variant>

namespace PSAccumulate {
#include <PSAccumulate.h>
}

namespace PSDivide {
#include <PSDivide.h>
}

namespace VSFullScreen {
#include <VSFullScreen.h>
}

using namespace Microsoft::WRL;
using namespace DirectX;

namespace {
ComPtr<ID3D11Texture2D> pGameBackBuffer;
ComPtr<ID3D11Texture2D> pGameBackBufferResolved;
ComPtr<ID3D11Texture2D> pGameDepthBufferQuarterLinear;
ComPtr<ID3D11Texture2D> pGameDepthBufferQuarter;
ComPtr<ID3D11Texture2D> pGameDepthBufferResolved;
ComPtr<ID3D11Texture2D> pGameDepthBuffer;
ComPtr<ID3D11Texture2D> pGameGBuffer0;
ComPtr<ID3D11Texture2D> pGameEdgeCopy;
ComPtr<ID3D11Texture2D> pLinearDepthTexture;
ComPtr<ID3D11Texture2D> pStencilTexture;
void* pCtxLinearizeBuffer = nullptr;
bool isCustomFrameRateSupported = false;
std::unique_ptr<Encoder::Session> encodingSession;
// EVE1G: state of the game's own Media Foundation writer (only produces an empty placeholder file)
bool g_gameWriterFailed = false;
DWORD g_gameStreams = 0;
std::mutex mxSession;
std::mutex mxOnPresent;
std::thread::id mainThreadId;
ComPtr<IDXGISwapChain> mainSwapChain;
ComPtr<IDXGIFactory> pDxgiFactory;
ComPtr<ID3D11DeviceContext> pDContext;
ComPtr<ID3D11Texture2D> pMotionBlurAccBuffer;
ComPtr<ID3D11Texture2D> pMotionBlurFinalBuffer;
ComPtr<ID3D11VertexShader> pVsFullScreen;
ComPtr<ID3D11PixelShader> pPsAccumulate;
ComPtr<ID3D11PixelShader> pPsDivide;
ComPtr<ID3D11Buffer> pDivideConstantBuffer;
ComPtr<ID3D11Buffer> pAccParamsBuffer; // the camera exposure for PSAccumulate (see namespace exposure)
namespace exposure {
bool Begin(uint64_t frame_num); // defined with the accumulation below
}
ComPtr<ID3D11RasterizerState> pRasterState;
ComPtr<ID3D11SamplerState> pPsSampler;
ComPtr<ID3D11RenderTargetView> pRtvAccBuffer;
ComPtr<ID3D11RenderTargetView> pRtvBlurBuffer;
ComPtr<ID3D11ShaderResourceView> pSrvAccBuffer;
ComPtr<ID3D11Texture2D> pSourceSrvTexture;
ComPtr<ID3D11ShaderResourceView> pSourceSrv;
ComPtr<ID3D11BlendState> pAccBlendState;

struct ExportContext {
    ExportContext() {
        PRE();
        POST();
    }
    ~ExportContext() {
        PRE();
        POST();
    }

    ExportContext(const ExportContext& other) = delete;
    ExportContext& operator=(const ExportContext& other) = delete;
    ExportContext(ExportContext&& other) = delete;
    ExportContext& operator=(ExportContext&& other) = delete;

    bool is_audio_export_disabled = false;
    ComPtr<ID3D11Texture2D> p_export_render_target;
    ComPtr<ID3D11DeviceContext> p_device_context;
    ComPtr<ID3D11Device> p_device;
    ComPtr<IDXGISwapChain> p_swap_chain;
    ComPtr<IMFMediaType> video_media_type;
    int acc_count = 0;
    int total_frame_num = 0;
};

std::unique_ptr<ExportContext> exportContext;
bool g_master16 = false; // this export's final frames are RGBA 16-bit (EVE1G.ini master_16bit)
std::string g_lastExportFile; // the video being written (for the "export finished" notice)
std::unique_ptr<YaraHelper> pYaraHelper;

} // namespace

void eve::OnInitDevice(reshade::api::device* device) {
    static bool deferredContextPrepared = false;
    if (!deferredContextPrepared) {
        eve::PrepareDeferredContext(reinterpret_cast<ID3D11Device*>(device->get_native()));
        deferredContextPrepared = true;
    }
}

void eve::OnInitSwapChain(reshade::api::swapchain* swapchain) {
    if (!mainSwapChain) {
        std::lock_guard onPresentLock(mxOnPresent);
        mainSwapChain = reinterpret_cast<IDXGISwapChain*>(swapchain->get_native());
    }
}

void eve::OnPresent(reshade::api::command_queue* queue, reshade::api::swapchain* swapchain,
                    const reshade::api::rect* source_rect, const reshade::api::rect* dest_rect,
                    uint32_t dirty_rect_count, const reshade::api::rect* dirty_rects) {
    // if (!mainSwapChain) {
    //     std::lock_guard onPresentLock(mxOnPresent);
    //     mainSwapChain = reinterpret_cast<IDXGISwapChain*>(swapchain->get_native());
    // }
}

// HRESULT ExportHooks::D3D11CreateDeviceAndSwapChain::Implementation(
//     IDXGIAdapter* p_adapter, D3D_DRIVER_TYPE driver_type, HMODULE software, UINT flags,
//     const D3D_FEATURE_LEVEL* p_feature_levels, UINT feature_levels, UINT sdk_version,
//     const DXGI_SWAP_CHAIN_DESC* p_swap_chain_desc, IDXGISwapChain** pp_swap_chain, ID3D11Device** pp_device,
//     D3D_FEATURE_LEVEL* p_feature_level, ID3D11DeviceContext** pp_immediate_context) {
//     PRE();
//     const HRESULT result =
//         OriginalFunc(p_adapter, driver_type, software, flags, p_feature_levels, feature_levels, sdk_version,
//                      p_swap_chain_desc, pp_swap_chain, pp_device, p_feature_level, pp_immediate_context);
//     // if (SUCCEEDED(result) && pp_swap_chain && *pp_swap_chain) {
//     //     try {
//     //         // mainSwapChain = *pp_swap_chain;
//     //         PERFORM_MEMBER_HOOK_REQUIRED(IDXGISwapChain, Present, *pp_swap_chain);
//     //         PERFORM_MEMBER_HOOK_REQUIRED(ID3D11DeviceContext, Draw, *pp_immediate_context);
//     //         PERFORM_MEMBER_HOOK_REQUIRED(ID3D11DeviceContext, DrawIndexed, *pp_immediate_context);
//     //         PERFORM_MEMBER_HOOK_REQUIRED(ID3D11DeviceContext, OMSetRenderTargets, *pp_immediate_context);
//     //     } catch (...) {
//     //         LOG(LL_ERR, "Hooking IDXGISwapChain functions failed");
//     //     }
//     // }
//     POST();
//     return result;
// }

// HRESULT IDXGISwapChainHooks::Present::Implementation(IDXGISwapChain* pThis, UINT SyncInterval, UINT Flags) {
//
//     ID3D11Device* pDevice;
//     pThis->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&pDevice));
//
//     ComPtr<ID3D11DeviceContext> pDeviceContext;
//     pDevice->GetImmediateContext(pDeviceContext.GetAddressOf());
//
//     PERFORM_MEMBER_HOOK_REQUIRED(ID3D11DeviceContext, Draw, pDeviceContext.Get());
//     PERFORM_MEMBER_HOOK_REQUIRED(ID3D11DeviceContext, DrawIndexed, pDeviceContext.Get());
//     PERFORM_MEMBER_HOOK_REQUIRED(ID3D11DeviceContext, OMSetRenderTargets, pDeviceContext.Get());
//
//     return OriginalFunc(pThis, SyncInterval, Flags);
// }

// void PerformD3D11Hooks() {
//     WNDCLASSEX windowClass;
//     windowClass.cbSize = sizeof(WNDCLASSEX);
//     windowClass.style = CS_HREDRAW | CS_VREDRAW;
//     windowClass.lpfnWndProc = DefWindowProc;
//     windowClass.cbClsExtra = 0;
//     windowClass.cbWndExtra = 0;
//     windowClass.hInstance = GetModuleHandle(NULL);
//     windowClass.hIcon = NULL;
//     windowClass.hCursor = NULL;
//     windowClass.hbrBackground = NULL;
//     windowClass.lpszMenuName = NULL;
//     windowClass.lpszClassName = "EVE";
//     windowClass.hIconSm = NULL;
//
//     ::RegisterClassEx(&windowClass);
//
//     HWND window = ::CreateWindow(windowClass.lpszClassName, "EVE Hook Window", WS_OVERLAPPED, 0, 0, 100, 100, NULL,
//                                  NULL, windowClass.hInstance, NULL);
//
//     NOT_NULL(window, "Failed to create window");
//
//     // Create D3D11 Device and SwapChain
//     D3D_FEATURE_LEVEL featureLevel;
//     const D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_11_0};
//
//     DXGI_RATIONAL refreshRate;
//     refreshRate.Numerator = 60;
//     refreshRate.Denominator = 1;
//
//     DXGI_MODE_DESC bufferDesc;
//     bufferDesc.Width = 100;
//     bufferDesc.Height = 100;
//     bufferDesc.RefreshRate = refreshRate;
//     bufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
//     bufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
//     bufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
//
//     DXGI_SAMPLE_DESC sampleDesc;
//     sampleDesc.Count = 1;
//     sampleDesc.Quality = 0;
//
//     DXGI_SWAP_CHAIN_DESC swapChainDesc;
//     swapChainDesc.BufferDesc = bufferDesc;
//     swapChainDesc.SampleDesc = sampleDesc;
//     swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
//     swapChainDesc.BufferCount = 1;
//     swapChainDesc.OutputWindow = window;
//     swapChainDesc.Windowed = 1;
//     swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
//     swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
//
//     ComPtr<IDXGISwapChain> pSwapChain;
//     ComPtr<ID3D11Device> pDevice;
//     ComPtr<ID3D11DeviceContext> pDeviceContext;
//
//     REQUIRE(D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, featureLevels,
//                                           ARRAYSIZE(featureLevels), D3D11_SDK_VERSION, &swapChainDesc,
//                                           pSwapChain.GetAddressOf(), pDevice.GetAddressOf(), &featureLevel,
//                                           pDeviceContext.GetAddressOf()),
//             "Failed to create D3D11 Device and SwapChain");
//
//     LOG(LL_NFO, "D3D11 Device and SwapChain created");
//
//     if (pSwapChain && pDeviceContext) {
//         try {
//             PERFORM_MEMBER_HOOK_REQUIRED(IDXGISwapChain, Present, pSwapChain.Get());
//         } catch (...) {
//             LOG(LL_ERR, "Hooking IDXGISwapChain functions failed");
//         }
//     }
//
//     pSwapChain = nullptr;
//     pDeviceContext = nullptr;
//     pDevice = nullptr;
//
//     DestroyWindow(window);
// }

// void eve::PresentCallback(void* p_swap_chain) {}

void eve::initialize() {
    PRE();

    try {
        mainThreadId = std::this_thread::get_id();

        LOG(LL_NFO, "Initializing Media Foundation hook");
        PERFORM_NAMED_IMPORT_HOOK_REQUIRED("mfreadwrite.dll", MFCreateSinkWriterFromURL);

        pYaraHelper.reset(new YaraHelper());
        pYaraHelper->Initialize();

        MODULEINFO info;
        GetModuleInformation(GetCurrentProcess(), GetModuleHandle(nullptr), &info, sizeof(info));
        LOG(LL_NFO, "Image base:", ((void*)info.lpBaseOfDll));

        uint64_t pGetRenderTimeBase = NULL;
        uint64_t pCreateTexture = NULL;
        uint64_t pCreateThread = NULL;
        uint64_t pScaleformDrawMovie = NULL;
        pYaraHelper->AddEntry("get_render_time_base_function", yara_get_render_time_base_function, &pGetRenderTimeBase);
        pYaraHelper->AddEntry("create_thread_function", yara_create_thread_function, &pCreateThread);
        pYaraHelper->AddEntry("create_texture_function", yara_create_texture_function, &pCreateTexture);
        pYaraHelper->AddEntry("scaleform_draw_movie_function", yara_scaleform_draw_movie_function,
                              &pScaleformDrawMovie);
        pYaraHelper->PerformScan();

        try {
            if (pGetRenderTimeBase) {
                PERFORM_X64_HOOK_REQUIRED(GetRenderTimeBase, pGetRenderTimeBase);
                isCustomFrameRateSupported = true;
            } else {
                LOG(LL_ERR, "Could not find the address for FPS function.");
                LOG(LL_ERR, "Custom FPS support is DISABLED!!!");
            }

            if (pCreateTexture) {
                PERFORM_X64_HOOK_REQUIRED(CreateTexture, pCreateTexture);
            } else {
                LOG(LL_ERR, "Could not find the address for CreateTexture function.");
            }

            if (pScaleformDrawMovie) {
                PERFORM_X64_HOOK_REQUIRED(ScaleformDrawMovie, pScaleformDrawMovie);
                LOG(LL_NON, "EVE1G: game UI (export progress bar) will be hidden while exporting");
            } else {
                LOG(LL_NON, "EVE1G: Scaleform draw not found on this game build - the export progress bar "
                            "may appear in captures made with capture_source = present");
            }

            if (pCreateThread) {
                PERFORM_X64_HOOK_REQUIRED(CreateThread, pCreateThread);
            } else {
                LOG(LL_ERR, "Could not find the address for CreateThread function.");
            }
        } catch (std::exception& ex) {
            LOG(LL_ERR, ex.what());
        }
    } catch (std::exception& ex) {
        // TODO cleanup
        POST();
        throw ex;
    }
    POST();
}


void eve::OnBindRenderTargets(reshade::api::command_list* cmd_list, uint32_t count,
                              const reshade::api::resource_view* render_targets,
                              reshade::api::resource_view depth_stencil) {

    // PRE();
    if (::exportContext) {
        for (uint32_t i = 0; i < count; i++) {
            if (render_targets[i].handle) {
                ComPtr<ID3D11Resource> pResource;
                ComPtr<ID3D11Texture2D> pTexture2D;
                reinterpret_cast<ID3D11RenderTargetView*>(render_targets[i].handle)
                    ->GetResource(pResource.GetAddressOf());
                if (SUCCEEDED(pResource.As(&pTexture2D))) {
                    if (pTexture2D.Get() == pGameDepthBufferQuarterLinear.Get()) {
                        LOG(LL_DBG, " i:", i, " num:", count, " dsv:", depth_stencil.handle);
                        pCtxLinearizeBuffer = reinterpret_cast<ID3D11DeviceContext*>(cmd_list->get_native());
                    }
                }
            }
        }
    }

    ComPtr<ID3D11Resource> pRTVTexture;
    if (render_targets && (render_targets[0].handle)) {
        reinterpret_cast<ID3D11RenderTargetView*>(render_targets[0].handle)->GetResource(pRTVTexture.GetAddressOf());
    }

    if (::exportContext != nullptr && ::exportContext->p_export_render_target != nullptr &&
        (::exportContext->p_export_render_target == pRTVTexture)) {
        // LOG(LL_DBG, "Exporting frame");
        //  Time to capture rendered frame
        try {
            ComPtr<ID3D11Device> pDevice;
            reinterpret_cast<ID3D11DeviceContext*>(cmd_list->get_native())->GetDevice(pDevice.GetAddressOf());

            ComPtr<ID3D11Texture2D> pDepthBufferCopy = nullptr;
            ComPtr<ID3D11Texture2D> pBackBufferCopy = nullptr;

            if (config::export_openexr) {
                TRY([&] {
                    {
                        D3D11_TEXTURE2D_DESC desc;
                        pLinearDepthTexture->GetDesc(&desc);

                        desc.CPUAccessFlags = D3D11_CPU_ACCESS_FLAG::D3D11_CPU_ACCESS_READ;
                        desc.BindFlags = 0;
                        desc.MiscFlags = 0;
                        desc.Usage = D3D11_USAGE::D3D11_USAGE_STAGING;

                        REQUIRE(pDevice->CreateTexture2D(&desc, NULL, pDepthBufferCopy.GetAddressOf()),
                                "Failed to create depth buffer copy texture");

                        reinterpret_cast<ID3D11DeviceContext*>(cmd_list->get_native())
                            ->CopyResource(pDepthBufferCopy.Get(), pLinearDepthTexture.Get());
                    }
                    {
                        D3D11_TEXTURE2D_DESC desc;
                        pGameBackBufferResolved->GetDesc(&desc);
                        desc.CPUAccessFlags = D3D11_CPU_ACCESS_FLAG::D3D11_CPU_ACCESS_READ;
                        desc.BindFlags = 0;
                        desc.MiscFlags = 0;
                        desc.Usage = D3D11_USAGE::D3D11_USAGE_STAGING;

                        REQUIRE(pDevice->CreateTexture2D(&desc, NULL, pBackBufferCopy.GetAddressOf()),
                                "Failed to create back buffer copy texture");

                        reinterpret_cast<ID3D11DeviceContext*>(cmd_list->get_native())
                            ->CopyResource(pBackBufferCopy.Get(), pGameBackBufferResolved.Get());
                    }
                    {
                        std::lock_guard<std::mutex> sessionLock(mxSession);
                        if ((encodingSession != nullptr) && (encodingSession->isCapturing)) {
                            encodingSession->enqueueEXRImage(
                                reinterpret_cast<ID3D11DeviceContext*>(cmd_list->get_native()), pBackBufferCopy,
                                pDepthBufferCopy);
                        }
                    }
                });
            }

            if (config::capture_source == config::CaptureSource::Clean) {
                // EVE1G "clean": what original EVE effectively did. Rockstar's exporter grabs the frame here,
                // before the game draws its UI (export progress bar, buttons, cursor). Take the back buffer now,
                // run the ReShade preset on that copy, and encode it. Nothing is presented, so the flip-model
                // swap chain of the GTA V Upscaler is never disturbed.
                eve::CaptureClean(pDevice.Get(), reinterpret_cast<ID3D11DeviceContext*>(cmd_list->get_native()),
                                  cmd_list);
                return;
            }

            if (config::capture_source != config::CaptureSource::Legacy) {
                // EVE1G: never force a Present() here. On a flip-model swap chain (which the GTA V
                // Upscaler / DLSS 5 installs) the back buffer is discarded by Present, so reading it
                // afterwards returned the next, empty buffer - the black video. Instead the frame is
                // marked, and read once it is finished: in reshade_present (after the upscaler, DLSS 5
                // neural rendering and ReShade effects) or straight from the game's VideoEncode target.
                eve::QueueExportFrame(pDevice, reinterpret_cast<ID3D11DeviceContext*>(cmd_list->get_native()));
                return;
            }

            // ---- legacy capture (original EVE behaviour), kept for A/B testing: capture_source = legacy
            ComPtr<ID3D11Texture2D> pSwapChainBuffer;
            REQUIRE(::exportContext->p_swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                                             reinterpret_cast<void**>(pSwapChainBuffer.GetAddressOf())),
                    "Failed to get swap chain's buffer");

            LOG_CALL(LL_DBG,
                     ::exportContext->p_swap_chain->Present(1, 0)); // IMPORTANT: This call makes ENB and ReShade
                                                                    // effects to be applied to the render target

            LOG_CALL(LL_DBG,
                     ::exportContext->p_swap_chain->Present(1, 0)); // IMPORTANT: This call makes ENB and ReShade
                                                                    // effects to be applied to the render target

            if (config::motion_blur_samples != 0) {
                if (exposure::Begin(::exportContext->total_frame_num)) {
                    eve::drawAdditive(pDevice, reinterpret_cast<ID3D11DeviceContext*>(cmd_list->get_native()),
                                      pSwapChainBuffer);
                }
            } else {
                // Trick to use the same buffers for when not using motion blur
                exposure::Begin(0);
                ::exportContext->acc_count = 0;
                eve::drawAdditive(pDevice, reinterpret_cast<ID3D11DeviceContext*>(cmd_list->get_native()),
                                  pSwapChainBuffer);
                ::exportContext->acc_count = 1;
                ::exportContext->total_frame_num = 1;
            }

            if ((::exportContext->total_frame_num % (::config::motion_blur_samples + 1)) ==
                config::motion_blur_samples) {

                const ComPtr<ID3D11Texture2D> result =
                    eve::divideBuffer(pDevice, reinterpret_cast<ID3D11DeviceContext*>(cmd_list->get_native()),
                                      ::exportContext->acc_count);
                ::exportContext->acc_count = 0;

                // Save image as dds file
                // ScratchImage image;
                // if (SUCCEEDED(CaptureTexture(pDevice.Get(),
                // reinterpret_cast<ID3D11DeviceContext*>(cmd_list->get_native()), pSwapChainBuffer.Get(), image))) {
                //    const std::wstring path = L"C:\\ExtendedVideoExport\\test.dds";
                //    if (FAILED(SaveToDDSFile(image.GetImages(), image.GetImageCount(), image.GetMetadata(),
                //                             DDS_FLAGS_NONE, path.c_str()))) {
                //        LOG(LL_ERR, "Failed to save image to PNG file.");
                //    }
                //}

                D3D11_MAPPED_SUBRESOURCE mapped;

                REQUIRE(reinterpret_cast<ID3D11DeviceContext*>(cmd_list->get_native())
                            ->Map(result.Get(), 0, D3D11_MAP_READ, 0, &mapped),
                        "Failed to capture swapbuffer.");
                {
                    std::lock_guard sessionLock(mxSession);
                    if ((encodingSession != nullptr) && (encodingSession->isCapturing)) {
                        REQUIRE(encodingSession->enqueueVideoFrame(mapped), "Failed to enqueue frame.");
                    }
                }
                reinterpret_cast<ID3D11DeviceContext*>(cmd_list->get_native())->Unmap(result.Get(), 0);
            }
            ::exportContext->total_frame_num++;
        } catch (std::exception&) {
            LOG(LL_ERR, "Reading video frame from D3D Device failed.");
            LOG_CALL(LL_DBG, encodingSession.reset());
            LOG_CALL(LL_DBG, ::exportContext.reset());
        }
    }
    // POST();
}

// =====================================================================================================================
//  EVE1G capture path
// ---------------------------------------------------------------------------------------------------------------------
//  The game binds its "VideoEncode" target once per exported game frame (EVE's time-step hook makes every game frame a
//  motion-blur sample). Original EVE answered that by calling Present() twice and reading swap-chain buffer 0. That
//  only works on a blt-model swap chain. GTA V Upscaler (DLSS / DLSS 5 neural rendering) swaps in a flip-model chain
//  (SwapEffect FLIP_DISCARD), where Present discards the back buffer - so EVE read an empty buffer: black video.
//
//  Here the bind only marks the frame. The pixels are read when the frame is finished:
//    capture_source = present : ReShade's reshade_present event, from the current back buffer. This is the image on
//                               screen after the upscaler, DLSS 5 and ReShade - the same place Rockstar Editor+ reads.
//    capture_source = encode  : the game's own VideoEncode texture, at the same moment. Rockstar's own export image,
//                               before any HUD - but anything applied at Present (ReShade, possibly DLSS 5) is missing.
//    capture_source = legacy  : original EVE code above, for comparison.
// =====================================================================================================================
namespace {
std::atomic<reshade::api::effect_runtime*> g_runtime{nullptr};
std::atomic<bool> g_capturePending{false};
std::atomic<uint32_t> g_backBufferWidth{0};
std::atomic<uint32_t> g_backBufferHeight{0};
uint64_t g_framesQueued = 0;
uint64_t g_framesCaptured = 0;
uint64_t g_framesWithoutPresent = 0;
bool g_toldSizeMismatch = false;
bool g_toldNoBuffers = false;
bool g_toldSource = false;

// Our copy of the source, with a view the accumulation shader can Load() from. Recreated when the source changes.
ComPtr<ID3D11Texture2D> pCapSrcTexture;
ComPtr<ID3D11ShaderResourceView> pCapSrcSrv;
D3D11_TEXTURE2D_DESC capSrcDesc{};

DXGI_FORMAT TypelessOf(const DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8X8_TYPELESS;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return DXGI_FORMAT_R10G10B10A2_TYPELESS;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
        return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return DXGI_FORMAT_R32G32B32A32_TYPELESS;
    default:
        return f;
    }
}

// The view we sample through. Deliberately NOT sRGB: an 8-bit video wants the stored (gamma-encoded) values as they
// are, which is what the screen shows.
DXGI_FORMAT ViewFormatOf(const DXGI_FORMAT f) {
    switch (TypelessOf(f)) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return f == DXGI_FORMAT_R16G16B16A16_UNORM ? DXGI_FORMAT_R16G16B16A16_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default:
        return f;
    }
}

void EnsureCaptureSource(ID3D11Device* pDevice, const D3D11_TEXTURE2D_DESC& src) {
    if (pCapSrcTexture && capSrcDesc.Width == src.Width && capSrcDesc.Height == src.Height &&
        capSrcDesc.Format == src.Format && capSrcDesc.SampleDesc.Count == src.SampleDesc.Count) {
        return;
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = src.Width;
    desc.Height = src.Height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = TypelessOf(src.Format);
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    REQUIRE(pDevice->CreateTexture2D(&desc, nullptr, pCapSrcTexture.ReleaseAndGetAddressOf()),
            "Failed to create the capture source texture");

    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    srv.Texture2D.MostDetailedMip = 0;
    srv.Format = ViewFormatOf(src.Format);
    REQUIRE(pDevice->CreateShaderResourceView(pCapSrcTexture.Get(), &srv, pCapSrcSrv.ReleaseAndGetAddressOf()),
            "Failed to create the capture source view");

    capSrcDesc = src;
    LOG(LL_NON, "EVE1G: capture source is ", src.Width, "x", src.Height, " ",
        conv_dxgi_format_to_string(src.Format), " (", src.SampleDesc.Count, " sample(s))");
}

// ---- the camera exposure ---------------------------------------------------------------------------------------------
// Every video frame is (samples + 1) game frames, evenly spread over the frame interval. Like a camera sensor, the
// shutter is open for the last (angle / 360) of the interval and each game frame adds light while it is open:
// in linear light (bright lights streak like on a real sensor), weighted by how open the shutter is (soft edges like a
// rotary shutter's blade) and, with a rolling shutter, a little later for every row further down the image.
namespace exposure {
float g_s = 1.0f;       // the sample being added: its time in the frame interval (0..1)
bool g_linear = false;  // the current frame is collected in linear light (PSDivide encodes it back)

// Readout time of a rolling shutter as a share of the frame interval - it has to fit next to the exposure.
float Readout() {
    if (config::motion_blur_samples == 0 || config::rolling_shutter_ms <= 0.0f || config::fps.first == 0) {
        return 0.0f;
    }
    const float period_ms = 1000.0f * static_cast<float>(config::fps.second) / static_cast<float>(config::fps.first);
    const float r = config::rolling_shutter_ms / period_ms;
    return std::clamp(r, 0.0f, (std::max)(0.0f, 1.0f - config::motion_blur_strength));
}

// For the game frame with this running number: sets its time, and says whether any row's shutter is open then.
bool Begin(uint64_t frame_num) {
    if (config::motion_blur_samples == 0) {
        g_s = 1.0f;
        return true;
    }
    g_s = static_cast<float>(frame_num % (config::motion_blur_samples + 1)) /
          static_cast<float>(config::motion_blur_samples);
    return g_s >= 1.0f - config::motion_blur_strength - Readout() - 1e-4f;
}

bool IsFloat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return true;
    default:
        return false;
    }
}

// Binds the exposure of the current sample for PSAccumulate (on the deferred context, after ClearState).
void Bind(UINT height, DXGI_FORMAT source) {
    if (!pAccParamsBuffer) {
        return;
    }
    // a float source (HDR) is already linear light; 8 / 10-bit images are display (sRGB) encoded
    g_linear = config::blend_linear && !IsFloat(source);
    const bool blur = config::motion_blur_samples != 0;
    const float angle = blur ? std::clamp(config::motion_blur_strength, 0.001f, 1.0f) : 1.0f;
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(pDContext->Map(pAccParamsBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return;
    }
    float* p = static_cast<float*>(mapped.pData);
    p[0] = g_s;
    p[1] = angle;
    p[2] = Readout();
    p[3] = blur ? config::shutter_soft : 0.0f;
    p[4] = g_linear ? 1.0f : 0.0f;
    p[5] = static_cast<float>(height);
    p[6] = blur ? 0.5f / (static_cast<float>(config::motion_blur_samples) * angle) : 0.0f; // half a sample step
    p[7] = 0.0f;
    pDContext->Unmap(pAccParamsBuffer.Get(), 0);
    pDContext->PSSetConstantBuffers(0, 1, pAccParamsBuffer.GetAddressOf());
}
} // namespace exposure

// Same draw as eve::drawAdditive, but from a copy that matches whatever the source is (back buffer or VideoEncode,
// any format, MSAA resolved), instead of assuming a BGRA8 texture of the swap chain's size.
bool DrawAdditiveFrom(ID3D11Device* pDevice, ID3D11DeviceContext* pContext, ID3D11Texture2D* pSource) {
    if (!pRtvAccBuffer || !pMotionBlurFinalBuffer) {
        if (!g_toldNoBuffers) {
            g_toldNoBuffers = true;
            LOG(LL_ERR, "EVE1G: a frame arrived before the encoder was set up - skipped");
        }
        return false;
    }

    D3D11_TEXTURE2D_DESC src;
    pSource->GetDesc(&src);
    D3D11_TEXTURE2D_DESC acc;
    pMotionBlurFinalBuffer->GetDesc(&acc);
    if (src.Width != acc.Width || src.Height != acc.Height) {
        if (!g_toldSizeMismatch) {
            g_toldSizeMismatch = true;
            LOG(LL_ERR, "EVE1G: the captured image is ", src.Width, "x", src.Height, " but the video is ",
                acc.Width, "x", acc.Height,
                ". Do not resize the game window during an export. Frames are skipped until they match.");
        }
        return false;
    }

    EnsureCaptureSource(pDevice, src);

    LOG_CALL(LL_DBG, pDContext->ClearState());
    if (src.SampleDesc.Count > 1) {
        pDContext->ResolveSubresource(pCapSrcTexture.Get(), 0, pSource, 0, ViewFormatOf(src.Format));
    } else {
        pDContext->CopyResource(pCapSrcTexture.Get(), pSource);
    }

    pDContext->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
    pDContext->IASetInputLayout(nullptr);
    pDContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    pDContext->VSSetShader(pVsFullScreen.Get(), nullptr, 0);
    pDContext->PSSetSamplers(0, 1, pPsSampler.GetAddressOf());
    pDContext->PSSetShader(pPsAccumulate.Get(), nullptr, 0);
    exposure::Bind(src.Height, ViewFormatOf(src.Format));
    pDContext->RSSetState(pRasterState.Get());
    const float factors[4] = {0, 0, 0, 0};
    pDContext->OMSetBlendState(pAccBlendState.Get(), factors, 0xFFFFFFFF);

    D3D11_VIEWPORT viewport;
    viewport.TopLeftX = 0;
    viewport.TopLeftY = 0;
    viewport.Width = static_cast<float>(src.Width);
    viewport.Height = static_cast<float>(src.Height);
    viewport.MinDepth = 0;
    viewport.MaxDepth = 1;
    pDContext->RSSetViewports(1, &viewport);

    pDContext->PSSetShaderResources(0, 1, pCapSrcSrv.GetAddressOf());
    pDContext->OMSetRenderTargets(1, pRtvAccBuffer.GetAddressOf(), nullptr);
    if (::exportContext->acc_count == 0) {
        const float color[4] = {0, 0, 0, 0}; // alpha collects the exposure weights
        pDContext->ClearRenderTargetView(pRtvAccBuffer.Get(), color);
    }
    pDContext->Draw(4, 0);

    ComPtr<ID3D11CommandList> pCmdList;
    REQUIRE(pDContext->FinishCommandList(FALSE, pCmdList.GetAddressOf()), "Failed to record the accumulation pass");
    pContext->ExecuteCommandList(pCmdList.Get(), TRUE);

    ::exportContext->acc_count++;
    return true;
}

// One exported game frame = one motion-blur sample. Identical bookkeeping to the legacy path.
void AccumulateAndEmit(ID3D11Device* pDevice, ID3D11DeviceContext* pContext, ID3D11Texture2D* pSource) {
    try {
        {
            // depth pass: grab ReShade's depth on the sample in the middle of the shutter
            const int perFrame = config::motion_blur_samples + 1;
            const int sample = config::motion_blur_samples != 0
                                   ? static_cast<int>(::exportContext->total_frame_num % perFrame)
                                   : 0;
            depth::OnSample(g_runtime.load(), pContext, sample, perFrame);
            passes::OnSample(pContext, pGameDepthBuffer.Get(), sample, perFrame); // ID pass (stencil)
        }
        hfr::OnGameFrame(pDevice, pContext, pSource); // every game frame, unblended (HFR export)
        if (config::motion_blur_samples != 0) {
            if (exposure::Begin(::exportContext->total_frame_num)) { // the shutter is open (for some rows) now
                DrawAdditiveFrom(pDevice, pContext, pSource);
            }
        } else {
            exposure::Begin(0);
            ::exportContext->acc_count = 0;
            DrawAdditiveFrom(pDevice, pContext, pSource);
            ::exportContext->acc_count = 1;
            ::exportContext->total_frame_num = 1;
        }

        if ((::exportContext->total_frame_num % (::config::motion_blur_samples + 1)) == config::motion_blur_samples) {
            // acc_count can be 0 if every sample of this frame was skipped; never divide by zero.
            const uint32_t k = ::exportContext->acc_count > 0 ? ::exportContext->acc_count : 1;
            const ComPtr<ID3D11Texture2D> result = eve::divideBuffer(pDevice, pContext, k);
            ::exportContext->acc_count = 0;

            D3D11_MAPPED_SUBRESOURCE mapped;
            REQUIRE(pContext->Map(result.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Failed to read the frame back.");
            {
                std::lock_guard sessionLock(mxSession);
                if ((encodingSession != nullptr) && (encodingSession->isCapturing)) {
                    REQUIRE(encodingSession->enqueueVideoFrame(mapped), "Failed to enqueue frame.");
                }
            }
            passes::OnMasterFrame(mapped.pData, mapped.RowPitch); // proxy
            pContext->Unmap(result.Get(), 0);
            depth::OnFrameEmitted(pContext);  // the matching depth / normals frame
            passes::OnFrameEmitted(pContext); // the matching ID frame
        }
        ::exportContext->total_frame_num++;
        ++g_framesCaptured;
    } catch (std::exception& ex) {
        LOG(LL_ERR, "EVE1G: reading the video frame failed: ", ex.what());
        LOG_CALL(LL_DBG, encodingSession.reset());
        LOG_CALL(LL_DBG, ::exportContext.reset());
        g_capturePending = false;
    }
}

ComPtr<ID3D11Texture2D> AsTexture2D(ID3D11Resource* pResource) {
    ComPtr<ID3D11Texture2D> tex;
    if (pResource) {
        pResource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(tex.GetAddressOf()));
    }
    return tex;
}

// ---- clean plate ([PASSES] clean_plate) -------------------------------------------------------------------------------
//  With capture = clean the export frame is copied before ReShade renders on it. That copy is also blended into a second
//  accumulator with exactly the same camera exposure as the video, so <video>_plate.mov is the same shot, same motion
//  blur, without the ReShade preset.
namespace plate {
ComPtr<ID3D11Texture2D> acc, fin, staging;
ComPtr<ID3D11RenderTargetView> rtvAcc, rtvFin;
ComPtr<ID3D11ShaderResourceView> srvAcc, srvSrc;
ID3D11Texture2D* srcFor = nullptr; // the texture srvSrc views
DXGI_FORMAT srcFmt = DXGI_FORMAT_UNKNOWN;
int count = 0;
std::atomic<bool> on{false};
uint32_t W = 0, H = 0;

void Release() {
    acc = fin = staging = nullptr;
    rtvAcc = rtvFin = nullptr;
    srvAcc = srvSrc = nullptr;
    srcFor = nullptr;
    count = 0;
    on = false;
}

void Create(ID3D11Device* dev, uint32_t w, uint32_t h, bool b16) {
    Release();
    if (!dev || !w || !h) {
        return;
    }
    const DXGI_FORMAT outFmt = b16 ? DXGI_FORMAT_R16G16B16A16_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, acc.GetAddressOf())) ||
        FAILED(dev->CreateRenderTargetView(acc.Get(), nullptr, rtvAcc.GetAddressOf())) ||
        FAILED(dev->CreateShaderResourceView(acc.Get(), nullptr, srvAcc.GetAddressOf()))) {
        LOG(LL_ERR, "EVE1G clean plate: could not create its accumulation buffer");
        Release();
        return;
    }
    d.Format = outFmt;
    d.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, fin.GetAddressOf())) ||
        FAILED(dev->CreateRenderTargetView(fin.Get(), nullptr, rtvFin.GetAddressOf()))) {
        LOG(LL_ERR, "EVE1G clean plate: could not create its output buffer");
        Release();
        return;
    }
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, staging.GetAddressOf()))) {
        LOG(LL_ERR, "EVE1G clean plate: could not create its read-back buffer");
        Release();
        return;
    }
    W = w;
    H = h;
    on = true;
}

void Draw(ID3D11DeviceContext* pContext, ID3D11Texture2D* src, DXGI_FORMAT viewFmt) {
    pDContext->ClearState();
    pDContext->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
    pDContext->IASetInputLayout(nullptr);
    pDContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    pDContext->VSSetShader(pVsFullScreen.Get(), nullptr, 0);
    pDContext->PSSetSamplers(0, 1, pPsSampler.GetAddressOf());
    pDContext->PSSetShader(pPsAccumulate.Get(), nullptr, 0);
    exposure::Bind(H, viewFmt);
    pDContext->RSSetState(pRasterState.Get());
    const float factors[4] = {0, 0, 0, 0};
    pDContext->OMSetBlendState(pAccBlendState.Get(), factors, 0xFFFFFFFF);
    D3D11_VIEWPORT vp{0, 0, static_cast<float>(W), static_cast<float>(H), 0, 1};
    pDContext->RSSetViewports(1, &vp);
    pDContext->PSSetShaderResources(0, 1, srvSrc.GetAddressOf());
    pDContext->OMSetRenderTargets(1, rtvAcc.GetAddressOf(), nullptr);
    if (count == 0) {
        const float clear[4] = {0, 0, 0, 0};
        pDContext->ClearRenderTargetView(rtvAcc.Get(), clear);
    }
    pDContext->Draw(4, 0);
    ComPtr<ID3D11CommandList> list;
    if (SUCCEEDED(pDContext->FinishCommandList(FALSE, list.GetAddressOf()))) {
        pContext->ExecuteCommandList(list.Get(), TRUE);
        ++count;
    }
}

void Resolve(ID3D11DeviceContext* pContext) {
    const uint32_t k = count > 0 ? static_cast<uint32_t>(count) : 1u;
    count = 0;
    pDContext->ClearState();
    pDContext->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
    pDContext->IASetInputLayout(nullptr);
    pDContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    pDContext->VSSetShader(pVsFullScreen.Get(), nullptr, 0);
    pDContext->PSSetShader(pPsDivide.Get(), nullptr, 0);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(pDContext->Map(pDivideConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        return;
    }
    static_cast<float*>(m.pData)[0] = static_cast<float>(k);
    static_cast<float*>(m.pData)[1] = exposure::g_linear ? 1.0f : 0.0f;
    pDContext->Unmap(pDivideConstantBuffer.Get(), 0);
    pDContext->PSSetConstantBuffers(0, 1, pDivideConstantBuffer.GetAddressOf());
    pDContext->PSSetShaderResources(0, 1, srvAcc.GetAddressOf());
    pDContext->RSSetState(pRasterState.Get());
    D3D11_VIEWPORT vp{0, 0, static_cast<float>(W), static_cast<float>(H), 0, 1};
    pDContext->RSSetViewports(1, &vp);
    pDContext->OMSetRenderTargets(1, rtvFin.GetAddressOf(), nullptr);
    pDContext->Draw(4, 0);
    ComPtr<ID3D11CommandList> list;
    if (FAILED(pDContext->FinishCommandList(FALSE, list.GetAddressOf()))) {
        return;
    }
    pContext->ExecuteCommandList(list.Get(), TRUE);
    pContext->CopyResource(staging.Get(), fin.Get());
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(pContext->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        passes::OnPlateFrame(mapped.pData, mapped.RowPitch);
        pContext->Unmap(staging.Get(), 0);
    }
}

// The pre-ReShade copy of export frame number frame_num (a typeless texture with a shader-resource binding).
void Add(ID3D11Device* dev, ID3D11DeviceContext* pContext, ID3D11Texture2D* src, uint64_t frame_num) {
    if (!on || !src || !pDContext) {
        return;
    }
    D3D11_TEXTURE2D_DESC sd;
    src->GetDesc(&sd);
    if (sd.Width != W || sd.Height != H) {
        return;
    }
    if (srcFor != src || srcFmt != sd.Format) {
        D3D11_SHADER_RESOURCE_VIEW_DESC v{};
        v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        v.Texture2D.MipLevels = 1;
        v.Format = ViewFormatOf(sd.Format);
        if (FAILED(dev->CreateShaderResourceView(src, &v, srvSrc.ReleaseAndGetAddressOf()))) {
            LOG(LL_ERR, "EVE1G clean plate: cannot read the captured frame - plate off for this export");
            on = false;
            return;
        }
        srcFor = src;
        srcFmt = sd.Format;
    }
    const uint32_t S = config::motion_blur_samples;
    if (S == 0) {
        exposure::Begin(0);
        count = 0;
        Draw(pContext, src, ViewFormatOf(sd.Format));
        Resolve(pContext);
        return;
    }
    if (exposure::Begin(frame_num)) {
        Draw(pContext, src, ViewFormatOf(sd.Format));
    }
    if ((frame_num % (S + 1)) == S) {
        Resolve(pContext);
    }
}
} // namespace plate

// ---- capture_source = clean -----------------------------------------------------------------------------------------
//  At Rockstar's export point (VideoEncode bind) the back buffer holds the finished game image without any UI. It is
//  copied to pCleanTex and held. When ReShade next renders its effects (normally at present, after the UI), EVE swaps
//  the held frame into ReShade's target in reshade_begin_effects, lets ReShade render the preset onto it with the same
//  depth buffer, reads the result in reshade_finish_effects and puts the on-screen image back. So the video gets the
//  preset (DOF, grade) on a frame without the progress bar / cursor, and ReShade is driven exactly as it normally is.
//  EVE never calls render_effects itself (the GTA V Upscaler hooks that call).
bool g_cleanPending = false;        // pCleanTex holds an export frame waiting for ReShade
ID3D11Resource* g_swappedRes = nullptr; // ReShade target currently holding our frame (between begin and finish)
ID3D11Resource* g_lastBackBuffer = nullptr;
// ReShade renders effects several times per frame here: Reshade Effect Shader Toggler / NVE run technique groups
// (clouds, sky, bloom...) mid-frame on game targets; the full preset (DOF, grade) runs once at present. The present
// passes render onto the back buffer before reshade_present (several per frame), so that target is learned from the
// previous frames and every render on it between the export point and present is applied to the held frame.
ID3D11Resource* g_lastFinishRes = nullptr;
ID3D11Resource* g_presentTarget = nullptr;
ID3D11Device* g_cleanDevice = nullptr;
uint64_t g_cleanEveEffects = 0;
uint32_t g_cleanPasses = 0;          // ReShade renders applied to the held frame so far (several per frame)
uint32_t g_cleanPassesMax = 0;
uint64_t g_cleanNoEffects = 0;
uint32_t g_diagEvents = 0;
bool g_toldMismatch = false;

ComPtr<ID3D11Texture2D> pCleanTex;
ComPtr<ID3D11Texture2D> pCleanBackup;
D3D11_TEXTURE2D_DESC cleanDesc{};

void EnsureCleanTarget(ID3D11Device* pDevice, const D3D11_TEXTURE2D_DESC& src) {
    if (pCleanTex && cleanDesc.Width == src.Width && cleanDesc.Height == src.Height && cleanDesc.Format == src.Format) {
        return;
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = src.Width;
    desc.Height = src.Height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = TypelessOf(src.Format);
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    REQUIRE(pDevice->CreateTexture2D(&desc, nullptr, pCleanTex.ReleaseAndGetAddressOf()),
            "Failed to create the clean capture texture");
    REQUIRE(pDevice->CreateTexture2D(&desc, nullptr, pCleanBackup.ReleaseAndGetAddressOf()),
            "Failed to create the clean backup texture");
    cleanDesc = src;
    g_cleanDevice = pDevice;
    LOG(LL_NON, "EVE1G clean: capturing ", src.Width, "x", src.Height, " ",
        conv_dxgi_format_to_string(src.Format), " at the export point (before the game UI)");
}

ID3D11Resource* ResourceOfView(reshade::api::effect_runtime* runtime, reshade::api::resource_view rtv) {
    if (!rtv.handle || !runtime) {
        return nullptr;
    }
    return reinterpret_cast<ID3D11Resource*>(runtime->get_device()->get_resource_from_view(rtv).handle);
}

void Diag(const char* what, reshade::api::effect_runtime* runtime, reshade::api::resource_view rtv) {
    if (g_diagEvents >= 24) {
        return;
    }
    ++g_diagEvents;
    std::string info = "none";
    if (rtv.handle && runtime) {
        const reshade::api::resource res = runtime->get_device()->get_resource_from_view(rtv);
        ComPtr<ID3D11Texture2D> tex = AsTexture2D(reinterpret_cast<ID3D11Resource*>(res.handle));
        if (tex) {
            D3D11_TEXTURE2D_DESC d;
            tex->GetDesc(&d);
            info = std::to_string(d.Width) + "x" + std::to_string(d.Height) + " " +
                   conv_dxgi_format_to_string(d.Format) +
                   (reinterpret_cast<ID3D11Resource*>(res.handle) == g_presentTarget ? " = present target"
                                                                                       : " (mid-frame group)");
        }
    }
    LOG(LL_NON, "EVE1G diag: ", what, " target=", info, " res=", reinterpret_cast<uintptr_t>(ResourceOfView(runtime, rtv)),
        " pending=", g_cleanPending ? 1 : 0);
}

ID3D11Resource* ResourceOf(reshade::api::effect_runtime* runtime, reshade::api::resource_view rtv) {
    if (!rtv.handle || !runtime) {
        return nullptr;
    }
    return reinterpret_cast<ID3D11Resource*>(runtime->get_device()->get_resource_from_view(rtv).handle);
}

// Emit the held export frame. Called at reshade_present (after every ReShade render of the frame went onto it) or,
// as a fallback, at the next export point if no present came in between.
void FlushClean(ID3D11Device* pDevice, ID3D11DeviceContext* pContext) {
    if (!g_cleanPending) {
        return;
    }
    g_cleanPending = false;
    if (g_cleanPasses > 0) {
        ++g_cleanEveEffects;
        if (g_cleanPasses > g_cleanPassesMax) {
            g_cleanPassesMax = g_cleanPasses;
        }
    } else {
        ++g_cleanNoEffects;
        if (config::clean_apply_reshade && (g_cleanNoEffects <= 3 || (g_cleanNoEffects % 200) == 0)) {
            LOG(LL_WRN, "EVE1G clean: ReShade did not render on the export frame - encoded without the preset (",
                g_cleanNoEffects, " so far)");
        }
    }
    g_cleanPasses = 0;
    AccumulateAndEmit(pDevice, pContext, pCleanTex.Get());
}
} // namespace

void eve::QueueExportFrame(ComPtr<ID3D11Device> pDevice, ComPtr<ID3D11DeviceContext> pContext) {
    ++g_framesQueued;
    if (g_capturePending.exchange(false)) {
        // The previous export frame never reached a present (the game skipped presenting it). VideoEncode still
        // holds that frame - the game is only now about to draw the next one into it - so take it from there
        // rather than lose a motion-blur sample and shift the timing.
        ++g_framesWithoutPresent;
        if (g_framesWithoutPresent <= 5 || (g_framesWithoutPresent % 100) == 0) {
            LOG(LL_NON, "EVE1G: export frame ", g_framesQueued - 1,
                " was not presented - captured from VideoEncode instead (", g_framesWithoutPresent, " so far)");
        }
        if (::exportContext && ::exportContext->p_export_render_target) {
            AccumulateAndEmit(pDevice.Get(), pContext.Get(), ::exportContext->p_export_render_target.Get());
        }
    }
    if (::exportContext) {
        g_capturePending = true;
    }
}

void eve::OnInitEffectRuntime(reshade::api::effect_runtime* runtime) {
    g_runtime = runtime;
}

void eve::OnDestroyEffectRuntime(reshade::api::effect_runtime* runtime) {
    reshade::api::effect_runtime* expected = runtime;
    g_runtime.compare_exchange_strong(expected, nullptr);
    g_swappedRes = nullptr;
}

void eve::CaptureClean(ID3D11Device* pDevice, ID3D11DeviceContext* pContext, reshade::api::command_list* cmd_list) {
    if (!::exportContext) {
        return;
    }
    ++g_framesQueued;
    try {
        // the previous frame never got its ReShade pass: encode it plain so timing stays right
        FlushClean(pDevice, pContext);

        reshade::api::effect_runtime* runtime = g_runtime.load();
        ComPtr<ID3D11Texture2D> pBackBuffer;
        if (runtime) {
            pBackBuffer = AsTexture2D(reinterpret_cast<ID3D11Resource*>(runtime->get_current_back_buffer().handle));
        }
        if (!pBackBuffer && ::exportContext->p_swap_chain) {
            ::exportContext->p_swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                                     reinterpret_cast<void**>(pBackBuffer.GetAddressOf()));
        }
        if (!pBackBuffer) {
            LOG(LL_ERR, "EVE1G clean: no back buffer at the export point - frame skipped");
            return;
        }
        g_lastBackBuffer = pBackBuffer.Get();
        if (g_framesQueued == 1) {
            g_diagEvents = 0; // log the first export frames, not the setup frames before them
            LOG(LL_NON, "EVE1G clean: ReShade present-pass target ", reinterpret_cast<uintptr_t>(g_presentTarget),
                g_presentTarget ? "" : " (not learned yet - first frames are encoded without the preset)");
        }
        if (g_diagEvents < 24) {
            ++g_diagEvents;
            LOG(LL_NON, "EVE1G diag: export point, runtime=", reinterpret_cast<uintptr_t>(runtime),
                " back buffer=", reinterpret_cast<uintptr_t>(pBackBuffer.Get()));
        }

        D3D11_TEXTURE2D_DESC src;
        pBackBuffer->GetDesc(&src);
        EnsureCleanTarget(pDevice, src);
        if (src.SampleDesc.Count > 1) {
            pContext->ResolveSubresource(pCleanTex.Get(), 0, pBackBuffer.Get(), 0, ViewFormatOf(src.Format));
        } else {
            pContext->CopyResource(pCleanTex.Get(), pBackBuffer.Get());
        }

        if (!g_toldSource) {
            g_toldSource = true;
            LOG(LL_NON, "EVE1G: capturing at Rockstar's export point (clean - no game UI, no cursor)");
        }
        // clean plate: this copy still has no ReShade on it
        plate::Add(pDevice, pContext, pCleanTex.Get(), static_cast<uint64_t>(::exportContext->total_frame_num));

        if (config::clean_apply_reshade && runtime) {
            g_cleanPending = true; // ReShade renders go onto it in begin/finish_effects, emitted at reshade_present
            g_cleanPasses = 0;
        } else {
            AccumulateAndEmit(pDevice, pContext, pCleanTex.Get());
        }
    } catch (std::exception& ex) {
        LOG(LL_ERR, "EVE1G clean: capture failed: ", ex.what());
    }
}

void eve::OnReshadeBeginEffects(reshade::api::effect_runtime* runtime, reshade::api::command_list* cmd_list,
                                reshade::api::resource_view rtv, reshade::api::resource_view rtv_srgb) {
    if (config::capture_source != config::CaptureSource::Clean || !::exportContext) {
        return;
    }
    Diag("begin_effects", runtime, rtv);
    if (!g_cleanPending || !pCleanTex) {
        return;
    }
    ID3D11Resource* res = ResourceOf(runtime, rtv);
    if (!res || res != g_presentTarget) {
        return; // a mid-frame technique group (Shader Toggler / NVE), not the full preset
    }
    ComPtr<ID3D11Texture2D> tex = AsTexture2D(res);
    if (!tex) {
        return;
    }
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    if (d.Width != cleanDesc.Width || d.Height != cleanDesc.Height || d.SampleDesc.Count != 1 ||
        TypelessOf(d.Format) != TypelessOf(cleanDesc.Format)) {
        if (!g_toldMismatch) {
            g_toldMismatch = true;
            LOG(LL_WRN, "EVE1G clean: ReShade renders into a ", d.Width, "x", d.Height, " ",
                conv_dxgi_format_to_string(d.Format), " target that does not match the captured frame - left alone");
        }
        return;
    }
    auto* pContext = reinterpret_cast<ID3D11DeviceContext*>(cmd_list->get_native());
    pContext->CopyResource(pCleanBackup.Get(), res); // what is on screen (with UI)
    pContext->CopyResource(res, pCleanTex.Get());    // the clean export frame
    g_swappedRes = res;
}

void eve::OnReshadePresent(reshade::api::effect_runtime* runtime) {
    g_runtime = runtime;               // the runtime that actually presents
    depth::OnPresent(runtime, ::exportContext != nullptr); // depth technique on while a depth export runs
    exportosd::OnPresent(::exportContext != nullptr);
    if (g_lastFinishRes) {
        g_presentTarget = g_lastFinishRes; // last effects render before present = the full preset pass
    }
    g_lastFinishRes = nullptr;
    if (config::capture_source == config::CaptureSource::Clean && ::exportContext && g_diagEvents < 24) {
        ++g_diagEvents;
        LOG(LL_NON, "EVE1G diag: reshade_present runtime=", reinterpret_cast<uintptr_t>(runtime),
            " pending=", g_cleanPending ? 1 : 0);
    }
    // Always track the back buffer size: the encoder is sized from it before the first frame is captured.
    const reshade::api::resource bb = runtime->get_current_back_buffer();
    ComPtr<ID3D11Texture2D> pBackBuffer = AsTexture2D(reinterpret_cast<ID3D11Resource*>(bb.handle));
    if (pBackBuffer) {
        D3D11_TEXTURE2D_DESC desc;
        pBackBuffer->GetDesc(&desc);
        g_backBufferWidth = desc.Width;
        g_backBufferHeight = desc.Height;
    }

    if (config::capture_source == config::CaptureSource::Clean) {
        if (::exportContext && g_cleanPending) {
            try {
                FlushClean(reinterpret_cast<ID3D11Device*>(runtime->get_device()->get_native()),
                           reinterpret_cast<ID3D11DeviceContext*>(
                               runtime->get_command_queue()->get_immediate_command_list()->get_native()));
            } catch (std::exception& ex) {
                LOG(LL_ERR, "EVE1G clean: capture failed: ", ex.what());
            }
        }
        return;
    }
    // effects mode reads the image in reshade_finish_effects instead (see below)
    if (config::capture_source == config::CaptureSource::Effects) {
        return;
    }
    if (!g_capturePending.exchange(false)) {
        return;
    }
    if (!::exportContext) {
        return;
    }

    ComPtr<ID3D11Texture2D> pSource;
    if (config::capture_source == config::CaptureSource::Encode) {
        pSource = ::exportContext->p_export_render_target;
    } else {
        pSource = pBackBuffer;
    }
    if (!pSource) {
        LOG(LL_ERR, "EVE1G: no image to capture this frame (back buffer unavailable)");
        return;
    }

    auto* pDevice = reinterpret_cast<ID3D11Device*>(runtime->get_device()->get_native());
    auto* pContext = reinterpret_cast<ID3D11DeviceContext*>(
        runtime->get_command_queue()->get_immediate_command_list()->get_native());

    if (!g_toldSource) {
        g_toldSource = true;
        LOG(LL_NON, "EVE1G: capturing from ",
            config::capture_source == config::CaptureSource::Encode ? "the game's VideoEncode target"
                                                                    : "the finished back buffer (reshade_present)");
    }
    AccumulateAndEmit(pDevice, pContext, pSource.Get());
}

// capture_source = effects: read the frame where ReShade renders its effects. With the GTA V Upscaler setting
// mRenderReShadeBeforeUI = true that is BEFORE the game draws its UI - so DLSS 5 is in, the export progress bar is
// not. If this point fires before the game marks the export frame, the capture lands one frame later, which only
// shifts the clip by a frame. If it never fires, QueueExportFrame falls back to VideoEncode.
void eve::OnReshadeFinishEffects(reshade::api::effect_runtime* runtime, reshade::api::command_list* cmd_list,
                                 reshade::api::resource_view rtv, reshade::api::resource_view rtv_srgb) {
    g_lastFinishRes = ResourceOfView(runtime, rtv);
    if (config::capture_source == config::CaptureSource::Clean) {
        if (!::exportContext) {
            g_swappedRes = nullptr;
            return;
        }
        Diag("finish_effects", runtime, rtv);
        ID3D11Resource* res = ResourceOf(runtime, rtv);
        if (!g_swappedRes || res != g_swappedRes) {
            return;
        }
        g_swappedRes = nullptr;
        auto* pContext = reinterpret_cast<ID3D11DeviceContext*>(cmd_list->get_native());
        pContext->CopyResource(pCleanTex.Get(), res);          // export frame + this ReShade pass
        if (!config::clean_preview_effects) {
            pContext->CopyResource(res, pCleanBackup.Get());   // screen gets its own frame (with UI) back
        }
        // else the screen keeps the effected export frame: DOF / grade visible live (focus pulls), no game UI
        ++g_cleanPasses;                                       // more passes may follow before present
        return;
    }
    if (config::capture_source != config::CaptureSource::Effects || !rtv.handle) {
        return;
    }
    if (!g_capturePending.exchange(false)) {
        return;
    }
    if (!::exportContext) {
        return;
    }
    const reshade::api::resource res = runtime->get_device()->get_resource_from_view(rtv);
    ComPtr<ID3D11Texture2D> pSource = AsTexture2D(reinterpret_cast<ID3D11Resource*>(res.handle));
    if (!pSource) {
        LOG(LL_ERR, "EVE1G: no image at the effects point this frame");
        return;
    }
    auto* pDevice = reinterpret_cast<ID3D11Device*>(runtime->get_device()->get_native());
    auto* pContext = reinterpret_cast<ID3D11DeviceContext*>(cmd_list->get_native());
    if (!g_toldSource) {
        g_toldSource = true;
        LOG(LL_NON, "EVE1G: capturing at ReShade's effects point (reshade_finish_effects)");
    }
    AccumulateAndEmit(pDevice, pContext, pSource.Get());
}

void eve::ResetCaptureStats() {
    if (g_framesQueued) {
        LOG(LL_NON, "EVE1G: export done - ", g_framesQueued, " game frame(s), ", g_framesCaptured,
            " captured, ", g_framesWithoutPresent, " taken from VideoEncode because no present followed");
    }
    if (g_cleanEveEffects || g_cleanNoEffects) {
        LOG(LL_NON, "EVE1G clean: ReShade preset rendered on ", g_cleanEveEffects, " export frame(s) (up to ",
            g_cleanPassesMax, " ReShade passes per frame), missing on ", g_cleanNoEffects);
    }
    if (g_framesQueued) {
        eve::WriteSidecarEnd(g_framesQueued, g_cleanEveEffects, g_cleanNoEffects, g_cleanPassesMax);
    }
    g_cleanPasses = g_cleanPassesMax = 0;
    g_cleanEveEffects = g_cleanNoEffects = 0;
    g_cleanPending = false;
    g_swappedRes = nullptr;
    g_diagEvents = 0;
    g_toldMismatch = false;
    g_capturePending = false;
    g_framesQueued = g_framesCaptured = g_framesWithoutPresent = 0;
    g_toldSizeMismatch = g_toldNoBuffers = g_toldSource = false;
}

std::pair<uint32_t, uint32_t> eve::CaptureSize(ID3D11Texture2D* pVideoEncode, uint32_t fallbackW,
                                               uint32_t fallbackH) {
    if (config::capture_source == config::CaptureSource::Encode && pVideoEncode) {
        D3D11_TEXTURE2D_DESC desc;
        pVideoEncode->GetDesc(&desc);
        return {desc.Width, desc.Height};
    }
    if ((config::capture_source == config::CaptureSource::Present ||
         config::capture_source == config::CaptureSource::Effects ||
         config::capture_source == config::CaptureSource::Clean) &&
        g_backBufferWidth && g_backBufferHeight) {
        return {g_backBufferWidth.load(), g_backBufferHeight.load()};
    }
    return {fallbackW, fallbackH};
}

// =====================================================================================================================
//  EVE1G shot naming + settings sidecar
//    EVE1G.ini (top section):  project = MyFilm   shot = shot03   take = 0 (0 = next free number)   sidecar = true
//    -> <Videos>\MyFilm_shot03_t02_24fps_16s_180.mov  +  MyFilm_shot03_t02_24fps_16s_180.txt
//    Without project and shot the old style is kept: EVE1G-20261006191500.mov
// =====================================================================================================================
namespace {
std::string g_sidecarPath;

// path -> UTF-8 std::string (u8string() is std::u8string in C++20)
std::string U8(const std::filesystem::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::string Cstr(const char* p, size_t n) { return std::string(p, strnlen(p, n)); }

std::string CleanName(const std::string& in) {
    std::string r;
    for (const char c : in) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 128 && (std::isalnum(u) || c == '-' || c == '_')) {
            r += c;
        } else if ((c == ' ' || c == '.') && !r.empty() && r.back() != '-') {
            r += '-';
        }
    }
    while (!r.empty() && (r.back() == '-' || r.back() == '_')) {
        r.pop_back();
    }
    return r;
}

std::string FpsLabel() {
    const uint32_t n = config::fps.first, d = config::fps.second;
    if (d == 0) {
        return "0";
    }
    if (n % d == 0) {
        return std::to_string(n / d);
    }
    char b[32];
    snprintf(b, sizeof(b), "%.3f", static_cast<double>(n) / d);
    std::string v = b;
    while (!v.empty() && v.back() == '0') {
        v.pop_back();
    }
    if (!v.empty() && v.back() == '.') {
        v.pop_back();
    }
    return v;
}

int ShutterAngle() { return static_cast<int>(std::lround(config::motion_blur_strength * 360.0f)); }

int NextTake(const std::string& dir, const std::string& prefix) {
    int best = 0;
    const std::string key = prefix + "_t";
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator(std::filesystem::u8path(dir), ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        const std::string name = U8(it->path().filename());
        if (name.size() > key.size() && name.compare(0, key.size(), key) == 0 &&
            std::isdigit(static_cast<unsigned char>(name[key.size()]))) {
            best = (std::max)(best, atoi(name.c_str() + key.size()));
        }
    }
    return best + 1;
}

// key = value from the GTA V Upscaler ini (any section); empty if not there
std::string UpscalerValue(const std::string& key) {
    std::ifstream f(AsiPath() + "\\mods\\GTAVUpscaler.ini");
    std::string line;
    while (std::getline(f, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos || line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        auto trim = [](std::string& t) {
            while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
            size_t i = 0;
            while (i < t.size() && (std::isspace(static_cast<unsigned char>(t[i])) || static_cast<unsigned char>(t[i]) > 127)) ++i;
            t = t.substr(i);
        };
        trim(k);
        trim(v);
        if (k == key) {
            return v;
        }
    }
    return {};
}
} // namespace

namespace {
VKENCODERCONFIG MakeConfig(const char* encoder, const char* container, const char* options, const char* audio,
                           const char* audioOptions) {
    VKENCODERCONFIG c{};
    c.version = 1;
    strncpy_s(c.video.encoder, encoder, _TRUNCATE);
    strncpy_s(c.video.options, options, _TRUNCATE);
    strncpy_s(c.audio.encoder, audio, _TRUNCATE);
    strncpy_s(c.audio.options, audioOptions, _TRUNCATE);
    strncpy_s(c.format.container, container, _TRUNCATE);
    c.format.faststart = strcmp(container, "mp4") == 0;
    return c;
}
} // namespace

std::vector<VKENCODERCONFIG> eve::FallbackConfigs(const VKENCODERCONFIG& chosen) {
    const char* aac = "_sampleFormat=fltp|b=320000|profile=aac_low";
    const VKENCODERCONFIG prores =
        MakeConfig("prores_ks", "mov", "_pixelFormat=yuv422p10le|profile=3", "pcm_s16le", "_sampleFormat=s16");
    const VKENCODERCONFIG hevc10 =
        MakeConfig("hevc_nvenc", "mp4", "_pixelFormat=p010le|preset=p7|rc=vbr|cq=16|b=0", "aac", aac);
    const VKENCODERCONFIG h264 =
        MakeConfig("h264_nvenc", "mp4", "_pixelFormat=yuv420p|preset=p7|rc=vbr|cq=16|b=0", "aac", aac);
    const VKENCODERCONFIG x264 =
        MakeConfig("libx264", "mp4", "_pixelFormat=yuv420p|rc=crf|preset=slow|crf=14", "aac", aac);
    // stay in the same family first: a GPU choice falls back to GPU formats, anything else to ProRes
    const bool gpu = strstr(chosen.video.encoder, "nvenc") != nullptr;
    std::vector<VKENCODERCONFIG> order = gpu ? std::vector<VKENCODERCONFIG>{hevc10, h264, prores, x264}
                                             : std::vector<VKENCODERCONFIG>{prores, hevc10, h264, x264};
    std::vector<VKENCODERCONFIG> out;
    for (const VKENCODERCONFIG& c : order) {
        if (strcmp(c.video.encoder, chosen.video.encoder) == 0 && strcmp(c.video.options, chosen.video.options) == 0) {
            continue;
        }
        out.push_back(c);
    }
    return out;
}

std::string eve::BuildOutputBase(const std::string& dir, const std::string& container) {
    std::string folder = dir;
    while (!folder.empty() && (folder.back() == '\\' || folder.back() == '/')) {
        folder.pop_back();
    }
    const std::string project = CleanName(config::name_project);
    const std::string shot = CleanName(config::name_shot);
    if (project.empty() && shot.empty()) {
        char buffer[64];
        time_t rawtime;
        struct tm timeinfo;
        time(&rawtime);
        localtime_s(&timeinfo, &rawtime);
        strftime(buffer, sizeof(buffer), "%Y%m%d%H%M%S", &timeinfo);
        return folder + "\\EVE1G-" + buffer;
    }
    std::string prefix = project;
    if (!shot.empty()) {
        prefix += (prefix.empty() ? "" : "_") + shot;
    }
    const int take = config::name_take > 0 ? config::name_take : NextTake(folder, prefix);
    char t[16];
    snprintf(t, sizeof(t), "_t%02d", take);
    std::string name = prefix + t + "_" + FpsLabel() + "fps";
    if (config::motion_blur_samples > 0) {
        name += "_" + std::to_string(config::motion_blur_samples) + "s_" + std::to_string(ShutterAngle());
    } else {
        name += "_noblur";
    }
    std::string base = folder + "\\" + name;
    std::error_code ec;
    for (int k = 2; std::filesystem::exists(std::filesystem::u8path(base + "." + container), ec) && k < 1000; ++k) {
        base = folder + "\\" + name + "_" + std::to_string(k); // never overwrite an existing take
    }
    return base;
}

void eve::WriteSidecarStart(const std::string& videoFile, uint32_t width, uint32_t height, bool audio) {
    g_sidecarPath.clear();
    if (!config::write_sidecar) {
        return;
    }
    const size_t dot = videoFile.find_last_of('.');
    const std::string path = (dot == std::string::npos ? videoFile : videoFile.substr(0, dot)) + ".txt";
    std::ofstream f(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
    if (!f) {
        LOG(LL_WRN, "EVE1G: could not write the settings sidecar ", path);
        return;
    }
    char when[64];
    time_t rawtime;
    struct tm timeinfo;
    time(&rawtime);
    localtime_s(&timeinfo, &rawtime);
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &timeinfo);

    const char* captureName = "clean";
    switch (config::capture_source) {
    case config::CaptureSource::Present: captureName = "present"; break;
    case config::CaptureSource::Encode: captureName = "encode"; break;
    case config::CaptureSource::Effects: captureName = "effects"; break;
    case config::CaptureSource::Legacy: captureName = "legacy"; break;
    default: break;
    }
    std::string preset = "(unknown)";
    if (reshade::api::effect_runtime* runtime = g_runtime.load()) {
        char p[1024] = {};
        size_t n = sizeof(p);
        runtime->get_current_preset_path(p, &n);
        preset = p;
    }
    const double gameFps = config::fps.second ? static_cast<double>(config::fps.first) / config::fps.second *
                                                    (config::motion_blur_samples + 1)
                                              : 0.0;
    const auto& ec = config::encoder_config;

    f << "EVE1G export\r\n"
      << "============\r\n"
      << "Started        : " << when << "\r\n"
      << "Video file     : " << U8(std::filesystem::u8path(videoFile).filename()) << "\r\n"
      << "Project / shot : " << (config::name_project.empty() ? "-" : config::name_project) << " / "
      << (config::name_shot.empty() ? "-" : config::name_shot) << "\r\n"
      << "Reel name      : " << U8(std::filesystem::u8path(videoFile).stem()) << "\r\n"
      << "Timecode       : starts at 00:00:00:00 (" << FpsLabel() << " fps, non-drop)\r\n"
      << "\r\nPICTURE\r\n"
      << "Resolution     : " << width << " x " << height << "\r\n"
      << "Frame rate     : " << FpsLabel() << " fps (" << config::fps.first << "/" << config::fps.second << ")\r\n"
      << "Motion blur    : "
      << (config::motion_blur_samples ? std::to_string(config::motion_blur_samples) + " samples, " +
                                            std::to_string(ShutterAngle()) + " deg shutter"
                                      : std::string("off"))
      << "\r\n"
      << "Camera         : " << (config::blend_linear ? "linear-light blend" : "display blend")
      << (config::motion_blur_samples && config::shutter_soft > 0.0f
              ? ", soft shutter edges " + std::to_string(static_cast<int>(std::lround(config::shutter_soft * 100))) + "%"
              : std::string(", hard shutter"))
      << (config::motion_blur_samples && config::rolling_shutter_ms > 0.0f
              ? ", rolling shutter " + std::to_string(static_cast<int>(std::lround(config::rolling_shutter_ms))) + " ms"
              : std::string(", global shutter"))
      << "\r\n"
      << "Game renders at: " << gameFps << " fps\r\n"
      << "Audio          : " << (audio ? "exported" : "OFF (game fps above 60 - record sound separately)") << "\r\n"
      << "\r\nCAPTURE\r\n"
      << "Capture mode   : " << captureName << (config::capture_source == config::CaptureSource::Clean
                                                    ? " (before the game UI - no progress bar, no cursor)"
                                                    : "")
      << "\r\n"
      << "ReShade preset : " << (config::clean_apply_reshade ? preset : std::string("not applied")) << "\r\n"
      << "\r\nGTA V UPSCALER\r\n"
      << "Enabled        : " << UpscalerValue("mEnable") << "   type " << UpscalerValue("mUpscaleType")
      << " (0 = DLSS)\r\n"
      << "DLSS preset    : " << UpscalerValue("mDLSSPreset") << "   target scale " << UpscalerValue("mTargetResolutionScale")
      << "\r\n"
      << "DLSS 5 NR      : " << UpscalerValue("mEnableDLSSNR") << "\r\n"
      << "Frame gen      : " << UpscalerValue("mEnableFrameGeneration") << "\r\n"
      << "\r\nENCODER (Voukoder)\r\n"
      << "Container      : " << Cstr(ec.format.container, sizeof(ec.format.container)) << "\r\n"
      << "Video          : " << Cstr(ec.video.encoder, sizeof(ec.video.encoder)) << "  "
      << Cstr(ec.video.options, sizeof(ec.video.options)) << "\r\n"
      << "Audio          : " << Cstr(ec.audio.encoder, sizeof(ec.audio.encoder)) << "  "
      << Cstr(ec.audio.options, sizeof(ec.audio.options)) << "\r\n"
      << "\r\nDEPTH PASS\r\n"
      << "Depth          : " << depth::Describe() << "\r\n"
      << "HFR export     : " << hfr::Describe() << "\r\n"
      << "\r\nPASSES\r\n"
      << passes::Describe() << "\r\n";
    g_sidecarPath = path;
    LOG(LL_NON, "EVE1G: settings sidecar ", path);
}

bool eve::WantMaster16(const VKENCODERCONFIG& cfg) {
    if (config::master_16bit == 0) {
        return false;
    }
    if (config::master_16bit == 1) {
        return true;
    }
    const std::string enc = Cstr(cfg.video.encoder, sizeof(cfg.video.encoder));
    const std::string opt = Cstr(cfg.video.options, sizeof(cfg.video.options));
    if (enc == "ffv1" || enc == "prores_ks" || enc == "prores_aw" || enc == "utvideo") {
        return true; // ProRes is 10/12-bit, FFV1 / UT Video lossless
    }
    for (const char* deep : {"10le", "12le", "16le", "p010", "p016", "48le", "64le", "10be", "12be"}) {
        if (opt.find(deep) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void eve::NotifyExportDone(const std::string& videoFile) {
    exportosd::OnExportEnd(U8(std::filesystem::u8path(videoFile).filename()));
    if (!config::notify_done) {
        return;
    }
    MessageBeep(MB_ICONASTERISK);
    HWND wnd = nullptr;
    if (mainSwapChain) {
        DXGI_SWAP_CHAIN_DESC d{};
        if (SUCCEEDED(mainSwapChain->GetDesc(&d))) {
            wnd = d.OutputWindow;
        }
    }
    if (wnd) {
        FLASHWINFO fi{sizeof(FLASHWINFO), wnd, FLASHW_ALL | FLASHW_TIMERNOFG, 5, 0};
        FlashWindowEx(&fi); // blinks in the taskbar until the game is in front again
    }
    LOG(LL_NON, "EVE1G: export finished - ", videoFile);
}

void eve::WriteSidecarEnd(uint64_t frames, uint64_t withPreset, uint64_t withoutPreset, uint32_t passes) {
    if (g_sidecarPath.empty()) {
        return;
    }
    std::ofstream f(std::filesystem::u8path(g_sidecarPath), std::ios::binary | std::ios::app);
    char when[64];
    time_t rawtime;
    struct tm timeinfo;
    time(&rawtime);
    localtime_s(&timeinfo, &rawtime);
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &timeinfo);
    const uint32_t per = config::motion_blur_samples + 1u;
    f << "\r\nRESULT\r\n"
      << "Finished       : " << when << "\r\n"
      << "Game frames    : " << frames << "  (= " << (frames / per) << " video frames)\r\n";
    if (config::depth_enabled) {
        f << "Depth frames   : " << depth::Result() << "\r\n";
    }
    if (config::hfr_enabled) {
        f << "HFR frames     : " << hfr::Result() << "\r\n";
    }
    f << passes::Result();
    if (config::capture_source == config::CaptureSource::Clean && config::clean_apply_reshade) {
        f << "ReShade        : applied on " << withPreset << " frame(s), missing on " << withoutPreset << ", up to "
          << passes << " pass(es) per frame\r\n";
    }
    g_sidecarPath.clear();
}

// The game's own Windows encoder only ever writes EVE1G's empty placeholder file (WriteSample never forwards a frame
// to it). Left alone, it still sets up a GPU H.264 encoder on the GAME's Direct3D device, from another thread, at the
// moment the export starts - and with a busy GPU (DLSS, big texture pools) that crashed inside the NVIDIA driver
// (nvwgf2umx.dll, mfh264enc.dll on the stack). So the game's writer is created without the Direct3D device manager
// and without hardware encoders: it stays on the CPU and never touches the GPU.
static IMFAttributes* CpuOnlyWriterAttributes(IMFAttributes* gameAttributes, ComPtr<IMFAttributes>& holder) {
    // GUIDs from mfreadwrite.h, defined here so no extra Media Foundation import library is needed
    static const GUID kD3DManager = { 0xec822da2, 0xe1e9, 0x4b29, { 0xa0, 0xd8, 0x56, 0x3c, 0x71, 0x9f, 0x52, 0x69 } };
    static const GUID kHardwareTransforms = { 0xa634a91c, 0x822b, 0x41b9,
                                              { 0xa4, 0x94, 0x4d, 0xe4, 0x64, 0x36, 0x12, 0xb0 } };
    using CreateAttributesFn = HRESULT(WINAPI*)(IMFAttributes**, UINT32);
    HMODULE mfplat = GetModuleHandleW(L"mfplat.dll");
    auto create = mfplat ? reinterpret_cast<CreateAttributesFn>(GetProcAddress(mfplat, "MFCreateAttributes")) : nullptr;
    if (!create || FAILED(create(holder.ReleaseAndGetAddressOf(), 4))) {
        LOG(LL_WRN, "EVE1G: could not make the game's encoder CPU-only (MFCreateAttributes unavailable)");
        return gameAttributes;
    }
    if (gameAttributes) {
        gameAttributes->CopyAllItems(holder.Get());
    }
    holder->DeleteItem(kD3DManager);
    holder->SetUINT32(kHardwareTransforms, FALSE);
    LOG(LL_NON, "EVE1G: the game's own (placeholder) encoder runs on the CPU - no GPU encoder on the game's device");
    return holder.Get();
}

HRESULT ImportHooks::MFCreateSinkWriterFromURL::Implementation(LPCWSTR pwszOutputURL, IMFByteStream* pByteStream,
                                                               IMFAttributes* pAttributes,
                                                               IMFSinkWriter** ppSinkWriter) {
    PRE();
    ComPtr<IMFAttributes> cpuOnly;
    IMFAttributes* attributes = CpuOnlyWriterAttributes(pAttributes, cpuOnly);
    HRESULT result = OriginalFunc(pwszOutputURL, pByteStream, attributes, ppSinkWriter);
    if (FAILED(result) && attributes != pAttributes) {
        LOG(LL_WRN, "EVE1G: CPU-only game encoder refused (", Logger::hex(static_cast<uint32_t>(result), 8), "), using the game's settings");
        result = OriginalFunc(pwszOutputURL, pByteStream, pAttributes, ppSinkWriter);
    }
    g_gameWriterFailed = false;
    g_gameStreams = 0;
    if (SUCCEEDED(result)) {
        try {
            PERFORM_MEMBER_HOOK_REQUIRED(IMFSinkWriter, AddStream, *ppSinkWriter);
            PERFORM_MEMBER_HOOK_REQUIRED(IMFSinkWriter, SetInputMediaType, *ppSinkWriter);
            PERFORM_MEMBER_HOOK_REQUIRED(IMFSinkWriter, BeginWriting, *ppSinkWriter);
            PERFORM_MEMBER_HOOK_REQUIRED(IMFSinkWriter, WriteSample, *ppSinkWriter);
            PERFORM_MEMBER_HOOK_REQUIRED(IMFSinkWriter, Finalize, *ppSinkWriter);
        } catch (...) {
            LOG(LL_ERR, "Hooking IMFSinkWriter functions failed");
        }
    }
    POST();
    return result;
}

HRESULT IMFSinkWriterHooks::AddStream::Implementation(IMFSinkWriter* pThis, IMFMediaType* pTargetMediaType,
                                                      DWORD* pdwStreamIndex) {
    PRE();
    LOG(LL_NFO, "IMFSinkWriter::AddStream: ", GetMediaTypeDescription(pTargetMediaType).c_str());
    // EVE1G: the game's own Windows encoder only writes an empty placeholder file - EVE1G encodes the real video.
    // At resolutions it does not support (4K via DSR...) it refuses the stream and the game cancels the export.
    // So its failures are logged and hidden; stream numbers stay video = 0, audio = 1.
    const DWORD index = g_gameStreams++;
    HRESULT hr = E_FAIL;
    if (!g_gameWriterFailed) {
        hr = OriginalFunc(pThis, pTargetMediaType, pdwStreamIndex);
    }
    if (FAILED(hr)) {
        if (!g_gameWriterFailed) {
            LOG(LL_WRN, "EVE1G: the game's own encoder refused stream ", index, " (", Logger::hex(static_cast<uint32_t>(hr), 8),
                ") - ignored, EVE1G writes the video itself");
        }
        g_gameWriterFailed = true;
        if (pdwStreamIndex) {
            *pdwStreamIndex = index;
        }
        hr = S_OK;
    }
    POST();
    return hr;
}

HRESULT IMFSinkWriterHooks::BeginWriting::Implementation(IMFSinkWriter* pThis) {
    PRE();
    HRESULT hr = g_gameWriterFailed ? S_OK : OriginalFunc(pThis);
    if (FAILED(hr)) {
        LOG(LL_WRN, "EVE1G: the game's own encoder could not start (", Logger::hex(static_cast<uint32_t>(hr), 8),
            ") - ignored, EVE1G writes the video itself");
        g_gameWriterFailed = true;
        hr = S_OK;
    }
    POST();
    return hr;
}

void CreateMotionBlurBuffers(ComPtr<ID3D11Device> p_device, uint32_t width, uint32_t height) {
    D3D11_TEXTURE2D_DESC motionBlurBufferDesc;
    motionBlurBufferDesc.Width = width;
    motionBlurBufferDesc.Height = height;
    motionBlurBufferDesc.MipLevels = 1;
    motionBlurBufferDesc.ArraySize = 1;
    motionBlurBufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    motionBlurBufferDesc.SampleDesc.Count = 1;
    motionBlurBufferDesc.SampleDesc.Quality = 0;
    motionBlurBufferDesc.Usage = D3D11_USAGE_DEFAULT;
    motionBlurBufferDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    motionBlurBufferDesc.CPUAccessFlags = 0;
    motionBlurBufferDesc.MiscFlags = 0;

    D3D11_TEXTURE2D_DESC accBufDesc = motionBlurBufferDesc;
    accBufDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    accBufDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    accBufDesc.CPUAccessFlags = 0;
    accBufDesc.MiscFlags = 0;
    accBufDesc.Usage = D3D11_USAGE::D3D11_USAGE_DEFAULT;
    accBufDesc.MipLevels = 1;
    accBufDesc.ArraySize = 1;
    accBufDesc.SampleDesc.Count = 1;
    accBufDesc.SampleDesc.Quality = 0;

    LOG_IF_FAILED(p_device->CreateTexture2D(&accBufDesc, NULL, pMotionBlurAccBuffer.ReleaseAndGetAddressOf()),
                  "Failed to create accumulation buffer texture");

    // EVE1G: the finished frame is 16 bits per channel for 10/12-bit and lossless codecs (master_16bit), so the
    // accumulated (float) motion blur reaches the encoder without being rounded to 8 bits first - no banding in skies.
    const DXGI_FORMAT finalFormat = g_master16 ? DXGI_FORMAT_R16G16B16A16_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    D3D11_TEXTURE2D_DESC mbBufferDesc = accBufDesc;
    mbBufferDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
    mbBufferDesc.Format = finalFormat;

    LOG_IF_FAILED(p_device->CreateTexture2D(&mbBufferDesc, NULL, pMotionBlurFinalBuffer.ReleaseAndGetAddressOf()),
                  "Failed to create motion blur buffer texture");

    D3D11_RENDER_TARGET_VIEW_DESC accBufRTVDesc;
    accBufRTVDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    accBufRTVDesc.Texture2D.MipSlice = 0;
    accBufRTVDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;

    LOG_IF_FAILED(p_device->CreateRenderTargetView(pMotionBlurAccBuffer.Get(), &accBufRTVDesc,
                                                   pRtvAccBuffer.ReleaseAndGetAddressOf()),
                  "Failed to create acc buffer RTV.");

    D3D11_RENDER_TARGET_VIEW_DESC mbBufferRTVDesc;
    mbBufferRTVDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    mbBufferRTVDesc.Texture2D.MipSlice = 0;
    mbBufferRTVDesc.Format = finalFormat;

    LOG_IF_FAILED(p_device->CreateRenderTargetView(pMotionBlurFinalBuffer.Get(), &mbBufferRTVDesc,
                                                   pRtvBlurBuffer.ReleaseAndGetAddressOf()),
                  "Failed to create blur buffer RTV.");

    D3D11_SHADER_RESOURCE_VIEW_DESC accBufSRVDesc;
    accBufSRVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    accBufSRVDesc.Texture2D.MipLevels = 1;
    accBufSRVDesc.Texture2D.MostDetailedMip = 0;
    accBufSRVDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;

    LOG_IF_FAILED(p_device->CreateShaderResourceView(pMotionBlurAccBuffer.Get(), &accBufSRVDesc,
                                                     pSrvAccBuffer.ReleaseAndGetAddressOf()),
                  "Failed to create blur buffer SRV.");

    D3D11_TEXTURE2D_DESC sourceSRVTextureDesc = motionBlurBufferDesc;
    sourceSRVTextureDesc.CPUAccessFlags = 0;
    sourceSRVTextureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    sourceSRVTextureDesc.MiscFlags = 0;
    sourceSRVTextureDesc.Usage = D3D11_USAGE::D3D11_USAGE_DEFAULT;
    sourceSRVTextureDesc.ArraySize = 1;
    sourceSRVTextureDesc.SampleDesc.Count = 1;
    sourceSRVTextureDesc.SampleDesc.Quality = 0;
    sourceSRVTextureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sourceSRVTextureDesc.MipLevels = 1;

    REQUIRE(p_device->CreateTexture2D(&sourceSRVTextureDesc, NULL, pSourceSrvTexture.ReleaseAndGetAddressOf()),
            "Failed to create back buffer copy texture");

    D3D11_SHADER_RESOURCE_VIEW_DESC sourceSRVDesc;
    sourceSRVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sourceSRVDesc.Texture2D.MipLevels = 1;
    sourceSRVDesc.Texture2D.MostDetailedMip = 0;
    sourceSRVDesc.Format = sourceSRVTextureDesc.Format;

    LOG_IF_FAILED(p_device->CreateShaderResourceView(pSourceSrvTexture.Get(), &sourceSRVDesc,
                                                     pSourceSrv.ReleaseAndGetAddressOf()),
                  "Failed to create backbuffer copy SRV.");
}

HRESULT IMFSinkWriterHooks::SetInputMediaType::Implementation(IMFSinkWriter* pThis, DWORD dwStreamIndex,
                                                              IMFMediaType* pInputMediaType,
                                                              IMFAttributes* pEncodingParameters) {
    PRE();
    LOG(LL_NFO, "IMFSinkWriter::SetInputMediaType: ", GetMediaTypeDescription(pInputMediaType).c_str());

    GUID majorType;
    if (SUCCEEDED(pInputMediaType->GetMajorType(&majorType))) {
        if (IsEqualGUID(majorType, MFMediaType_Video)) {
            ::exportContext->video_media_type = pInputMediaType;
        } else if (IsEqualGUID(majorType, MFMediaType_Audio)) {
            try {
                std::lock_guard<std::mutex> sessionLock(mxSession);
                UINT width, height, fps_num, fps_den;
                MFGetAttribute2UINT32asUINT64(::exportContext->video_media_type.Get(), MF_MT_FRAME_SIZE, &width,
                                              &height);
                MFGetAttributeRatio(::exportContext->video_media_type.Get(), MF_MT_FRAME_RATE, &fps_num, &fps_den);

                GUID pixelFormat;
                ::exportContext->video_media_type->GetGUID(MF_MT_SUBTYPE, &pixelFormat);

                if (isCustomFrameRateSupported) {
                    auto fps = config::fps;
                    fps_num = fps.first;
                    fps_den = fps.second;

                    float gameFrameRate =
                        (static_cast<float>(fps.first) * (static_cast<float>(config::motion_blur_samples) + 1) /
                         static_cast<float>(fps.second));
                    if (gameFrameRate > 60.0f) {
                        LOG(LL_NON, "fps * (motion_blur_samples + 1) > 60.0!!!");
                        LOG(LL_NON, "Audio export will be disabled!!!");
                        ::exportContext->is_audio_export_disabled = true;
                    }
                }

                UINT32 blockAlignment, numChannels, sampleRate, bitsPerSample;
                GUID subType;

                pInputMediaType->GetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, &blockAlignment);
                pInputMediaType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &numChannels);
                pInputMediaType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sampleRate);
                pInputMediaType->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bitsPerSample);
                pInputMediaType->GetGUID(MF_MT_SUBTYPE, &subType);

                char buffer[128];
                std::string output_file = config::output_dir;
                std::string exrOutputPath;

                {
                    // EVE1G: project/shot/take naming; the extension follows the Voukoder container (mov, mkv...)
                    std::string container(config::encoder_config.format.container,
                                          strnlen(config::encoder_config.format.container,
                                                  sizeof(config::encoder_config.format.container)));
                    if (container.empty()) {
                        container = "mp4";
                    }
                    output_file = eve::BuildOutputBase(config::output_dir, container);
                    exrOutputPath = std::regex_replace(output_file, std::regex("\\\\+"), "\\");
                    output_file += "." + container;
                }

                std::string filename = std::regex_replace(output_file, std::regex("\\\\+"), "\\");

                LOG(LL_NFO, "Output file: ", filename);

                DXGI_SWAP_CHAIN_DESC desc;
                ::exportContext->p_swap_chain->GetDesc(&desc);

                // EVE1G: size the video from what is actually captured (the back buffer the upscaler
                // presents, or VideoEncode), not from the swap chain description, which can differ.
                const auto captureSize = eve::CaptureSize(::exportContext->p_export_render_target.Get(),
                                                          desc.BufferDesc.Width, desc.BufferDesc.Height);
                const auto exportWidth = captureSize.first;
                const auto exportHeight = captureSize.second;

                // Get pBackBufferResolved dimensions for OpenEXR export
                D3D11_TEXTURE2D_DESC backBufferCopyDesc{};
                if (pGameBackBufferResolved) {
                    pGameBackBufferResolved->GetDesc(&backBufferCopyDesc);
                }
                const auto openExrWidth = backBufferCopyDesc.Width;
                const auto openExrHeight = backBufferCopyDesc.Height;

                LOG(LL_DBG, "Video Export Resolution: ", exportWidth, "x", exportHeight);
                LOG(LL_DBG, "OpenEXR Export Resolution: ", openExrWidth, "x", openExrHeight);

                g_master16 = eve::WantMaster16(config::encoder_config);
                CreateMotionBlurBuffers(::exportContext->p_device, exportWidth, exportHeight);

                // EVE1G: try the chosen format; if Voukoder refuses it (e.g. a format its FFmpeg cannot encode),
                // fall back to formats that work instead of cancelling the export.
                std::vector<VKENCODERCONFIG> attempts{config::encoder_config};
                for (const VKENCODERCONFIG& fb : eve::FallbackConfigs(config::encoder_config)) {
                    attempts.push_back(fb);
                }
                bool opened = false;
                for (int precisionTry = 0; precisionTry < 2 && !opened; ++precisionTry) {
                if (precisionTry == 1) {
                    if (!g_master16) {
                        break;
                    }
                    LOG(LL_WRN, "EVE1G: no format opened with 16-bit input - trying again with 8-bit input");
                    g_master16 = false;
                    CreateMotionBlurBuffers(::exportContext->p_device, exportWidth, exportHeight);
                }
                for (size_t a = 0; a < attempts.size() && !opened; ++a) {
                    const VKENCODERCONFIG& cfg = attempts[a];
                    std::string container(cfg.format.container,
                                          strnlen(cfg.format.container, sizeof(cfg.format.container)));
                    if (container.empty()) {
                        container = "mp4";
                    }
                    const std::string file = exrOutputPath + "." + container;
                    try {
                        REQUIRE(encodingSession->createContext(cfg, std::wstring(file.begin(), file.end()),
                                                               exportWidth, exportHeight,
                                                               g_master16 ? "rgba64le" : "rgba", fps_num, fps_den,
                                                               numChannels, sampleRate, "s16", blockAlignment,
                                                               config::export_openexr, openExrWidth, openExrHeight),
                                "Failed to create encoding context.");
                        opened = true;
                        filename = file;
                        if (a > 0) {
                            LOG(LL_WRN, "EVE1G: the chosen format did not open - exported with ",
                                std::string(cfg.video.encoder, strnlen(cfg.video.encoder, sizeof(cfg.video.encoder))),
                                " instead. Pick a working format in EVE1G-Settings (Test formats).");
                            config::encoder_config = cfg; // the sidecar reports what was really used
                        }
                    } catch (std::exception& ex) {
                        LOG(LL_WRN, "EVE1G: Voukoder could not open ",
                            std::string(cfg.video.encoder, strnlen(cfg.video.encoder, sizeof(cfg.video.encoder))),
                            " (", ex.what(), ")", a + 1 < attempts.size() ? " - trying the next format" : "");
                        std::error_code ec;
                        if (std::filesystem::exists(std::filesystem::u8path(file), ec) &&
                            std::filesystem::file_size(std::filesystem::u8path(file), ec) == 0) {
                            std::filesystem::remove(std::filesystem::u8path(file), ec);
                        }
                    }
                }
                }
                REQUIRE((opened ? S_OK : E_FAIL), "No video format could be opened by Voukoder.");
                LOG(LL_NON, "EVE1G: master frames are ", g_master16 ? "16-bit (rgba64le)" : "8-bit (rgba)",
                    " into the encoder");
                LOG(LL_NFO, "Output file: ", filename);
                eve::SetExportInfo(exrOutputPath, exportWidth, exportHeight, fps_num, fps_den);
                depth::Begin(exrOutputPath, exportWidth, exportHeight, fps_num, fps_den);
                hfr::Begin(exrOutputPath, exportWidth, exportHeight, fps_num, fps_den);
                passes::Begin(exrOutputPath, exportWidth, exportHeight, fps_num, fps_den, g_master16);
                if (passes::PlateWanted()) {
                    plate::Create(::exportContext->p_device.Get(), exportWidth, exportHeight, g_master16);
                } else {
                    plate::Release();
                }
                g_lastExportFile = filename;
                exportosd::OnExportBegin(U8(std::filesystem::u8path(filename).filename()), exportWidth, exportHeight,
                                         g_master16);
                eve::WriteSidecarStart(filename, exportWidth, exportHeight, !::exportContext->is_audio_export_disabled);
            } catch (std::exception& ex) {
                LOG(LL_ERR, ex.what());
                LOG_CALL(LL_DBG, encodingSession.reset());
                LOG_CALL(LL_DBG, ::exportContext.reset());
                return MF_E_INVALIDMEDIATYPE;
            }
        }
    }

    POST();
    if (g_gameWriterFailed) {
        return S_OK;
    }
    const HRESULT hr = OriginalFunc(pThis, dwStreamIndex, pInputMediaType, pEncodingParameters);
    if (FAILED(hr)) {
        LOG(LL_WRN, "EVE1G: the game's own encoder refused the input format of stream ", dwStreamIndex, " (",
            Logger::hex(static_cast<uint32_t>(hr), 8), ") - ignored, EVE1G writes the video itself");
        g_gameWriterFailed = true;
    }
    return S_OK;
}

HRESULT IMFSinkWriterHooks::WriteSample::Implementation(IMFSinkWriter* pThis, DWORD dwStreamIndex, IMFSample* pSample) {
    std::lock_guard sessionLock(mxSession);

    if ((encodingSession) && (dwStreamIndex == 1) && (!::exportContext->is_audio_export_disabled)) {

        ComPtr<IMFMediaBuffer> pBuffer = nullptr;
        try {
            LONGLONG sampleTime;
            pSample->GetSampleTime(&sampleTime);
            pSample->ConvertToContiguousBuffer(pBuffer.GetAddressOf());

            DWORD length;
            pBuffer->GetCurrentLength(&length);
            BYTE* buffer;
            if (SUCCEEDED(pBuffer->Lock(&buffer, NULL, NULL))) {
                try {
                    LOG_CALL(LL_DBG, encodingSession->writeAudioFrame(
                                         buffer, length / 4 /* 2 channels * 2 bytes per sample*/, sampleTime));
                } catch (std::exception& ex) {
                    LOG(LL_ERR, ex.what());
                }
                pBuffer->Unlock();
            }
        } catch (std::exception& ex) {
            LOG(LL_ERR, ex.what());
            LOG_CALL(LL_DBG, encodingSession.reset());
            LOG_CALL(LL_DBG, ::exportContext.reset());
        }
    }
    return S_OK;
}

HRESULT IMFSinkWriterHooks::Finalize::Implementation(IMFSinkWriter* pThis) {
    PRE();
    std::lock_guard<std::mutex> sessionLock(mxSession);
    try {
        if (encodingSession != NULL) {
            LOG_CALL(LL_DBG, encodingSession->finishAudio());
            LOG_CALL(LL_DBG, encodingSession->finishVideo());
            LOG_CALL(LL_DBG, encodingSession->endSession());
        }
    } catch (std::exception& ex) {
        LOG(LL_ERR, ex.what());
    }

    LOG_CALL(LL_DBG, encodingSession.reset());
    LOG_CALL(LL_DBG, ::exportContext.reset());
    LOG_CALL(LL_DBG, depth::End());
    LOG_CALL(LL_DBG, hfr::End());
    LOG_CALL(LL_DBG, passes::End());
    plate::Release();
    LOG_CALL(LL_DBG, eve::ResetCaptureStats());
    if (!g_lastExportFile.empty()) {
        eve::NotifyExportDone(g_lastExportFile);
        g_lastExportFile.clear();
    }
    POST();
    return S_OK;
}

void eve::finalize() {
    PRE();
    POST();
}

bool eve::OnDraw(reshade::api::command_list* cmd_list, uint32_t vertex_count, uint32_t instance_count,
                 uint32_t first_vertex, uint32_t first_instance) {
    thread_local bool is_recursive_call = false;

    if (is_recursive_call) {
        return true;
    }

    if (pCtxLinearizeBuffer == cmd_list) {
        pCtxLinearizeBuffer = nullptr;

        ComPtr<ID3D11Device> pDevice;
        reinterpret_cast<ID3D11DeviceContext*>(cmd_list->get_native())->GetDevice(pDevice.GetAddressOf());

        ComPtr<ID3D11RenderTargetView> pCurrentRTV;
        LOG_CALL(LL_DBG, reinterpret_cast<ID3D11DeviceContext*>(cmd_list->get_native())
                             ->OMGetRenderTargets(1, pCurrentRTV.GetAddressOf(), NULL));
        D3D11_RENDER_TARGET_VIEW_DESC rtvDesc;
        pCurrentRTV->GetDesc(&rtvDesc);
        LOG_CALL(LL_DBG, pDevice->CreateRenderTargetView(pLinearDepthTexture.Get(), &rtvDesc,
                                                         pCurrentRTV.ReleaseAndGetAddressOf()));
        LOG_CALL(LL_DBG, reinterpret_cast<ID3D11DeviceContext*>(cmd_list->get_native())
                             ->OMSetRenderTargets(1, pCurrentRTV.GetAddressOf(), NULL));

        D3D11_TEXTURE2D_DESC ldtDesc;
        pLinearDepthTexture->GetDesc(&ldtDesc);

        D3D11_VIEWPORT viewport;
        viewport.Width = static_cast<float>(ldtDesc.Width);
        viewport.Height = static_cast<float>(ldtDesc.Height);
        viewport.TopLeftX = 0;
        viewport.TopLeftY = 0;
        viewport.MinDepth = 0;
        viewport.MaxDepth = 1;
        reinterpret_cast<ID3D11DeviceContext*>(cmd_list->get_native())->RSSetViewports(1, &viewport);
        D3D11_RECT rect;
        rect.left = rect.top = 0;
        rect.right = static_cast<LONG>(ldtDesc.Width);
        rect.bottom = static_cast<LONG>(ldtDesc.Height);
        reinterpret_cast<ID3D11DeviceContext*>(cmd_list->get_native())->RSSetScissorRects(1, &rect);

        is_recursive_call = true;
        LOG_CALL(LL_TRC, cmd_list->draw(vertex_count, instance_count, first_vertex, first_instance));
        is_recursive_call = false;
    }

    return true;
}

bool eve::OnDrawIndexed(reshade::api::command_list* cmd_list, uint32_t index_count, uint32_t instance_count,
                        uint32_t first_index, int32_t vertex_offset, uint32_t first_instance) {
    thread_local bool is_recursive_call = false;

    if (is_recursive_call) {
        return true;
    }

    if (pCtxLinearizeBuffer == cmd_list) {
        pCtxLinearizeBuffer = nullptr;

        ComPtr<ID3D11Device> pDevice;
        reinterpret_cast<ID3D11DeviceContext*>(cmd_list->get_native())->GetDevice(pDevice.GetAddressOf());

        ComPtr<ID3D11RenderTargetView> pCurrentRTV;
        LOG_CALL(LL_DBG, reinterpret_cast<ID3D11DeviceContext*>(cmd_list->get_native())
                             ->OMGetRenderTargets(1, pCurrentRTV.GetAddressOf(), NULL));
        D3D11_RENDER_TARGET_VIEW_DESC rtvDesc;
        pCurrentRTV->GetDesc(&rtvDesc);
        LOG_CALL(LL_DBG, pDevice->CreateRenderTargetView(pLinearDepthTexture.Get(), &rtvDesc,
                                                         pCurrentRTV.ReleaseAndGetAddressOf()));
        LOG_CALL(LL_DBG, reinterpret_cast<ID3D11DeviceContext*>(cmd_list->get_native())
                             ->OMSetRenderTargets(1, pCurrentRTV.GetAddressOf(), NULL));

        D3D11_TEXTURE2D_DESC ldtDesc;
        pLinearDepthTexture->GetDesc(&ldtDesc);

        D3D11_VIEWPORT viewport;
        viewport.Width = static_cast<float>(ldtDesc.Width);
        viewport.Height = static_cast<float>(ldtDesc.Height);
        viewport.TopLeftX = 0;
        viewport.TopLeftY = 0;
        viewport.MinDepth = 0;
        viewport.MaxDepth = 1;
        reinterpret_cast<ID3D11DeviceContext*>(cmd_list->get_native())->RSSetViewports(1, &viewport);
        D3D11_RECT rect;
        rect.left = rect.top = 0;
        rect.right = static_cast<LONG>(ldtDesc.Width);
        rect.bottom = static_cast<LONG>(ldtDesc.Height);
        reinterpret_cast<ID3D11DeviceContext*>(cmd_list->get_native())->RSSetScissorRects(1, &rect);

        is_recursive_call = true;
        LOG_CALL(LL_TRC,
                 cmd_list->draw_indexed(index_count, instance_count, first_index, vertex_offset, first_instance));
        is_recursive_call = false;
    }

    return true;
}

HANDLE GameHooks::CreateThread::Implementation(void* pFunc, void* pParams, int32_t r8d, int32_t r9d, void* rsp20,
                                               int32_t rsp28, char* name) {
    PRE();
    static bool d3dHooked = false;
    // if (!d3dHooked) {
    // PerformD3D11Hooks();
    // d3dHooked = true;
    //}
    void* result = GameHooks::CreateThread::OriginalFunc(pFunc, pParams, r8d, r9d, rsp20, rsp28, name);
    LOG(LL_TRC, "CreateThread:", " pFunc:", pFunc, " pParams:", pParams, " r8d:", Logger::hex(r8d, 4),
        " r9d:", Logger::hex(r9d, 4), " rsp20:", rsp20, " rsp28:", rsp28, " name:", name ? name : "<NULL>");
    POST();
    return result;
}

// EVE1G: while an export runs, skip every Scaleform movie (progress bar, "Cancel" button, spinner), so the
// finished back buffer that capture_source = present reads has no UI on it. Outside an export: untouched.
void GameHooks::ScaleformDrawMovie::Implementation(uint32_t movie_id, void* p_pos, void* p_scale, void* p_depth,
                                                   void* p_colour, uint32_t a, uint32_t b, char c, char d) {
    if (::exportContext && config::hide_ui_during_export) {
        return;
    }
    OriginalFunc(movie_id, p_pos, p_scale, p_depth, p_colour, a, b, c, d);
}

float GameHooks::GetRenderTimeBase::Implementation(int64_t choice) {
    PRE();
    const std::pair<int32_t, int32_t> fps = config::fps;
    const float result = 1000.0f * static_cast<float>(fps.second) /
                         (static_cast<float>(fps.first) * (static_cast<float>(config::motion_blur_samples) + 1));
    // float result = 1000.0f / 60.0f;
    LOG(LL_NFO, "Time step: ", result);
    POST();
    return result;
}

// Game time per rendered frame while an export runs (seconds), else 0. Lets ReShade add-ons such as Focus Puller
// animate on export time instead of wall clock, so smoothing looks the same in the file as in playback.
extern "C" __declspec(dllexport) float EVE1G_ExportStepSeconds() {
    if (!::exportContext) {
        return 0.0f;
    }
    const std::pair<int32_t, int32_t> fps = config::fps;
    if (fps.first <= 0 || fps.second <= 0) {
        return 0.0f;
    }
    return static_cast<float>(fps.second) /
           (static_cast<float>(fps.first) * (static_cast<float>(config::motion_blur_samples) + 1));
}

// ---- what the current (or last) export is, for add-ons that write data next to the video (Focus Puller's camera track)
namespace {
std::mutex g_infoMx;
std::string g_exportBase; // video path without extension (UTF-8)
int g_infoW = 0, g_infoH = 0, g_infoNum = 0, g_infoDen = 1, g_infoSamples = 0;
float g_infoShutter = 0.5f;
} // namespace

void eve::SetExportInfo(const std::string& base, uint32_t width, uint32_t height, uint32_t fps_num, uint32_t fps_den) {
    std::lock_guard<std::mutex> lock(g_infoMx);
    g_exportBase = base;
    g_infoW = static_cast<int>(width);
    g_infoH = static_cast<int>(height);
    g_infoNum = static_cast<int>(fps_num);
    g_infoDen = static_cast<int>(fps_den ? fps_den : 1);
    g_infoSamples = config::motion_blur_samples;
    g_infoShutter = config::motion_blur_strength;
}

// The current / last export's video path without extension (UTF-8). Returns its length, 0 = no export yet.
extern "C" __declspec(dllexport) int EVE1G_ExportBase(char* out, int cap) {
    std::lock_guard<std::mutex> lock(g_infoMx);
    if (!out || cap <= 0) {
        return static_cast<int>(g_exportBase.size());
    }
    const int n = static_cast<int>(std::min<size_t>(g_exportBase.size(), static_cast<size_t>(cap - 1)));
    memcpy(out, g_exportBase.data(), n);
    out[n] = 0;
    return n;
}

// The current / last export's picture: frame rate, motion blur samples and shutter (share of the interval), size.
extern "C" __declspec(dllexport) int EVE1G_ExportFormat(int* fps_num, int* fps_den, int* samples, float* shutter,
                                                        int* width, int* height) {
    std::lock_guard<std::mutex> lock(g_infoMx);
    if (fps_num) *fps_num = g_infoNum;
    if (fps_den) *fps_den = g_infoDen;
    if (samples) *samples = g_infoSamples;
    if (shutter) *shutter = g_infoShutter;
    if (width) *width = g_infoW;
    if (height) *height = g_infoH;
    return g_infoNum > 0 ? 1 : 0;
}

// Focus Puller (timeline focus keys): the export time of the game frame being rendered now, in seconds from the start
// of the project export - every game frame is one motion-blur sample, EVE1G_ExportStepSeconds() apart. -1 = no export.
extern "C" __declspec(dllexport) float EVE1G_ExportTimeSeconds() {
    if (!::exportContext) {
        return -1.0f;
    }
    return EVE1G_ExportStepSeconds() * static_cast<float>(g_framesCaptured);
}

void AutoReloadConfig() {
    if (config::auto_reload_config) {
        LOG_CALL(LL_DBG, config::reload());
    }
}

void CreateNewExportContext() {
    encodingSession.reset(new Encoder::Session());
    NOT_NULL(encodingSession, "Could not create the session");
    ::exportContext.reset(new ExportContext());
    NOT_NULL(::exportContext, "Could not create export context");
}

void* GameHooks::CreateTexture::Implementation(void* rcx, char* name, const uint32_t r8d, uint32_t width,
                                               uint32_t height, const uint32_t format, void* rsp30) {
    void* result = OriginalFunc(rcx, name, r8d, width, height, format, rsp30);

    const auto vresult = static_cast<void**>(result);

    ComPtr<ID3D11Texture2D> pTexture;
    DXGI_FORMAT fmt = DXGI_FORMAT::DXGI_FORMAT_UNKNOWN;
    if (name && result) {
        if (const auto pUnknown = static_cast<IUnknown*>(*(vresult + 7));
            pUnknown && SUCCEEDED(pUnknown->QueryInterface(pTexture.GetAddressOf()))) {
            D3D11_TEXTURE2D_DESC desc;
            pTexture->GetDesc(&desc);
            fmt = desc.Format;
        }
    }

    LOG(LL_DBG, "CreateTexture:", " rcx:", rcx, " name:", name ? name : "<NULL>", " r8d:", Logger::hex(r8d, 8),
        " width:", width, " height:", height, " fmt:", conv_dxgi_format_to_string(fmt), " result:", result,
        " *r+0:", vresult ? *(vresult + 0) : "<NULL>", " *r+1:", vresult ? *(vresult + 1) : "<NULL>",
        " *r+2:", vresult ? *(vresult + 2) : "<NULL>", " *r+3:", vresult ? *(vresult + 3) : "<NULL>",
        " *r+4:", vresult ? *(vresult + 4) : "<NULL>", " *r+5:", vresult ? *(vresult + 5) : "<NULL>",
        " *r+6:", vresult ? *(vresult + 6) : "<NULL>", " *r+7:", vresult ? *(vresult + 7) : "<NULL>",
        " *r+8:", vresult ? *(vresult + 8) : "<NULL>", " *r+9:", vresult ? *(vresult + 9) : "<NULL>",
        " *r+10:", vresult ? *(vresult + 10) : "<NULL>", " *r+11:", vresult ? *(vresult + 11) : "<NULL>",
        " *r+12:", vresult ? *(vresult + 12) : "<NULL>", " *r+13:", vresult ? *(vresult + 13) : "<NULL>",
        " *r+14:", vresult ? *(vresult + 14) : "<NULL>", " *r+15:", vresult ? *(vresult + 15) : "<NULL>");

    if (pTexture && name) {
        ComPtr<ID3D11Device> pDevice;
        pTexture->GetDevice(pDevice.GetAddressOf());
        if ((std::strcmp("DepthBuffer_Resolved", name) == 0) || (std::strcmp("DepthBufferCopy", name) == 0)) {
            pGameDepthBufferResolved = pTexture;
        } else if (std::strcmp("DepthBuffer", name) == 0) {
            pGameDepthBuffer = pTexture;
        } else if (std::strcmp("Depth Quarter", name) == 0) {
            pGameDepthBufferQuarter = pTexture;
        } else if (std::strcmp("GBUFFER_0", name) == 0) {
            pGameGBuffer0 = pTexture;
        } else if (std::strcmp("Edge Copy", name) == 0) {
            pGameEdgeCopy = pTexture;
            D3D11_TEXTURE2D_DESC desc;
            pTexture->GetDesc(&desc);
            REQUIRE(pDevice->CreateTexture2D(&desc, nullptr, pStencilTexture.GetAddressOf()),
                    "Failed to create stencil texture");
        } else if (std::strcmp("Depth Quarter Linear", name) == 0) {
            pGameDepthBufferQuarterLinear = pTexture;
            D3D11_TEXTURE2D_DESC desc, resolvedDesc;

            if (!pGameDepthBufferResolved) {
                pGameDepthBufferResolved = pGameDepthBuffer;
            }

            pGameDepthBufferResolved->GetDesc(&resolvedDesc);
            resolvedDesc.ArraySize = 1;
            resolvedDesc.SampleDesc.Count = 1;
            resolvedDesc.SampleDesc.Quality = 0;
            pGameDepthBufferQuarterLinear->GetDesc(&desc);
            desc.Width = resolvedDesc.Width;
            desc.Height = resolvedDesc.Height;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.CPUAccessFlags = 0;
            LOG_CALL(LL_DBG, pDevice->CreateTexture2D(&desc, NULL, pLinearDepthTexture.GetAddressOf()));
        } else if (std::strcmp("BackBuffer", name) == 0) {
            pGameBackBufferResolved = nullptr;
            pGameDepthBuffer = nullptr;
            pGameDepthBufferQuarter = nullptr;
            pGameDepthBufferQuarterLinear = nullptr;
            pGameDepthBufferResolved = nullptr;
            pGameEdgeCopy = nullptr;
            pGameGBuffer0 = nullptr;

            pGameBackBuffer = pTexture;

        } else if ((std::strcmp("BackBuffer_Resolved", name) == 0) || (std::strcmp("BackBufferCopy", name) == 0)) {
            pGameBackBufferResolved = pTexture;
        } else if (std::strcmp("VideoEncode", name) == 0) {
            std::lock_guard<std::mutex> sessionLock(mxSession);
            const ComPtr<ID3D11Texture2D>& pExportTexture = pTexture;

            D3D11_TEXTURE2D_DESC desc;
            pExportTexture->GetDesc(&desc);

            LOG_CALL(LL_DBG, ::exportContext.reset());
            LOG_CALL(LL_DBG, encodingSession.reset());
            try {
                LOG(LL_NFO, "Creating session...");

                AutoReloadConfig();
                eve::ResetCaptureStats();
                CreateNewExportContext();
                ::exportContext->p_swap_chain = mainSwapChain;
                ::exportContext->p_export_render_target = pExportTexture;

                pExportTexture->GetDevice(::exportContext->p_device.GetAddressOf());
                ::exportContext->p_device->GetImmediateContext(::exportContext->p_device_context.GetAddressOf());

            } catch (std::exception& ex) {
                LOG(LL_ERR, ex.what());
                LOG_CALL(LL_DBG, encodingSession.reset());
                LOG_CALL(LL_DBG, ::exportContext.reset());
            }
        }
    }

    return result;
}

void eve::PrepareDeferredContext(ID3D11Device* p_device) {
    REQUIRE(p_device->CreateDeferredContext(0, pDContext.GetAddressOf()), "Failed to create deferred context");
    REQUIRE(p_device->CreateVertexShader(VSFullScreen::g_main, sizeof(VSFullScreen::g_main), NULL,
                                         pVsFullScreen.GetAddressOf()),
            "Failed to create g_VSFullScreen vertex shader");
    REQUIRE(p_device->CreatePixelShader(PSAccumulate::g_main, sizeof(PSAccumulate::g_main), NULL,
                                        pPsAccumulate.GetAddressOf()),
            "Failed to create pixel shader");
    REQUIRE(p_device->CreatePixelShader(PSDivide::g_main, sizeof(PSDivide::g_main), NULL, pPsDivide.GetAddressOf()),
            "Failed to create pixel shader");

    D3D11_BUFFER_DESC clipBufDesc{};
    clipBufDesc.BindFlags = D3D11_BIND_FLAG::D3D11_BIND_CONSTANT_BUFFER;
    clipBufDesc.ByteWidth = sizeof(XMFLOAT4);
    clipBufDesc.CPUAccessFlags = D3D11_CPU_ACCESS_FLAG::D3D11_CPU_ACCESS_WRITE;
    clipBufDesc.MiscFlags = 0;
    clipBufDesc.StructureByteStride = 0;
    clipBufDesc.Usage = D3D11_USAGE::D3D11_USAGE_DYNAMIC;

    REQUIRE(p_device->CreateBuffer(&clipBufDesc, NULL, pDivideConstantBuffer.GetAddressOf()),
            "Failed to create constant buffer for pixel shader");
    {
        D3D11_BUFFER_DESC accParams = clipBufDesc;
        accParams.ByteWidth = 2 * sizeof(XMFLOAT4);
        REQUIRE(p_device->CreateBuffer(&accParams, NULL, pAccParamsBuffer.GetAddressOf()),
                "Failed to create the exposure constant buffer");
    }

    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER::D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_MODE::D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_MODE::D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_MODE::D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.MipLODBias = 0.0f;
    samplerDesc.MaxAnisotropy = 1;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_FUNC::D3D11_COMPARISON_NEVER;
    samplerDesc.BorderColor[0] = 1.0f;
    samplerDesc.BorderColor[1] = 1.0f;
    samplerDesc.BorderColor[2] = 1.0f;
    samplerDesc.BorderColor[3] = 1.0f;
    samplerDesc.MinLOD = -FLT_MAX;
    samplerDesc.MaxLOD = FLT_MAX;

    REQUIRE(p_device->CreateSamplerState(&samplerDesc, pPsSampler.GetAddressOf()), "Failed to create sampler state");

    // EVE1G: fully initialised, no culling. The original left FrontCounterClockwise (and the depth-bias fields)
    // uninitialised with CullMode = FRONT: whether the full-screen quad was drawn at all depended on leftover stack
    // values - with the wrong ones every accumulation draw was culled and the video came out black.
    D3D11_RASTERIZER_DESC rasterStateDesc{};
    rasterStateDesc.AntialiasedLineEnable = FALSE;
    rasterStateDesc.CullMode = D3D11_CULL_NONE;
    rasterStateDesc.FrontCounterClockwise = FALSE;
    rasterStateDesc.DepthClipEnable = FALSE;
    rasterStateDesc.FillMode = D3D11_FILL_MODE::D3D11_FILL_SOLID;
    rasterStateDesc.MultisampleEnable = FALSE;
    rasterStateDesc.ScissorEnable = FALSE;
    REQUIRE(p_device->CreateRasterizerState(&rasterStateDesc, pRasterState.GetAddressOf()),
            "Failed to create raster state");

    D3D11_BLEND_DESC blendDesc{};
    blendDesc.AlphaToCoverageEnable = FALSE;
    blendDesc.IndependentBlendEnable = FALSE;
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE; // alpha adds up the exposure weights
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    REQUIRE(p_device->CreateBlendState(&blendDesc, pAccBlendState.GetAddressOf()),
            "Failed to create accumulation blend state");
}

void eve::drawAdditive(ComPtr<ID3D11Device> pDevice, ComPtr<ID3D11DeviceContext> pContext,
                       ComPtr<ID3D11Texture2D> pSource) {
    D3D11_TEXTURE2D_DESC desc;
    pSource->GetDesc(&desc);

    LOG_CALL(LL_DBG, pDContext->ClearState());
    LOG_CALL(LL_DBG, pDContext->IASetIndexBuffer(NULL, DXGI_FORMAT::DXGI_FORMAT_UNKNOWN, 0));
    LOG_CALL(LL_DBG, pDContext->IASetInputLayout(NULL));
    LOG_CALL(LL_DBG, pDContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP));

    LOG_CALL(LL_DBG, pDContext->VSSetShader(pVsFullScreen.Get(), NULL, 0));
    LOG_CALL(LL_DBG, pDContext->PSSetSamplers(0, 1, pPsSampler.GetAddressOf()));
    LOG_CALL(LL_DBG, pDContext->PSSetShader(pPsAccumulate.Get(), NULL, 0));
    exposure::Bind(desc.Height, desc.Format);
    LOG_CALL(LL_DBG, pDContext->RSSetState(pRasterState.Get()));

    float factors[4] = {0, 0, 0, 0};

    LOG_CALL(LL_DBG, pDContext->OMSetBlendState(pAccBlendState.Get(), factors, 0xFFFFFFFF));

    D3D11_VIEWPORT viewport;
    viewport.TopLeftX = 0;
    viewport.TopLeftY = 0;
    viewport.Width = static_cast<float>(desc.Width);
    viewport.Height = static_cast<float>(desc.Height);
    viewport.MinDepth = 0;
    viewport.MaxDepth = 1;
    LOG_CALL(LL_DBG, pDContext->RSSetViewports(1, &viewport));

    LOG_CALL(LL_DBG, pDContext->CopyResource(pSourceSrvTexture.Get(), pSource.Get()));

    LOG_CALL(LL_DBG, pDContext->PSSetShaderResources(0, 1, pSourceSrv.GetAddressOf()));
    LOG_CALL(LL_DBG, pDContext->OMSetRenderTargets(1, pRtvAccBuffer.GetAddressOf(), nullptr));
    if (::exportContext->acc_count == 0) {
        float color[4] = {0, 0, 0, 0};
        LOG_CALL(LL_DBG, pDContext->ClearRenderTargetView(pRtvAccBuffer.Get(), color));
    }

    LOG_CALL(LL_DBG, pDContext->Draw(4, 0));

    ComPtr<ID3D11CommandList> pCmdList;
    LOG_CALL(LL_DBG, pDContext->FinishCommandList(FALSE, pCmdList.GetAddressOf()));
    LOG_CALL(LL_DBG, pContext->ExecuteCommandList(pCmdList.Get(), TRUE));

    ::exportContext->acc_count++;
}

ComPtr<ID3D11Texture2D> eve::divideBuffer(ComPtr<ID3D11Device> pDevice, ComPtr<ID3D11DeviceContext> pContext,
                                          uint32_t k) {

    D3D11_TEXTURE2D_DESC desc;
    pMotionBlurFinalBuffer->GetDesc(&desc);

    LOG_CALL(LL_DBG, pDContext->ClearState());
    LOG_CALL(LL_DBG, pDContext->IASetIndexBuffer(NULL, DXGI_FORMAT::DXGI_FORMAT_UNKNOWN, 0));
    LOG_CALL(LL_DBG, pDContext->IASetInputLayout(NULL));
    LOG_CALL(LL_DBG, pDContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP));

    LOG_CALL(LL_DBG, pDContext->VSSetShader(pVsFullScreen.Get(), NULL, 0));
    LOG_CALL(LL_DBG, pDContext->PSSetShader(pPsDivide.Get(), NULL, 0));

    D3D11_MAPPED_SUBRESOURCE mapped;
    REQUIRE(pDContext->Map(pDivideConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped),
            "Failed to map divide shader constant buffer");
    ;
    const auto floats = static_cast<float*>(mapped.pData);
    floats[0] = static_cast<float>(k);
    floats[1] = exposure::g_linear ? 1.0f : 0.0f; // back from linear light to the display image
    /*floats[0] = 1.0f;*/
    LOG_CALL(LL_DBG, pDContext->Unmap(pDivideConstantBuffer.Get(), 0));

    LOG_CALL(LL_DBG, pDContext->PSSetConstantBuffers(0, 1, pDivideConstantBuffer.GetAddressOf()));
    LOG_CALL(LL_DBG, pDContext->PSSetShaderResources(0, 1, pSrvAccBuffer.GetAddressOf()));
    LOG_CALL(LL_DBG, pDContext->RSSetState(pRasterState.Get()));

    D3D11_VIEWPORT viewport;
    viewport.TopLeftX = 0;
    viewport.TopLeftY = 0;
    viewport.Width = static_cast<float>(desc.Width);
    viewport.Height = static_cast<float>(desc.Height);
    viewport.MinDepth = 0;
    viewport.MaxDepth = 1;
    LOG_CALL(LL_DBG, pDContext->RSSetViewports(1, &viewport));

    LOG_CALL(LL_DBG, pDContext->OMSetRenderTargets(1, pRtvBlurBuffer.GetAddressOf(), NULL));
    constexpr float color[4] = {0, 0, 0, 1};
    LOG_CALL(LL_DBG, pDContext->ClearRenderTargetView(pRtvBlurBuffer.Get(), color));

    LOG_CALL(LL_DBG, pDContext->Draw(4, 0));

    ComPtr<ID3D11CommandList> pCmdList;
    LOG_CALL(LL_DBG, pDContext->FinishCommandList(FALSE, pCmdList.GetAddressOf()));
    LOG_CALL(LL_DBG, pContext->ExecuteCommandList(pCmdList.Get(), TRUE));

    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;

    ComPtr<ID3D11Texture2D> ret;
    REQUIRE(pDevice->CreateTexture2D(&desc, NULL, ret.GetAddressOf()),
            "Failed to create copy of motion blur dest texture");
    LOG_CALL(LL_DBG, pContext->CopyResource(ret.Get(), pMotionBlurFinalBuffer.Get()));

    return ret;
}