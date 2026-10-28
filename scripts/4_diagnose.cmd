@echo off
rem Runs the port for 30 seconds with the diagnostics on (it closes by itself) and collects the logs, a summary and
rem the GS captures in logs\diag-<date>-<time>\.
cd /d "%~dp0.."
echo Running the port for 30 seconds with diagnostics (it closes by itself)...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0diagnose.ps1" -Seconds 30
timeout /t 15 >nul
