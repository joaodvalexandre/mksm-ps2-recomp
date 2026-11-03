# Builds the port on top of the PS2Recomp submodule: applies the patches in patches\, configures and builds the
# recompiler tools, generates the C++ from the game ELF, copies it into the runtime and builds ps2EntryRunner.
# Called by scripts\build.cmd, which loads the Visual Studio environment first (cmake, ninja, cl).
#   -ToolsOnly   : stop after building ps2_analyzer and ps2_recomp (no generation, no runner)
#   -NoGenerate  : keep the generated code already in the runtime (only rebuild the runner)
#   -Tests       : also build and run the PS2Recomp test suite (ps2x_tests)
param(
    [switch]$ToolsOnly,
    [switch]$NoGenerate,
    [switch]$Tests
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

function Step($text) { Write-Host ("==== [{0}] {1} ====" -f (Get-Date -Format 'HH:mm:ss'), $text) }
function Run($exe, [string[]]$arguments) {
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) { throw "$exe $($arguments -join ' ') failed with code $LASTEXITCODE" }
}

$rec = $PS2RecompDir
$bld = Join-Path $rec 'out\build'
$gen = Join-Path $rec 'out\generated'
$elf = Get-GameElf

Step "PS2Recomp at $PS2RecompCommit with the patches of patches\"
Run 'git' @('-C', $rec, 'checkout', '-q', '-f', $PS2RecompCommit)
# The generated code and the patched sources are not part of the pinned tree; out\ keeps the build.
Run 'git' @('-C', $rec, 'clean', '-fdq', '-e', 'out')
Run 'git' @('-C', $rec, 'submodule', 'update', '--init', '--recursive', '--quiet')
foreach ($patch in (Get-ChildItem -LiteralPath (Join-Path $RepoRoot 'patches') -Filter '*.patch' | Sort-Object Name)) {
    Write-Host ("  applying {0}" -f $patch.Name)
    Run 'git' @('-C', $rec, 'apply', '--ignore-whitespace', $patch.FullName)
}

Step 'Configuring CMake'
Run 'cmake' @('-S', $rec, '-B', $bld, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
              '-DPS2X_BUILD_STUDIO=OFF', '-DPS2X_ENABLE_SCCACHE=OFF',
              '-DPS2X_ENABLE_RUNNER_UNITY_BUILD=ON', '-DPS2X_ENABLE_RUNNER_PCH=ON')

Step 'Building the recompiler tools'
Run 'cmake' @('--build', $bld, '--target', 'ps2_analyzer', '--target', 'ps2_recomp')
if ($ToolsOnly) { Step 'Done (tools only)'; exit 0 }

$runner = Join-Path $rec 'ps2xRuntime\src\runner'
$include = Join-Path $rec 'ps2xRuntime\include'
if (-not $NoGenerate) {
    $analyzer = Get-ChildItem $bld -Recurse -Filter 'ps2_analyzer.exe' | Select-Object -First 1
    $recomp = Get-ChildItem $bld -Recurse -Filter 'ps2_recomp.exe' | Select-Object -First 1
    $config = Join-Path $RepoRoot 'config\recomp.toml'
    if (-not (Test-Path -LiteralPath $config)) {
        Step "Analyzing the ELF (config\recomp.toml does not exist yet)"
        Run $analyzer.FullName @($elf, $config)
    }

    Step 'Generating C++ from the game ELF'
    if (Test-Path -LiteralPath $gen) { Remove-Item -LiteralPath $gen -Recurse -Force }
    New-Item -ItemType Directory -Path $gen | Out-Null
    # The committed configuration keeps placeholders for the machine-specific paths.
    $fw = { param($p) $p -replace '\\', '/' }
    $work = Join-Path $rec 'out\recomp.toml'
    (Get-Content -LiteralPath $config -Raw) `
        -replace '@ELF@', (& $fw $elf) `
        -replace '@MAP@', (& $fw (Join-Path $RepoRoot 'config\funcmap.csv')) `
        -replace '@OUT@', ((& $fw $gen) + '/') |
        Set-Content -LiteralPath $work -Encoding ASCII
    Run $recomp.FullName @($work)
    Write-Host ("  generated files: {0}" -f (Get-ChildItem -LiteralPath $gen -File).Count)

    Step 'Copying the generated code into the runtime'
    Get-ChildItem -LiteralPath $runner -File | Where-Object { $_.Name -ne 'main.cpp' } | Remove-Item -Force
    Get-ChildItem -LiteralPath $gen -Filter '*.cpp' -File | Copy-Item -Destination $runner
    foreach ($header in @('ps2_recompiled_functions.h', 'ps2_recompiled_stubs.h')) {
        $source = Join-Path $gen $header
        if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination (Join-Path $include $header) -Force }
    }
    Get-ChildItem -LiteralPath (Join-Path $RepoRoot 'src') -Filter '*.cpp' -File | Copy-Item -Destination $runner
    Get-ChildItem -LiteralPath (Join-Path $RepoRoot 'src') -Filter '*.h' -File | Copy-Item -Destination $runner
    # The unity objects include the generated .cpp files; with a localized MSVC ninja does not record those
    # dependencies, so they are removed to be rebuilt with the fresh code.
    $unity = Join-Path $bld 'ps2xRuntime\CMakeFiles\ps2EntryRunner.dir'
    if (Test-Path -LiteralPath $unity) {
        Get-ChildItem -LiteralPath $unity -Recurse -Include '*.obj', '*.pch' -File | Remove-Item -Force
    }
    Run 'cmake' @('-S', $rec, '-B', $bld)
}

Step 'Building ps2EntryRunner'
Run 'cmake' @('--build', $bld, '--target', 'ps2EntryRunner')

if ($Tests) {
    Step 'Building and running ps2x_tests'
    Run 'cmake' @('--build', $bld, '--target', 'ps2x_tests')
    $tests = Get-ChildItem $bld -Recurse -Filter 'ps2x_tests.exe' | Select-Object -First 1
    Push-Location $rec
    try { Run $tests.FullName @() } finally { Pop-Location }
}

$exe = Get-ChildItem $bld -Recurse -Filter 'ps2EntryRunner.exe' | Select-Object -First 1
Step "OK: $($exe.FullName)"
