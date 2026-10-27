<#
.SYNOPSIS
  Extracts the files of the second layer (layer 1) of a PS2 DVD-9 ISO.
.DESCRIPTION
  Windows and 7-Zip only see the file system of layer 0. God of War keeps
  data on layer 1 (PART2.PAK), so this script locates the second
  ISO9660 volume descriptor and extracts its contents.
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\extract_layer2.ps1 -Iso "D:\God of War.iso" -Output "D:\GOW ISO extracted"
#>
param(
    [Parameter(Mandatory = $true)][string]$Iso,
    [Parameter(Mandatory = $true)][string]$Output
)
$ErrorActionPreference = 'Stop'
$out = $Output
New-Item -ItemType Directory -Force -Path $out | Out-Null
$fs  = [IO.File]::OpenRead((Resolve-Path -LiteralPath $Iso).Path)

function Read-Bytes([int64]$offset, [int]$len) {
    $buf = New-Object byte[] $len
    $fs.Position = $offset
    $got = 0
    while ($got -lt $len) {
        $n = $fs.Read($buf, $got, $len - $got)
        if ($n -le 0) { break }
        $got += $n
    }
    return ,$buf
}
function Is-PVD([byte[]]$b) {
    return ($b[0] -eq 1 -and [Text.Encoding]::ASCII.GetString($b, 1, 5) -eq 'CD001')
}

$totalSectors = [int64]($fs.Length / 2048)
$pvd0 = Read-Bytes (16 * 2048) 2048
if (-not (Is-PVD $pvd0)) { throw 'The volume descriptor of layer 0 was not found.' }
$layer0 = [BitConverter]::ToUInt32($pvd0, 80)
Write-Host "Layer 0: $layer0 sectors. Total ISO: $totalSectors sectors."

# Look for the volume descriptor of layer 1 near the end of layer 0
$base = -1
$limit = [Math]::Min($totalSectors - 17, [int64]$layer0 + 200000)
for ([int64]$s = [Math]::Max(1, $layer0 - 64); $s -lt $limit; $s++) {
    $b = Read-Bytes (($s + 16) * 2048) 6
    if ($b[0] -eq 1 -and [Text.Encoding]::ASCII.GetString($b, 1, 5) -eq 'CD001') { $base = $s; break }
}
if ($base -lt 0) { throw 'A second volume was not found. This ISO may not have a separate layer 1.' }
Write-Host "Layer 1 found at sector $base"

$pvd1 = Read-Bytes (($base + 16) * 2048) 2048
$rootLba  = [BitConverter]::ToUInt32($pvd1, 158)
$rootSize = [BitConverter]::ToUInt32($pvd1, 166)
$dir = Read-Bytes (($base + $rootLba) * 2048) $rootSize

$i = 0
while ($i -lt $dir.Length) {
    $len = $dir[$i]
    if ($len -eq 0) { $i = ([Math]::Floor($i / 2048) + 1) * 2048; continue }
    $lba   = [BitConverter]::ToUInt32($dir, $i + 2)
    $size  = [int64][BitConverter]::ToUInt32($dir, $i + 10)
    $flags = $dir[$i + 25]
    $nlen  = $dir[$i + 32]
    $name  = [Text.Encoding]::ASCII.GetString($dir, $i + 33, $nlen) -replace ';1$', ''
    $i += $len
    if (($flags -band 2) -or $nlen -le 1) { continue }

    $dest = Join-Path $out $name
    if ((Test-Path $dest) -and ((Get-Item $dest).Length -eq $size)) {
        Write-Host "Ya existe: $name"; continue
    }
    Write-Host ("Extrayendo {0} ({1:N0} bytes)..." -f $name, $size)
    $o = [IO.File]::Create($dest)
    $fs.Position = ($base + $lba) * 2048
    $buf = New-Object byte[] (8MB)
    $left = $size
    while ($left -gt 0) {
        $want = [int][Math]::Min([int64]$buf.Length, [int64]$left)
        $n = $fs.Read($buf, 0, $want)
        if ($n -le 0) { throw 'Lectura incompleta de la ISO.' }
        $o.Write($buf, 0, $n)
        $left -= $n
        Write-Progress -Activity $name -PercentComplete ([int]((($size - $left) * 100) / $size))
    }
    $o.Close()
}
$fs.Close()
Write-Host 'Listo.'
