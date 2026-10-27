# Logs the state of the build processes every 30 s in logs\monitor.log
# It can be closed at any time; it does not affect the build.
$log = Join-Path (Split-Path -Parent $PSScriptRoot) 'logs\monitor.log'
New-Item -ItemType Directory -Force -Path (Split-Path $log) | Out-Null
$cores = [Environment]::ProcessorCount
$prev = @{}
while ($true) {
    $os = Get-CimInstance Win32_OperatingSystem
    $free = [math]::Round($os.FreePhysicalMemory / 1MB, 1)
    $procs = Get-Process -Name link, cl, ninja, cmake -ErrorAction SilentlyContinue
    $lines = @()
    foreach ($p in $procs) {
        $cpu = $p.TotalProcessorTime.TotalSeconds
        $usage = if ($prev.ContainsKey($p.Id)) { [math]::Round((($cpu - $prev[$p.Id]) / 30) * 100 / $cores, 1) } else { '?' }
        $prev[$p.Id] = $cpu
        $lines += ('{0}(pid {1}): CPU {2}%  RAM {3} GB  CPU time {4} min' -f $p.Name, $p.Id, $usage,
                    [math]::Round($p.WorkingSet64 / 1GB, 2), [math]::Round($cpu / 60, 1))
    }
    if (-not $lines) { $lines = @('no build process active') }
    $text = "[{0}] Free RAM: {1} GB | {2}" -f (Get-Date -Format 'HH:mm:ss'), $free, ($lines -join ' | ')
    Write-Host $text
    Add-Content -LiteralPath $log -Value $text
    Start-Sleep -Seconds 30
}
