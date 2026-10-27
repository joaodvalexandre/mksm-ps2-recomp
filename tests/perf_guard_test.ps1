# GOW-Port: run the real scripts with simulated processes; never start or stop the game.
$ErrorActionPreference = 'Stop'
$testRepo = Split-Path -Parent $PSScriptRoot
$testTempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$testRoot = Join-Path $testTempRoot ('gow-perf-guard-' + [Guid]::NewGuid().ToString('N'))
$testPrevious = @{}
foreach ($name in @('GOW_WORK','GOW_ELF','GOW_PERF_DIAG','GOW_PAD_TEST','GOW_PAD_TEST_NO_CAPTURE','PS2X_GS_GPU_PROF','PS2X_GS_DIRECT_PRESENT','GOW_GS_REPLAY_STATE','GOW_CAMERA_DIAG',
                    'PS2X_VU1_HILO','PS2X_GS_FRENTE','GOW_GS_FINISH_ASINCRONO','PS2X_GS_HW_LOG')) {
    $testPrevious[$name] = [Environment]::GetEnvironmentVariable($name,'Process')
}
function Assert-Guard([bool]$condition,[string]$message) {
    if (-not $condition) { throw $message }
}

# The mocks only replace processes. Paths, redirections, scripts and environment restoration are real.
function global:Get-Process {
    param([string[]]$Name,[string]$ErrorAction)
    $global:GowPerfGuardFixture.snapshots++
    if ($global:GowPerfGuardFixture.scenario -eq 'startup') {
        return [pscustomobject]@{Id=777;ProcessName='cl'}
    }
    if ($global:GowPerfGuardFixture.snapshots -eq 1) { return }
    $global:GowPerfGuardFixture.process
    if ($global:GowPerfGuardFixture.snapshots -ge 3 -and $global:GowPerfGuardFixture.scenario -ne 'quiet') {
        [pscustomobject]@{Id=777;ProcessName=$global:GowPerfGuardFixture.scenario}
    }
}
function global:Start-Process {
    param($FilePath,$ArgumentList,$WorkingDirectory,$RedirectStandardOutput,$RedirectStandardError,
          [switch]$PassThru,$WindowStyle)
    $global:GowPerfGuardFixture.starts++
    $global:GowPerfGuardFixture.actualPresentation = $env:PS2X_GS_DIRECT_PRESENT
    $global:GowPerfGuardFixture.actualCaptureState = $env:GOW_GS_REPLAY_STATE
    $global:GowPerfGuardFixture.actualCameraDiag = $env:GOW_CAMERA_DIAG
    $global:GowPerfGuardFixture.actualVu1Thread = $env:PS2X_VU1_HILO
    $global:GowPerfGuardFixture.actualGsFrontThread = $env:PS2X_GS_FRENTE
    $global:GowPerfGuardFixture.actualFinishAsync = $env:GOW_GS_FINISH_ASINCRONO
    $global:GowPerfGuardFixture.actualGpuProfile = $env:PS2X_GS_GPU_PROF
    $global:GowPerfGuardFixture.actualHardwareLog = $env:PS2X_GS_HW_LOG
    Set-Content -LiteralPath $RedirectStandardOutput -Value 'proceso simulado'
    Set-Content -LiteralPath $RedirectStandardError -Value '[gow-perf] t=20.00 window=5.00 guest_flip_hz=10.00'
    $global:GowPerfGuardFixture.process
}
function global:Stop-Process {
    param([int]$Id,[switch]$Force,[string]$ErrorAction)
    Assert-Guard ($Id -eq 4242) 'Se intento detener un proceso ajeno'
    $global:GowPerfGuardFixture.stopped.Add($Id)
    $global:GowPerfGuardFixture.process.HasExited = $true
}
try {
    New-Item -ItemType Directory -Path (Join-Path $testRoot 'scripts') -Force | Out-Null
    foreach ($script in @('common.ps1','run.ps1','test_performance.ps1')) {
        Copy-Item -LiteralPath (Join-Path $testRepo "scripts/$script") -Destination (Join-Path $testRoot 'scripts')
    }
    $env:GOW_WORK = Join-Path $testRoot 'work'
    $env:GOW_ELF = Join-Path $testRoot 'SCUS_973.99'
    $env:GOW_PERF_DIAG = 'valor_previo'
    $env:PS2X_GS_GPU_PROF = 'diagnostico_previo'
    $env:PS2X_GS_DIRECT_PRESENT = '1'
    $env:GOW_GS_REPLAY_STATE = '4'
    $env:GOW_CAMERA_DIAG = '1'
    $env:PS2X_VU1_HILO = '1'
    $env:PS2X_GS_FRENTE = '1'
    $env:GOW_GS_FINISH_ASINCRONO = '1'
    $env:PS2X_GS_HW_LOG = '1'
    New-Item -ItemType Directory -Path (Join-Path $env:GOW_WORK 'PS2Recomp/out/build') -Force | Out-Null
    New-Item -ItemType File -Path $env:GOW_ELF | Out-Null
    New-Item -ItemType File -Path (Join-Path $env:GOW_WORK 'PS2Recomp/out/build/ps2EntryRunner.exe') | Out-Null
    $captureFixture = Join-Path $env:GOW_WORK 'PS2Recomp/out/build/gow_pad_test_preserve.ppm'
    Set-Content -LiteralPath $captureFixture -Value 'captura preexistente'
    foreach ($scenario in @('startup','cl','ps2EntryRunner','quiet')) {
        $process = [pscustomobject]@{Id=4242;ProcessName='ps2EntryRunner';HasExited=$false;waits=0}
        $process | Add-Member -MemberType ScriptMethod -Name WaitForExit -Value {
            param($milliseconds)
            if ($null -eq $milliseconds) { $this.HasExited=$true;return }
            $this.waits++
            if ($this.waits -ge 2) { $this.HasExited=$true;return $true }
            return $false
        }
        $global:GowPerfGuardFixture = @{
            scenario=$scenario;process=$process;starts=0;snapshots=0
            stopped=[Collections.Generic.List[int]]::new()
        }
        $caught = $null
        try { & (Join-Path $testRoot 'scripts/test_performance.ps1') -Seconds 5 -Label $scenario }
        catch { $caught = $_ }
        Assert-Guard ($env:GOW_PERF_DIAG -eq 'valor_previo' -and $env:PS2X_GS_GPU_PROF -eq 'diagnostico_previo') 'The environment was not restored'
        Assert-Guard ($env:PS2X_GS_DIRECT_PRESENT -eq '1') 'The previous presentation mode was not restored'
        Assert-Guard ($env:GOW_GS_REPLAY_STATE -eq '4') 'The previous capture state was not restored'
        Assert-Guard ($env:GOW_CAMERA_DIAG -eq '1') 'The camera diagnostic was not restored'
        Assert-Guard (Test-Path -LiteralPath $captureFixture) 'The capture-less profile deleted an existing capture'
        Assert-Guard ($env:PS2X_VU1_HILO -eq '1' -and $env:PS2X_GS_FRENTE -eq '1') 'The previous threads were not restored'
        Assert-Guard ($env:GOW_GS_FINISH_ASINCRONO -eq '1') 'The previous async FINISH setting was not restored'
        Assert-Guard ($env:PS2X_GS_HW_LOG -eq '1') 'The previous hardware log setting was not restored'
        if ($global:GowPerfGuardFixture.starts -gt 0) {
            Assert-Guard ($global:GowPerfGuardFixture.actualPresentation -eq '0') 'RAM presentation inherited the previous environment'
            Assert-Guard ([string]::IsNullOrEmpty($global:GowPerfGuardFixture.actualCaptureState)) 'The profile inherited the capture state'
            Assert-Guard ([string]::IsNullOrEmpty($global:GowPerfGuardFixture.actualCameraDiag)) 'The profile inherited the camera diagnostic'
            Assert-Guard ($global:GowPerfGuardFixture.actualVu1Thread -eq '0' -and $global:GowPerfGuardFixture.actualGsFrontThread -eq '0') 'El perfil base heredo hilos activos'
            Assert-Guard ($global:GowPerfGuardFixture.actualFinishAsync -eq '0') 'El perfil base heredo FINISH asincrono'
            Assert-Guard ($global:GowPerfGuardFixture.actualGpuProfile -eq '0' -and $global:GowPerfGuardFixture.actualHardwareLog -eq '0') 'El perfil base heredo diagnosticos GPU'
        }
        $destination = Join-Path $testRoot "logs/perf_$scenario.log"
        if ($scenario -eq 'startup') {
            Assert-Guard ($null -ne $caught -and $global:GowPerfGuardFixture.starts -eq 0) 'The earlier build was not rejected'
            Assert-Guard (-not (Test-Path -LiteralPath $destination)) 'A profile that never started was saved'
        }
        elseif ($scenario -eq 'quiet') {
            Assert-Guard ($null -eq $caught -and $global:GowPerfGuardFixture.starts -eq 1) 'The own PID invalidated the clean pass'
            Assert-Guard ($global:GowPerfGuardFixture.stopped.Count -eq 0) 'A game that had already finished was stopped'
            Assert-Guard ((Get-Content -LiteralPath $destination -Raw) -notmatch '\[gow-perf:invalid\]') 'The clean pass was marked'
        }
        else {
            Assert-Guard ($null -ne $caught -and $global:GowPerfGuardFixture.starts -eq 1) 'The load that started during the profile was not rejected'
            Assert-Guard ($global:GowPerfGuardFixture.stopped.Count -eq 1 -and $global:GowPerfGuardFixture.stopped[0] -eq 4242) 'The own instance was not the only one stopped'
            Assert-Guard ((Get-Content -LiteralPath $destination -Raw) -match '\[gow-perf:invalid\]') 'The mark is missing from the kept profile'
        }
        Write-Host "Watch check: $scenario OK"
    }
    # GOW-Port: the shared option must also replace and restore the inherited mode.
    $env:PS2X_GS_DIRECT_PRESENT = '0'
    $global:GowPerfGuardFixture.process.HasExited = $false
    $global:GowPerfGuardFixture.process.waits = 0
    $global:GowPerfGuardFixture.snapshots = 0
    & (Join-Path $testRoot 'scripts/test_performance.ps1') -Seconds 5 -Renderer opengl -Presentation shared `
        -Vu1Thread -GsFront -FinishAsync -GpuProfile -HardwareLog -Label shared
    Assert-Guard ($global:GowPerfGuardFixture.actualPresentation -eq '1') 'Shared presentation inherited RAM'
    Assert-Guard ($global:GowPerfGuardFixture.actualVu1Thread -eq '1' -and $global:GowPerfGuardFixture.actualGsFrontThread -eq '1') 'The requested threads were not enabled'
    Assert-Guard ($global:GowPerfGuardFixture.actualFinishAsync -eq '1') 'The requested async FINISH was not enabled'
    Assert-Guard ($global:GowPerfGuardFixture.actualGpuProfile -eq '1' -and $global:GowPerfGuardFixture.actualHardwareLog -eq '1') 'The requested GPU diagnostics were not enabled'
    Assert-Guard ($env:PS2X_GS_DIRECT_PRESENT -eq '0') 'RAM was not restored after the shared option'
    Assert-Guard ($env:PS2X_VU1_HILO -eq '1' -and $env:PS2X_GS_FRENTE -eq '1') 'Threads were not restored after the shared option'
    Assert-Guard ($env:GOW_GS_FINISH_ASINCRONO -eq '1') 'FINISH was not restored after the shared option'
    Assert-Guard ($env:PS2X_GS_GPU_PROF -eq 'diagnostico_previo' -and $env:PS2X_GS_HW_LOG -eq '1') 'GPU diagnostics were not restored after the shared option'
    $startsBefore = $global:GowPerfGuardFixture.starts
    $caught = $null
    try { & (Join-Path $testRoot 'scripts/test_performance.ps1') -GsFront -Renderer opengl }
    catch { $caught = $_ }
    Assert-Guard ($null -ne $caught -and $global:GowPerfGuardFixture.starts -eq $startsBefore) 'GS front was accepted without a VU1 thread'
    foreach ($duration in @(0, -1)) {
        $caught = $null
        try { & (Join-Path $testRoot 'scripts/test_performance.ps1') -Seconds $duration }
        catch { $caught = $_ }
        Assert-Guard ($null -ne $caught -and $global:GowPerfGuardFixture.starts -eq $startsBefore) "An invalid duration was accepted: $duration"
    }
    $startsBefore = $global:GowPerfGuardFixture.starts
    $caught = $null
    try { & (Join-Path $testRoot 'scripts/test_performance.ps1') -Renderer cpu -Presentation shared }
    catch { $caught = $_ }
    Assert-Guard ($null -ne $caught -and $global:GowPerfGuardFixture.starts -eq $startsBefore) 'Shared presentation was accepted with the CPU renderer'
    Write-Host 'Presentation checks: RAM, shared and CPU-incompatible OK'

    # GOW-Port: tests with capture must keep discarding old files.
    $staleCapture = Join-Path $env:GOW_WORK 'PS2Recomp/out/build/gow_pad_test_stale.ppm'
    Set-Content -LiteralPath $staleCapture -Value 'captura antigua'
    $env:GOW_PAD_TEST = '1'
    Remove-Item Env:GOW_PAD_TEST_NO_CAPTURE -ErrorAction SilentlyContinue
    $global:GowPerfGuardFixture.process.HasExited = $false
    $global:GowPerfGuardFixture.process.waits = 0
    & (Join-Path $testRoot 'scripts/run.ps1') -Seconds 5
    Assert-Guard (-not (Test-Path -LiteralPath $staleCapture)) 'The capture-enabled test did not clean the old capture'
    Write-Host 'Capture cleanup for capture-enabled tests OK'
}
finally {
    foreach ($name in $testPrevious.Keys) {
        if ($null -eq $testPrevious[$name]) { Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue }
        else { [Environment]::SetEnvironmentVariable($name,$testPrevious[$name],'Process') }
    }
    Remove-Item Function:Get-Process,Function:Start-Process,Function:Stop-Process -ErrorAction SilentlyContinue
    Remove-Variable GowPerfGuardFixture -Scope Global -ErrorAction SilentlyContinue
    # Verify the absolute target before cleaning only the temporary directory created above.
    $resolved = [IO.Path]::GetFullPath($testRoot)
    $prefix = $testTempRoot.TrimEnd([IO.Path]::DirectorySeparatorChar,[IO.Path]::AltDirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    if (-not $resolved.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path -Leaf $resolved) -notmatch '^gow-perf-guard-[a-f0-9]{32}$') { throw 'Invalid cleanup target' }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
