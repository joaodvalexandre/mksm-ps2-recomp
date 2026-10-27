@echo off
rem Loads the Visual Studio environment and runs scripts\build.ps1.
cd /d "%~dp0.."
if not exist logs mkdir logs
echo ==========================================================
echo  Building the port. This can take a while (about 20-40 min).
echo  Progress is saved in logs\2_build.log
echo ==========================================================
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSPATH="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
  echo Visual Studio with C++ was not found. > logs\2_build.log
  echo Visual Studio with C++ was not found. Run scripts\1_install_tools.cmd first
  pause
  exit /b 1
)
set VSCMD_SKIP_SENDTELEMETRY=1
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" > logs\vcvars.log 2>&1
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %* > logs\2_build.log 2>&1
set "RC=%errorlevel%"
echo [final code %RC%] >> logs\2_build.log
echo.
echo Finished (code %RC%). Check logs\2_build.log
pause
exit /b %RC%
