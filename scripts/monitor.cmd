@echo off
rem Logs the CPU/RAM use of the build every 30 s in logs\monitor.log
cd /d "%~dp0.."
if not exist logs mkdir logs
echo Watching the build every 30 seconds (close this window whenever you want)...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0monitor.ps1"
