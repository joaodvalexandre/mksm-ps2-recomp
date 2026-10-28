# Runs the port for a fixed time with the diagnostics on and collects everything in logs\diag-<date>-<time>\:
# run.log and run_err.log, summary.txt (EE threads, GS statistics, render targets, warnings) and the GS captures as
# PNG (displayed buffer, render target 0 after each batch of one frame, the first textures, the window).
#   -Seconds N : how long the game runs (default 30).
# Variables already defined in the environment are respected (see docs\MKSM.md for each one).
param([int]$Seconds = 30)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

$build = Join-Path (Get-MksmWorkDir) 'PS2Recomp\out\build'
$exe = $null
if (Test-Path -LiteralPath $build) {
    $exe = Get-ChildItem -LiteralPath $build -Recurse -Filter 'ps2EntryRunner.exe' |
           Sort-Object LastWriteTime -Descending | Select-Object -First 1
}
if (-not $exe) { throw 'ps2EntryRunner.exe does not exist. Run scripts\2_build.cmd first.' }

# The captures are written next to the executable; those of an earlier run would get mixed with this one's.
$captures = @('gs_hw_*.ppm', 'gs_hw_*.pgm', 'host_shot.png')
foreach ($pattern in $captures) {
    Get-ChildItem -LiteralPath $exe.DirectoryName -Filter $pattern -File | Remove-Item -Force
}

$diagnostics = [ordered]@{
    MKSM_THREAD_WATCH      = '5'     # EE threads (status, wait, pc, callers on the stack) every 5 s
    MKSM_CD_READ_TRACE     = '400'   # the first 400 sceCdRead calls (sector, count, destination, non-zero bytes)
    MKSM_LOADIMAGE_TRACE   = '400'   # the first 400 sceGsExecLoadImage uploads (destination, size, source, non-zero bytes)
    MKSM_GS_HW_XFERRANGE   = '0-4000' # every upload that reaches the GPU backend (destination, format, size)
    MKSM_GS_HW_STATS       = '1'     # draws, batches, textures and skipped primitives every 5 s
    MKSM_GS_HW_RTLOG       = '1'     # render targets as they are created
    MKSM_GS_HW_DUMPAT      = '15'    # from 15 s: every draw of three frames (target, box, colour, TEST/ALPHA, scissor)
    MKSM_GS_HW_DUMPALL     = '1'
    MKSM_GS_HW_DUMPMAX     = '2100'
    MKSM_GS_HW_RTDUMPAT    = '15'    # from 15 s: render target 0 after each of the first 15 batches of a frame
    MKSM_GS_HW_RTDUMPRANGE = '1-15'
    MKSM_GS_HW_TEXDUMP     = '16'    # the first 16 textures the GPU backend decodes (RGB)
    MKSM_GS_HW_DISPDUMP    = '900'   # the displayed buffer 900 frames after its size last changed
    MKSM_HOST_SHOT         = '1200'  # the window at host frame 1200 (~20 s)
}
foreach ($name in $diagnostics.Keys) {
    if (-not [Environment]::GetEnvironmentVariable($name)) { Set-Item -Path ('env:' + $name) -Value $diagnostics[$name] }
}

& (Join-Path $PSScriptRoot 'run.ps1') -Seconds $Seconds

$dir = Join-Path $LogsDir ('diag-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$errLog = Join-Path $LogsDir 'run_err.log'
Copy-Item -LiteralPath (Join-Path $LogsDir 'run.log'), $errLog -Destination $dir
foreach ($pattern in $captures) {
    Get-ChildItem -LiteralPath $exe.DirectoryName -Filter $pattern -File | Move-Item -Destination $dir
}
if (Get-Command python -ErrorAction SilentlyContinue) {
    & python -I (Join-Path $RepoRoot 'tools\ppm_to_png.py') --delete $dir | Out-File -LiteralPath (Join-Path $dir 'captures.txt') -Encoding utf8
}

# summary.txt: the last thread snapshot, the GS state and statistics, render targets, movies and anything that looks
# like a problem (the IOP's "no native code" notices are expected and left out).
$lines = Get-Content -LiteralPath $errLog
$summary = New-Object System.Collections.Generic.List[string]
$watch = @($lines | Select-String -SimpleMatch '[mksm-threads] watch')
if ($watch.Count -gt 0) {
    $summary.Add('== Last EE thread snapshot')
    $last = $watch[-1].LineNumber - 1
    $summary.AddRange([string[]]($lines[$last..[Math]::Min($lines.Count - 1, $last + 12)] |
        Where-Object { $_ -like '*mksm-threads*' }))
}
$sections = [ordered]@{
    'GS state (last)'     = @(@($lines | Select-String -SimpleMatch '[mksm-gs]') | Select-Object -Last 1)
    'GS statistics'       = @($lines | Select-String -SimpleMatch '[gs-hw] shuffles')
    'Render targets'      = @($lines | Select-String -SimpleMatch '[gs-hw-rt]')
    'Movies'              = @($lines | Select-String -SimpleMatch '[mksm-movie]')
    'Possible problems'   = @($lines | Select-String -Pattern 'error|fail|unresolved|not handled|missing|unknown|unsupported' |
                              Where-Object { $_.Line -notlike '*no native code*' } | Select-Object -First 60)
}
foreach ($title in $sections.Keys) {
    $summary.Add('')
    $summary.Add("== $title")
    foreach ($match in $sections[$title]) { $summary.Add($match.Line) }
}
$summary | Out-File -LiteralPath (Join-Path $dir 'summary.txt') -Encoding utf8
Write-Host "Diagnostics in $dir"
