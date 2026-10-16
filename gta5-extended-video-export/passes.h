#pragma once
// EVE1G extra passes, written next to the video, frame for frame ([PASSES] in EVE1G.ini):
//
//   clean_plate = true : <video>_plate.mov - the same shot WITHOUT the ReShade preset (ProRes 422 HQ), with the same
//                        motion blur. Only with capture = clean (the frame is taken before ReShade renders on it).
//                        Lets you re-grade in post or mix the preset back in.
//   proxy = true       : <video>_proxy.mp4 - half size H.264, for fast editing; relink to the master at the end.
//   id_pass = true     : <video>_id\ - EXPERIMENTAL. The game's stencil buffer (what kind of thing each pixel is:
//                        world, peds, vehicles, sky ... as the game marks them) as a PNG sequence, one flat colour per
//                        stencil value, plus <video>_id_legend.txt with the colours and values. Use a colour key in
//                        After Effects for a rough matte. Taken on the sample in the middle of the shutter (no blur).
//   (normals = true is written by the depth pass, see depth_pass.h)
#define NOMINMAX
#include <d3d11.h>
#include <cstdint>
#include <string>

namespace passes {

// Export start (game's export thread), after the video file is open. base = video path without extension.
// master16 = the final frames are RGBA 16-bit (rgba64le) instead of RGBA 8-bit.
void Begin(const std::string& base, uint32_t width, uint32_t height, uint32_t fps_num, uint32_t fps_den, bool master16);

// The clean plate is collected (GPU) by script.cpp; is it wanted for this export?
bool PlateWanted();

// A finished video frame of the master (render thread, mapped memory): the proxy is made from it.
void OnMasterFrame(const void* pixels, uint32_t rowPitch);

// A finished clean plate frame (render thread, mapped memory; same layout as the master).
void OnPlateFrame(const void* pixels, uint32_t rowPitch);

// Every exported game frame (render thread): on the middle-of-shutter sample the stencil is copied.
// depthStencil = the game's depth-stencil texture (may be null).
void OnSample(ID3D11DeviceContext* context, ID3D11Texture2D* depthStencil, int sample, int perFrame);

// After a video frame was handed to the encoder (render thread): writes the matching ID frame.
void OnFrameEmitted(ID3D11DeviceContext* context);

// Export end: finishes writing and closes the files.
void End();

// For the settings sidecar and the on-screen display.
std::string Describe();
std::string Result();
std::string ActiveList(); // e.g. "plate + proxy" (empty when none)

} // namespace passes
