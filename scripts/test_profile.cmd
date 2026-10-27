@echo off
rem GOW-Port: DbgHelp and recovered-stack regression, without the game.
setlocal
cd /d "%~dp0.."
if not exist logs mkdir logs
set "TASK_PROFILE_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "TASK_PROFILE_VSPATH="
for /f "usebackq tokens=*" %%i in (`"%TASK_PROFILE_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "TASK_PROFILE_VSPATH=%%i"
if not defined TASK_PROFILE_VSPATH goto :failed
set VSCMD_SKIP_SENDTELEMETRY=1
call "%TASK_PROFILE_VSPATH%\VC\Auxiliary\Build\vcvars64.bat" > logs\profile_vcvars.log 2>&1
if errorlevel 1 goto :failed
cl /nologo /O2 /EHsc /MD /utf-8 /std:c++20 tools\profile\sample.cpp /Fe:logs\sample.exe /Fo:logs\sample.obj > logs\profile_build.log 2>&1
if errorlevel 1 goto :failed
cl /nologo /O2 /EHsc /MD /Zi /utf-8 /std:c++20 tests\profile_wait_fixture.cpp /Fe:logs\profile_wait.exe /Fo:logs\profile_wait.obj /Fd:logs\profile_compiler.pdb /link /DEBUG /PDB:logs\profile_wait.pdb >> logs\profile_build.log 2>&1
if errorlevel 1 goto :failed
set "TASK_PROFILE_PS=powershell.exe"
where pwsh.exe > nul 2>&1
if not errorlevel 1 set "TASK_PROFILE_PS=pwsh.exe"
"%TASK_PROFILE_PS%" -NoProfile -ExecutionPolicy Bypass -File tests\profile_wait_test.ps1 -Profiler "%CD%\logs\sample.exe" -Fixture "%CD%\logs\profile_wait.exe" > logs\profile_test.log 2>&1
if errorlevel 1 goto :failed
type logs\profile_test.log
exit /b 0
:failed
exit /b 1
