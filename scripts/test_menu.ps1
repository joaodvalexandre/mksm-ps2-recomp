# Boot test: presses Start and X at preset intervals and saves GS frames.
# It does not certify a playable game: review the images and the log.
param([int]$Seconds = 115, [switch]$PathDiagnostic, [switch]$VifDiagnostic)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')
$previousTest = $env:GOW_PAD_TEST
$previousPaths = $env:GOW_PATH_DIAG
$previousVif = $env:GOW_VIF_DIAG
$previousRender = $env:GOW_RENDER_DIAG
try {
    $env:GOW_PAD_TEST = '1'
    if ($PathDiagnostic) { $env:GOW_PATH_DIAG = '1' }
    if ($VifDiagnostic) { $env:GOW_VIF_DIAG = '1'; $env:GOW_RENDER_DIAG = '1' }
    Get-ChildItem -LiteralPath $LogsDir -Filter 'gow_pad_test_*.ppm' -File | Remove-Item -Force
    & (Join-Path $PSScriptRoot 'run.ps1') -Seconds $Seconds
    $build = Join-Path (Get-GowWorkDir) 'PS2Recomp\out\build'
    Get-ChildItem -LiteralPath $build -Recurse -Filter 'gow_pad_test_*.ppm' -File |
        Copy-Item -Destination $LogsDir -Force
    Write-Host "Review run.log, run_err.log and gow_pad_test_*.ppm in $LogsDir"
}
finally {
    $env:GOW_PAD_TEST = $previousTest
    $env:GOW_PATH_DIAG = $previousPaths
    $env:GOW_VIF_DIAG = $previousVif
    $env:GOW_RENDER_DIAG = $previousRender
}
