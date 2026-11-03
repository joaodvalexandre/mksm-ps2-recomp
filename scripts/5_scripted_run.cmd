@echo off
rem Runs the port with a scripted pad for 60 seconds, capturing the window every three seconds, and collects the
rem logs, captures and a summary in logs\scripted-<date>\. Arguments are passed to scripts\scripted_run.ps1
rem (-Seconds, -Pad, -Shots, -Tag, -NoPad).
cd /d "%~dp0.."
echo Running the port with the scripted pad (it closes by itself)...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripted_run.ps1" %*
timeout /t 15 >nul
