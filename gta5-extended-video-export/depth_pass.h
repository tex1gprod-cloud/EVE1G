#pragma once
// EVE1G depth pass: a depth map exported together with the video, frame for frame.
//
// The depth comes from ReShade (the EVE1G_Depth.fx technique, which EVE1G switches on while it exports), so it is the
// same depth your depth of field uses, linearised with ReShade's own settings. Per video frame one motion-blur sample
// is taken - the one in the middle of the shutter - so the depth lines up with the centre of the blurred image.
//
// Output (EVE1G.ini, [DEPTH]):
//   format = png16  : 16-bit grayscale PNG sequence (lossless; After Effects / Premiere / Resolve import it)
//            prores : ProRes 4444 .mov next to the video (one file, 10-bit)
//            exr    : 32-bit float OpenEXR sequence, channel Z = distance in (approximate) metres
//   near / far (metres), curve = log | linear, white = near | far   (png16 and prores)

#define NOMINMAX
#include <d3d11.h>
#include <cstdint>
#include <string>

#include "reshade.hpp"

namespace depth {

// Export start (game's export thread), after the video file is open. base = video path without extension.
void Begin(const std::string& base, uint32_t width, uint32_t height, uint32_t fps_num, uint32_t fps_den);

// Every reshade_present (render thread): switches the EVE1G_Depth technique on while a depth export runs.
void OnPresent(reshade::api::effect_runtime* runtime, bool exporting);

// Every exported game frame = motion-blur sample (render thread). sample = index within the video frame (0..perFrame-1)
void OnSample(reshade::api::effect_runtime* runtime, ID3D11DeviceContext* context, int sample, int perFrame);

// After a video frame was handed to the encoder (render thread): writes the matching depth frame.
void OnFrameEmitted(ID3D11DeviceContext* context);

// Export end: finishes writing and closes the files.
void End();

// For the settings sidecar.
std::string Describe();
std::string Result();

} // namespace depth
