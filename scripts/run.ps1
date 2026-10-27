# Runs the port and saves the output in logs\.
#   -Seconds N : closes it after N seconds (0 = no limit). Default 60.
param([int]$Seconds = 60, [switch]$WatchPerformance)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

$work = Get-GowWorkDir
$bld  = Join-Path $work 'PS2Recomp\out\build'
$exe  = $null
if (Test-Path -LiteralPath $bld) {
    $exe = Get-ChildItem -LiteralPath $bld -Recurse -Filter 'ps2EntryRunner.exe' |
           Sort-Object LastWriteTime -Descending | Select-Object -First 1
}
if (-not $exe) { throw 'ps2EntryRunner.exe does not exist. Run scripts\2_build.cmd first.' }
$elf = Get-GowElf
Write-Host "Executable: $($exe.FullName)"
Write-Host "ELF:        $elf"

$elfDir = Split-Path -Parent $elf

# GOW-Port: as in Waffle565's port, the game reads its data from the disc files extracted next to the ELF,
# each at its original sector (.disc_layout from tools\iso_extract.py --layout; see configureGowCdImage in
# src\gow_overrides.cpp). The first time they are extracted from "..\God of War.iso". With GOW_ISO that image is read.
if (-not $env:GOW_ISO -and -not (Test-Path -LiteralPath (Join-Path $elfDir '.disc_layout'))) {
    $iso = Join-Path (Split-Path -Parent $elfDir) 'God of War.iso'
    if (Test-Path -LiteralPath $iso) {
        Write-Host "Extracting the disc into $elfDir (first time only)..."
        & python -I (Join-Path $RepoRoot 'tools\iso_extract.py') $iso $elfDir --layout
        if ($LASTEXITCODE -ne 0) { throw 'Disc extraction failed (tools\iso_extract.py)' }
    }
    else {
        Write-Warning "There is no .disc_layout next to the ELF nor an ISO at $iso. Set GOW_ISO or extract the disc with tools\iso_extract.py --layout"
    }
}

# The game loads its IOP modules as "IOP_MOD/xxx.irx" relative to the ELF folder, but on the
# disc they are in the root. The IOP emulator runs the original IRX modules (989snd, smpd...), so they are
# copied to IOP_MOD\ the first time.
$iopMod = Join-Path $elfDir 'IOP_MOD'
$irx = @(Get-ChildItem -LiteralPath $elfDir -Filter '*.IRX' -File)
if ($irx.Count -gt 0 -and -not (Test-Path -LiteralPath (Join-Path $iopMod 'SMPD_IOP.IRX'))) {
    New-Item -ItemType Directory -Force -Path $iopMod | Out-Null
    $irx | Copy-Item -Destination $iopMod
    Write-Host "Copied $($irx.Count) IRX modules to $iopMod"
}
elseif ($irx.Count -eq 0 -and -not (Test-Path -LiteralPath $iopMod)) {
    Write-Warning "There are no .IRX modules next to the ELF: copy the IRX files from the disc to $iopMod"
}
# GOW-Port: game values from play.cmd (Waffle565). Those already defined in the environment are respected.
foreach ($par in @(@('PS2X_GS_WORKER', '1'), @('GOW_GS_FINISH_ASINCRONO', '1'), @('GOW_AUDIO_DEVICE', 'default'))) {
    if (-not [Environment]::GetEnvironmentVariable($par[0])) { Set-Item -Path ('env:' + $par[0]) -Value $par[1] }
}

$out = Join-Path $LogsDir 'run.log'
$err = Join-Path $LogsDir 'run_err.log'
if ($env:GOW_PAD_TEST -eq '1' -and $env:GOW_PAD_TEST_NO_CAPTURE -ne '1') {
    # Do not mix old captures with a test that stops before producing all of them.
    Get-ChildItem -LiteralPath $exe.DirectoryName -Filter 'gow_pad_test_*.ppm' -File |
        Remove-Item -Force
}
$launch = @{
    FilePath = $exe.FullName; ArgumentList = ('"' + $elf + '"'); WorkingDirectory = $exe.DirectoryName
    RedirectStandardOutput = $out; RedirectStandardError = $err; PassThru = $true
}
if ($WatchPerformance) { $launch.WindowStyle = 'Hidden' }
$p = Start-Process @launch
if ($WatchPerformance) {
    # GOW-Port: watch the whole pass, excluding only the PID that was just launched.
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
        Add-Content -LiteralPath $err -Value ('[gow-perf:invalid] ' + $invalid.Exception.Message) -Encoding utf8
        throw $invalid
    }
    Write-Host ('Watched profile finished after {0:N2} s' -f $watch.Elapsed.TotalSeconds)
}
elseif ($Seconds -le 0) { $p.WaitForExit(); Write-Host "Finished (code $($p.ExitCode))" }
elseif (-not $p.WaitForExit($Seconds * 1000)) { Stop-Process -Id $p.Id -Force; Write-Host "Stopped after $Seconds s" }
else { Write-Host "The program finished by itself (code $($p.ExitCode))" }
