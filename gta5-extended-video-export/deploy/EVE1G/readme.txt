EVE1G
=====
Version 0.9.0-beta, by TEX1G.
Rockstar Editor video export for FiveM with DLSS 5 NR, ReShade, motion blur and clean (UI-free) capture.
A modified version of Extended Video Export by ali-alidoust (github.com/ali-alidoust/gta5-extended-video-export),
Apache License 2.0 - see THIRD-PARTY-NOTICES.txt in the release.

Files (FiveM.app\plugins)
  EVE1G.asi                 the exporter (a ReShade add-on)
  EVE1G\EVE1G.ini           settings
  EVE1G\preset.json         video format (Voukoder)
  EVE1G\EVE1G-Settings.exe  settings window
  EVE1G\EVE1G-Encoder.exe   Voukoder's own encoder window (advanced)
  EVE1G\EVE1G.log           what happened during the last exports

Needs: ReShade with add-on support (dxgi.dll), Voukoder installed.

How it captures (capture_source = clean)
  The frame is taken at Rockstar's export point, before the game draws its UI, so there is no
  export progress bar and no cursor. Your ReShade preset is then rendered onto that frame
  (DOF, grade, every ReShade pass of the frame) and it is encoded with motion blur.

Naming
  Fill in project and shot to get files like  MyFilm_shot03_t02_24fps_16s_180.mov
  plus a MyFilm_shot03_t02_24fps_16s_180.txt with the settings the clip was made with.

Depth pass ([DEPTH] in EVE1G.ini, or the Depth pass box in EVE1G-Settings)
  Exports a depth map together with the video, frame for frame: a 16-bit PNG sequence or a float
  OpenEXR sequence (real distance). (ProRes depth is not available yet - PNG is written instead.)
  The depth is read from the game's own depth buffer, so every frame matches its video frame.
  range = auto (default) sets near / far from the first frame of each clip, so the map uses the full
  black-to-white range; range = fixed uses the near / far you set. It needs reshade-shaders\Shaders\EVE1G_Depth.fx
  (installed with EVE1G). EVE1G switches that technique on by itself while it exports - it draws nothing.
  The depth is the same one your ReShade depth of field uses. With motion blur, the depth is taken from
  the sample in the middle of the shutter (depth is never blurred).

Real camera motion blur (EVE1G-Settings: Frame rate and motion blur)
  Every video frame is (samples + 1) game frames. 24 fps with 39 samples = 960 game frames per second -
  the blur is one clean smear instead of separate copies. The shutter angle sets how long the shutter is
  open (180 = film look, smaller = crisper).
  Real camera blur (blend_linear, shutter_soft): the frames are added up in linear light, like light on a
  sensor, so headlights and neons streak instead of turning dull, and the shutter fades in and out like a
  rotary shutter. Rolling shutter (rolling_shutter_ms): each row is exposed a little later than the one
  above, like a CMOS sensor - fast pans lean. 0 = global shutter (cinema camera).
  For the cleanest result also turn off the game's own motion blur, and try TAA / DLSS low or off.

Export progress on screen (progress_osd)
  While exporting, the top-left corner shows %, frames and time left. ReShade draws it after EVE1G has
  taken the frame, so it is never in the video (shown with capture clean / encode / effects only).

HFR export ([HFR] in EVE1G.ini, or the HFR export box in EVE1G-Settings)
  Saves every game frame of the export on its own, unblended, next to the normal video: at 24 fps with
  39 samples that is a 960 fps file. Real frames for slow motion, speed ramps or your own motion blur in
  post. ProRes 422 HQ .mov (name_hfr960.mov) or a PNG sequence. No audio. These files are big - use a
  fast drive, or keep every 2nd / 4th frame.

EVE1G-Settings is in four tabs
  Output           folder, naming, video format, precision (8 / 16-bit into the encoder)
  Motion + Camera  frame rate, motion blur, shutter, capture mode and ReShade
  Passes           depth, normals, clean plate, proxy, ID pass, HFR
  Workflow         progress on screen, finished notice, camera data (Focus Puller), log

Precision (master_16bit)
  The motion blur is collected in 32-bit float. With "auto" the finished frame goes to 10-bit and
  lossless formats (ProRes, DNxHR HQX, HEVC 10-bit, FFV1...) at 16 bits per channel instead of 8, so
  skies and soft gradients stay free of banding. If an export ever fails to start, set it to 8-bit.

Passes ([PASSES] in EVE1G.ini, or the Passes tab)
  Clean plate   <video>_plate.mov: the same shot with the same motion blur, WITHOUT your ReShade preset
                (ProRes 422 HQ). Re-grade in post, or mix the preset back in. Needs capture = clean.
  Proxy         <video>_proxy.mp4: half size H.264 for fast editing. Edit with it, then relink to the
                master (same name without _proxy).
  Normals       <video>_normals.mov / _normals\*.png (or N.X/N.Y/N.Z channels in the EXR files): view
                space normals made from the depth - x right, y up, z towards the camera. For relighting,
                AO and fog in post. The field of view comes from Focus Puller when it runs, otherwise
                from normals_fov.
  ID pass       <video>_id\*.png + <video>_id_legend.txt. EXPERIMENTAL: the game's stencil buffer, one
                flat colour per value (the game marks characters, vehicles, world, sky... with it). Use a
                colour key for rough mattes. Not with MSAA. Taken on the middle sample (not blurred).

When an export finishes (notify_done)
  A sound, the game's taskbar button flashes and "EXPORT FINISHED" shows on screen for a few seconds.
  The sidecar .txt also gets a reel name and the start timecode (00:00:00:00) for your edit.

Camera data (Focus Puller)
  With the Focus Puller ReShade add-on, every export can also write <video>_camera.jsx (After Effects),
  <video>_camera.py (Blender, FBX from there) and <video>_camera.csv. Settings: Focus Puller > Export.
