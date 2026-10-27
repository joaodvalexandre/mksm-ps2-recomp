@echo off
rem Runs the port for 60 seconds and saves the output in logs\.
cd /d "%~dp0.."
echo Running the port for 60 seconds (it closes by itself)...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run.ps1" -Seconds 60
echo Done. Output in logs\run.log and logs\run_err.log
timeout /t 15 >nul
