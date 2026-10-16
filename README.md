# EVE1G

**Export the Rockstar Editor with DLSS 5 neural rendering (NR).** EVE1G is a Rockstar Editor video exporter for GTA V / FiveM that records the finished DLSS 5 NR image, together with your ReShade preset, real camera motion blur and a clean, UI-free picture.

Version **0.9.0-beta** · by **TEX1G**

EVE1G is a modified version of [Extended Video Export](https://github.com/ali-alidoust/gta5-extended-video-export) by Ali Alidoust, licensed under the [Apache License 2.0](LICENSE.md). See [NOTICE](NOTICE) for what was changed.

> Looking for the ready-to-use download? Get the **EVE1G + Focus Puller** release zip — it has the built files and an install guide. This repository is the source code of EVE1G.

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
- **Real motion blur**: every video frame is made of several game frames, accumulated in linear light, with shutter angle, soft shutter and optional rolling shutter.
- **Passes**: depth (PNG16 / OpenEXR - read from the game's own depth buffer, with an automatic near/far range per clip; ProRes depth is not available yet), normals, clean plate (no ReShade), proxy, experimental ID pass, and HFR (every game frame on its own).
- **Export progress on screen** (never in the video), finished notice, sidecar `.txt` with the settings of each clip.
- **EVE1G Settings** window for everything in `EVE1G.ini`.
- Frame-exact timing for the **Focus Puller** ReShade add-on (exported via `EVE1G_ExportTimeSeconds`).

## Requirements to run

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

Everything is linked statically (including the C++ runtime), because FiveM ships an older `msvcp140.dll`.

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

EVE1G is a tool for making videos with the Rockstar Editor. It is not affiliated with Rockstar Games, Take-Two or Cfx.re. Use it at your own risk; keep other game-modifying plugins out of FiveM, as its anti-cheat can ban for them.
