@echo off
rem Full build: PS2Recomp (patched) + generated game code + ps2EntryRunner. Logs in logs\build.log.
rem Arguments are passed to scripts\build.ps1 (-ToolsOnly, -NoGenerate, -Tests).
setlocal
cd /d "%~dp0.."
if not exist logs mkdir logs
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSPATH="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
  echo Visual Studio with the C++ tools was not found.
  exit /b 1
)
set VSCMD_SKIP_SENDTELEMETRY=1
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" > logs\vcvars.log 2>&1
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build.ps1 %* > logs\build.log 2>&1
set "RC=%errorlevel%"
echo [final code %RC%] >> logs\build.log
if "%RC%"=="0" (echo Build OK, see logs\build.log) else (echo Build FAILED with code %RC%, see logs\build.log)
exit /b %RC%
