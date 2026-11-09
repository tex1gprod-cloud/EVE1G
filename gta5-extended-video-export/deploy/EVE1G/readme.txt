EVE1G
=====
Version 0.11.2-beta, by TEX1G.
Rockstar Editor video export for GTA V (Legacy) and FiveM with DLSS 5 NR, ReShade, motion blur and clean
(UI-free) capture.
A modified version of Extended Video Export by ali-alidoust (github.com/ali-alidoust/gta5-extended-video-export),
Apache License 2.0 - see THIRD-PARTY-NOTICES.txt in the release.

New in 0.11.2: a new EVE1G-Settings window (five pages: Output, Motion, Capture, Passes, Camera data).
Options that were experimental or unfinished are no longer in it: real camera blur, light streaks, HFR export,
the ID pass, ProRes depth and the Effects / Legacy capture modes - saving in the window switches them off.
An export you cancel in the editor is now closed after about 90 s (the video keeps the frames written so far).
Before, EVE1G kept it open until the game restarted: the progress box stayed up and Focus Puller followed the
stopped export instead of the editor, so focus did not change.

New in 0.11.1: works in GTA V story mode as well as FiveM (same files - see README.txt in the release).

New in 0.11.0: Camera stabilizer (EVE1G-Settings > Motion, on by default) - slow Smooth Blend camera moves
no longer shake in the export. Idea from EVEM (Extended Video Export Modified).
Source code: https://github.com/tex1gprod-cloud/EVE1G

Files (GTA V: the game folder, next to GTA5.exe  /  FiveM: FiveM.app\plugins)
  EVE1G.asi                 the exporter (a ReShade add-on)
  EVE1G\EVE1G.ini           settings
  EVE1G\preset.json         video format (Voukoder)
  EVE1G\EVE1G-Settings.exe  settings window
  EVE1G\EVE1G-Encoder.exe   Voukoder's own encoder window (advanced)
  EVE1G\EVE1G.log           what happened during the last exports

Needs: ReShade with add-on support (dxgi.dll), Voukoder installed. GTA V also needs an ASI loader
(dinput8.dll, e.g. the one that comes with Script Hook V).

How it captures (capture_source = clean)
  The frame is taken at Rockstar's export point, before the game draws its UI, so there is no
  export progress bar and no cursor. Your ReShade preset is then rendered onto that frame
  (DOF, grade, every ReShade pass of the frame) and it is encoded with motion blur.

Naming
  Fill in project and shot to get files like  MyFilm_shot03_t02_24fps_16s_180.mov
  plus a MyFilm_shot03_t02_24fps_16s_180.txt with the settings the clip was made with.

Depth pass ([DEPTH] in EVE1G.ini, or EVE1G-Settings > Passes)
  Exports a depth map together with the video, frame for frame: a 16-bit PNG sequence or a float
  OpenEXR sequence (real distance).
  The depth is read from the game's own depth buffer, so every frame matches its video frame.
  range = auto (default) sets near / far from the first frame of each clip, so the map uses the full
  black-to-white range; range = fixed uses the near / far you set. It needs reshade-shaders\Shaders\EVE1G_Depth.fx
  (installed with EVE1G). EVE1G switches that technique on by itself while it exports - it draws nothing.
  The depth is the same one your ReShade depth of field uses. With motion blur, the depth is taken from
  the sample in the middle of the shutter (depth is never blurred).

Motion blur (EVE1G-Settings > Motion)
  Every video frame is (samples + 1) game frames. 24 fps with 39 samples = 960 game frames per second -
  the blur is one clean smear instead of separate copies. The shutter angle sets how long the shutter is
  open (180 = film look, smaller = crisper).
  Rolling shutter (rolling_shutter_ms): each row is exposed a little later than the one above, like a
  CMOS sensor - fast pans lean. 0 = global shutter (cinema camera).
  For the cleanest result also turn off the game's own motion blur, and try TAA / DLSS low or off.

Export progress on screen (progress_osd)
  While exporting, the top-left corner shows %, frames and time left. ReShade draws it after EVE1G has
  taken the frame, so it is never in the video (shown with capture clean / encode only).

EVE1G-Settings has five pages
  Output        folder, naming, video format, precision (8 / 16-bit into the encoder)
  Motion        frame rate, motion blur, shutter, camera stabilizer, rolling shutter
  Capture       where the frame is taken, ReShade on the video, progress on screen, finished notice
  Passes        depth, normals, clean plate, proxy
  Camera data   camera files from Focus Puller, the log

Precision (master_16bit)
  The motion blur is collected in 32-bit float. With "auto" the finished frame goes to 10-bit and
  lossless formats (ProRes, DNxHR HQX, HEVC 10-bit, FFV1...) at 16 bits per channel instead of 8, so
  skies and soft gradients stay free of banding. If an export ever fails to start, set it to 8-bit.

Passes ([PASSES] in EVE1G.ini, or EVE1G-Settings > Passes)
  Clean plate   <video>_plate.mov: the same shot with the same motion blur, WITHOUT your ReShade preset
                (ProRes 422 HQ). Re-grade in post, or mix the preset back in. Needs capture = clean.
  Proxy         <video>_proxy.mp4: half size H.264 for fast editing. Edit with it, then relink to the
                master (same name without _proxy).
  Normals       <video>_normals\*.png (or N.X/N.Y/N.Z channels in the EXR files): view
                space normals made from the depth - x right, y up, z towards the camera. For relighting,
                AO and fog in post. The field of view comes from Focus Puller when it runs, otherwise
                from normals_fov.

When an export finishes (notify_done)
  A sound, the game's taskbar button flashes and "EXPORT FINISHED" shows on screen for a few seconds.
  The sidecar .txt also gets a reel name and the start timecode (00:00:00:00) for your edit.

Camera data (Focus Puller)
  With the Focus Puller ReShade add-on, every export can also write <video>_camera.jsx (After Effects),
  <video>_camera.py (Blender, FBX from there) and <video>_camera.csv. Settings: Focus Puller > Export, or EVE1G-Settings > Camera data.
