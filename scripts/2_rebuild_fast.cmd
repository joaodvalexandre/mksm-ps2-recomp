@echo off
rem Fast rebuild (~1 min): copies src\mksm_overrides.cpp into the runtime and builds only what changed,
rem without regenerating the game C++. Requires scripts\2_build.cmd to have been run before.
cd /d "%~dp0.."
if not exist logs mkdir logs
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSPATH="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
  echo Visual Studio with C++ was not found. Run scripts\1_install_tools.cmd first
  exit /b 1
)
set VSCMD_SKIP_SENDTELEMETRY=1
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" > logs\vcvars.log 2>&1
rem Same work folder as scripts\common.ps1 (Get-MksmWorkDir)
if defined MKSM_WORK (set "WORK=%MKSM_WORK%") else (for /f "usebackq delims=" %%w in (`powershell -NoProfile -ExecutionPolicy Bypass -Command ". '%~dp0common.ps1'; Get-MksmWorkDir"`) do set "WORK=%%w")
copy /y "src\mksm_overrides.cpp" "%WORK%\PS2Recomp\ps2xRuntime\src\runner\mksm_overrides.cpp" >nul
if errorlevel 1 exit /b 1
copy /y "src\*.h" "%WORK%\PS2Recomp\ps2xRuntime\src\runner\" >nul
if errorlevel 1 exit /b 1
rem With a localized MSVC ("Note: including file:") ninja does not record the /showIncludes dependencies,
rem so a change in mksm_overrides.cpp (included from a unity file) rebuilds nothing.
rem The unity file that includes it is touched to force the rebuild.
powershell -NoProfile -ExecutionPolicy Bypass -Command "$unityDir = Join-Path $env:WORK 'PS2Recomp\out\build\ps2xRuntime\CMakeFiles\ps2EntryRunner.dir\Unity'; Get-ChildItem -LiteralPath $unityDir -Filter '*.cxx' -File | Where-Object { Select-String -LiteralPath $_.FullName -SimpleMatch 'mksm_overrides.cpp' -Quiet } | ForEach-Object { $_.LastWriteTime = Get-Date }"
if errorlevel 1 exit /b 1
cmake --build "%WORK%\PS2Recomp\out\build" --target ps2EntryRunner > logs\2_rebuild_fast.log 2>&1
set "RC=%errorlevel%"
echo [final code %RC%] >> logs\2_rebuild_fast.log
echo Finished (code %RC%). Check logs\2_rebuild_fast.log
exit /b %RC%
