@echo off
setlocal
cd /d "%~dp0.."
echo ============================================================
echo  EVE1G build   %DATE% %TIME%
echo ============================================================

rem ---- 1. Visual Studio 2022 with the C++ tools ------------------------------
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo [X] Visual Studio Installer not found.
  goto :fail
)
set "VSPATH="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
  echo [X] Visual Studio is installed but without the C++ tools.
  echo     Open "Visual Studio Installer", click Modify on Visual Studio 2022,
  echo     tick "Desktop development with C++" and install, then run build.bat again.
  goto :fail
)
echo [OK] Visual Studio: %VSPATH%
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 goto :fail

where cmake >nul 2>&1
if errorlevel 1 (
  echo [X] CMake not found. In the Visual Studio Installer, tick "C++ CMake tools for Windows".
  goto :fail
)
where fxc >nul 2>&1
if errorlevel 1 (
  echo [X] fxc.exe ^(shader compiler^) not found - the Windows SDK is missing from the C++ workload.
  goto :fail
)
where git >nul 2>&1
if errorlevel 1 (
  echo [X] Git not found on PATH.
  goto :fail
)

rem ---- 2. vcpkg, pinned to the commit EVE was written against -------------------
set "VCPKG_COMMIT=000d1bda1ffa95a73e0b40334fa4103d6f4d3d48"
if not exist "vcpkg\.git" (
  echo [..] Getting vcpkg...
  git init -q vcpkg || goto :fail
  git -C vcpkg remote add origin https://github.com/microsoft/vcpkg || goto :fail
)
git -C vcpkg fetch -q --depth 1 origin %VCPKG_COMMIT% || goto :fail
git -C vcpkg checkout -q FETCH_HEAD || goto :fail
rem The vcpkg TOOL from that commit (2023-11) cannot see Visual Studio 17.10+ (MSVC 14.4x):
rem "Unable to find a valid Visual Studio instance". Use a newer tool with the same pinned libraries.
set "VCPKG_TOOL=2024-07-10"
powershell -NoProfile -Command "(Get-Content vcpkg\scripts\vcpkg-tool-metadata.txt) -replace '^VCPKG_TOOL_RELEASE_TAG=.*','VCPKG_TOOL_RELEASE_TAG=%VCPKG_TOOL%' | Set-Content vcpkg\scripts\vcpkg-tool-metadata.txt"
if errorlevel 1 goto :fail
if not exist "vcpkg\.tool-%VCPKG_TOOL%" (
  if exist "vcpkg\vcpkg.exe" del /q "vcpkg\vcpkg.exe"
  if exist "build" rmdir /s /q "build"
  call vcpkg\bootstrap-vcpkg.bat -disableMetrics || goto :fail
  type nul > "vcpkg\.tool-%VCPKG_TOOL%"
)
rem The pinned scripts download pkgconf 2.1.0 from msys2, which has been deleted from every mirror.
rem Point them at the current package (same one today's vcpkg uses).
powershell -NoProfile -Command "$f='vcpkg\scripts\cmake\vcpkg_find_acquire_program(PKGCONFIG).cmake'; $t=[IO.File]::ReadAllText($f); $t=$t.Replace('mingw64/mingw-w64-x86_64-pkgconf-1~2.1.0-1-any.pkg.tar.zst','mingw64/mingw-w64-x86_64-pkgconf-1~3.0.7-1-any.pkg.tar.zst').Replace('1567ba9fc947b3a1a983f5a23dcc0982950190cd92d7527684ba219253c5fa4b340b315f25ee695be1cdf6bfbb2cc5c3bdf7a5758b1d66b761748b5aad9afe39','27a88825f334d8fe2477f193eaa30785f3b79aa03e07f5793611fe29c9b7dc552338216a4a1f24d2801a467dd453295c077fa07a9c6b83b882afd6bb3928ddec'); [IO.File]::WriteAllText($f,$t)"
if errorlevel 1 goto :fail
findstr /c:"pkgconf-1~3.0.7" "vcpkg\scripts\cmake\vcpkg_find_acquire_program(PKGCONFIG).cmake" >nul || (echo [X] could not patch pkgconf & goto :fail)
rem ...and the pkg-config CHECK also needs msys2-runtime 3.4.9, deleted too. The check only validates
rem .pc files (which EVE never uses), so skip it instead of chasing old msys2 packages.
powershell -NoProfile -Command "$f='vcpkg\scripts\cmake\vcpkg_fixup_pkgconfig.cmake'; $t=[IO.File]::ReadAllText($f); $t=$t.Replace('if(NOT arg_SKIP_CHECK) # The check','if(FALSE) # EVE1G skipped. The check'); [IO.File]::WriteAllText($f,$t)"
if errorlevel 1 goto :fail
findstr /c:"EVE1G skipped" "vcpkg\scripts\cmake\vcpkg_fixup_pkgconfig.cmake" >nul || (echo [X] could not patch the pkg-config check & goto :fail)
echo [OK] vcpkg ready

rem ---- 3. configure (downloads and builds the libraries the first time: 10-20 min) ---
echo [..] Configuring - the first run builds OpenEXR, PolyHook, DirectXTex. Be patient.
rem Start from a clean cache every time, so a changed setting can never be shadowed by an old one.
if exist "build\CMakeCache.txt" rmdir /s /q build
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DVCPKG_TARGET_TRIPLET=x64-windows-static
if errorlevel 1 goto :fail

rem ---- 4. build ----------------------------------------------------------------
cmake --build build --config Release -- /m /v:minimal
if errorlevel 1 goto :fail

rem ---- 5. collect the result -----------------------------------------------------
if exist out rmdir /s /q out
mkdir out
copy /y "x64\Release\EVE1G.asi" out\ >nul || goto :fail
xcopy /e /i /y "gta5-extended-video-export\deploy\EVE1G" out\EVE1G >nul || goto :fail
xcopy /e /i /y "gta5-extended-video-export\deploy\reshade-shaders" out\reshade-shaders >nul || goto :fail
copy /y "x64\Release\EVE1G-Encoder.exe" out\EVE1G\ >nul || goto :fail
copy /y "x64\Release\EVE1G-Settings.exe" out\EVE1G\ >nul || goto :fail
echo.
echo [..] Testing which video formats Voukoder can encode on this PC at 4K (about a minute)...
start "" /wait "out\EVE1G\EVE1G-Settings.exe" --probe
if exist "out\EVE1G\formats.ini" (type "out\EVE1G\formats.ini") else (echo [!!] format test did not run)
echo.
echo ============================================================
echo  [DONE] out\EVE1G.asi, out\EVE1G (Settings + Encoder) and out\reshade-shaders (EVE1G_Depth.fx) are ready.
echo ============================================================
exit /b 0

:fail
echo.
echo ============================================================
echo  [FAILED] - send build.log with your bug report.
echo ============================================================
exit /b 1
