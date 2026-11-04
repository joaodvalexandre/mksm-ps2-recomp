@echo off
rem Runs the quick build for 60 s: scripts\run.cmd [seconds] [-Release] [-Iso <image>] (0 seconds = no limit).
rem Logs in logs\run.log and logs\run_err.log.
setlocal
cd /d "%~dp0.."
set "SECONDS=%~1"
if "%SECONDS%"=="" set "SECONDS=60"
shift
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run.ps1 -Seconds %SECONDS% %1 %2 %3 %4
exit /b %errorlevel%
