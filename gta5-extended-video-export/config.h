#pragma once

#ifndef _EVE_CONFIG_H_
#define _EVE_CONFIG_H_

#include "VoukoderTypeLib_h.h"
#include "logger.h"
#include "util.h"

#include <INIReader.h>
#include <ShlObj.h>
#include <algorithm>
#include <memory>
#include <nlohmann/json.hpp>
#include <regex>
#include <sstream>
#include <string>

#define CFG_AUTO_RELOAD_CONFIG "auto_reload_config"
#define CFG_ENABLE_XVX "enable_mod"
#define CFG_OUTPUT_DIR "output_folder"

#define CFG_EXPORT_SECTION "EXPORT"
#define CFG_EXPORT_MB_SAMPLES "motion_blur_samples"
#define CFG_EXPORT_MB_STRENGTH "motion_blur_strength"
#define CFG_EXPORT_FPS "fps"
#define CFG_EXPORT_OPENEXR "export_openexr"
#define CFG_EXPORT_CAPTURE "capture_source"
#define CFG_EXPORT_HIDE_UI "hide_ui"
#define CFG_EXPORT_CLEAN_RESHADE "clean_reshade"
#define CFG_EXPORT_CLEAN_PREVIEW "clean_preview"

#define CFG_FORMAT_SECTION "FORMAT"
#define CFG_EXPORT_FORMAT "format"
#define CFG_FORMAT_EXT "extension"
#define CFG_FORMAT_CFG "options"

#define CFG_LOG_LEVEL "log_level"
#define CFG_VIDEO_SECTION "VIDEO"
#define CFG_VIDEO_ENC "encoder"
#define CFG_VIDEO_FMT "pixel_format"
#define CFG_VIDEO_CFG "options"

#define CFG_AUDIO_SECTION "AUDIO"
#define CFG_AUDIO_ENC "encoder"
#define CFG_AUDIO_FMT "sample_format"
#define CFG_AUDIO_CFG "options"

#define INI_FILE_NAME "EVE1G\\" TARGET_NAME ".ini"
#define PRESET_FILE_NAME "EVE1G\\preset.json"

class config {
  public:
    // EVE1G: where the exported image is read from (see script.cpp)
    enum class CaptureSource { Clean, Present, Encode, Effects, Legacy };
    static CaptureSource capture_source;
    static bool hide_ui_during_export;
    static bool clean_apply_reshade;
    static bool clean_preview_effects;
    static bool progress_osd; // export progress on screen (never in the video)
    static float osd_scale;   // its size: x the screen height / 1080 (so it grows with 4K), 1 = normal
    // the camera exposure (motion blur), see shaders/PSAccumulate.hlsl
    static bool blend_linear;        // blend in linear light, like a sensor (else the display image, as before)
    static float shutter_soft;       // share of the exposure faded in / out (0 = hard shutter edges)
    static float rolling_shutter_ms; // CMOS line-by-line readout time (0 = global shutter)
    // HFR export ([HFR] in EVE1G.ini, see hfr_pass.h)
    static bool hfr_enabled;
    static std::string hfr_format; // prores | png
    static int hfr_every;          // keep every Nth game frame
    // shot naming + sidecar (root section of EVE1G.ini)
    static std::string name_project;
    static std::string name_shot;
    static int name_take;      // 0 = next free take number
    static bool write_sidecar;
    // depth pass ([DEPTH] in EVE1G.ini, see depth_pass.h)
    static bool depth_enabled;
    static std::string depth_format; // png16 | prores | exr
    static float depth_near;         // metres
    static float depth_far;          // metres
    static bool depth_log_curve;     // log (more detail close up) or linear
    static bool depth_white_near;    // white = near (else white = far)
    // extra passes ([PASSES] in EVE1G.ini, see passes.h)
    static bool plate_enabled;   // clean plate: the same shot without the ReShade preset (capture = clean)
    static bool proxy_enabled;   // half-size H.264 proxy for fast editing
    static bool normals_enabled; // surface normals from the depth (format follows [DEPTH] format)
    static bool id_enabled;      // experimental: the game's stencil buffer as colour IDs (PNG sequence)
    static float normals_fov;    // vertical field of view for the normals when Focus Puller is not there (degrees)
    // master precision: 0 = off (8-bit), 1 = on (16-bit), 2 = auto (16-bit for 10/12-bit and lossless codecs)
    static int master_16bit;
    static bool notify_done;     // sound + taskbar flash + on-screen note when an export finishes

    static bool is_mod_enabled;
    static bool auto_reload_config;
    static bool export_openexr;
    static std::string output_dir;
    static LogLevel log_level;
    static std::pair<uint32_t, uint32_t> fps;
    static uint8_t motion_blur_samples;
    static float motion_blur_strength;
    static VKENCODERCONFIG encoder_config;

    static void reload() {
        config_parser = std::make_shared<INIReader>(AsiPath() + "\\" INI_FILE_NAME);

        is_mod_enabled = parse_lossless_export();
        auto_reload_config = parse_auto_reload_config();
        output_dir = parse_output_dir();
        log_level = parse_log_level();
        fps = parse_fps();
        motion_blur_samples = parse_motion_blur_samples();
        motion_blur_strength = parse_motion_blur_strength();
        export_openexr = parse_export_openexr();
        capture_source = parse_capture_source();
        {
            const std::string s = toLower(getTrimmed(config_parser, CFG_EXPORT_HIDE_UI, CFG_EXPORT_SECTION));
            hide_ui_during_export = !(s == "false" || s == "0" || s == "no");
            succeeded(CFG_EXPORT_HIDE_UI, hide_ui_during_export);
        }
        {
            const std::string s2 = toLower(getTrimmed(config_parser, CFG_EXPORT_CLEAN_RESHADE, CFG_EXPORT_SECTION));
            clean_apply_reshade = !(s2 == "false" || s2 == "0" || s2 == "no");
            succeeded(CFG_EXPORT_CLEAN_RESHADE, clean_apply_reshade);
        }
        {
            const std::string s3 = toLower(getTrimmed(config_parser, CFG_EXPORT_CLEAN_PREVIEW, CFG_EXPORT_SECTION));
            clean_preview_effects = !(s3 == "false" || s3 == "0" || s3 == "no" || s3 == "ui");
            succeeded(CFG_EXPORT_CLEAN_PREVIEW, clean_preview_effects);
        }
        {
            const std::string s5 = toLower(getTrimmed(config_parser, "progress_osd", CFG_EXPORT_SECTION));
            progress_osd = !(s5 == "false" || s5 == "0" || s5 == "no");
            osd_scale = std::clamp(static_cast<float>(config_parser->GetReal(CFG_EXPORT_SECTION, "osd_scale", 1.0)),
                                   0.5f, 4.0f);
        }
        {
            const std::string s6 = toLower(getTrimmed(config_parser, "blend_linear", CFG_EXPORT_SECTION));
            blend_linear = (s6 == "true" || s6 == "1" || s6 == "yes"); // off unless switched on
            shutter_soft = std::clamp(static_cast<float>(config_parser->GetReal(CFG_EXPORT_SECTION, "shutter_soft", 0.0)),
                                      0.0f, 1.0f);
            rolling_shutter_ms = std::clamp(
                static_cast<float>(config_parser->GetReal(CFG_EXPORT_SECTION, "rolling_shutter_ms", 0.0)), 0.0f, 100.0f);
            LOG(LL_NON, "Camera: ", blend_linear ? "linear-light blend" : "display blend", ", soft shutter edges ",
                shutter_soft, ", rolling shutter ", rolling_shutter_ms, " ms");
        }
        {
            hfr_enabled = config_parser->GetBoolean("HFR", "enabled", false);
            hfr_format = toLower(getTrimmed(config_parser, "format", "HFR"));
            if (hfr_format != "png") {
                hfr_format = "prores";
            }
            hfr_every = std::clamp(static_cast<int>(config_parser->GetInteger("HFR", "every", 1)), 1, 64);
            LOG(LL_NON, "HFR export: ", hfr_enabled ? "on" : "off", " format=", hfr_format, " every=", hfr_every);
        }

        name_project = getTrimmed(config_parser, "project", "");
        name_shot = getTrimmed(config_parser, "shot", "");
        name_take = static_cast<int>(config_parser->GetInteger("", "take", 0));
        {
            const std::string s4 = toLower(getTrimmed(config_parser, "sidecar", ""));
            write_sidecar = !(s4 == "false" || s4 == "0" || s4 == "no");
        }
        {
            depth_enabled = config_parser->GetBoolean("DEPTH", "enabled", false);
            depth_format = toLower(getTrimmed(config_parser, "format", "DEPTH"));
            if (depth_format != "prores" && depth_format != "exr") {
                depth_format = "png16";
            }
            depth_near = static_cast<float>(config_parser->GetReal("DEPTH", "near", 0.5));
            depth_far = static_cast<float>(config_parser->GetReal("DEPTH", "far", 200.0));
            depth_log_curve = toLower(getTrimmed(config_parser, "curve", "DEPTH")) != "linear";
            depth_white_near = toLower(getTrimmed(config_parser, "white", "DEPTH")) != "far";
            LOG(LL_NON, "Depth pass: ", depth_enabled ? "on" : "off", " format=", depth_format, " near=", depth_near,
                " far=", depth_far, " curve=", depth_log_curve ? "log" : "linear", " white=",
                depth_white_near ? "near" : "far");
        }
        {
            plate_enabled = config_parser->GetBoolean("PASSES", "clean_plate", false);
            proxy_enabled = config_parser->GetBoolean("PASSES", "proxy", false);
            normals_enabled = config_parser->GetBoolean("PASSES", "normals", false);
            id_enabled = config_parser->GetBoolean("PASSES", "id_pass", false);
            normals_fov = std::clamp(static_cast<float>(config_parser->GetReal("PASSES", "normals_fov", 50.0)), 5.0f,
                                     150.0f);
            const std::string m = toLower(getTrimmed(config_parser, "master_16bit", CFG_EXPORT_SECTION));
            master_16bit = (m == "true" || m == "1" || m == "on" || m == "yes")    ? 1
                           : (m == "auto") ? 2
                                                                                   : 0; // 8-bit unless switched on
            const std::string n = toLower(getTrimmed(config_parser, "notify_done", CFG_EXPORT_SECTION));
            notify_done = (n == "true" || n == "1" || n == "yes" || n == "on");
            LOG(LL_NON, "Passes: clean plate=", plate_enabled ? 1 : 0, " proxy=", proxy_enabled ? 1 : 0,
                " normals=", normals_enabled ? 1 : 0, " id=", id_enabled ? 1 : 0, " master_16bit=", master_16bit,
                " notify_done=", notify_done ? 1 : 0);
        }
        LOG(LL_NON, "Naming: project=\"", name_project, "\" shot=\"", name_shot, "\" take=", name_take,
            " sidecar=", write_sidecar ? 1 : 0);

        readEncoderConfig();
    }

    static void readEncoderConfig() {
        try {
            std::ifstream ifs(AsiPath() + "\\" PRESET_FILE_NAME);

            nlohmann::json j = nlohmann::json::parse(ifs);

            encoder_config = {};

            encoder_config.version = j["version"];

            j["format"]["container"].get<std::string>().copy(encoder_config.format.container,
                                                             sizeof(encoder_config.format.container));
            encoder_config.format.faststart = j["format"]["faststart"];

            j["video"]["encoder"].get<std::string>().copy(encoder_config.video.encoder,
                                                          sizeof(encoder_config.video.encoder));
            j["video"]["options"].get<std::string>().copy(encoder_config.video.options,
                                                          sizeof(encoder_config.video.options));
            j["video"]["filters"].get<std::string>().copy(encoder_config.video.filters,
                                                          sizeof(encoder_config.video.filters));
            j["video"]["sidedata"].get<std::string>().copy(encoder_config.video.sidedata,
                                                           sizeof(encoder_config.video.sidedata));

            j["audio"]["encoder"].get<std::string>().copy(encoder_config.audio.encoder,
                                                          sizeof(encoder_config.audio.encoder));
            j["audio"]["options"].get<std::string>().copy(encoder_config.audio.options,
                                                          sizeof(encoder_config.audio.options));
            j["audio"]["filters"].get<std::string>().copy(encoder_config.audio.filters,
                                                          sizeof(encoder_config.audio.filters));
            j["audio"]["sidedata"].get<std::string>().copy(encoder_config.audio.sidedata,
                                                           sizeof(encoder_config.audio.sidedata));
        } catch (std::exception&) {
            LOG(LL_ERR, "Failed to load preset.json, loading default values for encoder!");
            encoder_config = {
                .version = 1,                                                                            //
                .video{                                                                                  //
                       .encoder{"libx264"},                                                              //
                       .options{"_pixelFormat=yuv420p|crf=17.000|opencl=1|preset=medium|rc=crf|"         //
                                "x264-params=qpmax=22:aq-mode=2:aq-strength=0.700:rc-lookahead=180:"     //
                                "keyint=480:min-keyint=3:bframes=11:b-adapt=2:ref=3:deblock=0:0:direct=" //
                                "auto:me=umh:merange=32:subme=10:trellis=2:no-fast-pskip=1"},            //
                       .filters{""},                                                                     //
                       .sidedata{""}},                                                                   //
                .audio{                                                                                  //
                       .encoder{"aac"},                                                                  //
                       .options{"_sampleFormat=fltp|b=320000|profile=aac_main"},                         //
                       .filters{""},                                                                     //
                       .sidedata{""}},                                                                   //
                .format{                                                                                 //
                        .container{"mp4"},                                                               //
                        .faststart = true}};                                                             //
        }
    }

    static void writeEncoderConfig() {
        nlohmann::json j;

        j["version"] = encoder_config.version;

        j["format"]["container"] = encoder_config.format.container;
        j["format"]["faststart"] = encoder_config.format.faststart;

        j["video"]["encoder"] = encoder_config.video.encoder;
        j["video"]["options"] = encoder_config.video.options;
        j["video"]["filters"] = encoder_config.video.filters;
        j["video"]["sidedata"] = encoder_config.video.sidedata;

        j["audio"]["encoder"] = encoder_config.audio.encoder;
        j["audio"]["options"] = encoder_config.audio.options;
        j["audio"]["filters"] = encoder_config.audio.filters;
        j["audio"]["sidedata"] = encoder_config.audio.sidedata;

        std::ofstream ofs(AsiPath() + "\\" PRESET_FILE_NAME);
        ofs << j;
        ofs.flush();
    }

  private:
    static std::shared_ptr<INIReader> config_parser;
    static std::shared_ptr<INIReader> preset_parser;

    static std::string getTrimmed(const std::shared_ptr<INIReader>& parser, const std::string& config_name) {
        const std::string orig_str = parser->GetString("", config_name, "");
        return std::regex_replace(orig_str, std::regex("(^\\s*)|(\\s*$)"), "");
    }

    static std::string getTrimmed(const std::shared_ptr<INIReader>& parser, const std::string& config_name,
                                  const std::string& section) {
        const std::string orig_str = parser->GetString(section, config_name, "");
        return std::regex_replace(orig_str, std::regex("(^\\s*)|(\\s*$)"), "");
    }

    static HRESULT GetVideosDirectory(LPSTR output) {
        PWSTR vidPath = nullptr;

        RET_IF_FAILED((SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &vidPath) != S_OK),
                      "Failed to get Videos directory for the current user.", E_FAIL);

        const int pathlen = lstrlenW(vidPath);

        int buflen = WideCharToMultiByte(CP_UTF8, 0, vidPath, pathlen, nullptr, 0, nullptr, nullptr);
        if (buflen <= 0) {
            return E_FAIL;
        }

        buflen = WideCharToMultiByte(CP_UTF8, 0, vidPath, pathlen, output, buflen, nullptr, nullptr);

        output[buflen] = 0;

        CoTaskMemFree(vidPath);

        return S_OK;
    }

    static std::string toLower(const std::string& input) {
        std::string result = input;
        std::ranges::transform(result, result.begin(), ::tolower);
        return result;
    }

    static bool stringToBoolean(std::string booleanString) {
        std::ranges::transform(booleanString, booleanString.begin(), ::tolower);

        bool value;
        std::istringstream(booleanString) >> std::boolalpha >> value;
        return value;
    }

    template <typename T> static T failed(const std::string& key, const std::string& value, T def) {
        LOG(LL_NON, "Failed to parse value for \"", key, "\": ", value);
        LOG(LL_NON, "Using default value of \"", def, "\" for \"", key, "\"");
        return def;
    }

    template <typename T1, typename T2>
    static std::pair<T1, T2> failed(const std::string& key, const std::string& value, std::pair<T1, T2> def) {
        LOG(LL_NON, "Failed to parse value for \"", key, "\": ", value);
        LOG(LL_NON, "Using default value of <", def.first, ", ", def.second, "> for \"", key, "\"");
        return def;
    }

    template <typename T> static T succeeded(const std::string& key, T value) {
        LOG(LL_NON, "Loaded value for \"", key, "\": ", value);
        return value;
    }

    template <typename T1, typename T2>
    static std::pair<T1, T2> succeeded(const std::string& key, std::pair<T1, T2> value) {
        LOG(LL_NON, "Loaded value for \"", key, "\": <", value.first, ", ", value.second, ">");
        return value;
    }

    static bool parse_lossless_export() {
        const std::string string = config_parser->GetString("", CFG_ENABLE_XVX, "");

        try {
            return succeeded(CFG_ENABLE_XVX, stringToBoolean(string));
        } catch (std::exception& ex) {
            LOG(LL_ERR, ex.what());
        }

        return failed(CFG_ENABLE_XVX, string, true);
    }

    static bool parse_auto_reload_config() {
        const std::string string = config_parser->GetString("", CFG_AUTO_RELOAD_CONFIG, "");

        try {
            return succeeded(CFG_AUTO_RELOAD_CONFIG,
                             stringToBoolean(config_parser->GetString("", CFG_AUTO_RELOAD_CONFIG, "")));
        } catch (std::exception& ex) {
            LOG(LL_ERR, ex.what());
        }

        return failed(CFG_AUTO_RELOAD_CONFIG, string, true);
    }

    static bool parse_export_openexr() {
        const std::string string = config_parser->GetString(CFG_EXPORT_SECTION, CFG_EXPORT_OPENEXR, "");

        try {
            return succeeded(CFG_EXPORT_OPENEXR, stringToBoolean(string));
        } catch (std::exception& ex) {
            LOG(LL_ERR, ex.what());
        }

        return failed(CFG_EXPORT_OPENEXR, string, false);
    }

    static std::string parse_output_dir() {
        try {
            std::string string = config_parser->GetString("", CFG_OUTPUT_DIR, "");
            string = std::regex_replace(string, std::regex("(^\\s*)|(\\s*$)"), "");

            if ((!string.empty()) && (string.find_first_not_of(' ') != std::string::npos)) {
                return succeeded(CFG_OUTPUT_DIR, string);
            } else {
                char buffer[MAX_PATH] = {0};
                REQUIRE(GetVideosDirectory(buffer), "Failed to get Videos directory for the current user.");
                return failed(CFG_OUTPUT_DIR, string, buffer);
            }
        } catch (std::exception& ex) {
            LOG(LL_ERR, ex.what());
        }
        throw std::logic_error("Could not parse output directory");
    }

    static std::string parse_format_cfg() {
        const std::string string = getTrimmed(preset_parser, CFG_FORMAT_CFG, CFG_FORMAT_SECTION);
        try {
            return succeeded(CFG_FORMAT_CFG, string);
        } catch (std::exception& ex) {
            LOG(LL_ERR, ex.what());
        }

        return failed(CFG_FORMAT_CFG, string, "");
    }

    static std::string parse_format_ext() {
        const std::string string = getTrimmed(preset_parser, CFG_FORMAT_EXT, CFG_FORMAT_SECTION);
        try {
            return succeeded(CFG_FORMAT_EXT, string);
        } catch (std::exception& ex) {
            LOG(LL_ERR, ex.what());
        }

        return failed(CFG_FORMAT_EXT, string, "");
    }

    static std::string parse_video_enc() {
        const std::string string = getTrimmed(preset_parser, CFG_VIDEO_ENC, CFG_VIDEO_SECTION);
        try {
            if (!string.empty()) {
                return succeeded(CFG_VIDEO_ENC, string);
            }
        } catch (std::exception& ex) {
            LOG(LL_ERR, ex.what());
        }

        LOG(LL_NFO, "No video encoder specified. Video encoding will be disabled.");
        return "";
        // return failed(CFG_VIDEO_ENC, string, "");
    }

    static std::string parse_video_fmt() {
        const std::string string = getTrimmed(preset_parser, CFG_VIDEO_FMT, CFG_VIDEO_SECTION);
        try {
            if (!string.empty()) {
                return succeeded(CFG_VIDEO_FMT, string);
            }
        } catch (std::exception& ex) {
            LOG(LL_ERR, ex.what());
        }

        return failed(CFG_VIDEO_FMT, string, "yuv420p");
    }

    static std::string parse_video_cfg() {
        const std::string string = getTrimmed(preset_parser, CFG_VIDEO_CFG, CFG_VIDEO_SECTION);
        try {
            if (!string.empty()) {
                return succeeded(CFG_VIDEO_CFG, string);
            }
        } catch (std::exception& ex) {
            LOG(LL_ERR, ex.what());
        }

        return failed(CFG_VIDEO_CFG, string, "");
    }

    static std::string parse_audio_enc() {
        const std::string string = getTrimmed(preset_parser, CFG_AUDIO_ENC, CFG_AUDIO_SECTION);
        try {
            if (!string.empty()) {
                return succeeded(CFG_AUDIO_ENC, string);
            }
        } catch (std::exception& ex) {
            LOG(LL_ERR, ex.what());
        }

        LOG(LL_NFO, "No audio encoder specified. Audio encoding will be disabled.");
        return "";
        // return failed(CFG_AUDIO_ENC, string, "ac3");
    }

    static std::string parse_audio_cfg() {
        const std::string string = getTrimmed(preset_parser, CFG_AUDIO_CFG, CFG_AUDIO_SECTION);
        try {
            if (!string.empty()) {
                return succeeded(CFG_AUDIO_CFG, string);
            }
        } catch (std::exception& ex) {
            LOG(LL_ERR, ex.what());
        }

        return failed(CFG_AUDIO_CFG, string, "");
    }

    static std::string parse_audio_fmt() {
        const std::string string = getTrimmed(preset_parser, CFG_AUDIO_FMT, CFG_AUDIO_SECTION);
        try {
            if (!string.empty()) {
                return succeeded(CFG_AUDIO_FMT, string);
            }
        } catch (std::exception& ex) {
            LOG(LL_ERR, ex.what());
        }

        return failed(CFG_AUDIO_FMT, string, "fltp");
    }

    static LogLevel parse_log_level() {
        const std::string string = toLower(getTrimmed(config_parser, CFG_LOG_LEVEL));
        try {
            if (string == "error") {
                return succeeded(CFG_LOG_LEVEL, LL_ERR);
            } else if (string == "warn") {
                return succeeded(CFG_LOG_LEVEL, LL_WRN);
            } else if (string == "info") {
                return succeeded(CFG_LOG_LEVEL, LL_NFO);
            } else if (string == "debug") {
                return succeeded(CFG_LOG_LEVEL, LL_DBG);
            } else if (string == "trace") {
                return succeeded(CFG_LOG_LEVEL, LL_TRC);
            }
        } catch (std::exception& ex) {
            LOG(LL_ERR, ex.what());
        }

        return failed(CFG_LOG_LEVEL, string, LL_ERR);
    }

    static uint8_t parse_motion_blur_samples() {
        std::string string = config_parser->GetString(CFG_EXPORT_SECTION, CFG_EXPORT_MB_SAMPLES, "");
        string = std::regex_replace(string, std::regex("\\s+"), "");
        try {
            const uint64_t value = std::stoul(string);
            if (value > 255) {
                LOG(LL_NON, "Specified motion blur samples exceed 255");
                LOG(LL_NON, "Using maximum value of 255");
                return 255;
            } else {
                return static_cast<uint8_t>(succeeded(CFG_EXPORT_MB_SAMPLES, value));
            }
        } catch (std::exception& ex) {
            LOG(LL_NON, ex.what());
        }

        return failed(CFG_EXPORT_MB_SAMPLES, string, static_cast<uint8_t>(0));
    }

    static std::string parse_container_format() {
        std::string string = preset_parser->GetString(CFG_FORMAT_SECTION, CFG_EXPORT_FORMAT, "");
        string = std::regex_replace(string, std::regex("\\s+"), "");
        string = toLower(string);
        try {
            // if (string == "mkv" || string == "avi" || string == "mp4") {
            return succeeded(CFG_EXPORT_FORMAT, string);
            //}
        } catch (std::exception& ex) {
            LOG(LL_ERR, ex.what());
        }

        return failed(CFG_EXPORT_FORMAT, string, "mp4");
    }

    static std::pair<int32_t, int32_t> parse_fps() {
        std::string string = config_parser->GetString(CFG_EXPORT_SECTION, CFG_EXPORT_FPS, "");
        string = std::regex_replace(string, std::regex("\\s+"), "");
        try {
            std::smatch match;
            if (std::regex_match(string, match, std::regex(R"(^(\d+)/(\d+)$)"))) {
                return succeeded(CFG_EXPORT_FPS, std::make_pair(std::stoi(match[1]), std::stoi(match[2])));
            }

            match = std::smatch();
            if (std::regex_match(string, match, std::regex(R"(^\d+(\.\d+)?$)"))) {
                const float value = std::stof(string);
                auto num = static_cast<int32_t>(value * 1000);
                int32_t den = 1000;
                return succeeded(CFG_EXPORT_FPS, std::make_pair(num, den));
            }
        } catch (std::exception& ex) {
            LOG(LL_ERR, ex.what());
        }

        return failed(CFG_EXPORT_FPS, string, std::make_pair(30000, 1001));
    }

    static CaptureSource parse_capture_source() {
        const std::string string = toLower(getTrimmed(config_parser, CFG_EXPORT_CAPTURE, CFG_EXPORT_SECTION));
        if (string == "encode") {
            succeeded(CFG_EXPORT_CAPTURE, string);
            return CaptureSource::Encode;
        }
        if (string == "effects") {
            succeeded(CFG_EXPORT_CAPTURE, string);
            return CaptureSource::Effects;
        }
        if (string == "legacy") {
            succeeded(CFG_EXPORT_CAPTURE, string);
            return CaptureSource::Legacy;
        }
        if (string == "present") {
            succeeded(CFG_EXPORT_CAPTURE, string);
            return CaptureSource::Present;
        }
        if (string == "clean" || string == "bind") {
            succeeded(CFG_EXPORT_CAPTURE, string);
        } else {
            failed(CFG_EXPORT_CAPTURE, string, std::string("clean"));
        }
        return CaptureSource::Clean;
    }

    static float parse_motion_blur_strength() {
        const std::string string = config_parser->GetString(CFG_EXPORT_SECTION, CFG_EXPORT_MB_STRENGTH, "");
        try {
            float value = std::stof(string);
            if (value < 0) {
                value = 0;
            } else if (value > 1) {
                value = 1;
            }
            return succeeded(CFG_EXPORT_MB_STRENGTH, value);
        } catch (std::exception& ex) {
            LOG(LL_ERR, ex.what());
        }
        return failed(CFG_EXPORT_MB_STRENGTH, string, 0.5f);
    }
};

#endif