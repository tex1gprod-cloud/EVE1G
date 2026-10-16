#include "version.h"
#include "encoder.h"
#include "export_osd.h"
#include "logger.h"
#include "polyhook2/ErrorLog.hpp"
#include "reshade.hpp"
#include "script.h"
#include "stdafx.h"

#include <filesystem>

class PolyHookLogger : public PLH::Logger {
    void log(const std::string& msg, PLH::ErrorLevel level) override {
        switch (level) {
        case PLH::ErrorLevel::INFO:
            LOG(LL_TRC, msg);
            break;
        case PLH::ErrorLevel::WARN:
            LOG(LL_WRN, msg);
            break;
        case PLH::ErrorLevel::SEV:
            LOG(LL_ERR, msg);
            break;
        case PLH::ErrorLevel::NONE:
        default:
            LOG(LL_DBG, msg);
            break;
        }
    }
};

// Name and description ReShade shows in its Add-ons tab.
extern "C" __declspec(dllexport) const char* NAME = "EVE1G";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "v" EVE1G_VERSION " by TEX1G - Rockstar Editor video export with ReShade, motion blur and clean (UI-free) capture.";

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        LOG(LL_NON, std::filesystem::current_path());
        SetDllDirectoryW(utf8_decode(AsiPath() + R"(\EVE1G\dlls\)").c_str());
        LOG(LL_DBG, "Initializing PolyHook logger");
        PLH::Log::registerLogger(std::make_unique<PolyHookLogger>());
        config::reload();
        if (!config::is_mod_enabled) {
            LOG(LL_NON, "EVE1G is disabled in EVE1G.ini (enable_mod). Exiting...");
            return TRUE;
        }

        LOG(LL_NON, "EVE1G " EVE1G_VERSION " is enabled. Initializing...");
        if (GetModuleHandleW(L"ExtendedVideoExport.asi") != nullptr) {
            LOG(LL_ERR, "The old ExtendedVideoExport.asi is also loaded. Delete it from the plugins folder - two "
                        "exporters at once will break exports.");
        }

        if (!reshade::register_addon(hModule)) {
            LOG(LL_ERR, "Failed to register ReShade addon");
            return FALSE;
        }

        Logger::instance().level = config::log_level;
        LOG_CALL(LL_DBG, eve::initialize());
        exportosd::Register(); // export progress on screen (never in the video)
        LOG(LL_NFO, "Registering script...");
        LOG_CALL(LL_NFO, reshade::register_event<reshade::addon_event::init_device>(&eve::OnInitDevice));
        LOG_CALL(LL_NFO, reshade::register_event<reshade::addon_event::init_swapchain>(&eve::OnInitSwapChain));
        LOG_CALL(LL_NFO, reshade::register_event<reshade::addon_event::present>(&eve::OnPresent));
        LOG_CALL(LL_NFO, reshade::register_event<reshade::addon_event::init_effect_runtime>(&eve::OnInitEffectRuntime));
        LOG_CALL(LL_NFO, reshade::register_event<reshade::addon_event::destroy_effect_runtime>(&eve::OnDestroyEffectRuntime));
        LOG_CALL(LL_NFO, reshade::register_event<reshade::addon_event::reshade_present>(&eve::OnReshadePresent));
        LOG_CALL(LL_NFO, reshade::register_event<reshade::addon_event::reshade_begin_effects>(&eve::OnReshadeBeginEffects));
        LOG_CALL(LL_NFO, reshade::register_event<reshade::addon_event::reshade_finish_effects>(&eve::OnReshadeFinishEffects));
        //LOG_CALL(LL_NFO, reshade::register_event<reshade::addon_event::draw>(&eve::OnDraw));
        //LOG_CALL(LL_NFO, reshade::register_event<reshade::addon_event::draw_indexed>(&eve::OnDrawIndexed));
        LOG_CALL(LL_NFO, reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(&eve::OnBindRenderTargets));
        break;
    case DLL_PROCESS_DETACH:
        LOG_CALL(LL_DBG, eve::finalize());
        exportosd::Unregister();
        reshade::unregister_addon(hModule);
        break;
    }
    return TRUE;
}
