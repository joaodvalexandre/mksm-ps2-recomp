# Runs the port and saves the output in logs\.
#   -Seconds N : closes it after N seconds (0 = no limit). Default 60.
param([int]$Seconds = 60, [switch]$WatchPerformance)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

$work = Get-MksmWorkDir
$bld  = Join-Path $work 'PS2Recomp\out\build'
$exe  = $null
if (Test-Path -LiteralPath $bld) {
    $exe = Get-ChildItem -LiteralPath $bld -Recurse -Filter 'ps2EntryRunner.exe' |
           Sort-Object LastWriteTime -Descending | Select-Object -First 1
}
if (-not $exe) { throw 'ps2EntryRunner.exe does not exist. Run scripts\2_build.cmd first.' }
$elf = Get-MksmElf
Write-Host "Executable: $($exe.FullName)"
Write-Host "ELF:        $elf"

$elfDir = Split-Path -Parent $elf

# MKSM-Port: as in Waffle565's port, the game reads its data from the disc files extracted next to the ELF,
# each at its original sector (.disc_layout from tools\iso_extract.py --layout; see configureMksmCdImage in
# src\mksm_overrides.cpp). The first time they are extracted from the first .iso found next to the ELF or in the
# folder above it. With MKSM_ISO that image is read.
if (-not $env:MKSM_ISO -and -not (Test-Path -LiteralPath (Join-Path $elfDir '.disc_layout'))) {
    $iso = @($elfDir, (Split-Path -Parent $elfDir) | ForEach-Object {
        Get-ChildItem -LiteralPath $_ -Filter '*.iso' -File -ErrorAction SilentlyContinue
    } | Select-Object -First 1)
    $iso = if ($iso.Count -gt 0) { $iso[0].FullName } else { Join-Path $elfDir '<game>.iso' }
    if (Test-Path -LiteralPath $iso) {
        Write-Host "Extracting the disc into $elfDir (first time only)..."
        & python -I (Join-Path $RepoRoot 'tools\iso_extract.py') $iso $elfDir --layout
        if ($LASTEXITCODE -ne 0) { throw 'Disc extraction failed (tools\iso_extract.py)' }
    }
    else {
        Write-Warning "There is no .disc_layout next to the ELF nor an ISO at $iso. Set MKSM_ISO or extract the disc with tools\iso_extract.py --layout"
    }
}

# The game loads its IOP modules from the disc tree (cdrom0:\IOP\xxx.IRX), which the extraction leaves in IOP\.
if (-not (Get-ChildItem -LiteralPath (Join-Path $elfDir 'IOP') -Filter '*.IRX' -File -ErrorAction SilentlyContinue)) {
    Write-Warning "There are no .IRX modules in $elfDir\IOP: extract the disc with tools\iso_extract.py --layout"
}
# MKSM-Port: game values from play.cmd (Waffle565). Those already defined in the environment are respected.
# MKSM_WINDOW: start in a 720x540 (4:3) window instead of borderless fullscreen (F11 still toggles fullscreen).
# The diagnostics (thread watch, GS statistics and captures) are turned on by scripts\diagnose.ps1.
foreach ($par in @(@('PS2X_GS_WORKER', '1'), @('MKSM_GS_FINISH_ASINCRONO', '1'), @('MKSM_AUDIO_DEVICE', 'default'),
                   @('MKSM_WINDOW', '720x540'))) {
    if (-not [Environment]::GetEnvironmentVariable($par[0])) { Set-Item -Path ('env:' + $par[0]) -Value $par[1] }
}

$out = Join-Path $LogsDir 'run.log'
$err = Join-Path $LogsDir 'run_err.log'
$launch = @{
    FilePath = $exe.FullName; ArgumentList = ('"' + $elf + '"'); WorkingDirectory = $exe.DirectoryName
    RedirectStandardOutput = $out; RedirectStandardError = $err; PassThru = $true
}
if ($WatchPerformance) { $launch.WindowStyle = 'Hidden' }
$p = Start-Process @launch
if ($WatchPerformance) {
    # MKSM-Port: watch the whole pass, excluding only the PID that was just launched.
    # Do not stop compilers or other game sessions: close only our own instance.
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $invalid = $null
    try {
        while ($true) {
            $busy = @(Get-Process -Name cl,link,clang,clang-cl,cc1plus,ninja,cmake,ps2EntryRunner -ErrorAction SilentlyContinue |
                      Where-Object { $_.Id -ne $p.Id })
            if ($busy.Count -gt 0) {
                throw ('Profile invalidated by external load: ' +
                       (($busy.ProcessName | Sort-Object -Unique) -join ', ') + '.')
            }
            if ($Seconds -gt 0 -and $watch.Elapsed.TotalSeconds -ge $Seconds) { break }
            $waitMs = 1000
            if ($Seconds -gt 0) {
                $waitMs = [Math]::Max(1, [Math]::Min(1000, [int](($Seconds - $watch.Elapsed.TotalSeconds) * 1000)))
            }
            if ($p.WaitForExit($waitMs)) { break }
        }
    }
    catch { $invalid = $_ }
    finally {
        if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
        $p.WaitForExit() # Flush the redirections before marking or copying the log.
    }
    if ($null -ne $invalid) {
        Add-Content -LiteralPath $err -Value ('[mksm-perf:invalid] ' + $invalid.Exception.Message) -Encoding utf8
        throw $invalid
    }
    Write-Host ('Watched profile finished after {0:N2} s' -f $watch.Elapsed.TotalSeconds)
}
elseif ($Seconds -le 0) { $p.WaitForExit(); Write-Host "Finished (code $($p.ExitCode))" }
elseif (-not $p.WaitForExit($Seconds * 1000)) { Stop-Process -Id $p.Id -Force; Write-Host "Stopped after $Seconds s" }
else { Write-Host "The program finished by itself (code $($p.ExitCode))" }
