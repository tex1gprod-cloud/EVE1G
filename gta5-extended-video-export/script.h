#pragma once

#define NOMINMAX

#include <d3d11.h>
#include <dxgi.h>
#include <future>
#include <queue>
#include <utility>
#include <vector>
#include <wrl/client.h>

#include "reshade.hpp"
#include "VoukoderTypeLib_h.h"

using namespace Microsoft::WRL;

template <class T> void SafeRelease(T** ppT) {
    if (*ppT) {
        (*ppT)->Release();
        *ppT = NULL;
    }
}

template <class B, class A> B ForceCast(A a) {
    union {
        A a;
        B b;
    } x;
    x.a = a;
    return x.b;
}

namespace eve {
void initialize();

// EVE1G capture path (see script.cpp)
void QueueExportFrame(ComPtr<ID3D11Device> pDevice, ComPtr<ID3D11DeviceContext> pContext);
void OnReshadePresent(reshade::api::effect_runtime* runtime);
void OnReshadeFinishEffects(reshade::api::effect_runtime* runtime, reshade::api::command_list* cmd_list,
                            reshade::api::resource_view rtv, reshade::api::resource_view rtv_srgb);
void CaptureClean(ID3D11Device* pDevice, ID3D11DeviceContext* pContext, reshade::api::command_list* cmd_list);
void OnReshadeBeginEffects(reshade::api::effect_runtime* runtime, reshade::api::command_list* cmd_list,
                           reshade::api::resource_view rtv, reshade::api::resource_view rtv_srgb);
void OnInitEffectRuntime(reshade::api::effect_runtime* runtime);
void OnDestroyEffectRuntime(reshade::api::effect_runtime* runtime);
void ResetCaptureStats();
std::string BuildOutputBase(const std::string& dir, const std::string& container);
std::vector<VKENCODERCONFIG> FallbackConfigs(const VKENCODERCONFIG& chosen);
void WriteSidecarStart(const std::string& videoFile, uint32_t width, uint32_t height, bool audio);
// What the export is, for EVE1G_ExportBase / EVE1G_ExportFormat (add-ons writing data next to the video).
void SetExportInfo(const std::string& base, uint32_t width, uint32_t height, uint32_t fps_num, uint32_t fps_den);
void WriteSidecarEnd(uint64_t frames, uint64_t withPreset, uint64_t withoutPreset, uint32_t passes);
// master_16bit: should this encoder get 16-bit frames (auto = for 10/12-bit and lossless codecs)?
bool WantMaster16(const VKENCODERCONFIG& cfg);
// Sound, taskbar flash and an on-screen note when an export has finished (notify_done).
void NotifyExportDone(const std::string& videoFile);
std::pair<uint32_t, uint32_t> CaptureSize(ID3D11Texture2D* pVideoEncode, uint32_t fallbackW, uint32_t fallbackH);
void OnPresent(reshade::api::command_queue* queue, reshade::api::swapchain* swapchain,
               const reshade::api::rect* source_rect, const reshade::api::rect* dest_rect, uint32_t dirty_rect_count,
               const reshade::api::rect* dirty_rects);

bool OnDraw(reshade::api::command_list* cmd_list, uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex,
            uint32_t first_instance);

bool OnDrawIndexed(reshade::api::command_list* cmd_list, uint32_t index_count, uint32_t instance_count,
                   uint32_t first_index, int32_t vertex_offset, uint32_t first_instance);

void OnBindRenderTargets(reshade::api::command_list* cmd_list, uint32_t count,
                         const reshade::api::resource_view* render_targets,
                         reshade::api::resource_view depth_stencil);

void OnInitDevice(reshade::api::device* device);
void OnInitSwapChain(reshade::api::swapchain* swapchain);

// void OnPresent(IDXGISwapChain* p_swap_chain);
void PresentCallback(void* p_swap_chain);

void finalize();
static void PrepareDeferredContext(ID3D11Device* p_device);
static ComPtr<ID3D11Texture2D> divideBuffer(ComPtr<ID3D11Device> pDevice, ComPtr<ID3D11DeviceContext> pContext,
                                            uint32_t k);
static void drawAdditive(ComPtr<ID3D11Device> pDevice, ComPtr<ID3D11DeviceContext> pContext,
                         ComPtr<ID3D11Texture2D> pSource);
} // namespace eve
