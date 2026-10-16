#include "config.h"

#include <codecvt>

std::shared_ptr<INIReader> config::config_parser;
std::shared_ptr<INIReader> config::preset_parser;

bool config::is_mod_enabled;
bool config::auto_reload_config;
std::string config::output_dir;
LogLevel config::log_level;
std::pair<uint32_t, uint32_t> config::fps;
uint8_t config::motion_blur_samples;
float config::motion_blur_strength;
bool config::export_openexr;
config::CaptureSource config::capture_source = config::CaptureSource::Clean;
bool config::hide_ui_during_export = true;
bool config::clean_apply_reshade = true;
bool config::clean_preview_effects = true;
bool config::progress_osd = true;
float config::osd_scale = 1.0f;
bool config::blend_linear = false;
float config::shutter_soft = 0.0f;
float config::rolling_shutter_ms = 0.0f;
bool config::hfr_enabled = false;
std::string config::hfr_format = "prores";
int config::hfr_every = 1;
std::string config::name_project;
std::string config::name_shot;
int config::name_take = 0;
bool config::write_sidecar = true;
bool config::depth_enabled = false;
std::string config::depth_format = "png16";
float config::depth_near = 0.5f;
float config::depth_far = 200.0f;
bool config::depth_log_curve = true;
bool config::depth_white_near = true;
bool config::depth_auto_range = true;
bool config::plate_enabled = false;
bool config::proxy_enabled = false;
bool config::normals_enabled = false;
bool config::id_enabled = false;
float config::normals_fov = 50.0f;
int config::master_16bit = 0;
bool config::notify_done = false;
VKENCODERCONFIG config::encoder_config;