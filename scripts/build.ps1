# Clones PS2Recomp, recompiles the game ELF to C++ and builds the PC executable.
# Called by scripts\2_build.cmd, which first loads the Visual Studio environment (vcvars64).
param([switch]$Traces)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

$elf  = Get-MksmElf
$work = Get-MksmWorkDir
New-Item -ItemType Directory -Force -Path $work | Out-Null
$rec  = Join-Path $work 'PS2Recomp'
$gen  = Join-Path $work 'generado'
$bld  = Join-Path $rec 'out\build'

$template  = Join-Path $RepoRoot 'config\recomp.template.toml'
$funcmap   = Join-Path $RepoRoot 'config\funcmap.csv'
# MKSM-Port: Waffle565's native runtime (runtime branch, c5a9d02..runtime diff).
$waffleRuntimePatch = Join-Path $RepoRoot 'patches\ps2recomp-waffle-runtime.patch'
# MKSM-Port: removes from ps2xTest the tests of parts that Waffle565's runtime replaces by design.
$waffleTestsPatch = Join-Path $RepoRoot 'patches\ps2recomp-waffle-tests.patch'
# MKSM-Port: Waffle565's runtime is built with MSVC; this lets it compile and link with GCC (Linux CI).
$waffleGccPatch = Join-Path $RepoRoot 'patches\ps2recomp-waffle-gcc.patch'
# MKSM-Port: the native VU1 NativeState was lost with every VU1Interpreter (std::shared_ptr).
$vu1nLeakPatch = Join-Path $RepoRoot 'patches\ps2recomp-vu1n-leak.patch'
# MKSM-Port: fast paths for VIF1 UNPACK (from the previous base), with STMOD 3 like Waffle565's generic path.
$vif1UnpackFastPatch = Join-Path $RepoRoot 'patches\ps2recomp-vif1-unpack-fast.patch'
# MKSM-Port: reusable DMA chain buffers (the EE flattened ~2.6 MB per frame into a new vector).
$dmaBuffersPatch = Join-Path $RepoRoot 'patches\ps2recomp-dma-buffers.patch'
# MKSM-Port: rumble looks for Sony (HID) controllers on its own thread, not on the game thread.
$padRumbleScanPatch = Join-Path $RepoRoot 'patches\ps2recomp-pad-rumble-scan.patch'
# MKSM-Port: VBlank at the real NTSC field rate (59.94 Hz): movies ran at 30.00 images/s and audio fell behind.
$vblankNtscPatch = Join-Path $RepoRoot 'patches\ps2recomp-vblank-ntsc.patch'
# MKSM-Port: optional per-frame trace diagnostic (PS2X_GS_WORKER_TRACE_SECS/_PCS, VBlank, GS IRQ, threads).
$gsWorkerTracePatch = Join-Path $RepoRoot 'patches\ps2recomp-gs-worker-trace.patch'
# MKSM-Port: on-screen FPS meter (F2 or MKSM_FPS=1) and a [mksm-fps] line per second in the log.
$fpsOverlayPatch = Join-Path $RepoRoot 'patches\ps2recomp-fps-overlay.patch'
# MKSM-Port: the FPS meter also counts the images that reach the window.
$fpsImagesPatch = Join-Path $RepoRoot 'patches\ps2recomp-fps-images.patch'
# MKSM-Port: the window shows frames in order (it does not jump to the newest one if fewer than 3 are pending).
$presentOrderPatch = Join-Path $RepoRoot 'patches\ps2recomp-present-order.patch'
# MKSM-Port: the worker passes GIF packets to the GS stage in batches (PS2X_GS_STAGE_BATCH).
$stageBatchesPatch = Join-Path $RepoRoot 'patches\ps2recomp-gs-stage-batches.patch'
# MKSM-Port: borderless fullscreen by default (F11) and the debugger hidden at start (F1).
$fullscreenPatch = Join-Path $RepoRoot 'patches\ps2recomp-fullscreen.patch'
# MKSM-Port: the EE checkpoint cycles are handed to the IOP only when their batch is due.
$iopAccumulatedPatch = Join-Path $RepoRoot 'patches\ps2recomp-iop-accumulated.patch'
# MKSM-Port: 64 pacing points per field (was 16): less EE clock lead than sleeping when the GS finishes.
$finePacingPatch = Join-Path $RepoRoot 'patches\ps2recomp-fine-pacing.patch'
# MKSM-Port: 16 EE cycles per backward jump of the generated code and 4 per dispatch (was 32 and 8).
$cyclesHalfPatch = Join-Path $RepoRoot 'patches\ps2recomp-cycles-half.patch'
# MKSM-Port: native VU1 dispatch slot in one cache line (memo in BlockTiming, idleLast).
$vu1Slot64Patch = Join-Path $RepoRoot 'patches\ps2recomp-vu1-slot64.patch'
# MKSM-Port: the rounding of each VU run is switched with the MXCSR on x86-64 (not fegetround/fesetround).
$vuRoundingPatch = Join-Path $RepoRoot 'patches\ps2recomp-vu-rounding.patch'
# MKSM-Port: an MPG only invalidates the native VU1 blocks that read the written range.
$vu1RangeInvalidationPatch = Join-Path $RepoRoot 'patches\ps2recomp-vu1-range-invalidation.patch'
# MKSM-Port: the EE clock catches up with the host clock if it falls behind (every 2048 checkpoints).
$eeCatchUpPatch = Join-Path $RepoRoot 'patches\ps2recomp-ee-catch-up.patch'
# MKSM-Port: with MKSM_PAD_TEST=1 physical controllers are ignored (only the test script drives the game).
$testNoPadPatch = Join-Path $RepoRoot 'patches\ps2recomp-test-no-pad.patch'
# MKSM-Port: presentation uses the display registers latched at the start of each VBlank (no menu flicker in movies).
$displayLatchPatch = Join-Path $RepoRoot 'patches\ps2recomp-display-latch.patch'
# MKSM-Port: sceCdStop (cdvdman:15) of the native IOP: it finishes immediately (unimplemented, it used to hang the IOP).
$cdStopPatch = Join-Path $RepoRoot 'patches\ps2recomp-cd-stop.patch'
# MKSM-Port: DelayThread outside a thread lets the IOP run during the delay (it used to return immediately and
# 989snd hung when stopping the sound system).
$iopDelayOutsidePatch = Join-Path $RepoRoot 'patches\ps2recomp-iop-delay-outside.patch'
# MKSM-Port: PSS demultiplexing stops at the program end code (0x1B9), like libmpeg (without it the game stayed
# at the end of P04MP05.PSW asking for data forever).
$mpegProgramEndPatch = Join-Path $RepoRoot 'patches\ps2recomp-mpeg-program-end.patch'
# MKSM-Port: EE checkpoints only add cycles up to the next event (timer, IOP batch, deadline, end of slice);
# before, each one did all the bookkeeping (over a third of the EE thread).
$eeFastPathPatch = Join-Path $RepoRoot 'patches\ps2recomp-ee-fast-path.patch'
# MKSM-Port: the GS stage yields the GS lock between packets when presentation is waiting for it (the window showed
# 2-8 images per second while the game ran at 60-120).
$gsPresentPriorityPatch = Join-Path $RepoRoot 'patches\ps2recomp-gs-present-priority.patch'
# MKSM-Port: fog/DOF passes that read the Z buffer as PSMCT24 read it on the GPU with the Z format (before, a VRAM
# without depth was decoded on the CPU: no fog and 1024x1024 texels per frame).
$gsZAsColorPatch = Join-Path $RepoRoot 'patches\ps2recomp-gs-z-as-color.patch'
# MKSM-Port: groups consecutive triangles with no common pixels in the shadow volume passes, reducing draw calls and
# texture barriers without changing the order of overlaps.
$gsGroupedVolumesPatch = Join-Path $RepoRoot 'patches\ps2recomp-gs-grouped-volumes.patch'
# MKSM-Port: barriers so GCC/Clang do not hoist DIV.S/SQRT.S out of the MXCSR switch to round-to-nearest.
$fpuBarrierPatch = Join-Path $RepoRoot 'patches\ps2recomp-fpu-barrier.patch'
# MKSM-Port: MKSM_HZ=120: the EE VBlank twice as frequent (game logic up to 120 frames/s); the movie clock stays on
# NTSC fields and host presentation goes up to 120.
$ee120hzPatch = Join-Path $RepoRoot 'patches\ps2recomp-ee-120hz.patch'
# MKSM-Port: at most 2 presented frames of GPU work queued (with the GPU at its limit, the window sat at 0.5-2 images
# per second).
$gsFramesInFlightPatch = Join-Path $RepoRoot 'patches\ps2recomp-gs-frames-in-flight.patch'
# MKSM-Port: native VU1 CLIP without branches (two SSE comparisons instead of six branches).
$vu1ClipBranchlessPatch = Join-Path $RepoRoot 'patches\ps2recomp-vu1-clip-branchless.patch'
# MKSM-Port: native VU1 madd/msub return overflow already at +-FLT_MAX (blocks do not repeat clampv).
$vu1MaddSaturatedPatch = Join-Path $RepoRoot 'patches\ps2recomp-vu1-madd-saturated.patch'
# MKSM-Port: OpenGL context of the GS on Linux with EGL (before only WGL: on Linux the window stayed magenta).
$gsEglLinuxPatch = Join-Path $RepoRoot 'patches\ps2recomp-gs-egl-linux.patch'
# MKSM-Port: MKSM_VU1_CAPTURA also with the native VU1 active (before it only saved with MKSM_VU1_NATIVE_OFF=1, at ~6 FPS).
$vu1NativeCapturePatch = Join-Path $RepoRoot 'patches\ps2recomp-vu1-native-capture.patch'
# MKSM-Port: VF0 and VI0 of VU0 are constants (PR #79), not included in Waffle565's runtime.
$vu0Vf0Patch = Join-Path $RepoRoot 'patches\ps2recomp-vu0-vf0.patch'
$vu0Vi0Patch = Join-Path $RepoRoot 'patches\ps2recomp-vu0-vi0.patch'
# MKSM-Port: lets this repository pass tools\vu1\vu1n_heads.txt (extra native VU1 entries).
$vu1nHeadsPatch = Join-Path $RepoRoot 'patches\ps2recomp-vu1n-heads.patch'
# MKSM-Port: PS2X_ANALYZER_FUNCMAP=<csv> makes ps2_analyzer write its functions as a function map (stripped ELFs).
$analyzerFuncmapPatch = Join-Path $RepoRoot 'patches\ps2recomp-analyzer-funcmap.patch'
# MKSM-Port: sceGsSetDefDispEnv writes the whole display env like libgraph (PMODE with a read circuit on), not only
# DISPFB/DISPLAY: games that apply it with sceGsPutDispEnv showed nothing but the blank (magenta) frame.
$gsDefDispEnvPatch = Join-Path $RepoRoot 'patches\ps2recomp-gs-def-disp-env.patch'
$overrides = Join-Path $RepoRoot 'src\mksm_overrides.cpp'

$git = 'git'
if (-not (Get-Command git -ErrorAction SilentlyContinue)) { $git = Join-Path $env:ProgramFiles 'Git\cmd\git.exe' }

function Run([string]$exe, [string[]]$a) {
    Write-Host ">> $exe $($a -join ' ')"
    & $exe @a
    if ($LASTEXITCODE -ne 0) { throw "Failed ($LASTEXITCODE): $exe" }
}
function Step([string]$t) { Write-Host ''; Write-Host "==== [$(Get-Date -Format HH:mm:ss)] $t ====" }

Step 'Tools'
Write-Host "ELF:  $elf"
Write-Host "Work: $work"
Run $git @('--version'); Run 'cmake' @('--version'); Run 'ninja' @('--version')

if (-not (Test-Path -LiteralPath (Join-Path $rec '.git'))) {
    Step 'Cloning PS2Recomp'
    Run $git @('clone', $PS2RecompUrl, $rec)
}
Push-Location $rec
Run $git @('fetch', 'origin', $PS2RecompCommit)
Run $git @('checkout', '-f', $PS2RecompCommit)
Run $git @('submodule', 'update', '--init', '--recursive')
# checkout -f does not remove the new files that patches created in a previous build, and git apply would fail
# when creating them again. Every file created by any patch in patches\ ("new file mode") is deleted.
foreach ($patchFile in Get-ChildItem -LiteralPath (Join-Path $RepoRoot 'patches') -Filter '*.patch' -File) {
    $created = $null
    foreach ($line in Get-Content -LiteralPath $patchFile.FullName) {
        if ($line -match '^diff --git a/(\S+) b/') { $created = $Matches[1] }
        elseif ($created -and $line -like 'new file mode*') {
            $extra = Join-Path $rec ($created -replace '/', '\')
            if (Test-Path -LiteralPath $extra) { Remove-Item -LiteralPath $extra -Force }
        }
    }
}
Run $git @('apply', '--ignore-whitespace', '--verbose', $waffleRuntimePatch)
Run $git @('apply', '--ignore-whitespace', '--verbose', $waffleTestsPatch)
Run $git @('apply', '--ignore-whitespace', '--verbose', $vu0Vf0Patch)
Run $git @('apply', '--ignore-whitespace', '--verbose', $vu0Vi0Patch)
Run $git @('apply', '--ignore-whitespace', '--verbose', $vu1nHeadsPatch)
Run $git @('apply', '--ignore-whitespace', '--verbose', $waffleGccPatch)
Run $git @('apply', '--ignore-whitespace', '--verbose', $vu1nLeakPatch)
Run $git @('apply', '--ignore-whitespace', '--verbose', $vif1UnpackFastPatch)
Run $git @('apply', '--ignore-whitespace', '--verbose', $dmaBuffersPatch)
Run $git @('apply', '--ignore-whitespace', '--verbose', $padRumbleScanPatch)
Run $git @('apply', '--ignore-whitespace', '--verbose', $vblankNtscPatch)
Run $git @('apply', '--ignore-whitespace', '--verbose', $gsWorkerTracePatch)
Run $git @('apply', '--ignore-whitespace', '--verbose', $fpsOverlayPatch)
Run $git @('apply', '--ignore-whitespace', '--verbose', $fpsImagesPatch)
Run $git @('apply', '--ignore-whitespace', '--verbose', $presentOrderPatch)
Run $git @('apply', '--ignore-whitespace', '--verbose', $stageBatchesPatch)
Run $git @('apply', '--ignore-whitespace', '--verbose', $fullscreenPatch)
Run $git @('apply', '--ignore-whitespace', '--verbose', $iopAccumulatedPatch)
Run $git @('apply', '--ignore-whitespace', '--verbose', $finePacingPatch)
Run $git @('apply', '--ignore-whitespace', '--verbose', $cyclesHalfPatch)
Run $git @('apply', '--ignore-whitespace', '--verbose', $vu1Slot64Patch)
Run $git @('apply', '--ignore-whitespace', '--verbose', $vuRoundingPatch)
Run $git @('apply', '--ignore-whitespace', '--verbose', $vu1RangeInvalidationPatch)
Run $git @('apply', '--ignore-whitespace', '--verbose', $eeCatchUpPatch)
Run $git @('apply', '--ignore-whitespace', '--verbose', $testNoPadPatch)
Run $git @('apply', '--ignore-whitespace', '--verbose', $displayLatchPatch)
Run $git @('apply', '--ignore-whitespace', '--verbose', $cdStopPatch)
Run $git @('apply', '--ignore-whitespace', '--verbose', $iopDelayOutsidePatch)
Run $git @('apply', '--ignore-whitespace', '--verbose', $mpegProgramEndPatch)
Run $git @('apply', '--ignore-whitespace', '--verbose', $eeFastPathPatch)
Run $git @('apply', '--ignore-whitespace', '--verbose', $gsPresentPriorityPatch)
Run $git @('apply', '--ignore-whitespace', '--verbose', $gsEglLinuxPatch)
Run $git @('apply', '--ignore-whitespace', '--verbose', $vu1NativeCapturePatch)
Run $git @('apply', '--ignore-whitespace', '--verbose', $gsZAsColorPatch)
Run $git @('apply', '--ignore-whitespace', '--verbose', $gsGroupedVolumesPatch)
Run $git @('apply', '--ignore-whitespace', '--verbose', $fpuBarrierPatch)
Run $git @('apply', '--ignore-whitespace', '--verbose', $ee120hzPatch)
Run $git @('apply', '--ignore-whitespace', '--verbose', $gsFramesInFlightPatch)
Run $git @('apply', '--ignore-whitespace', '--verbose', $vu1ClipBranchlessPatch)
Run $git @('apply', '--ignore-whitespace', '--verbose', $vu1MaddSaturatedPatch)
Run $git @('apply', '--ignore-whitespace', '--verbose', $analyzerFuncmapPatch)
Run $git @('apply', '--ignore-whitespace', '--verbose', $gsDefDispEnvPatch)
Pop-Location

# The generated code includes <ps2_recompiled_functions.h> from src/runner
$cm = Join-Path $rec 'ps2xRuntime\CMakeLists.txt'
if (-not (Select-String -LiteralPath $cm -Pattern 'MKSM-Port include' -Quiet)) {
    Add-Content -LiteralPath $cm "`n# MKSM-Port include`ntarget_include_directories(ps2EntryRunner PRIVATE `${CMAKE_CURRENT_SOURCE_DIR}/src/runner)"
}

# Optimized in parallel: /O2 on every file, no global linker optimization (/GL, /LTCG)
$rm = Join-Path $rec 'ps2xRuntime\cmake\ReleaseMode.cmake'
$t = Get-Content -LiteralPath $rm -Raw
$t2 = $t -replace '(?m)^\s*/GL\b.*\r?\n', '' -replace '(?m)^\s*/LTCG\b.*\r?\n', '' `
         -replace 'set_property\(TARGET \$\{TargetName\} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE\)', '# IPO disabled (MKSM-Port)'
if ($t2 -ne $t) { Set-Content -LiteralPath $rm -Value $t2 -NoNewline; Write-Host 'ReleaseMode.cmake: LTCG disabled' }

# MKSM-Port: native IOP (tools\iop\iopn_gen.py, Waffle565). MKSM_IOP_MODULOS is the folder left by a run with
# PS2X_IOP_DUMP_MODULES=<folder>. Without it the empty registry is linked and the IRX run in the interpreter.
if ($env:MKSM_IOP_MODULOS -and (Test-Path -LiteralPath (Join-Path $env:MKSM_IOP_MODULOS 'modules.txt'))) {
    Step 'Generating the native IOP'
    Run 'python' @('-I', (Join-Path $RepoRoot 'tools\iop\iopn_gen.py'), $env:MKSM_IOP_MODULOS, (Join-Path $rec 'ps2xIOP\src\native'))
}

Step 'Configuring CMake'
# Per-function traces flush the file on every entry/exit. Enable them only to investigate.
$logsEnabled = if ($Traces) { 'ON' } else { 'OFF' }
Run 'cmake' @('-S', $rec, '-B', $bld, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF',
              "-DPS2X_ENABLE_RUNTIME_LOGS=$logsEnabled", "-DPS2X_ENABLE_AGRESSIVE_LOGS=$logsEnabled", "-DPS2X_ENABLE_IOP_RPC_TRACE=$logsEnabled",
              '-DPS2X_ENABLE_RUNNER_UNITY_BUILD=ON', '-DPS2X_ENABLE_RUNNER_PCH=ON')
# Compiled VU1 (optional): MKSM_VU1_MICROCODIGO is a local folder with microprograms captured with
# MKSM_VU1_CAPTURA. Without it (or empty) the game uses the interpreter.
$vu1Micro = if ($env:MKSM_VU1_MICROCODIGO) { (Resolve-Path -LiteralPath $env:MKSM_VU1_MICROCODIGO).Path -replace '\\', '/' } else { '' }
$vu1Gen = if ($vu1Micro) { (Join-Path $RepoRoot 'tools\vu1\generar_vu1.cpp') -replace '\\', '/' } else { '' }
# MKSM-Port: native VU1 generator (adapted from Waffle565's native port); enabled with MKSM_VU1_NATIVE=1.
$vu1nGen = if ($vu1Micro) { (Join-Path $RepoRoot 'tools\vu1\vu1n_gen.py') -replace '\\', '/' } else { '' }
$vu1nHeads = if ($vu1Micro) { (Join-Path $RepoRoot 'tools\vu1\vu1n_heads.txt') -replace '\\', '/' } else { '' }
Run 'cmake' @('-S', $rec, '-B', $bld, "-DPS2X_VU1_MICROCODE_DIR=$vu1Micro", "-DPS2X_VU1_GENERATOR_SOURCE=$vu1Gen", "-DPS2X_VU1N_GEN_OVERRIDE=$vu1nGen", "-DPS2X_VU1N_HEADS_FILE=$vu1nHeads")

Step 'Building the recompiler'
Run 'cmake' @('--build', $bld, '--target', 'ps2_recomp')

Step 'Generating C++ from the game ELF'
if (Test-Path $gen) { Remove-Item $gen -Recurse -Force }
New-Item -ItemType Directory $gen | Out-Null
$fw = { param($p) $p -replace '\\', '/' }
# ps2_recomp does not open paths with non-ASCII characters (e.g. accents in the repository folder):
# the ELF and the function map are copied to the work folder, which has a short path.
$elfWork = Join-Path $work (Split-Path -Leaf $elf)
$mapWork = Join-Path $work 'funcmap.csv'
Copy-Item -LiteralPath $elf -Destination $elfWork -Force
Copy-Item -LiteralPath $funcmap -Destination $mapWork -Force
(Get-Content -LiteralPath $template -Raw) `
    -replace '@ELF@', (& $fw $elfWork) `
    -replace '@MAP@', (& $fw $mapWork) `
    -replace '@OUT@', ((& $fw $gen) + '/') |
    Set-Content -LiteralPath (Join-Path $work 'config.toml') -Encoding ASCII
$recompExe = Get-ChildItem $bld -Recurse -Filter 'ps2_recomp.exe' | Select-Object -First 1
Run $recompExe.FullName @((Join-Path $work 'config.toml')) | Select-Object -Last 5
Write-Host ("Generated files: {0}" -f (Get-ChildItem $gen).Count)

Step 'Copying the generated code into the runtime'
$runner = Join-Path $rec 'ps2xRuntime\src\runner'
Get-ChildItem $runner -File | Remove-Item -Force
Get-ChildItem -LiteralPath $gen -File | Copy-Item -Destination $runner
Copy-Item -LiteralPath $overrides -Destination (Join-Path $runner 'mksm_overrides.cpp')
Get-ChildItem -LiteralPath (Join-Path $RepoRoot 'src') -Filter '*.h' -File | Copy-Item -Destination $runner
# With a localized MSVC, ninja does not record the /showIncludes dependencies: the .cpp files included from the
# unity files (generated code, register_functions.cpp, overrides) do not force a rebuild. The unity objects are
# deleted so they are always rebuilt with the freshly generated code.
# MKSM-Port: a header change also alters the ABI of the runtime and the tests.
# With no recorded dependencies, keeping those objects or the PCH can mix old interfaces with the new runner
# (C3668 or an access violation).
foreach ($objectRelative in @('ps2xRuntime\CMakeFiles\ps2_runtime.dir',
                             'ps2xTest\CMakeFiles\ps2_test_lib.dir',
                             'ps2xTest\CMakeFiles\ps2_test_function_table.dir')) {
    $objectDir = Join-Path $bld $objectRelative
    if (Test-Path -LiteralPath $objectDir) {
        Get-ChildItem -LiteralPath $objectDir -Recurse -Filter '*.obj' -File | Remove-Item -Force
    }
}
$runnerObjectDir = Join-Path $bld 'ps2xRuntime\CMakeFiles\ps2EntryRunner.dir'
if (Test-Path -LiteralPath $runnerObjectDir) {
    Get-ChildItem -LiteralPath $runnerObjectDir -Filter '*.pch' -File | Remove-Item -Force
    Get-ChildItem -LiteralPath $runnerObjectDir -Filter 'cmake_pch*.obj' -File | Remove-Item -Force
}
$unityDir = Join-Path $bld 'ps2xRuntime\CMakeFiles\ps2EntryRunner.dir\Unity'
if (Test-Path -LiteralPath $unityDir) { Get-ChildItem -LiteralPath $unityDir -Filter '*.obj' | Remove-Item -Force }
# MKSM-Port: the compiled VU1 (vu1_generado\programa*.cpp) does not use the PCH. If the generator produces the same
# text, ninja does not rebuild it even when ps2_vu1.h or ps2_vu1_compiled.inl change, and its blocks access
# VU1Interpreter with the previous layout (infinite wait loops at startup).
$vu1ObjectDir = Join-Path $bld 'ps2xRuntime\CMakeFiles\ps2EntryRunner.dir\vu1_generado'
if (Test-Path -LiteralPath $vu1ObjectDir) { Get-ChildItem -LiteralPath $vu1ObjectDir -Filter '*.obj' | Remove-Item -Force }
Run 'cmake' @('-S', $rec, '-B', $bld)

Step 'Building the optimized game in parallel'
Run 'cmake' @('--build', $bld, '--target', 'ps2EntryRunner')

$exe = Get-ChildItem $bld -Recurse -Filter 'ps2EntryRunner.exe' | Select-Object -First 1
Step "OK: $($exe.FullName)"
