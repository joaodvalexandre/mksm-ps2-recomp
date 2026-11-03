@echo off
rem Runs the game for 60 s (scripts\run.cmd <seconds> for another limit, 0 = no limit). Logs in logs\run.log and logs\run_err.log.
setlocal
cd /d "%~dp0.."
set "SECONDS=%~1"
if "%SECONDS%"=="" set "SECONDS=60"
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run.ps1 -Seconds %SECONDS%
exit /b %errorlevel%
