#pragma once
// EVE1G HFR export: every game frame of the export also written on its own, unblended - a high-frame-rate file next
// to the normal (motion-blurred) video. At 24 fps with 39 motion blur samples that is 960 fps: real frames for slow
// motion, speed ramps or your own blur in post. The frames are the same ones the video is made from (your ReShade
// preset, no game UI with capture = clean).
//
// Output (EVE1G.ini, [HFR]):
//   format = prores : ProRes 422 HQ .mov (one file, 10-bit 4:2:2)
//            png    : PNG sequence (8-bit, lossless)
//   every  = 1      : keep every game frame (2 = every second one: half the frame rate, half the size)
#define NOMINMAX
#include <d3d11.h>
#include <cstdint>
#include <string>

namespace hfr {

// Export start (game's export thread), after the video file is open. base = video path without extension.
void Begin(const std::string& base, uint32_t width, uint32_t height, uint32_t fps_num, uint32_t fps_den);

// Every exported game frame (render thread), with the image the video is made from.
void OnGameFrame(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* source);

// Export end: finishes writing and closes the files.
void End();

// For the settings sidecar.
std::string Describe();
std::string Result();

} // namespace hfr
