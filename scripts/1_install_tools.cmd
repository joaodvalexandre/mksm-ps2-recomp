@echo off
rem Installs Git and Visual Studio 2022 Build Tools (C++, CMake and Ninja included).
cd /d "%~dp0.."
if not exist logs mkdir logs
echo ==========================================================
echo  Installing Git and Visual Studio 2022 Build Tools (C++)
echo  Windows will ask for administrator permission: accept it.
echo  It can take 15-30 minutes. Do not close this window.
echo ==========================================================
echo [%date% %time%] Start > logs\1_install.log
winget install -e --id Git.Git --accept-source-agreements --accept-package-agreements >> logs\1_install.log 2>&1
echo [git] code %errorlevel% >> logs\1_install.log
winget install -e --id Microsoft.VisualStudio.2022.BuildTools --accept-source-agreements --accept-package-agreements --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended" >> logs\1_install.log 2>&1
echo [vs] code %errorlevel% >> logs\1_install.log
echo [%date% %time%] FIN >> logs\1_install.log
echo.
echo Done. Check logs\1_install.log
pause
