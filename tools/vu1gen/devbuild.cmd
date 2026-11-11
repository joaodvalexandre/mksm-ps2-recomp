@echo off
rem Development build of the VU1 translator (tools\vu1gen) in a build tree of its own. The tree is configured from a
rem PS2Recomp tree that has the repository's patches, the way the quick profile of scripts\build.ps1 configures the
rem game's: RelWithDebInfo with scripts\quick-profile.cmake (the runtime, and with it upstream's VU1 interpreter, at
rem /O2 /Ob2 and the default /fp:precise), plus the targets of this folder (cmake\ps2recomp.cmake). Nothing is written
rem outside the build tree; the game's own build (PS2Recomp\out\build-quick) is not touched.
rem
rem   tools\vu1gen\devbuild.cmd [-configure] [target ...]        default targets: vu1gen vu1n_tests vu1difftest
rem
rem Environment, all optional:
rem   MKSM_VU1_PS2RECOMP    the PS2Recomp tree (pinned commit plus patches)        default <repository>\PS2Recomp
rem   MKSM_VU1_BUILD        the build tree                                         default <repository>\PS2Recomp\out\build-vu1
rem   MKSM_VU1_DEPS         the _deps folder of another build tree: its <name>-src folders are used in place of
rem                         downloads (FETCHCONTENT_SOURCE_DIR_<NAME>)
rem   MKSM_VU1_PARALLEL_GS  a paraLLEl-GS checkout, used in place of a download
rem   MKSM_VU1_CMAKE_ARGS   more arguments for the configure step
rem   MKSM_VU1_JOBS         parallel compile jobs (default 8)
setlocal EnableExtensions
for %%i in ("%~dp0..\..") do set "REPO=%%~fi"
if not defined MKSM_VU1_PS2RECOMP set "MKSM_VU1_PS2RECOMP=%REPO%\PS2Recomp"
if not defined MKSM_VU1_BUILD set "MKSM_VU1_BUILD=%REPO%\PS2Recomp\out\build-vu1"
if not defined MKSM_VU1_JOBS set "MKSM_VU1_JOBS=8"

set "CONFIGURE="
set "TARGETS="
:args
if "%~1"=="" goto args_done
if /i "%~1"=="-configure" (set "CONFIGURE=1") else (set "TARGETS=%TARGETS% --target %~1")
shift
goto args
:args_done
if not defined TARGETS set "TARGETS= --target vu1gen --target vu1n_tests --target vu1difftest"
if not exist "%MKSM_VU1_BUILD%\build.ninja" set "CONFIGURE=1"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSPATH="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
  echo Visual Studio with the C++ tools was not found.
  exit /b 1
)
set VSCMD_SKIP_SENDTELEMETRY=1
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" > nul 2>&1

if not defined CONFIGURE goto build
set "FWD_REPO=%REPO:\=/%"
set "DEPARGS="
if defined MKSM_VU1_DEPS (
  for %%d in (RAYLIB:raylib IMGUI:imgui RLIMGUI:rlimgui ELFIO:elfio FMT:fmt TOML11:toml11 RABBITIZER:rabbitizer LIBDWARF:libdwarf NLOHMANN_JSON:nlohmann_json) do (
    for /f "tokens=1,2 delims=:" %%a in ("%%d") do call :dep %%a %%b
  )
)
if defined MKSM_VU1_PARALLEL_GS call :parallelgs
echo [%time%] configuring %MKSM_VU1_BUILD% from %MKSM_VU1_PS2RECOMP%
cmake -S "%MKSM_VU1_PS2RECOMP%" -B "%MKSM_VU1_BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
  "-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=/Zi /DNDEBUG" "-DCMAKE_C_FLAGS_RELWITHDEBINFO=/Zi /DNDEBUG" ^
  "-DCMAKE_PROJECT_INCLUDE=%FWD_REPO%/scripts/quick-profile.cmake" ^
  "-DCMAKE_PROJECT_PS2Runtime_INCLUDE=%FWD_REPO%/tools/vu1gen/cmake/ps2recomp.cmake" ^
  -DPS2X_BUILD_STUDIO=OFF -DPS2X_ENABLE_SCCACHE=OFF -DPS2X_ENABLE_RUNNER_UNITY_BUILD=ON -DPS2X_ENABLE_RUNNER_PCH=ON ^
  -DPS2X_ENABLE_LTO=OFF -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF %DEPARGS% %MKSM_VU1_CMAKE_ARGS%
if errorlevel 1 exit /b 1

:build
echo [%time%] building%TARGETS%
cmake --build "%MKSM_VU1_BUILD%" -j %MKSM_VU1_JOBS%%TARGETS%
if errorlevel 1 exit /b 1
echo [%time%] done
exit /b 0

:dep
if not exist "%MKSM_VU1_DEPS%\%2-src" exit /b 0
set DEPARGS=%DEPARGS% "-DFETCHCONTENT_SOURCE_DIR_%1=%MKSM_VU1_DEPS:\=/%/%2-src"
exit /b 0

:parallelgs
set DEPARGS=%DEPARGS% "-DFETCHCONTENT_SOURCE_DIR_PARALLEL_GS=%MKSM_VU1_PARALLEL_GS:\=/%"
exit /b 0
