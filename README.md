# EVE1G

**Export the Rockstar Editor with DLSS 5 neural rendering (NR).** EVE1G is a Rockstar Editor video exporter for GTA V (Legacy, story mode) and FiveM that records the finished DLSS 5 NR image, together with your ReShade preset, motion blur and a clean, UI-free picture.

Version **0.11.2-beta** · by **TEX1G**

EVE1G is a modified version of [Extended Video Export](https://github.com/ali-alidoust/gta5-extended-video-export) by Ali Alidoust, licensed under the [Apache License 2.0](LICENSE.md). See [NOTICE](NOTICE) for what was changed.

> Looking for the ready-to-use download? Get the **EVE1G + Focus Puller** zip from [Releases](https://github.com/tex1gprod-cloud/EVE1G/releases). There is one for GTA V and one for FiveM, both with the built files and an install guide. This repository is the source code of EVE1G.

## What's new

**0.11.2**
- New EVE1G Settings window: five pages (Output, Motion, Capture, Passes, Camera data), drawn with its own monochrome controls (GDI+), the same on Windows 10 and 11.
- Options that were experimental or unfinished are no longer in the window: real camera blur (linear-light blend, soft shutter), light streaks, HFR export, the ID pass, ProRes depth and the Effects / Legacy capture modes. Saving in the window writes them as off. The code is still in EVE1G; `EVE1G.ini` can switch it on for testing.
- A cancelled export is closed: Rockstar never calls `IMFSinkWriter::Finalize` for it, so EVE1G used to keep it open until the game restarted (the progress box stayed up and Focus Puller followed the stopped export clock). A watchdog in the present hook now closes an export once the game has stopped asking for export time steps for 90 s (and 900+ frames). The video keeps the frames written so far.

**0.11.1**
- Works in GTA V story mode (Legacy) as well as FiveM, with the same files.
- Fixes the crash at start-up with the November 2026 FiveM update (`std::runtime_error` in EVE1G.asi, `Failed to create deferred context` in `EVE1G.log`). FiveM now makes a small single-threaded D3D11 device before the game's own. EVE1G skips such devices, logs a failure instead of throwing, and falls back to the device of the game's swap chain.

**0.11.0**
- Camera stabilizer: slow Smooth Blend camera moves no longer shake in the export (EVE1G Settings, on by default). Idea from EVEM (Extended Video Export Modified).

**0.10.1**
- Light streaks: with Real camera blur on, lights and bokeh keep their strength when the motion blur smears them.

**0.10.0**
- The Scaleform hook that hid the game UI was removed, so EVE1G no longer patches game code to hide the UI. The clean capture takes the frame before the UI is drawn.

## Why EVE1G: DLSS 5 NR in your exports

DLSS 5 in GTA V (through the GTA V Upscaler mod) switches the game to a flip-model swap chain. The original Extended Video Export reads the frame after forcing a `Present()` - on that swap chain the buffer is already gone, so exports come out **black**.

EVE1G reads the frame where it is finished instead:

- **DLSS 5 NR is in the video** - the upscaled, neural-rendered image, not the raw game frame.
- **Your ReShade preset is on top of it** - DOF, grade and every ReShade pass, exactly like on screen.
- **No UI** - with the upscaler set to render ReShade before the UI (`mRenderReShadeBeforeUI = true`), the frame is taken before the game draws its HUD, export progress bar or cursor.
- **Every motion blur sample** goes through DLSS 5 NR too, so the blur is made of fully rendered frames.
- The sidecar `.txt` of every clip records the upscaler state (DLSS preset, scale, DLSS 5 NR, frame generation), so you always know how a clip was made.

## Features

- Runs as a **ReShade add-on**: your ReShade preset (DOF, grade, every pass) is in the export.
- **Clean capture**: the frame is taken before the game draws its UI — no export progress bar, no cursor.
- **Real motion blur**: every video frame is made of several game frames, with shutter angle and optional rolling shutter.
- **Passes**: depth (PNG16 / OpenEXR - read from the game's own depth buffer, with an automatic near/far range per clip), normals, clean plate (no ReShade) and proxy.
- **Export progress on screen** (never in the video), finished notice, sidecar `.txt` with the settings of each clip.
- **EVE1G Settings** window for everything in `EVE1G.ini`.
- **Camera stabilizer** for slow Smooth Blend camera moves.
- Frame-exact timing for the **Focus Puller** ReShade add-on (exported via `EVE1G_ExportTimeSeconds`).

## Requirements to run

- GTA V **Legacy** (story mode, with an ASI loader such as the `dinput8.dll` from Script Hook V) or FiveM. GTA V Enhanced is not supported.
- ReShade 6.x **with add-on support**
- For DLSS 5 NR: an NVIDIA RTX GPU and the GTA V Upscaler mod with DLSS 5 enabled (EVE1G also works without it)
- [Voukoder](https://www.voukoder.org/) (video encoding)
- Microsoft Visual C++ Redistributable 2015–2022 (x64)

## Building

Requirements: Visual Studio 2022 with **Desktop development with C++** (includes CMake, the Windows SDK / `fxc.exe`), and Git.

```
build.bat
```

`build.bat` fetches a pinned vcpkg, builds the libraries (OpenEXR, PolyHook 2, DirectXTex, …) the first time (10–20 minutes), builds everything in Release and collects the result in `out\`:

```
out\EVE1G.asi
out\EVE1G\            EVE1G.ini, preset.json, EVE1G-Settings.exe, EVE1G-Encoder.exe, readme.txt
out\reshade-shaders\  EVE1G_Depth.fx
```

Everything is linked statically (including the C++ runtime), because FiveM ships an older `msvcp140.dll`. Only the OpenMP runtime (`vcomp140.dll`, part of the Visual C++ Redistributable) is loaded from Windows.

## Layout

| Folder | What |
|---|---|
| `gta5-extended-video-export/` | EVE1G itself (`EVE1G.asi`, a ReShade add-on) |
| `eve-settings/` | EVE1G Settings window |
| `gta5-extended-video-export-config/` | Voukoder's encoder dialog (`EVE1G-Encoder.exe`) |
| `reshade/include/` | ReShade add-on API headers (BSD 3-Clause) |
| `inih/` | inih INI parser (BSD 3-Clause) |
| `voukoder/` | Voukoder type library (GPL-2.0) |
| `tools/` | build script |

## Licenses

- EVE1G: Apache License 2.0 — [LICENSE.md](LICENSE.md), changes in [NOTICE](NOTICE).
- Third-party components and their licenses: [THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt).

## Disclaimer

EVE1G is a tool for making videos with the Rockstar Editor. It is not affiliated with Rockstar Games, Take-Two or Cfx.re. Use it at your own risk. In GTA V use it in story mode only, with BattlEye off, and never take mods into GTA Online. In FiveM, keep other game-modifying plugins out of the plugins folder, as its anti-cheat can ban for them.
