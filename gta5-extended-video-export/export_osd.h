#pragma once
// EVE1G export progress on screen: percent, frames, time left - drawn by ReShade's OSD layer, which is drawn after
// EVE1G has taken the frame, so it never reaches the video. Only shown with capture sources that take the frame before
// ReShade draws its overlay (clean / encode / effects); with present / legacy it stays off, because those read the
// finished screen.
#include <Windows.h>
#include <cstdint>
#include <string>

namespace exportosd {

// After reshade::register_addon (DllMain). Fetches ReShade's ImGui on its own, so an older or newer ReShade that does
// not offer it only costs the progress display, never the add-on.
void Register();
void Unregister();

// Export start (game's export thread, after the video file is open): the file's name, the export size and whether the
// master frames are 16-bit - shown on screen with the rest of the export settings.
void OnExportBegin(const std::string& fileName, uint32_t width, uint32_t height, bool master16);

// Export end (game's export thread): shows "export finished" on screen for a few seconds.
void OnExportEnd(const std::string& fileName);

// Every reshade_present (render thread).
void OnPresent(bool exporting);

} // namespace exportosd
