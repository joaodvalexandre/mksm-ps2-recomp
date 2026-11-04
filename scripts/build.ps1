# Builds the port on top of the PS2Recomp submodule: applies the patches in patches\, configures and builds the
# recompiler tools, generates the C++ from the game ELF when needed, copies it into the runtime and builds
# ps2EntryRunner. Called by scripts\build.cmd, which loads the Visual Studio environment first.
#
#   (default)     quick profile: RelWithDebInfo, /O1, debug info, no whole-program optimization, 8 parallel jobs.
#                 Compiles in a fraction of the time of the release profile and links in seconds; the game runs
#                 somewhat slower. Build folder PS2Recomp\out\build-quick.
#   -Release      release profile: Release, /O2 /arch:AVX2, no LTO (PS2X_ENABLE_LTO=OFF, patches\0002).
#                 Build folder PS2Recomp\out\build-release.
#   -Jobs <n>     parallel compile jobs (default 8; the release link is single-threaded anyway)
#   -ToolsOnly    stop after building ps2_analyzer and ps2_recomp
#   -Regenerate   regenerate the C++ even if config\recomp.toml and the ELF are older than the last generation
#   -Tests        also build and run the PS2Recomp test suite (ps2x_tests)
param(
    [switch]$Release,
    [int]$Jobs = 8,
    [switch]$ToolsOnly,
    [switch]$Regenerate,
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
$profile = if ($Release) { 'release' } else { 'quick' }
$bld = Join-Path $rec ("out\build-" + $profile)
$gen = Join-Path $rec 'out\generated'
$stamp = Join-Path $gen '.generated-from'
$elf = Get-GameElf
$config = Join-Path $RepoRoot 'config\recomp.toml'

Step "PS2Recomp at $PS2RecompCommit with the patches of patches\ (profile: $profile)"
Run 'git' @('-C', $rec, 'checkout', '-q', '-f', $PS2RecompCommit)
# The patched sources and the generated code are not part of the pinned tree; out\ keeps the builds.
Run 'git' @('-C', $rec, 'clean', '-fdq', '-e', 'out')
Run 'git' @('-C', $rec, 'submodule', 'update', '--init', '--recursive', '--quiet')
foreach ($patch in (Get-ChildItem -LiteralPath (Join-Path $RepoRoot 'patches') -Filter '*.patch' | Sort-Object Name)) {
    Write-Host ("  applying {0}" -f $patch.Name)
    Run 'git' @('-C', $rec, 'apply', '--ignore-whitespace', $patch.FullName)
}

Step 'Configuring CMake'
$common = @('-S', $rec, '-B', $bld, '-G', 'Ninja', '-DPS2X_BUILD_STUDIO=OFF', '-DPS2X_ENABLE_SCCACHE=OFF',
            '-DPS2X_ENABLE_RUNNER_UNITY_BUILD=ON', '-DPS2X_ENABLE_RUNNER_PCH=ON', '-DPS2X_ENABLE_LTO=OFF')
if ($Release) {
    Run 'cmake' ($common + @('-DCMAKE_BUILD_TYPE=Release'))
} else {
    Run 'cmake' ($common + @('-DCMAKE_BUILD_TYPE=RelWithDebInfo',
                             '-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=/O1 /Ob1 /Zi /DNDEBUG',
                             '-DCMAKE_C_FLAGS_RELWITHDEBINFO=/O1 /Ob1 /Zi /DNDEBUG'))
}

Step 'Building the recompiler tools'
Run 'cmake' @('--build', $bld, '-j', $Jobs, '--target', 'ps2_analyzer', '--target', 'ps2_recomp')
if ($ToolsOnly) { Step 'Done (tools only)'; exit 0 }

$runner = Join-Path $rec 'ps2xRuntime\src\runner'
$include = Join-Path $rec 'ps2xRuntime\include'
$analyzer = Get-ChildItem $bld -Recurse -Filter 'ps2_analyzer.exe' | Select-Object -First 1
$recomp = Get-ChildItem $bld -Recurse -Filter 'ps2_recomp.exe' | Select-Object -First 1
if (-not (Test-Path -LiteralPath $config)) {
    Step "Analyzing the ELF (config\recomp.toml does not exist yet)"
    Run $analyzer.FullName @($elf, $config)
}

# The generated code is reused while config\recomp.toml, the ELF and the recompiler are older than it.
$needGenerate = $Regenerate -or -not (Test-Path -LiteralPath $stamp)
if (-not $needGenerate) {
    $last = (Get-Item -LiteralPath $stamp).LastWriteTime
    foreach ($input in @($config, $elf, $recomp.FullName)) {
        if ((Get-Item -LiteralPath $input).LastWriteTime -gt $last) { $needGenerate = $true }
    }
}
if ($needGenerate) {
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
    Set-Content -LiteralPath $stamp -Value (Get-Date -Format 'o')
} else {
    Step 'Generated code is up to date (use -Regenerate to force)'
}

Step 'Copying the generated code and the override module into the runtime'
Get-ChildItem -LiteralPath $runner -File | Where-Object { $_.Name -ne 'main.cpp' } | Remove-Item -Force
Get-ChildItem -LiteralPath $gen -Filter '*.cpp' -File | Copy-Item -Destination $runner
foreach ($header in @('ps2_recompiled_functions.h', 'ps2_recompiled_stubs.h')) {
    $source = Join-Path $gen $header
    if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination (Join-Path $include $header) -Force }
}
$overrides = Get-ChildItem -LiteralPath (Join-Path $RepoRoot 'src') -Include '*.cpp', '*.h' -File -Recurse
$overrides | Copy-Item -Destination $runner
Run 'cmake' @('-S', $rec, '-B', $bld)
# The unity sources include the runner .cpp files; with a localized MSVC ninja does not record those dependencies,
# so the unity files that include a changed file are touched. After a regeneration every object is rebuilt.
$unity = Join-Path $bld 'ps2xRuntime\CMakeFiles\ps2EntryRunner.dir\Unity'
if (Test-Path -LiteralPath $unity) {
    if ($needGenerate) {
        Get-ChildItem -LiteralPath (Split-Path -Parent $unity) -Recurse -Include '*.obj', '*.pch' -File | Remove-Item -Force
    } else {
        foreach ($file in $overrides) {
            foreach ($unityFile in (Get-ChildItem -LiteralPath $unity -Filter '*.cxx' -File)) {
                if ((Get-Content -LiteralPath $unityFile.FullName -Raw) -match [regex]::Escape($file.Name)) {
                    (Get-Item -LiteralPath $unityFile.FullName).LastWriteTime = Get-Date
                }
            }
        }
    }
}

Step "Building ps2EntryRunner ($Jobs jobs)"
Run 'cmake' @('--build', $bld, '-j', $Jobs, '--target', 'ps2EntryRunner')

if ($Tests) {
    Step 'Building and running ps2x_tests'
    Run 'cmake' @('--build', $bld, '-j', $Jobs, '--target', 'ps2x_tests')
    $tests = Get-ChildItem $bld -Recurse -Filter 'ps2x_tests.exe' | Select-Object -First 1
    Push-Location $rec
    try { Run $tests.FullName @() } finally { Pop-Location }
}

$exe = Get-ChildItem $bld -Recurse -Filter 'ps2EntryRunner.exe' | Select-Object -First 1
Step "OK: $($exe.FullName)"
