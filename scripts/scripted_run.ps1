# Runs the port for a fixed time with a scripted pad (MKSM_PAD_SCRIPT), the thread watch and window captures, and
# collects everything in logs\scripted-<tag>\: run.log, run_err.log and host_shot_<frame>.png for every frame listed.
#   -Tag <name>     : folder suffix (default: the date and time)
#   -Seconds <n>    : how long the game runs (default 60)
#   -Pad <script>   : MKSM_PAD_SCRIPT, e.g. "12:cross,16:cross,20:start,24:cross,28:cross,33:cross,38:cross"
#                     (the sequence that reaches the difficulty select from a cold start; see docs\MKSM.md)
#   -Shots <frames> : MKSM_HOST_SHOT, host frames to capture (about 60 per second), e.g. "600,1200,1800"; by default
#                     one every three seconds from 9 s to the end
#   -NoPad          : MKSM_PAD_TEST=1, ignore the physical controllers (only the script drives the game)
# Any other diagnostic variable already set in the environment is kept (MKSM_CD_READ_TRACE, PS2X_TRACE_RPC...).
param(
    [string]$Tag = (Get-Date -Format 'yyyyMMdd-HHmmss'),
    [int]$Seconds = 60,
    [string]$Pad = '12:cross,16:cross,20:start,24:cross,28:cross,33:cross,38:cross',
    [string]$Shots = '',
    [switch]$NoPad
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

$build = Join-Path (Get-MksmWorkDir) 'PS2Recomp\out\build'
$exe = $null
if (Test-Path -LiteralPath $build) {
    $exe = Get-ChildItem -LiteralPath $build -Recurse -Filter 'ps2EntryRunner.exe' |
           Sort-Object LastWriteTime -Descending | Select-Object -First 1
}
if (-not $exe) { throw 'ps2EntryRunner.exe does not exist. Run scripts\2_build.cmd first.' }

if (-not $Shots) {
    $Shots = ((9..[Math]::Max(9, $Seconds - 3)) | Where-Object { ($_ - 9) % 3 -eq 0 } | ForEach-Object { $_ * 60 }) -join ','
}
# Captures of an earlier run would get mixed with this one's.
Get-ChildItem -LiteralPath $exe.DirectoryName -Filter 'host_shot_*.png' -File -ErrorAction SilentlyContinue | Remove-Item -Force

$env:MKSM_PAD_SCRIPT = $Pad
$env:MKSM_HOST_SHOT = $Shots
if (-not $env:MKSM_THREAD_WATCH) { $env:MKSM_THREAD_WATCH = '5' }
if ($NoPad) { $env:MKSM_PAD_TEST = '1' }

& (Join-Path $PSScriptRoot 'run.ps1') -Seconds $Seconds

$dir = Join-Path $LogsDir ('scripted-' + $Tag)
New-Item -ItemType Directory -Force -Path $dir | Out-Null
Copy-Item -LiteralPath (Join-Path $LogsDir 'run.log'), (Join-Path $LogsDir 'run_err.log') -Destination $dir
Get-ChildItem -LiteralPath $exe.DirectoryName -Filter 'host_shot_*.png' -File -ErrorAction SilentlyContinue |
    Move-Item -Destination $dir

# A short summary: the button presses, the captures and the last thread snapshot.
$lines = Get-Content -LiteralPath (Join-Path $dir 'run_err.log')
$summary = New-Object System.Collections.Generic.List[string]
$summary.Add("== Pad script: $Pad")
$summary.AddRange([string[]]@($lines | Where-Object { $_ -like '*[mksm-pad]*' -or $_ -like '*[mksm-movie]*' -or $_ -like '*host_shot_*' }))
$watch = @($lines | Select-String -SimpleMatch '[mksm-threads] watch')
if ($watch.Count -gt 0) {
    $summary.Add('== Last EE thread snapshot')
    $last = $watch[-1].LineNumber - 1
    $summary.AddRange([string[]]($lines[$last..[Math]::Min($lines.Count - 1, $last + 12)] | Where-Object { $_ -like '*mksm-threads*' -or $_ -like '*mksm-gs]*' }))
}
$summary | Out-File -LiteralPath (Join-Path $dir 'summary.txt') -Encoding utf8
Write-Host "Scripted run in $dir"
