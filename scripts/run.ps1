# Runs the port for a limited time and keeps its output in logs\run.log (stdout) and logs\run_err.log (stderr).
#   -Seconds <n> : stop the game after n seconds (default 60; 0 = no limit, close the window to stop)
# The game files must be extracted into game\ (the runtime looks for the disc files next to the ELF).
param([int]$Seconds = 60)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

$build = Join-Path $PS2RecompDir 'out\build'
$exe = $null
if (Test-Path -LiteralPath $build) {
    $exe = Get-ChildItem -LiteralPath $build -Recurse -Filter 'ps2EntryRunner.exe' | Select-Object -First 1
}
if (-not $exe) { throw 'ps2EntryRunner.exe does not exist. Run scripts\build.cmd first.' }
$elf = Get-GameElf

$out = Join-Path $LogsDir 'run.log'
$err = Join-Path $LogsDir 'run_err.log'
Write-Host "Runner:     $($exe.FullName)"
Write-Host "ELF:        $elf"
$process = Start-Process -FilePath $exe.FullName -ArgumentList ('"' + $elf + '"') -WorkingDirectory $exe.DirectoryName `
    -RedirectStandardOutput $out -RedirectStandardError $err -PassThru
if ($Seconds -gt 0) {
    if (-not $process.WaitForExit($Seconds * 1000)) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        Write-Host "Stopped after $Seconds s"
    } else {
        Write-Host "The game exited with code $($process.ExitCode)"
    }
} else {
    $process.WaitForExit()
    Write-Host "The game exited with code $($process.ExitCode)"
}
Write-Host "Logs: $out, $err"
