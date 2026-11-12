# Builds the port on top of the PS2Recomp submodule: applies the patches in patches\, configures and builds the
# recompiler tools, generates the C++ from the game ELF when needed, copies it into the runtime and builds
# ps2EntryRunner. Called by scripts\build.cmd, which loads the Visual Studio environment first.
#
#   (default)     quick profile: RelWithDebInfo, debug info, no whole-program optimization, 8 parallel jobs; the
#                 generated game code at /O1, everything else (the runtime) at /O2 (scripts\quick-profile.cmake).
#                 Compiles in a fraction of the time of the release profile and links in seconds; the game runs
#                 somewhat slower. Build folder PS2Recomp\out\build-quick.
#   -Release      release profile: Release, /O2 /arch:AVX2, no LTO (PS2X_ENABLE_LTO=OFF, patches\0002).
#                 Build folder PS2Recomp\out\build-release.
#   -Jobs <n>     parallel compile jobs (default 8; the release link is single-threaded anyway)
#   -ToolsOnly    stop after building ps2_analyzer and ps2_recomp
#   -Regenerate   regenerate the C++ even if config\recomp.toml and the ELF are older than the last generation
#   -Reset        check out the pinned PS2Recomp again and re-apply the patches even if the tree already matches
#   -Tests        also build and run the PS2Recomp test suite (ps2x_tests)
#
# Only what changed is rebuilt: the submodule is left alone while it is exactly the pinned commit plus the patches,
# and regenerated functions are copied into the runtime only when their content differs, so a configuration or
# recompiler change recompiles the affected unity batches instead of all 13,000 functions.
param(
    [switch]$Release,
    [int]$Jobs = 8,
    [switch]$ToolsOnly,
    [switch]$Regenerate,
    [switch]$Reset,
    [switch]$Tests
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

function Step($text) { Write-Host ("==== [{0}] {1} ====" -f (Get-Date -Format 'HH:mm:ss'), $text) }
function Run($exe, [string[]]$arguments) {
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) { throw "$exe $($arguments -join ' ') failed with code $LASTEXITCODE" }
}
function Hash-Text([string]$text) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try { return [System.BitConverter]::ToString($sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($text))) -replace '-', '' }
    finally { $sha.Dispose() }
}
# The generated code lives in the submodule's tree: the runner directory (where upstream also tracks a placeholder
# register_functions.cpp) and the two generated headers. It is left out of the state and survives a reset.
$GeneratedPathspec = @(':(exclude)ps2xRuntime/src/runner', ':(exclude)ps2xRuntime/include/ps2_recompiled_functions.h',
                       ':(exclude)ps2xRuntime/include/ps2_recompiled_stubs.h')
function Tree-State($repo) {
    # The diff of the tracked files plus the untracked files outside the generated code and the builds (a patch may add
    # a file): identical for "pinned commit + patches", different as soon as anything else is touched.
    $untracked = git -C $repo ls-files --others --exclude-standard |
        Where-Object { $_ -notlike 'ps2xRuntime/src/runner/*' -and $_ -notlike 'out/*' -and $_ -notlike 'ps2xRuntime/include/ps2_recompiled_*' }
    $diff = git -C $repo diff --no-color -- . @GeneratedPathspec | Out-String
    return Hash-Text ($diff + "`n" + (($untracked | Sort-Object) -join "`n"))
}
# The files a reset rewrites: the patched tracked files and the files the patches add.
function Patched-Files($repo) {
    $tracked = @(git -C $repo diff --name-only -- . @GeneratedPathspec)
    $added = @(git -C $repo ls-files --others --exclude-standard |
        Where-Object { $_ -notlike 'ps2xRuntime/src/runner/*' -and $_ -notlike 'out/*' -and $_ -notlike 'ps2xRuntime/include/ps2_recompiled_*' })
    return @($tracked + $added | Where-Object { $_ } | Sort-Object -Unique)
}
# File comparison and copying in C#: 13,000 generated files are too many for a PowerShell loop.
Add-Type @"
using System;
using System.Collections.Generic;
using System.IO;
public static class MksmBuild {
    public static bool SameContent(string a, string b) {
        if (new FileInfo(a).Length != new FileInfo(b).Length) return false;
        byte[] ba = File.ReadAllBytes(a); byte[] bb = File.ReadAllBytes(b);
        for (int i = 0; i < ba.Length; i++) if (ba[i] != bb[i]) return false;
        return true;
    }
    // Copies the sources into targetDir when their content differs and, with removeOthers, removes the files of
    // targetDir that are neither a source nor in keepNames; returns the names copied or removed.
    public static string[] Sync(string[] sources, string targetDir, string[] keepNames, bool removeOthers) {
        var changed = new List<string>();
        var wanted = new HashSet<string>(keepNames, StringComparer.OrdinalIgnoreCase);
        foreach (string source in sources) {
            string name = Path.GetFileName(source);
            wanted.Add(name);
            string target = Path.Combine(targetDir, name);
            if (File.Exists(target) && SameContent(source, target)) continue;
            File.Copy(source, target, true);
            changed.Add(name);
        }
        if (!removeOthers) return changed.ToArray();
        foreach (string existing in Directory.GetFiles(targetDir)) {
            string name = Path.GetFileName(existing);
            if (wanted.Contains(name)) continue;
            File.Delete(existing);
            changed.Add(name);
        }
        return changed.ToArray();
    }
    // Sets the modification time of the unity sources that include one of the names (or of all of them).
    public static int TouchUnity(string unityDir, string[] names, bool all) {
        var set = new HashSet<string>(names, StringComparer.OrdinalIgnoreCase);
        int touched = 0;
        foreach (string unity in Directory.GetFiles(unityDir, "*.cxx")) {
            bool hit = all;
            if (!hit) {
                foreach (string line in File.ReadLines(unity)) {
                    int start = line.IndexOf('"'); int end = line.LastIndexOf('"');
                    if (!line.StartsWith("#include") || start < 0 || end <= start) continue;
                    string included = line.Substring(start + 1, end - start - 1);
                    if (set.Contains(Path.GetFileName(included))) { hit = true; break; }
                }
            }
            if (hit) { File.SetLastWriteTimeUtc(unity, DateTime.UtcNow); touched++; }
        }
        return touched;
    }
}
"@

$rec = $PS2RecompDir
$profile = if ($Release) { 'release' } else { 'quick' }
$bld = Join-Path $rec ("out\build-" + $profile)
$gen = Join-Path $rec 'out\generated'
$stamp = Join-Path $gen '.generated-from'
# Written after a complete copy of the generated code into the runtime; removed when the submodule is reset, because
# the checkout restores upstream's placeholder register_functions.cpp in the runner directory.
$syncStamp = Join-Path $gen '.synced'
$elf = Get-GameElf
$config = Join-Path $RepoRoot 'config\recomp.toml'

Step "PS2Recomp at $PS2RecompCommit with the patches of patches\ (profile: $profile)"
$patches = @(Get-ChildItem -LiteralPath (Join-Path $RepoRoot 'patches') -Filter '*.patch' | Sort-Object Name)
# The state of the tree is "pinned commit + patches" when HEAD is the pinned commit and the working tree matches what
# was recorded after the last application; then nothing is touched and the tools keep their timestamps.
New-Item -ItemType Directory -Path (Join-Path $rec 'out') -Force | Out-Null
$stateFile = Join-Path $rec 'out\.patched-state'
$wanted = $PS2RecompCommit + ' ' + (($patches | ForEach-Object { (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }) -join ' ')
$head = (git -C $rec rev-parse HEAD 2>$null | Out-String).Trim()
$recorded = if (Test-Path -LiteralPath $stateFile) { (Get-Content -LiteralPath $stateFile -Raw).Trim() } else { '' }
if (-not $Reset -and $head.StartsWith($PS2RecompCommit) -and $recorded -eq ($wanted + ' ' + (Tree-State $rec))) {
    Write-Host '  the tree is already the pinned commit with the patches applied'
} else {
    # A reset rewrites every patched file. The files whose content comes back unchanged get their previous time
    # stamps again, so that a new or edited patch rebuilds what it changes and not everything that includes, say,
    # the runtime macros header of the precompiled header.
    $before = @{}
    foreach ($relative in (Patched-Files $rec)) {
        $path = Join-Path $rec $relative
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            $before[$relative] = @((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash, (Get-Item -LiteralPath $path).LastWriteTimeUtc)
        }
    }
    Run 'git' @('-C', $rec, 'checkout', '-q', '-f', $PS2RecompCommit)
    # The patched sources are not part of the pinned tree; out\ keeps the builds and the generated code stays in
    # place (the sync below removes what a regeneration no longer produces).
    Run 'git' @('-C', $rec, 'clean', '-fdq', '-e', 'out', '-e', 'ps2xRuntime/src/runner', '-e', 'ps2xRuntime/include/ps2_recompiled_functions.h', '-e', 'ps2xRuntime/include/ps2_recompiled_stubs.h')
    Run 'git' @('-C', $rec, 'submodule', 'update', '--init', '--recursive', '--quiet')
    foreach ($patch in $patches) {
        Write-Host ("  applying {0}" -f $patch.Name)
        Run 'git' @('-C', $rec, 'apply', '--ignore-whitespace', $patch.FullName)
    }
    $kept = 0
    foreach ($relative in $before.Keys) {
        $path = Join-Path $rec $relative
        if ((Test-Path -LiteralPath $path -PathType Leaf) -and
            (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -eq $before[$relative][0]) {
            (Get-Item -LiteralPath $path).LastWriteTimeUtc = $before[$relative][1]
            $kept++
        }
    }
    Write-Host ("  {0} patched file(s) unchanged by the reset kept their time stamps" -f $kept)
    if (Test-Path -LiteralPath $syncStamp) { Remove-Item -LiteralPath $syncStamp -Force }
    Set-Content -LiteralPath $stateFile -Value ($wanted + ' ' + (Tree-State $rec)) -Encoding ASCII
}

# paraLLEl-GS, the library of upstream's GPU GS backend (patches/0025): one checkout in PS2Recomp\out shared by the
# build profiles, at the commit cmake\ParallelGS.cmake pins, with only the submodules the library is built from (Granite,
# its volk and its Vulkan headers). Left to FetchContent, every build tree would clone all nested submodules (1.4 GB).
$parallelGsArgs = @()
$parallelGsCmake = Join-Path $rec 'ps2xRuntime\cmake\ParallelGS.cmake'
if (Test-Path -LiteralPath $parallelGsCmake) {
    $parallelGsTag = [regex]::Match((Get-Content -LiteralPath $parallelGsCmake -Raw), 'GIT_TAG\s+([0-9a-f]{40})').Groups[1].Value
    if (-not $parallelGsTag) { throw "No GIT_TAG in $parallelGsCmake" }
    $parallelGs = Join-Path $rec 'out\parallel-gs'
    if (-not (Test-Path -LiteralPath (Join-Path $parallelGs '.git'))) {
        Step "Cloning paraLLEl-GS into $parallelGs"
        Run 'git' @('clone', '-q', '--no-checkout', 'https://github.com/Arntzen-Software/parallel-gs.git', $parallelGs)
    }
    if ((git -C $parallelGs rev-parse HEAD 2>$null | Out-String).Trim() -ne $parallelGsTag) {
        Step "paraLLEl-GS at $parallelGsTag"
        git -C $parallelGs cat-file -e "$parallelGsTag^{commit}" 2>$null
        if ($LASTEXITCODE -ne 0) { Run 'git' @('-C', $parallelGs, 'fetch', '-q', 'origin', $parallelGsTag) }
        Run 'git' @('-C', $parallelGs, 'checkout', '-q', '-f', $parallelGsTag)
        Run 'git' @('-C', $parallelGs, 'submodule', 'update', '--init', '-q', 'Granite')
        Run 'git' @('-C', (Join-Path $parallelGs 'Granite'), 'submodule', 'update', '--init', '-q',
                    'third_party/volk', 'third_party/khronos/vulkan-headers')
    }
    $parallelGsArgs = @("-DFETCHCONTENT_SOURCE_DIR_PARALLEL_GS=$($parallelGs.Replace('\', '/'))")
}

Step 'Configuring CMake'
$common = @('-S', $rec, '-B', $bld, '-G', 'Ninja', '-DPS2X_BUILD_STUDIO=OFF', '-DPS2X_ENABLE_SCCACHE=OFF',
            '-DPS2X_ENABLE_RUNNER_UNITY_BUILD=ON', '-DPS2X_ENABLE_RUNNER_PCH=ON', '-DPS2X_ENABLE_LTO=OFF',
            # Upstream's very verbose logs cost more than the game: with them on, writing log files took two thirds
            # of the EE thread's time. The regular runtime logs (PS2X_ENABLE_RUNTIME_LOGS) stay on.
            '-DPS2X_ENABLE_AGRESSIVE_LOGS=OFF', '-DPS2X_IOP_BUILD_TESTS=ON') + $parallelGsArgs
# The VU1 programs translated ahead of time (tools\vu1gen): the hook adds the translator's targets to the runtime's
# CMakeLists without a patch, and the generated library, translated from the ELF during the build, to the runner.
$vu1Hook = (Join-Path $RepoRoot 'tools\vu1gen\cmake\ps2recomp.cmake').Replace('\', '/')
$common += @("-DCMAKE_PROJECT_PS2Runtime_INCLUDE=$vu1Hook", "-DVU1GEN_ELF=$($elf.Replace('\', '/'))")
if ($Release) {
    Run 'cmake' ($common + @('-DCMAKE_BUILD_TYPE=Release'))
} else {
    # The optimization level is set by target (scripts\quick-profile.cmake): /O1 for the generated game code, /O2 for
    # the runtime, where the host time goes.
    $quickProfile = (Join-Path $PSScriptRoot 'quick-profile.cmake').Replace('\', '/')
    Run 'cmake' ($common + @('-DCMAKE_BUILD_TYPE=RelWithDebInfo',
                             '-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=/Zi /DNDEBUG',
                             '-DCMAKE_C_FLAGS_RELWITHDEBINFO=/Zi /DNDEBUG',
                             "-DCMAKE_PROJECT_INCLUDE=$quickProfile"))
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

Step 'Syncing the generated code and the override module into the runtime'
# Files are copied only when their content differs, and the runner files that no longer exist are removed, so that
# ninja recompiles only the unity batches that changed. The unity sources include the runner .cpp files; with a
# localized MSVC ninja does not record those dependencies, so the unity files that include a changed file are
# touched by hand (the CMake re-run below rewrites the unity files when the set of sources changed).
$overrides = @(Get-ChildItem -LiteralPath (Join-Path $RepoRoot 'src') -File -Recurse |
    Where-Object { $_.Extension -in '.cpp', '.h' } | ForEach-Object { $_.FullName })
# Comparing the 13,000 generated files takes a minute, so it is done after a generation (the stamp written at the
# end of a complete sync disappears with the regenerated folder); otherwise only the override module is synced.
if ($needGenerate -or -not (Test-Path -LiteralPath $syncStamp)) {
    $generated = @(Get-ChildItem -LiteralPath $gen -Filter '*.cpp' -File | ForEach-Object { $_.FullName })
    $changed = @([MksmBuild]::Sync([string[]]($generated + $overrides), $runner, [string[]]@('main.cpp'), $true))
    Set-Content -LiteralPath $syncStamp -Value (Get-Date -Format 'o')
} else {
    $changed = @([MksmBuild]::Sync([string[]]$overrides, $runner, [string[]]@('main.cpp'), $false))
}
$headerChanged = $false
foreach ($header in @('ps2_recompiled_functions.h', 'ps2_recompiled_stubs.h')) {
    $source = Join-Path $gen $header
    $target = Join-Path $include $header
    if (-not (Test-Path -LiteralPath $source)) { continue }
    if ((Test-Path -LiteralPath $target) -and [MksmBuild]::SameContent($source, $target)) { continue }
    Copy-Item -LiteralPath $source -Destination $target -Force
    $headerChanged = $true
}
Write-Host ("  runner files copied or removed: {0}, generated headers changed: {1}" -f $changed.Count, $headerChanged)
$unity = Join-Path $bld 'ps2xRuntime\CMakeFiles\ps2EntryRunner.dir\Unity'
if (Test-Path -LiteralPath $unity) {
    $touched = [MksmBuild]::TouchUnity($unity, [string[]]$changed, $headerChanged)
    Write-Host ("  unity batches touched: {0}" -f $touched)
}
Run 'cmake' @('-S', $rec, '-B', $bld)

Step "Building ps2EntryRunner ($Jobs jobs)"
Run 'cmake' @('--build', $bld, '-j', $Jobs, '--target', 'ps2EntryRunner')

if ($Tests) {
    Step 'Building and running ps2x_tests'
    Run 'cmake' @('--build', $bld, '-j', $Jobs, '--target', 'ps2x_tests')
    $testsExe = Get-ChildItem $bld -Recurse -Filter 'ps2x_tests.exe' | Select-Object -First 1
    Push-Location $rec
    try { Run $testsExe.FullName @() } finally { Pop-Location }
    Step 'Building and running the IOP emulator tests'
    # ps2_iop_spu2_tests: the sound unit of patches\0040.
    Run 'cmake' @('--build', $bld, '-j', $Jobs, '--target', 'ps2_iop_emulator_tests', '--target', 'ps2_iop_import_tests',
                  '--target', 'ps2_iop_spu2_tests')
    foreach ($name in @('ps2_iop_emulator_tests.exe', 'ps2_iop_import_tests.exe', 'ps2_iop_spu2_tests.exe')) {
        $iopTest = Get-ChildItem $bld -Recurse -Filter $name | Select-Object -First 1
        if ($iopTest) { Run $iopTest.FullName @() }
    }
}

$exe = Get-ChildItem $bld -Recurse -Filter 'ps2EntryRunner.exe' | Select-Object -First 1
Step "OK: $($exe.FullName)"
