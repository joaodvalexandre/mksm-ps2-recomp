# Reproducible profile: counts vid::Flip and presentations, with no dumps or captures of the game.
param(
    [ValidateScript({ $_ -gt 0 })][int]$Seconds = 150,
    [switch]$FramesOnly,
    [switch]$Vu1Thread,
    [switch]$GsFront,
    [switch]$FinishAsync,
    [switch]$GpuProfile,
    [switch]$HardwareLog,
    [ValidateSet('cpu', 'cpu-hilo', 'opengl')][string]$Renderer = 'cpu',
    [ValidateSet('ram', 'shared')][string]$Presentation = 'ram',
    [ValidatePattern('^[a-zA-Z0-9_-]+$')][string]$Label = 'clean'
)
$ErrorActionPreference = 'Stop'
if ($Presentation -eq 'shared' -and $Renderer -ne 'opengl') {
    throw 'Shared presentation requires -Renderer opengl.'
}
if ($GsFront -and -not $Vu1Thread) {
    throw '-GsFront requires -Vu1Thread.'
}
. (Join-Path $PSScriptRoot 'common.ps1')
$clear = @('GOW_VIF_DIAG', 'GOW_RENDER_DIAG', 'GOW_EE_PRIM_DIAG', 'GOW_MODEL_DIAG', 'GOW_PATH_DIAG', 'GOW_ANM_DIAG', 'GOW_CLIP_TRACE',
           'GOW_GEOM_DIAG', 'GOW_GEOMETRY_DIAG', 'GOW_CAMERA_DIAG', 'GOW_SIO2_DIAG', 'GOW_GS_GPU_TEST', 'GOW_VU1_BUDGET_DIAG',
           'GOW_REPLAY_TRACE', 'GOW_REPLAY_STEM', 'GOW_GS_REPLAY_TRACE', 'GOW_GS_REPLAY_AFTER',
           'GOW_GS_REPLAY_SECONDS', 'GOW_GS_REPLAY_TEXFLUSH', 'GOW_GS_REPLAY_STATE', 'PS2X_GS_DIAG', 'PS2X_GS_TRACE_TBP', 'PS2X_GS_TRACE_AFTER',
           'PS2X_GS_DUMP_SECONDS', 'PS2X_GS_DISCARD_DRAWS', 'PS2X_GS_GPU_STATS', 'PS2X_GS_GPU_PROF',
           'PS2X_GS_HW_LOG', 'PS2X_FRAME_HASH', 'PS2X_IOP_PC_EVERY', 'PS2X_IOP_TRACE', 'PS2X_IOP_TRACE_EVERY',
           'PS2X_IOP_TRACE_NOCLIB', 'PS2X_IOP_TRACE_FROM', 'PS2X_IOP_TRACE_DMA',
           'PS2X_VU1_HILO', 'PS2X_GS_FRENTE', 'GOW_GS_FINISH_ASINCRONO')
$set = @{
    GOW_PERF_DIAG = $(if ($FramesOnly) { 'frames' } else { '1' })
    GOW_PERF_FRAME_PC = '0x001837B8' # vid::Flip of SCUS-97399; its resumptions do not count.
    GOW_PAD_TEST = '1'
    GOW_PAD_TEST_NO_CAPTURE = '1'
    # GOW-Port: only the script; a real controller or keyboard would change the measured scene.
    GOW_PAD_SOLO_GUION = '1'
    GOW_SKIP_FMV = '1'
    GOW_FAST_BOOT = '1'
    PS2X_GS_GPU = $(if ($Renderer -eq 'opengl') { '1' } else { '0' })
    PS2X_GS_THREAD = $(if ($Renderer -eq 'cpu-hilo') { '1' } else { '0' })
    # GOW-Port: setting both modes avoids inheriting a different presentation in the comparison.
    PS2X_GS_DIRECT_PRESENT = $(if ($Presentation -eq 'shared') { '1' } else { '0' })
    # GOW-Port: explicitly set the optional threads so the environment does not alter the profile.
    PS2X_VU1_HILO = $(if ($Vu1Thread) { '1' } else { '0' })
    PS2X_GS_FRENTE = $(if ($GsFront) { '1' } else { '0' })
    GOW_GS_FINISH_ASINCRONO = $(if ($FinishAsync) { '1' } else { '0' })
    # GOW-Port: GPU diagnostics alter the pacing; never use them to compare FPS.
    PS2X_GS_GPU_PROF = $(if ($GpuProfile) { '1' } else { '0' })
    PS2X_GS_HW_LOG = $(if ($HardwareLog) { '1' } else { '0' })
}
$previous = @{}
foreach ($name in @($clear) + @($set.Keys)) { $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
try {
    # GOW-Port: another build or game session invalidates the FPS comparison from the start.
    $busy = @(Get-Process -Name cl,link,clang,clang-cl,cc1plus,ninja,cmake,ps2EntryRunner -ErrorAction SilentlyContinue)
    if ($busy.Count -gt 0) {
        throw ('Profile cancelled: a build or another game session is active (' +
               (($busy.ProcessName | Sort-Object -Unique) -join ', ') + '). Repeat when it finishes.')
    }
    # In PowerShell 7.5/.NET 9, passing $null to SetEnvironmentVariable leaves an empty value.
    # getenv() still detects it: removing the entry avoids enabling diagnostics by mere presence.
    foreach ($name in $clear) { Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue }
    foreach ($name in $set.Keys) { [Environment]::SetEnvironmentVariable($name, $set[$name], 'Process') }
    Write-Host "Renderer: $Renderer; presentation: $Presentation; VU1 thread: $Vu1Thread; GS front: $GsFront; async FINISH: $FinishAsync; GPU profiler: $GpuProfile; hardware log: $HardwareLog"
    $destination = Join-Path $LogsDir "perf_$Label.log"
    try {
        & (Join-Path $PSScriptRoot 'run.ps1') -Seconds $Seconds -WatchPerformance
    }
    catch {
        # GOW-Port: keep the marked log, without announcing it as a valid profile.
        $failedLog = Join-Path $LogsDir 'run_err.log'
        if ((Test-Path -LiteralPath $failedLog) -and
            (Select-String -LiteralPath $failedLog -SimpleMatch '[gow-perf:invalid]' -Quiet)) {
            Copy-Item -LiteralPath $failedLog -Destination $destination -Force
        }
        throw
    }
    Copy-Item -LiteralPath (Join-Path $LogsDir 'run_err.log') -Destination $destination -Force
    Write-Host "Profile saved to $destination"
}
finally {
    foreach ($name in $previous.Keys) {
        if ($null -eq $previous[$name]) { Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue }
        else { [Environment]::SetEnvironmentVariable($name, $previous[$name], 'Process') }
    }
}
