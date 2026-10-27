@echo off
rem Runs the port with no time limit. Close it with the X of the game window.
cd /d "%~dp0.."
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run.ps1" -Seconds 0
