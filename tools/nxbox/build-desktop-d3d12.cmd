@echo off
setlocal
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
for /f "usebackq delims=" %%i in (`vswhere -latest -products * -property installationPath`) do set "NXBOX_VSROOT=%%i"
if not defined NXBOX_VSROOT exit /b 1
call "%NXBOX_VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" x64
if errorlevel 1 exit /b 1
set "PATH=C:\Strawberry\perl\bin;C:\Program Files\NASM;%NXBOX_VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
if not defined NXBOX_JOBS set "NXBOX_JOBS=3"
if not exist build-desktop-d3d12 mkdir build-desktop-d3d12
set "NXBOX_CACHE_ARGS="
rem Match the UWP cache policy: embedded debug info avoids parallel /Zi PDB contention.
if defined SCCACHE_PATH set "NXBOX_CACHE_ARGS=-DCMAKE_C_COMPILER_LAUNCHER=sccache -DCMAKE_CXX_COMPILER_LAUNCHER=sccache -DCMAKE_POLICY_DEFAULT_CMP0141=NEW -DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded -DCMAKE_C_FLAGS_INIT=/FS -DCMAKE_CXX_FLAGS_INIT=/FS"
python tools\nxbox\run_logged.py build-desktop-d3d12\configure.log cmake -S . -B build-desktop-d3d12 -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_D3D12=ON -DENABLE_QT=OFF -DYUZU_CMD=ON -DYUZU_TESTS=OFF -DYUZU_ROOM=OFF -DYUZU_ROOM_STANDALONE=OFF -DENABLE_WEB_SERVICE=OFF -DENABLE_UPDATE_CHECKER=OFF -DENABLE_LIBUSB=OFF -DENABLE_RESHADE=OFF -DYUZU_USE_BUNDLED_SIRIT=OFF -DYUZU_USE_BUNDLED_SDL3=ON -DYUZU_USE_BUNDLED_FFMPEG=ON %NXBOX_CACHE_ARGS%
if errorlevel 1 (
  type build-desktop-d3d12\configure.log
  exit /b 1
)
python tools\nxbox\run_logged.py build-desktop-d3d12\build.log cmake --build build-desktop-d3d12 --target yuzu-cmd --parallel %NXBOX_JOBS%
if errorlevel 1 exit /b 1
