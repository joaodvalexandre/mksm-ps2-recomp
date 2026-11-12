# Research log: Mortal Kombat: Shaolin Monks (PAL, SLES-53524)

Everything tried, with its result, newest entries at the bottom. Addresses are those of `SLES_535.24` unless
marked otherwise. Do not delete findings; correct them when they turn out to be wrong.

## The release

- Disc image: `SLES_535.24.Mortal Kombat Shaolin Monks.iso` (3.36 GB, single layer, PAL), kept outside the
  repository (`D:\Projects\Recomps\iso\` on the development machine). Extracted into `game\` with 7-Zip
  (`7z x <iso> -ogame`): `SLES_535.24` (4,934,920 bytes), `SYSTEM.CNF` (`BOOT2 = cdrom0:\SLES_535.24;1`,
  `VER = 1.02`, `VMODE = PAL`), `FILELIST.DIR`, `GAMEDATA.WAD` to `.WAJ` (seven 270-420 MB archives), `FRONT\`
  (menus, movies, music), `IOP\` (the IRX modules and `IOPRP300.IMG`), `WADCRC.BIN`.
- Executable: entry point `0x0011C038`, `.text` from `0x00100000`. Stripped (no symbols).

## Carried over from the first attempt (2026-10 to 2026-11-03, branch `archive/ntsc-waffle-runtime`)

The first attempt ran on PS2Recomp `c5a9d02` with a 60-patch native runtime taken from a God of War port; it was
abandoned on 2026-11-03 to restart on upstream PS2Recomp. What it established about the game and the recompiler,
independent of that runtime:

- **VU0 clip flags.** The recompiler's `VCLIPw` translation set the clipping flags in the reverse order of the
  hardware (bit 0 must be `x > +w`, bit 1 `x < -w`, then y and z; VU manual, PCSX2 `_vuCLIP`). The game classifies
  every bounding sphere against the frustum with two clips and `andi 0xAAA`; with the bits swapped every object
  was "behind a plane" and nothing in the 3D world was drawn. `patches/0001-vu0-clip-flags.patch` fixes it, with a
  test in `ps2xTest`.
- **Software double libm.** The game ships its own double-precision `floor`, `sqrt`, `log` (software, through
  `dpadd`/`dpmul`/`fptodp`). The recompiler stubs any function whose *name* matches a runtime handler, whatever
  the stubs list says, so naming them after the SDK routed them to the runtime's float handlers: `log(1.0)`
  returned 1.0 and the sound effects played 17 semitones low (the SNDF driver subtracts a base pitch of
  `1200 * log(x) / log(2)` cents that the game computes at init). Keep such functions unnamed (or named
  `libm_dp_*`) so they are recompiled.
- **BC2T/BC2F** must test the VU0 busy state (VPU_STAT bit 0), not the Z flag of the VU0 status register: the
  game's sync path spins with `bc2t .` until VU0 is idle and otherwise never leaves it.
- **DMAC D_STAT.** The VU0 upload path polls `D_STAT` for the channel interrupt bits; a runtime that never sets
  them leaves the fight loader waiting forever.
- **Function map.** A function reached only through a vtable right after a `jr $ra; nop` plus a padding `nop`
  gets merged into the previous function by naive splitting; the level loader calls one of those.
- **Sound.** The sound effects are uploaded by the EE through `SDRDRV.IRX` (RPC `0x80000701`); faking the
  transfers leaves them silent. The ADX music played with a crackle (open). Text flickered on screen (open).
- **Rendering.** Per frame the game sends about 284 KB of 2D GIF data through VIF1 `DIRECT` (full-screen passes,
  HUD, particles) plus the world through VU1 microprograms launched with `MSCAL`/`MSCNT`; the scene is rendered
  into frame buffer `fbp=0x70` and composed with full-screen untextured sprites. Its frustum test is at
  NTSC `0x22C838` (PAL address not yet located).

## 2026-11-03 — restart on upstream PS2Recomp

- Repository wiped (history kept in `archive/ntsc-waffle-runtime`), `PS2Recomp` added as a submodule pinned at
  `e563698` ("GIF: treat FLG=3 like IMAGE everywhere a GIFtag is parsed"), four commits after the previous pin.
  Upstream's runtime has a CPU GS backend, a VU1 interpreter, VIF1, audio, pad, a virtual file system and the IOP
  emulator (`ps2xIOP` runs the disc IRX modules; generic HLE for MCSERV, LIBSD, DBCMAN), windowed through raylib.
  Its README warns that VU and GS performance is very bad and that many hardware paths are stubbed.
- Build: `scripts\build.cmd` (patches, CMake/Ninja, tools, analyzer, generation, runner); `scripts\run.cmd`.
- The disc is served from the files in `game\` (upstream's `sceCdSearchFile` registers each file with a pseudo
  sector number); `MKSM_ISO=<image>` also gives the runtime the image for reads outside those files.
- **First build stopped by the user ("super slow"): the recompiler was hanging on the configuration, not compiling.**
  `ps2_recomp` printed `parsing config` and then sat at 4.6 MB forever. Bisecting the TOML showed every section
  parses; the culprit was a hand-written header comment containing a backspace character (`scripts\build.ps1`
  typed through a non-raw string became `scripts<BS>uild.ps1`): toml11 loops on a control character inside a
  comment instead of rejecting it (worth an upstream report). With the comment rewritten the recompiler finishes
  the whole generation in under a minute: 13,588 functions discovered on the stripped ELF, 0 decode failures.
- **Second build stopped by the user: the final link, not the compile.** All 425 unity objects had compiled in
  about 20 minutes; what then sat there eating memory was `link.exe` with `/LTCG` (upstream's
  `EnableFastReleaseMode` adds `/GL` and `/LTCG` to Release builds). `patches/0002-lto-option.patch` adds
  `PS2X_ENABLE_LTO` (default ON, as upstream) and the build turns it off. The default build is now a **quick
  profile** (RelWithDebInfo, `/O1`, debug info, 8 jobs, `out\build-quick`), with `-Release` for the optimized one,
  and the C++ is only regenerated when `config\recomp.toml`, the ELF or the recompiler changed.

## 2026-11-04 — first boot on upstream's runtime

- **Quick build timing.** Tools and generation in 2 minutes, the 425 unity batches of the runner in 15 minutes
  with 8 jobs, link in seconds (`PS2Recomp\out\build-quick\ps2xRuntime\ps2EntryRunner.exe`, 145 MB with debug
  info). A change to the override module or to a runtime source rebuilds in about a minute.
- **The game boots.** The runtime loads the ELF, applies the override, and the game initializes the SIF, loads
  its nine IOP modules from `IOP\` into the IOP emulator (`SIO2MAN`, `PADMAN`, `LIBSD`, `SDRDRV`, `MCMAN`,
  `MCSERV`, `CDVDSTM`, `CRI_ADXI`, `SNDFI`; the ADX driver prints its banner and calls `sceSdInit`), registers
  its VSync callback and then hangs with one active thread at `0x386030`.
- **Hang 1: `sceCdLayerSearchFile` (0x385F90).** The analyzer named the function; it is the SDK routine that
  `sceCdSearchFile` wraps in this SDK version. It binds the IOP server `0x80000597` (cdvdfsv's search file
  service) lazily and retries forever while `sceSifBindRpc` returns an error or leaves `client->server`
  null. Upstream has no cdvdfsv server (the IOP emulator only routes the services of loaded modules) and
  serves the file system from the files next to the ELF through its `sceCdSearchFile` stub instead, so the
  game never reached it. Fix: `"sceCdSearchFile@0x00385F90"` in the `stubs` list of `config/recomp.toml`
  (a stub entry with an address binds that function to the named runtime handler; the extra `layer` argument
  is ignored, the disc is single layer).
- **18 seconds of boot in a `malloc` search.** Before loading the IOP modules the game looks for its largest
  allocatable block: `malloc(0x1FF0000 - heap start)`, and on failure 4 bytes less, until a call succeeds
  (`0x213318`-`0x21332C`). The runtime ends the guest heap at `0x1F00000` (its RPC, TLS and boot pools sit
  above it, `Kernel/Syscalls/Helpers/State.h`), so about a quarter of a million calls fail, each one through
  the game's `sbrk` (`0x4807F0`: DI, `EndOfHeap` syscall, EI). The override module now serves `sbrk`
  natively (`replaceFunction`), with the same break pointer (`0x564294`), `errno` (through `_impure_ptr` at
  `0x563A7C`) and limit (`guestHeapLimit()`), after checking two instruction words of the original. The
  game keeps 1 MB less heap than on the console; something to remember if it ever runs out.
- **Frame counter** (`patches/0003-window-title-frame-counter.patch`): the window title shows the displayed
  frames and their rate (`PS2-Recomp | SLES_535.24 | frame 1234 | 50.0 fps`), refreshed twice a second. A
  displayed frame is a change of a `DISPFB` register (the display buffer flip), counted per output circuit
  with the busier circuit reported; the vsync tick of the scheduler was not usable because it advances with
  host time whether or not the game draws. Test in `ps2xTest` (`PS2Memory` case).
- **Runtime debug panel.** Upstream's runner opens an ImGui "Runtime Debugger" over the game (CPU registers,
  threads, kernel objects, IOP, RPC history, pad, file/CD, GS, DMA, logs, memory); `F1` hides and shows it.
  `scripts\screenshot.ps1` needed its own `POINT` struct (`System.Drawing.Point` is not visible to `Add-Type`
  without a reference).
- **Aggressive logs.** `PS2X_ENABLE_AGRESSIVE_LOGS` is on by default upstream: `[run:tick]` lines every 120
  host frames with the guest PC, the first GS packets and registers, every `sceDmaSend`. Useful while booting;
  to be turned off in the quick profile once the game renders.
- **Every build was a full build.** `scripts\build.ps1` checked out the pinned submodule and re-applied the patches on
  every run; the patched recompiler sources got new timestamps, `ps2_recomp.exe` was relinked, and since the
  generation stamp compared timestamps, the 13,588 functions were regenerated and all 428 unity batches
  recompiled (20 minutes) for a one-line override change. The script now leaves the submodule alone while
  `HEAD` is the pinned commit and the working-tree diff hashes to what was recorded after the last patch
  application (`out\.patched-state`, `-Reset` forces the checkout), and copies regenerated files into the runtime
  only when their content differs, touching the unity batches that include them (ninja does not record those
  dependencies with a localized MSVC).
- **Hang 2: the loader waits for a disc read that nothing completes.** With the search-file fix the game finds
  `FILELIST.DIR`, `WADCRC.BIN`, `GAMEDATA.WAD`-`.WAJ` and the movies (`BLITZ.SFD` and `GAUNTLET.SFD` do not
  exist on the PAL disc; the game goes on), then sits in a loop at `0x211E40`-`0x211F08` that calls `0x319CB8`
  and `sceGsSyncV(0)` every frame (`MKSM_TRACE_SYNCV=1`: one call per vsync tick, return address `0x211E58`;
  the runtime debugger shows the main thread in `WAIT/VSYNC`). The loop is a state machine over the streaming
  reader (`0x26F8F8` start, `0x26F998` poll, `0x26FA48` finish, object at `0x678710`): state 2 polls a status
  byte that the disc device layer (`0x413518`, `0x41E278`: `sceCdRead`, then `sceCdSync(1)` until done, then
  `0x41E148`) sets to 3 when the read completes. That device state machine is pumped (`0x41E638`) from the
  game's worker threads, not from the main loop.
- **The worker threads were never created.** The streaming init (`0x26F350`, the function that also registers
  the vsync callback) calls `0x414FF0`, which creates four threads (`0x414858`, `0x4148B0`, `0x4149B8`,
  `0x414AA8`; stacks `0x534510`...) with `initial_priority` = the priority `ReferThreadStatus` reports for the
  main thread, and only afterwards raises the main thread's priority. The runtime's main thread has priority 0
  and upstream's `EeScheduler::createThread` rejects priorities below 1 (`MKSM_TRACE_THREADS=1`: all four
  `CreateThread` calls return -403, `KE_ILLEGAL_PRIORITY`). On the EE kernel priorities run from 0 (highest) to
  127 and the main thread starts at 0, so the game works on hardware. `patches/0004-thread-priority.patch`
  accepts 0 in `createThread` and `changePriority` (test in the kernel tests). The vsync callback (`0x26F2A0`
  -> `0x415650`) wakes those threads with `iReferThreadStatus`/`iWakeupThread` (negative syscall numbers, which
  upstream's dispatcher handles).
- **Diagnostics in the override module** (environment variables, off by default): `MKSM_TRACE_SYNCV=1` logs the
  game's vsync wait wrapper (`0x24D050`) with the caller's return address, the vsync tick and, every 300 calls,
  the scheduler's thread list; `MKSM_TRACE_THREADS=1` logs the arguments and results of the game's
  `CreateThread`/`StartThread` wrappers. The override also makes stdout unbuffered so that `logs\run.log` is
  complete when `scripts\run.cmd` stops the game.
- **Tools.** `tools/ee_disasm.py` from the archived branch (capstone, with the names of `config/recomp.toml`)
  was used for all of the above; the generated code's `dispatchGuestBranch(rdram, ctx, 0x...u` calls give the
  static call graph (callers of a function: grep the runner sources), and the syscall wrappers of the game are
  the `addiu v1, zero, N; syscall; jr ra` triplets (negative N = interrupt-safe variant).
- **Hang 3: the main thread starves its workers.** With the threads created, the first loader pass completes and
  the game enters its resource loader (`0x3794B0`: `while ([0x755DB8] & 1) pump(0x378D08)`, no vsync wait): the
  pump polls the streaming read's status byte (lock, read, unlock: `MKSM_TRACE_CALLS` counted 155,000 polls and
  247,000 `WaitSema`/`SignalSema` pairs in 30 s) while the read itself is completed by the worker threads, which
  the vsync callback wakes every frame. On the console the workers preempt the main thread because the streaming
  init lowered it to priority 24. Here it ran at 0: the game's critical-section helpers (`0x414638` enter,
  `0x4146C0` leave) do `old = ChangeThreadPriority(self, 1)` and later `ChangeThreadPriority(self, old)`, and
  upstream's syscall returned `KE_OK` (0) instead of the previous priority, which the EE kernel returns. The
  trace showed `ChangeThreadPriority(1, 24)`, then `(1, 1)`, then `(1, 0)`. `patches/0004-thread-priority.patch`
  now also returns the previous priority (test extended).
- **Call tracer.** `MKSM_TRACE_CALLS=0xADDR,...` (up to 32 functions) logs the first eight calls of each with the
  argument registers and the caller, and a count summary every 3000 calls of the first one (`MKSM_TRACE_SUMMARY=<n>`
  for another interval); it replaces the functions in the runtime's dispatch table, so it sees every caller. With
  each summary come the six busiest callers of every traced function since the previous summary (`[mksm-calls]
  callers of 0xADDR: 0xRA xCOUNT ...`, the return addresses: the loop that calls it), the watched memory
  (`MKSM_TRACE_WATCH`), the threads (`[mksm-threads] at call <n>`) and the display registers (`[mksm-gs]`).
- **Hang 4: the EE clock falls behind the host clock.** With the priority fixed, the loader progressed (ten
  reads, the worker jobs ran in bursts) and then stalled again in the same polling loop, with the game's vsync
  counter (`0x534470`) frozen at 18 and the vsync callback never invoked (`MKSM_TRACE_CALLS` on `0x26F2A0`,
  `0x415650`, `0x414BE8`). Upstream's scheduler fires the VBlank when the EE cycle counter reaches the event's
  cycle and the host time has passed its deadline; the cycle counter only advances by the cycles accounted at
  checkpoints (8 per dispatch, 32 per loop back-edge), while the polling loop spends about 150 microseconds of
  host time per iteration in `WaitSema`/`SignalSema`. One frame of EE cycles (4.9 million) took minutes of host
  time, so no vsync, no callback, no worker wake-ups. Idle waits were fine because `waitForEvent` jumps the cycle
  counter to the deadline. `patches/0005-ee-clock-catches-up-with-host.patch`: `checkpointDue` compares the host
  clock with the next deadline's host time (every 64 checkpoints) and accounts the missing cycles when it has
  passed, so the timers and the IOP advance too; test in the interrupt tests (a guest loop sleeping 250 us per
  checkpoint receives the VBlank within a couple of host frames).
- **Where the time goes (host profile).** With the four hangs fixed the game's worker threads ran continuously
  and the main thread, the lowest priority, never got the CPU. A sampling profiler of the executor's host call
  stack (`MKSM_PROFILE=host` in the override: `SuspendThread`, `StackWalk64`, names from the quick build's PDB)
  showed where the time really went: about 90 per cent inside the IOP interpreter
  (`IopEmulator::Impl::runCycles` -> `step` -> `executeInstruction`, `IopMemory::read32`,
  `IopImportRegistry::decode`, `servicePendingDmaInterrupts`), entered from `EeScheduler::accountCycles` on every
  checkpoint; the game's own code was a few per cent. Upstream runs the IOP on the EE executor thread in lock
  step with the accounted EE cycles, one IOP cycle per eight. That is cheap while the IOP is idle (the emulator
  skips to the next wake-up), but once the game's IOP modules are busy the interpreter, at roughly ten million
  instructions per second, cannot emulate a 36 MHz IOP in real time, and patch 0005's catch-up handed it whole
  frames of cycles at once. `patches/0006-iop-emulation-budget.patch`: large cycle steps become a bounded debt
  that is run in 2048-cycle chunks while the IOP has used less than a quarter of the host time of the current
  10 ms window; beyond that the IOP falls behind instead of the EE (test: a synthetic module whose thread spins
  must not stall a half-second request). With it the main thread runs its loader loop again (0x378D08 at 2,700
  calls per second).
- **Logging was the next cost.** The following profile put `NtWriteFile` at 68 per cent: upstream's aggressive
  logs (`[gs:reg]`, `[sceDmaSend]`, `[sceSifSetDma]`, `[run:tick]`, two `std::cerr` lines per SIF transfer, 7 ms
  per `sceSifSetDma`) written unbuffered to the log files. The quick profile now configures
  `PS2X_ENABLE_AGRESSIVE_LOGS=OFF` (a full rebuild, the define is public to the runner); the regular runtime logs
  stay. The 10 Hz vsync experiment (patch 0099) was removed: the game progressed no further with it, which is
  what pointed at the IOP rather than at the frame budget.
- **Not usable:** `PS2Runtime::iopDebugSnapshot()` returned zero cycles, instructions and threads from a host
  thread while nine modules were loaded; the IOP instruction rate had to be inferred from the profile.
- **Hang 5: a 64-bit sign test compiled as 32-bit.** With the logs off the loader loop runs at full speed
  (28 million polls a minute) but its last request never completes. The WAD set's table of contents (`PWF `
  header, 8-byte entries, `0x19000000`-byte file segments) has two entry formats, told apart by bit 63 of the
  entry loaded with `ld`: `0x211B74 ld a2; bgez a2` (plain offset/size pair) versus a packed form. The recompiler
  translated `bgez` as `GPR_S32(ctx, 6) >= 0`, the sign of the low word (`0x6D400028`, positive), so the packed
  entry `6D400028 803AA002` was read as offset `0x6D400028` (file 4, WAH, at `0x09400028`) with size
  `0x803AA002`: a negative sector count, a failed device read, status 1 instead of 3, and the loader polls for
  ever. On the R5900 `blez`, `bgtz`, `bltz`, `bgez` and their likely/link forms test the whole 64-bit register
  (for a 32-bit value the result is the same, since results are sign-extended).
  `patches/0007-branch-sign-tests-64bit.patch` emits `GPR_S64` for those branches (test in the code generator
  tests). Found with `MKSM_TRACE_CALLS` on the loader's request chain and `MKSM_TRACE_WATCH` on the device and
  CD objects, then by reading the WAD header and the decode routine `0x211AF0`.
- **Crash 6: guest callbacks ran on the main thread's stack.** With patch 0007 the game got through its loader,
  and a new, intermittent failure appeared: the game thread died with "No mapping for the Unicode character
  exists in the target multi-byte code page" while loading its second IOP module. `MKSM_TRACE_STRINGS=1` on the
  module-load wrapper showed the path buffer (on the main thread's stack at `0x1FFFF20`) with garbage in its
  first eight bytes and the name cut after eight; a run with watches instead of string dumps did not crash at
  all. Upstream reserves the stacks for guest invocations (the vsync callback, interrupt handlers) from the top
  of RAM (`m_asyncCallbackStackTop = PS2_RAM_SIZE`), which is where the game's main thread keeps its stack
  (SetupThread with stack -1: sp just below `0x2000000`), so a vsync callback that fires inside a function
  overwrites that function's locals. It became visible once the vsync ran in real time (patch 0005) and the
  boot was slow enough (the string dumps) for a callback to land during the path build.
  `patches/0008-callback-stacks-below-main-stack.patch` moves the invocation stacks to `0x1F40000`-`0x1FE0000`,
  above the runtime's guest pools and below any plausible main stack (test: the IRQ handler's sp is in that
  range).
- **Crash 7: `sceDmaRecvN` was not implemented.** With patches 0007 and 0008 the game finished its loader in
  about two seconds and went on (new vsync-waiting loops at `0x271A58`, 25 frames, and `0x226EA8`), then ended
  with "Unimplemented PS2 stub called: sceDmaRecvN" from `0x43DF60`, a routine that saves the scratchpad:
  `sceDmaGetChan(8)` (fromSPR), a direct write of SADR (`sw` to `0x1000D080`) and
  `sceDmaRecvN(chan, 0x75F400, 0x400)` (0x400 quadwords, the whole 16 KB). Upstream implements the sends, not the
  receives, although its memory model already performs scratchpad transfers (`tryProcessScratchpadDma`).
  `patches/0009-dma-recv-normal.patch`: normal-mode receive (MADR, QWC, CHCR with STR, normal mode, direction
  to memory, exactly what the game's own libdma does at `0x3848B0`), test in the memory tests.
- **Background audit.** A read-only workflow (two auditors, two skeptical verifiers) was started to list, ahead
  of the crashes, the stubs the game binds that upstream leaves unimplemented or incomplete, and other 32/64-bit
  translation errors of the kind fixed by patch 0007.
- **The game runs without crashing.** With patch 0009 the game ran a full 75-second session: the vsync callback
  at the real rate (4,200 in 71 s), the scratchpad save 8,000 times, the EE thread idle 78 per cent of the time,
  every guest thread asleep between frames. A backtrace of the main thread (heuristic: stack words that follow a
  `jal`/`jalr`, printed by the override's thread dump) shows the frame loop
  `0x1DB9F8` -> ... -> `0x272DE0` -> `0x43E0F8` -> `0x4158D8`, the game's frame sync: it sets "main waiting"
  (`0x53443C`) and sleeps until worker thread 3 (`0x4148B0`) wakes it after its per-frame job. That is the
  normal steady state of a frame loop, not a hang.
- **The frame counter never counted.** The title kept saying `frame 0` because this game flips with
  `sceGsSwapDBuff`/`sceGsPutDispEnv`, which upstream implements as handlers that assign the display registers
  directly (`applyGsDispEnv`), bypassing the privileged register stores that patch 0003 hooked. Patch 0003 now
  counts at each vertical blank in the EE scheduler: a frame is a vblank at which DISPFB1 or DISPFB2 differs
  from the previous vblank, whatever wrote it (test updated).
- **The magenta screen** is upstream's placeholder when the GS backend finds no valid display (PMODE without an
  enabled circuit or an empty DISPLAY/DISPFB), not a rendering of the game. The override's call tracer now also
  prints PMODE, SMODE2, DISPFB1/2, DISPLAY1/2 and BGCOLOR with each summary (`[mksm-gs]`).
- **The display comes up, the first movie never shows.** With the frame counter fixed the title read `frame 3`:
  `sceGsResetGraph(0, 1, 3, 1)` (PAL), `sceGsSetDefDBuff(psm 0, 512 x 512)` and one `sceGsSwapDBuff`, after
  which PMODE, DISPFB and DISPLAY describe a valid 512 x 512 display and the window turns black instead of
  magenta. The main thread was then in CRI's Sofdec movie player (`0x270038`, `c:/Projects/Ps2/Cri/movie.cpp` in
  the ELF, the Midway logo): once per vsync it sleeps in the frame sync and runs one step (`0x26FEA8`); it
  created its MPEG decoder (`sceMpegCreate`, `sceMpegInit`, four `sceMpegAddCallback`) but never called
  `sceMpegGetPicture` in 1,500 steps, so no picture, no swap request (`0x4FC09C`, cleared only by the vsync
  pump `0x272E08` that calls `sceGsSwapDBuff`), and the game would stay on that movie. Movie playback (Sofdec's
  stream through the IOP, the runtime's MPEG/IPU handlers) is left for later.
- **Movie skip.** The player's loop leaves when the word at offset `0x1EC` of the player object is 1 and then
  tears the movie down as on a normal stop. The override wraps the step function to set it:
  `MKSM_SKIP_MOVIES` (default on, `MKSM_SKIP_MOVIES=0` plays the movies), logged as `[mksm] movie skipped`.
- **Missing targets.** After the skip the game tail-jumped to `0x468B50`, which had no recompiled function:
  "[guest-branch:missing-target] ... target=0x468b50". It is libmpeg's `sceMpegDelete` (`jr ra; li v0, 1`), merged
  by the analyzer into the stubbed `sceMpegInit` range. `tools/missing_targets.py` lists every direct `jal`/`j`
  whose target is neither a generated function nor a resume label of one: 76, of which 72 come from
  `sub_0049A9A0`, a "function" spanning the last megabyte of the code segment (data decoded as code), two
  only from stubbed functions whose bodies never run, and two real: `0x385BD8` (the libcdvd callback dispatch,
  merged into the stubbed `sceVu0RotTransPers` range, reached from the CD read interrupt) and `0x487890` (the
  end of the kernel init after `iSetTimerAlarm`). `config/recomp.toml` now binds `sceMpegDelete@0x00468B50` and
  declares the other two as entry points.
- **Translation audit.** A read-only audit of the recompiler for 32/64-bit and MMI errors (the workflow above)
  found, among others: `CVT.W.S` rounded to nearest (`nearbyintf`) where the EE truncates toward zero and
  saturates, so every float-to-int conversion of the game could be one off; `PABSW`/`PABSH` read `rs` (always
  0 in that encoding), so their 25 uses in the game (seven functions around `0x2A6260`-`0x2ACCA0`) always gave 0;
  `PHMADH` squared its operands and accumulated into one 64-bit value (12 uses in `0x438280`, a 32-tap
  fixed-point filter); `PEXEW` and `PEXEH` swapped adjacent pairs instead of exchanging elements 0 and 2;
  `PROT3W` rotated all four words; `MOVZ`/`MOVN` copied 128 bits. `patches/0010-mmi-fpu-semantics.patch` fixes
  them (tests in the code generator and float semantics tests). Not changed: `LQ`/`SQ` do not clear the low four
  address bits (no unaligned use found in the game), and latent errors in instructions the game does not use
  (`BLTZALL`/`BGEZALL` link, `PMTHI`/`PMFHI` widths, `PSLLVW` lanes).
- **The game renders.** With the movies skipped and `sceMpegDelete` reachable, the window title reached
  `frame 815 | 10.0 fps` after 80 seconds: the game flips its 640-wide double buffer (`DISPFB` `FBP 0`/`0x50`)
  and the host profile is dominated by the CPU GS backend (`WritePixel`, `DrawTriangle`, texture sampling with
  CLUT). The window still showed the magenta placeholder: PMODE and SMODE2 were 0, so no read circuit was on.
- **`sceGsSetDefDispEnv` left PMODE at 0.** The only code that writes the display registers is
  `sceGsPutDispEnv` (a runtime handler) from a structure filled by `sceGsSetDefDispEnv` (also a handler). The
  game's own libgraph version (`0x3831F0`) stores `pmode = 0x66` (read circuit 2) and SMODE2 from the video mode
  (2 non-interlaced, 3 interlaced in frame mode, 1 interlaced in field mode); upstream's handler only wrote DISPFB and DISPLAY,
  leaving zeros in a fresh structure. `patches/0011-gs-def-disp-env-pmode.patch` fills both (test in the GS
  tests). DISPLAY's DX/DY still ignore the video mode's base offsets (the host presentation does not use them).
- **Stub bindings need a function start.** `sceIpuSync@0x0046F700`, `sceVu0RotMatrixX@0x00385898`,
  `InitTLB@0x00487C10` and `sceMpegDelete@0x00468B50` are in the stubs list but nothing is bound: the recompiler
  applies a stub only to an existing function start, and the analyzer merged these into the previous range. An
  entry point at the same address makes the recompiler synthesize the function (or a resume label), which then
  runs the game's own code; the runtime handler is still not bound to it. `sceMpegDelete` works that way (the
  game's body is `return 1`). A recompiler change (split the range at a stub address) would be the real fix.
- **Builds were full rebuilds.** Upstream tracks a placeholder `register_functions.cpp` in the runner directory;
  the build overwrote it with the generated one, so the "pinned commit + patches" check never matched and every
  build reset the submodule, rewriting all patched files; since patch 0010 changes the runtime macros header of
  the precompiled header, every build recompiled all 425 unity batches. `scripts\build.ps1` now leaves the
  generated files out of the check and, when a reset is needed (a new or edited patch), gives the files whose
  content did not change their previous time stamps back, so only what a patch changes is rebuilt.
- **First screen (2026-11-04, 23:47).** With patch 0011 the display is on (PMODE `0x66`, SMODE2 3) and the
  window shows the game's first screen: a dialog frame with ornate gold corners on a dark blue panel and a faint
  dragon emblem in the middle, at a steady 12 frames per second (the game flips every frame it finishes; the
  quick build is CPU-bound in the GS rasterizer). The picture does not change over a minute and has no text: it
  is most likely the boot message box (memory card check) waiting for something, with the text missing. Two
  audit findings can explain the missing text and are next: the libpkt tag counts doubled by the stubbed helpers
  (2D drawing) and `sceGsExecLoadImage` uploading images eight times too far into VRAM.
- **Debug panel.** Upstream's runtime debugger now starts hidden (`patches/0012-debug-panel-starts-hidden.patch`,
  F1 shows it), so the window shows the game. No test: the panel is only built into the runner.
- **Build state check, again.** After a reset the checkout restores upstream's placeholder
  `register_functions.cpp`; with nothing to regenerate, the copy of the generated code was skipped and the runner
  was built with an empty dispatch table ("No exact recompiled function for guest PC 0x26fea8", the entry point
  missing). A reset now forces a complete copy of the generated code (the `.synced` stamp is removed).
- **Audit results (all verified by a second agent).** Confirmed and pending: libpkt double QWC (remove the five
  libpkt stubs, the game's own library is plain memory code), `sceGsExecLoadImage`/`StoreImage` DBP times 8,
  `sceGsPutDrawEnv` ignoring NLOOP (TEST_1, context 2 and the clear sprite never applied) and `sceGsSetDefClear`
  writing nothing, `sceGsSetDefDBuff` reading arguments 5-7 from the stack instead of `$t0`-`$t2` (EE ABI),
  `sceVu0RotTransPers` with the 12.4/integer modes swapped, `sceVu0InnerProduct`/`Normalize`/
  `NormalLightMatrix` using w, `rand` returning 15-bit host values instead of the game's LCG, the IOP heap
  library half stubbed (free and query do nothing), and the MPEG HLE having no data path for Sofdec (movies).
  Refuted as unreachable today: `sceSdTransToIOP` (TODO), `sceSifSendCmd`, and the unbound stub entries.

## 2026-11-05 — libgraph packets, libpkt and scratchpad DMA

- **The old port's path from this screen.** The archived NTSC port got from this same screen to the main menu with
  five fixes: the drawing environment's packet, the image upload's block address, libpkt's tag counts, scratchpad
  DMA addresses and the pad's DMA buffer (`docs/MKSM.md` of `archive/ntsc-waffle-runtime`). Each was checked again
  against upstream's current code and the PAL ELF. The PAL code is not at the NTSC addresses in most places
  (`sceGifPkCloseGifTag` at `0x384F10` instead of `0x384C60`, the movie player at `0x270038` instead of `0x26FD80`), so every address here
  was found again in the PAL code.
- **libgraph's drawing environment** (`patches/0013-gs-draw-env-packets.patch`, three tests). `sceGsPutDrawEnv`
  takes the GIF tag in front of a drawing environment and, like the game's own version, sends it with its NLOOP
  quadwords through the GIF channel; upstream read eight register pairs from the tag itself, so the tag became a
  register and TEST_1, context 2 and the clear were lost. `sceGsSwapDBuff` sends the environment the same way, the
  clear sprite included. `sceGsSetDefDBuff` takes ztest, zpsm and clear from `$t0`-`$t2` (the EE passes eight
  arguments in registers; upstream read the caller's stack), counts the clear in NLOOP (8 or 14), fills both clears
  and puts the second buffer after the first, as libgraph does. `sceGsSetDefClear` writes libgraph's six pairs
  (upstream wrote nothing). `sceGszbufaddr` takes `(psm, w, h)` from `$a0`-`$a2` and the video mode from the
  runtime: upstream read its width and height one register too far, and the video mode back from scratchpad offset
  0x100, where the game builds packets, with libgraph's layout instead of the runtime's.
- **Image uploads** (`patches/0014-gs-load-image-packet.patch`, one new test, upstream's round trip adapted).
  `sceGsSetDefLoadImage` writes libgraph's 6-quadword packet (DBP as given) and `sceGsExecLoadImage` sends it with
  the image as one GIF packet: upstream's GS parses each transfer as a whole packet, so libgraph's two transfers
  cannot be reproduced as they are. Upstream kept a private 12-byte record there and scaled its DBP by 8, which put
  the CLUTs the game uploads with `sceGsExecLoadImage` outside GS memory. (The game's texture loader, `0x269F1C`-
  `0x269FC4`, also builds such packets in its objects but never sends them: it uploads with a libpkt chain of its
  own, `0x268900`.) The store side keeps its record, with SBP no longer scaled.
- **libpkt and `rand`** (`config/recomp.toml`). The five libpkt helpers the analyzer bound to runtime handlers are
  the game's code again. The handlers rewrite the open DMA tag's QWC each time they move the packet pointer, and the
  game's own terminate (`0x384C58`, `0x385028`) adds the count when it closes the tag, so every chain they touched
  counted its quadwords twice and the DMAC read data as tags. `rand` is the game's code too: a 31-bit LCG on its
  own seed, where the handler returned 15-bit host values and ignored `srand`.
- **Scratchpad DMA addresses** (`patches/0015-dma-scratchpad-address.patch`, test in the memory tests). Bit 31 of
  a DMA address selects the scratchpad, which is what libdma's `CheckAddress` (`0x384480`) makes of `0x7xxxxxxx`;
  the game builds its text packets in the scratchpad and sends them with `sceDmaSend(gif, (pkt & 0x3FF0) |
  0x80000000)` (`0x37E820`, also `0x242D20` and `0x27D7C8`). Upstream read main memory at the offset, turned
  `0x7xxxxxxx` into `0x1xxxxxxx`, and dropped bit 63 of a tag's address. `ps2DmaAddress` (in a header of its own,
  since `ps2_memory.h` is in the runner's precompiled header) now applies to D_MADR, D_TADR, the tags and the
  recompiler's direct GIF kicks.
- **Patch 0011's wording.** Its comment and test called SMODE2 3 "interlaced field mode"; libgraph's FFMODE 1 is
  frame mode (SMODE2 FFMD set, every line read), so they now say frame mode. The values were right.
- **Full rebuild after a stub change.** Removing stubs changes the generated headers, which are in the runner's
  precompiled header, so all 425 unity batches were rebuilt (about 20 minutes). Patch changes alone rebuild only what
  they touch.
- **Result: text on screen (2026-11-05, 00:44).** Build and tests passed (461 tests, IOP tests included). A
  100-second run shows the autosave notice in the panel, the same screen the old port reached with these fixes:
  "MKSM uses an Autosave feature. Insertion of a memory card (PS2) beyond this point may cause data to be
  overwritten. Press ✕ to continue", at a steady 12 frames per second (`frame 1050 | 12.0 fps` at 90 s). The faint
  emblem the panel showed before is gone. The error log only has two failed searches for `BLITZ.SFD` and
  `GAUNTLET.SFD`, which the PAL disc does not have.
- **Review pending.** An adversarial review of patches 0013-0015 and the configuration change (one reviewer per
  patch, then a verifier) was stopped before it reported, at the end of the session; it is to be run again.
- **Next.** The notice waits for ✕. The old port's next fix was the pad's DMA buffer: the IOP's pad driver keeps
  the buffer given to `scePadPortOpen` up to date, and the game read its buttons from it. In PAL the pad layer
  (`0x273008`) opens the port with its own 64-byte-aligned object as that buffer (`0x273170`) and reads
  `scePadRead`'s copy (`0x273684`), which upstream fills; whether another reader takes the buttons from the object
  itself (in PAL `0x212808` does) is the first thing to check. Then the libvu0 handlers flagged by the audit
  (`sceVu0RotTransPers`, `sceVu0InnerProduct`, `sceVu0Normalize`, `sceVu0NormalLightMatrix`), the IOP heap library,
  movies and sound.

## 2026-11-05 (evening) — the pad, the front end, the picture, sound findings

- **Two reviews.** Read-only workflows (one reviewer per area, then a skeptical verifier) checked patches 0013-0015 and
  today's work. Confirmed and fixed below: `sceGsExecLoadImage` and other runtime handlers allocated in the heap the
  game's own malloc manages (patch 0020); `sendGifNormal` and `toDmaPhys` mapped kseg0 and uncached addresses wrongly,
  and four libgraph handlers read arguments from the stack in edge cases (patch 0021); a buffer held on screen stayed
  blended with the one before (patch 0019); the pad buffer read as every button held until the first `scePadRead`
  (patch 0016); the pad script lost overlapping presses; Esc, upstream's key for circle, also closes the window; and
  the pointer scan missed function starts after padding or tail calls. Refuted: a minimized window does not change the
  aspect (raylib pauses the loop). The claim of patch 0014 that the game sends its `sceGsLoadImage` packets itself was
  wrong: its texture loader builds them but uploads with its own libpkt chain (`0x268900`).
- **The pad's DMA buffer** (`patches/0016-pad-dma-buffer.patch`, test in the pad tests). `0x212808` reads the buttons
  and sticks straight from the pad object at `0x652200 + port * 0x700` (bytes 2-7), the buffer the game's pad layer
  (`0x273008`) gives `scePadPortOpen` (`0x273170`); its own read path takes `scePadRead`'s copy (`0x273684`). Upstream
  zeroed the buffer at open and never wrote it again, so every button looked held and no press registered.
  `scePadPortOpen` now writes a pad at rest (buttons released, sticks centred) into both 128-byte halves, and
  `scePadRead` copies its 32 bytes there, as the IOP's pad driver keeps them on the console.
- **Scripted pad** (`MKSM_PAD_SCRIPT`, override module): `<seconds>:<buttons>[:<hold>],...`, buttons joined with `+`
  (cross, circle, square, triangle, start, select, up, down, left, right, l1, l2, r1, r2, l3, r3), held 0.25 s by
  default, tagged `[mksm-pad]`. A host thread sets the runtime's pad override for each press, which every port reads;
  a press waits until 0.1 s after the one before is released (logged as late), and the keyboard and gamepad work in
  between. Keyboard (upstream's mapping): X or Space cross, C circle, Z square, V triangle, Enter start, Tab select,
  the arrows or WASD, Q and E L1 and R1, the Shift keys L2 and R2; Esc closes the window.
- **The front end.** With `MKSM_PAD_SCRIPT=45:cross,60:cross,75:start,90:cross,105:cross,120:cross,135:cross` the game
  goes from the autosave notice to "No MKSM save data present on memory card (PS2) in MEMORY CARD slot 1. Do you wish
  to create save data and save this file now? No / Yes" (No), the title screen ("Press START button"), the brightness
  screen, the main menu (Single Player, Ko-op, Versus Mode, Load Game, Options, Kontent), the character select (Liu
  Kang and Kung Lao drawn in 3D) and "Select difficulty" (Normal, Hard).
- **Function pointers to merged code** (`tools/missing_targets.py --data-pointers`). The old port froze after the
  character select on a vtable call to `0x38C730`, a function the analyzer had merged into the one before it; PAL
  has it at the same address (pointed to from `0x567034`). The tool scans `.data` and `.rodata` (the ELF has one
  segment for code and data; a first version took instruction words for pointers) for words pointing into `.text` at
  no registered address that look like a function start: a stack frame, or just after a return or another
  unconditional jump (padding allowed); jump tables (a neighbour in the same function at no such start) are left out.
  1185 such targets are entry points in `config/recomp.toml`: most are `.eh_frame` pc_begin values, the rest vtable
  and callback entries, landing pads and a few switch cases (inside a decoded function an entry point only adds a
  resume label). The difficulty select now follows the character select.
- **The picture.** The game renders 640x256 fields (SMODE2 INT and FFMD) and puts every other field half a line lower
  in its own vertices, which a TV interleaves; shown one after the other, the picture moved a line every frame
  (`patches/0017-gs-field-blend.patch`, then `patches/0019-gs-field-weave-bilinear.patch`). The presented frame is now
  the TV picture: the displayed field and the one before it woven at twice the height, each on its own rows in the
  order that combs least, every row blended with its neighbours (PCSX2's "blend"); a buffer held on screen longer than
  a quarter of a second is shown on its own. The window scales it with bilinear filtering (nearest sampling stretched
  640x256 unevenly). `MKSM_DEINTERLACE=0` shows the fields one by one, `MKSM_FILTER=nearest` scales with nearest
  sampling.
- **Aspect** (`patches/0018-host-display-aspect.patch`). Upstream scaled the frame with square pixels (640x448 came out
  1.43:1 instead of a TV's 4:3) in a 640x448 window. The picture is now shown at a display aspect, as large as the
  resizable window allows and centred; the window opens at 960x720; 4:3 by default. `MKSM_ASPECT=<w>:<h>` (16:9...) or
  `MKSM_ASPECT=window` (follow the window) sets another, and the override keeps the game's 3D view in step: `0x4F4E08`
  holds the horizontal scale of its projection, `(4/3) / aspect`, read where the projection is set up (`0x236D70`,
  `0x24A178`; 1.0 in the ELF, the game's own setter `0x2268B0` has no caller). The game's test for its 16:9 camera
  tuning (`0x36FB20`, scale 0.75 within 0.0005; it switches a camera constant from 2.6 to 4.5) is replaced to accept
  any picture wider than about 3:2. The 2D screens are stretched.
- **No more magenta** (`patches/0022-host-no-magenta.patch`). Upstream filled the window with magenta whenever the GS
  had no frame to present (no read circuit, an empty display, a display being set up between two screens); the
  window now keeps the last picture for up to a second, then shows black.
- **Private guest heap** (`patches/0020-private-guest-heap.patch`). The runtime's own guest allocations (SIF command
  packets, GS upload packets, MPEG callback data) came from the heap `SetupHeap` names, which this game's own malloc
  manages, so they overwrote its data. `PS2Runtime::setPrivateGuestHeap` moves them for good (the override puts them at
  `0xA0000`-`0x100000`, below the ELF); `SetupHeap` then gives the game its own base and `EndOfHeap` its own end.
- **Quick profile by target** (`scripts/quick-profile.cmake`). The generated code stays at `/O1 /Ob1` (build time),
  everything else is `/O2 /Ob2`. Frame rates (window title): 20 fps on the 2D screens (12 before), 7 to 11 in the
  main menu and the character select (4 to 6 before). The host profile is still 80 to 90 per cent software GS
  (triangles, sprites, pixel writes, texture and CLUT lookups), run synchronously from the game's DMA writes, plus up
  to a third in the VU1 interpreter in 3D scenes.
- **Sound: no SPU2 at all.** An investigation workflow found that upstream PS2Recomp has no SPU2: the IOP emulator keeps
  the SPU2 registers as a plain store, fakes SPU2 DMA completion without moving data, and has no sound RAM, voices,
  ADPCM decoding, mixing or output. The only host audio path plays VAG files read through fio. The game's libsdr calls
  (`0x382C70`, `0x382D20`, `0x382DB0`) are stubbed to bookkeeping handlers (`sceSdTransToIOP` would end the game
  thread), so the game's sample uploads never reach SDRDRV.IRX; CRI ADX music streams through LIBSD's block transfers
  on the IOP. Sound needs an SPU2 in ps2xIOP (sound RAM, DMA, 48 ADPCM voices with envelopes and pitch, mixing,
  48 kHz host stream), like the archived port's native sound engine, plus un-stubbing libsdr: left for later.
- **Window title** (`patches/0023-window-title-game-name.patch`). The override names the window "Mortal Kombat: Shaolin
  Monks | frame <n> | <fps> fps" (displayed frames and their rate, `patches/0003`).
- **BC2F/BC2T** (`patches/0024-bc2-tests-vu1-busy.patch`, test in the code generator tests; needs `-Regenerate`). The
  recompiler tested the Z flag of the VU0 STATUS register for the COP2 branch condition; on the console (and in PCSX2)
  it is VU1 busy (VPU-STAT bit 8). The game's wait-for-VU1 loop at `0x24EA10` (`sub_0024E910`) spun forever once a
  VU0 macro result had been zero, right after the movie before a stage.
- **After the difficulty select.** The game sets up a 640x512 display (PSMCT16, then PSMCT32), plays a movie (skipped
  by the override) and re-initialises its double buffer, then presents nothing more (black since patch 0022). With
  0024 the VU1 loop is gone, but every EE thread is then idle and the IOP runs about 0.1 M instructions a second.
  `MKSM_TRACE_CALLS=0x26F2A0,0x483980,0x463B30` shows `sceSifCallRpc` (`0x483980`) on the client at `0x53E0B0`
  (functions 0x2, 0x403, 0x40C, 0x422, 0x400) and the SFSV end function `0x463B30` called over and over with
  `a0=0x771740 a1=0x8000 a2=1 a3=0x770D00`: the game most likely waits for a sound request (SNDF/SFSV) to complete.
  Not solved yet.
- **Frame rate: where the time goes and what others did.** Read-only analyses (logs/profile_menu.txt): one textured,
  bilinear, palette pixel costs 31-35 out-of-line calls (about 1,500 instructions) in the CPU GS backend; the GS is
  79-82 per cent of the game thread in the menus. Exact fixes to that path (rounding, palette cache, span loops,
  sprite fast paths, a GS thread) are estimated at 13-32 fps, 22-50 with a GS thread, still bounded by the EE side
  (16-31 ms a frame). Every PS2 port found that reaches full speed replaced the software GS with a GPU path (PCSX2's
  GS, paraLLEl-GS or its own renderer) and compiled VU1 microprograms or threaded VIF1/VU1. Upstream's own draft
  (PR #285, branch `feature/performance-patch-1`) adds a paraLLEl-GS (Vulkan compute) backend behind
  `GSRasterBackend`. PAL SLES-53524 is 50 Hz only (no 60 Hz mode, no progressive scan): full speed is 50 pictures a
  second, one per field.
- **Sound** (same investigation, more detail): besides the missing SPU2, the IOP emulator starts modules with (byte
  count, buffer) instead of (argc, argv), so CRI_ADXI ignores its `sdinit=0 spucore=2` options; and the libsdr
  handlers fake their results without contacting SDRDRV.IRX, which is loaded and serves `0x80000701`.

## 2026-11-06 — the GS on the GPU (upstream's paraLLEl-GS), libgraph's display modes, the game's NTSC mode

- **Why.** The frame rate analyses of 2026-11-05 put 79-82 per cent of the game thread in the CPU GS backend in the
  menus and estimated exact CPU fixes at 13-32 fps (22-50 with a GS thread). Every PS2 port found at full speed draws
  the GS on the GPU. Upstream's own draft (PR #285, branch `feature/performance-patch-1`, commits e27a658 and 06ce0c8)
  adds a paraLLEl-GS backend (Arntzen Software's Vulkan compute GS, LGPLv3) behind `GSRasterBackend`. A design workflow
  (the integration with patches 0001-0024, the library and its threading, the build and the presentation, then a
  planner) checked the work while it was done; its corrections are folded in below.
- **libgraph's display modes** (`patches/0025-gs-libgraph-display-modes.patch`, test "libgraph's PAL display modes:
  SMODE1, DISPLAY in clocks and lines, a field's height"). paraLLEl-GS models the CRTC: it takes the video mode from
  SMODE1 (CMOD PAL 3 or NTSC 2, LC 32), which nothing set ("Unknown video format", no picture), and reads DISPLAY in
  output clocks and raster lines, where the stubs wrote framebuffer pixels (a 640x256 field would have come out
  160x128, off screen). From upstream's branch: `configureGsCrt` (SMODE2 and SMODE1 for a video mode) in `GsSetCrt` and
  in the `sceGsResetGraph` handler, which now sets the display registers directly (its GIF packet used
  privileged-register numbers only the CPU frontend understood; to a raw backend 0x41 and 0x42 are SCISSOR_2 and
  ALPHA_1); and libgraph's DISPLAY (`makeLibgraphDisplay`, upstream's `makeDefaultGsDispEnv`): for this game in PAL
  DX 656, DY 72, MAGH 3, DW 2559, DH 511. The CPU backend now shows half of DH's lines per field in interlaced frame
  mode, so it still presents 640x256.
- **The GPU backend** (`patches/0026-parallel-gs-backend.patch`, test "a raw-command GS backend gets the GIF packets
  with their PATH and its pictures are not woven again"). Upstream's `gs_parallel_backend.cpp` and its frontend hooks: a
  backend that `UsesRawCommands()` gets every GIF packet with its PATH (the GIF arbiter passes it along) and every
  register write, and the frontend decodes nothing itself. Changes from upstream, marked `// MKSM:`:
  - no OpenGL interop: upstream shares the frames with the window's OpenGL context (`gs_gl_interop.cpp`); the first
    run here crashed in AMD's OpenGL driver (Application event 1000, `atio6axx.dll`, 0xc0000005, 17:36:07, about 12 s
    in). The backend owns its Vulkan device and copies each picture into host memory, and the host shows it as it
    shows the CPU backend's (aspect, filter, hold-then-black unchanged; the window's texture grows to the picture);
  - one scanout per field, on the EE thread, at the start of the vertical blank before CSR.FIELD and the game's
    display buffer change (`GS::vblank`), into a ring of three host-visible buffers: upstream scanned out from the
    host loop, which skips and repeats fields, and the first port of it scanned out after the field changed, so each
    picture had the other field's parity;
  - the picture: the fields come raw and the frontend weaves them as on the CPU backend (patch 0019), each scanned
    field with the one just before it (the backend hands both over with their numbers, parities and display buffers,
    so a field the host's refresh skipped is still woven in, and the weave knows each field by the buffer it showed,
    not by the buffer on screen when the host looks), in the order of the field's parity, learned from the combing
    and swapped only after 8 decisive contrary pairs in a row (moving pictures swapped it now and then: the picture
    shook from time to time). paraLLEl-GS's own options stay available: `PS2X_GS_SSAA` super-samples, and with 4
    samples or more `PS2X_GS_PROGRESSIVE=1` gives its field-aware progressive scanout at twice the resolution
    (`PS2X_GS_FIELD_PHASE=flip` for the other field order); a first try of it as the default shook (the field
    parity of this game is not the scheduler's) and, read with the CPU backend's row stride, came out doubled side by
    side (the frontend now reads a packed picture with its own stride);
  - the scanout ends in paraLLEl-GS's own READ_ONLY layout, moved to TRANSFER_SRC for the copy (the interlaced and
    high-resolution paths never reached the TRANSFER_SRC upstream asked for); CRTC offsets off (libgraph's PAL origin
    against paraLLEl-GS's moved the picture 5 pixels left);
  - FINISH no longer drains the GPU on the game thread; 8 Granite frame contexts (each flush waited for the GPU with
    2); a VIF1 DIRECT image continued in the next VIF packet goes on without the IMAGE tag the CPU frontend needs
    (`PS2Memory::setGifPathsStateful`); VU1's XGKICK fallback passes PATH1; `PS2X_GS_PARALLEL` is a private definition
    (switching backends does not rebuild the generated runner). `PS2X_GS_BACKEND=cpu` keeps the CPU backend, as does a
    failure to create the GPU one.
- **AMD's Vulkan driver crash.** Every GPU run crashed after one to two minutes in `amdvlk64.dll` (0xC0000005, at the
  same offset 0x2323076 at 18:06:55 and 18:36:30). `MKSM_CRASH_TRACE=1` (diagnostic of patch 0026, `[gs-crash]` lines:
  the faulting thread's stack, symbolized from the runner's PDB) showed a null read in `vkCreateComputePipelines`,
  compiling paraLLEl-GS's sampler-feedback compute pipeline (`GSRenderer::dispatch_texture_analysis`, from a draw of
  `sceGsPutDrawEnv`'s packet). Sampler feedback (texture analysis) is new in the paraLLEl-GS commit upstream pins and
  optional; the backend turns it off (`DebugMode::disable_sampler_feedback`).
- **Measurements** (quick build, NTSC mode, GPU backend, `MKSM_PAD_SCRIPT=45:cross,60:cross,75:start,90:cross,
  105:cross,120:cross,135:cross`, 240 s): 60 fps on every screen up to the difficulty select, the 3D character select
  included (20 fps on the 2D screens and 7-11 in the 3D menus with the CPU backend); dips at screen changes while
  paraLLEl-GS compiles its pipelines on first use ("Stalled compile", 30-230 ms each). Bursts of twelve window captures
  30-70 ms apart (scratchpad `burst.ps1`, `shifts.py`): no vertical jump between any two captures on the autosave
  notice, the memory card prompt, the brightness screen, the character select and the main menu; the static screens
  identical capture to capture. The user confirms: no shaking, good frame rate. No crash in 240 s with sampler
  feedback off (the two runs before crashed at 66 s and 135 s).
- **Tall frames** (patch 0026). The splash at boot (512x448 PSMCT32, PMODE 0x8007: both read circuits blended) and the
  loading screens (512x448 PSMCT16) are whole frames shown in interlaced frame mode with DISPLAY DH 895: the game's
  own libgraph (`sceGsSetDefDispEnv`, `0x3831F0`, still in the ELF) computes DH as `2h - 1` in interlaced frame mode
  (the stub matches it), so a 448-line frame asks for a 448-line field, which no NTSC field holds; paraLLEl-GS, a CRTC
  model, showed its top half stretched and cut at the bottom (the CPU backend shows the whole frame). The backend scans
  such a frame (field taller than 240 lines in NTSC, 288 in PAL) out as the progressive frame it is: field mode, half
  of DH. Verified with captures: the memory card prompt kept on screen while the title loads and the stage loading
  screen ("LOADING" and its bar) fill the 4:3 picture; no vertical jump in the bursts. `MKSM_TRACE_DISPLAY=1`
  (override module, diagnostic) prints the display registers whenever they change (`[mksm-display]`, decoded).
- **Still wrong.** After the difficulty select the stage loads (the loading screen and its bar now show right) and
  the game then stops presenting (the stall of 2026-11-05): the display stays on the 16-bit loading frame at FBP 0,
  which the stage's textures overwrite, so the GPU backend shows VRAM garbage where the CPU backend showed black.
  `MKSM_TRACE_THREADS_EVERY=5` (override module, diagnostic: from a host thread, the EE threads, the IOP's progress and
  diagnostics, the display registers): from about 15 s after the difficulty select the IOP executes nothing at all
  (`[mksm-iop] instructions +0`, the cycle count frozen; 44-45 M instructions per 5 s before), while the EE main thread
  stays Running at `0x47FFB8`, the return of the `ChangeThreadPriority` syscall wrapper (`0x47FFB0`, syscall 0x29): a
  yield loop, while every IOP thread waits. A sound wait was the first suspect (the IOP has no SPU2) and it was
  wrong: the CPU backend gets past this point (0.6 fps, the stage's letterboxed opening shot at 330 s), so the stall
  belongs to the GPU path. It was the VIF1 stream going out of step, see 2026-11-11.
- **PAL's 50 Hz** (`patches/0027-pal-vblank-rate.patch`, test "the vertical blank comes at the video mode's rate"). The
  EE scheduler gave every game NTSC's 60 Hz vertical blank; this PAL game's vblank-paced logic would have run a fifth
  too fast at full speed. The period now follows SMODE1: 20 ms for PAL.
- **The game's NTSC mode, 60 Hz** (override module, default; `MKSM_VIDEO=pal` keeps the disc's PAL mode). The PAL
  disc keeps the NTSC build's code: the display set-up (`sub_00274030`) picks video mode 3 (PAL) or 2 (NTSC) from a PAL
  flag at `0x4E5CAC`, which some 180 places read and the boot function sets to 1 (`addiu $v0, $zero, 1; sw $v0,
  0x5CAC($fp)` at `0x2132D8`). The override clears the flag right after that store, in the call that follows it
  (`sub_00476C38` from `0x2132E4`), so the game runs in its NTSC mode: sceGsResetGraph in mode 2, 640x224 fields,
  60 Hz vertical blank (patch 0027), the game's own NTSC timing.
- **Build.** `scripts\build.ps1` keeps one paraLLEl-GS checkout in `PS2Recomp\out\parallel-gs` at the commit
  `cmake\ParallelGS.cmake` pins, with only Granite, volk and the Vulkan headers (216 MB; FetchContent would clone every
  nested submodule, 1.4 GB per build tree, some paths beyond MAX_PATH), and hands it to FetchContent. The library builds
  in about 12 s.

## 2026-11-11 — the 3D stage: VIF1 DIRECT data, VU0 macro flags, SQRT.S, and where the time goes

- **The stage froze on the GPU backend: PATH2 out of step** (`patches/0028-vif1-direct-data.patch`). A temporary trace
  in the GPU backend (the register writes no game sends, with the GIF path and tag they came in) showed PATH2 packets
  of 189,104 and 276,976 bytes whose "GIF tags" were pixel data (`B1E2FB7F271D41BD B7D2DDE212D2DB34`), then
  paraLLEl-GS executing them (`Unexpected Z format: 15`, `Unrecognized FIFO readback PSM: 21`) until the game stopped
  presenting. A second trace in the VIF1 interpreter (commands cut by the end of a transfer, unknown VIF codes) found
  the cause in upstream's image continuation: after a `DIRECT` whose GIF IMAGE packet is not finished, the interpreter
  sent the next qwords of the VIF stream to PATH2 as that image's data, without looking for a VIF code. That dates
  from before the DMA tag's upper half was transferred (TTE); now that it is, the game's texture upload (a `CNT` tag
  with `DIRECT`, the GS set-up and the IMAGE tag, then a `REF` tag to the pixels with `NOP` and `DIRECT n` in its upper
  half) had those two codes sent as the image's first 8 bytes and the image's last 8 bytes read as VIF codes: random
  commands, among them `DIRECT`s of thousands of qwords. With the patch the qwords after a complete `DIRECT` are VIF
  codes again; only a `DIRECT` cut by the end of a transfer continues in the next one (`m_vif1PendingDirectQwc`), sent
  as it is to a GS that keeps each path's state and with an IMAGE tag of its own to the CPU frontend. Tests: "VIF1
  DIRECT cut by the end of a transfer continues in the next one", "VIF1 DMA texture upload with the IMAGE tag in one
  DIRECT and the pixels in the next", "stateful GIF paths get VIF1 DIRECT data as it is"; upstream's two tests that
  fed raw image qwords after a complete `DIRECT` now put the `DIRECT` code the hardware needs before them. Four of
  these five tests fail without the patch. **Verified in the game:** the scripted run reaches the first level's opening cutscene on
  the GPU backend, with none of the errors above.
- **Then a call through a null pointer, 280 frames into the cutscene: VF0 and the MAC flags**
  (`patches/0029-vu0-macro-vf0-flags.patch`; a recompiler patch, the build regenerates the game's code). `[guest-branch:missing-target]
  kind=IndirectCall op=JALR source=0x20a5dc target=0x0`: `sub_0020A590` grows a Havok `hkArray` through the memory
  manager at `*0x5B2310` (`0xB0A880`), whose vtable pointer (+0x20) read `0xFFFFFFFF`. `MKSM_TRACE_CALLS=0x3A2E08,
  0x20A590` with `MKSM_TRACE_WATCH=*0x5B2310:16` showed that object moved up by one word between two summaries, and 2,000
  pair insertions in a row from one caller (`0x3A06A8`, in `sub_0039FC90`, the broad phase's AABB update). That routine
  quantizes each AABB with `vmax.xyz vf, vf, vf0`, `vmini`, `vftoi0` and then moves the endpoint down its sorted axis
  while `value < previous`; the array starts with a sentinel of value 0, so a negative value never stops and every
  4-byte endpoint (and everything below the array) moves up one slot. The value was negative because VF0 was not
  (0, 0, 0, 1): Havok compares vectors with `vsub.xyzw vf0, a, b`, four `vnop`s and `cfc2 $t, $vi17` (the MAC flags;
  26 functions in `0x38D000-0x3D6000`), and the recompiler (a) wrote the difference into VF0, hardwired on the
  machine, and (b) never set the MAC or status flags in macro mode, so every such comparison read a constant. The
  patch restores VF0 after any macro instruction that names it as its destination (`LQC2` into it is dropped) and sets
  the flags of the arithmetic instructions (ADD, SUB, MUL, MADD, MSUB with their broadcast, Q, I and ACC forms,
  OPMULA, OPMSUB) from the whole result: zero and sign per lane in the hardware's layout, the lanes outside the
  destination mask clear, status zero/sign and their sticky bits; overflow and underflow stay clear because the
  results are already saturated. The update is left out where the next arithmetic instruction overwrites the flags
  before anything can read them (a straight line with no `cfc2` from `$vi16`/`$vi17`, no micro program, no branch):
  1,195 of the game's 6,318 macro arithmetic instructions keep it. Tests in the code generator and float semantics
  suites (VF0, both function tables instruction by instruction, the liveness rule, the flag layout). **Verified in
  the game:** no null call; the cutscene ends and the level's HUD ("EXP: 0", "LIU KANG", the bars) appears.
- **The camera was wrong: SQRT.S took the wrong register** (`patches/0030-fpu-sqrt-operand.patch`). With the two
  fixes above the level drew, but from inside the scenery and without the characters.
  On the EE `SQRT.S fd, ft` has its operand in the ft field (`COP1 | S | ft | 00000 | fd | 000100`); the translator read
  fs, as on other MIPS processors. All 90 `SQRT.S` of the game have fs = 0 (46 are `sqrt.s $fN, $f12`), so 75 of
  them returned the root of `$f0`: every length and distance outside the 15 lucky ones. Test "SQRT.S takes its
  operand from ft". **Verified in the game:** the opening cutscene shows the "Goro's Lair" title over the courtyard
  and then Liu Kang, Kung Lao and Raiden framed as the scene intends.
- **Where the time goes in 3D.** The cutscene runs at 2-8 fps. `MKSM_PROFILE=host` there: 81 per cent of the game
  thread inside `VU1Interpreter::run` (`commitReadyPipelines` alone 27-29 per cent, then the pair scheduling, the
  operand normalisation and the exact FMAC results), about 7.5 per cent in the IOP, the rest in the game's code, the
  GS submission and the scheduler. A temporary counter around the VU1 launches (not in a patch) measured 2,200-2,800
  launches and 3.3-4.2 million VU1 cycles a second, 180-200 ns of host time a cycle: upstream's VU1 is a cycle-exact
  model of the pipelines (register writes queued with their latencies, per-lane scalar arithmetic with operand and
  result normalisation), far from the 20 million cycles a second 60 fps would need here.
- **The game's VU1 code.** Three programs, each a whole 16 KB image in the ELF uploaded to VU1 address 0: `0x49BB70`
  (front end, 1,392 pairs before the next one starts), `0x49E6F0` (gameplay, 1,936), `0x4A2370` (the cutscenes', 1,937);
  the 330-pair VU0 program is the VIF packet at `0x49A9D0`. They are transforms (`MULA`/`MADDA`/`MADD` by broadcast,
  `DIV` and `MULq`, `FTOI4`, `MAX`/`MINI` clamps), loads and stores, integer loops and `XGKICK`; the gameplay program
  reads no flags at all, the cutscene one uses `CLIP` with `FCAND` (29), `FMAND` (3), the EFU (`ESADD`, `ERLENG`, `MFP`)
  and `JALR`/`JR`.
- **Audit of the VU0 macro and FPU translation.** A read-only workflow (one auditor for COP2 macro mode, one for
  COP1, each followed by a verifier told to refute every finding against PCSX2's interpreter, a second source and the
  hardware rows of ps2autotests) confirmed 22 differences from the hardware, `SQRT.S` among them. Fixed the same day:
  - `patches/0031-vu0-macro-divider-ftoi-clip-max.patch`. `VDIV`, `VSQRT` and `VRSQRT` computed `fs / ft` with 0 for
    a zero divisor, `sqrt(max(0, ft))` and `1 / sqrt(ft)` (the numerator ignored, 0 for a zero or negative operand).
    They now follow the VU divider as the runtime's own VU interpreter does in micro mode (`ps2_vu0_div`,
    `ps2_vu0_sqrt`, `ps2_vu0_rsqrt`): the largest number for a zero divisor, the root of the magnitude, the invalid
    and division-by-zero status flags. `VFTOI` saturates positive values to `0x7FFFFFFF` (`cvttps2dq` gives
    `0x80000000` for both signs). `VCLIPw` compares the bit patterns with |w| (a negative w set every flag). `VMAX` and
    `VMINI` compare the bit patterns as sign and magnitude (`maxps`/`minps` return their second operand for any
    pattern the host reads as a NaN, which is what a saturated result is). A new context (a started thread, an
    interrupt handler's or a callback's) has VF0 = (0, 0, 0, 1) like the main one; it was all zeros, harmless until
    `VRSQRT` and `VDIV` read their `vf0w` numerator. In this ELF the two `VRSQRT` with another numerator (`0x2B5CC0`,
    `0x2B5FD0`, sphere push-out loops) and eight of the fourteen `VCLIPw` sit in functions nothing references, so the
    visible effect is on degenerate inputs (w = 0, a slightly negative `1 - sin^2`, saturated colours).
  - `patches/0032-fpu-compare-abs.patch`. `C.EQ`, `C.LT` and `C.LE` (4,305 sites) were host compares, false for every
    pattern the host reads as a NaN. Since upstream clamps results, `x / 0` and an overflow give `0x7FFFFFFF`, such a
    pattern: `div.s` followed by a range check (about 60 places, `0x151F3C` after `tanf` for one) kept the huge value
    where the console clamps it, and the 52 `c.eq.s fN, fN` self-tests were false for such a value. The compares now order sign and magnitude
    with exponent 255 as an ordinary exponent and exponent 0 as zero (`ps2_fcmp_key`; the verifier's model matches
    all 160 compare rows of ps2autotests). `ABS.S` clears the sign bit only (`fabsf` went through a double in the
    quick profile and set a mantissa bit of such patterns).
  - Tests for both in the float semantics and code generator suites; 488 of 489 pass (the one failure, "VU0 macro
    mappings cover all S1/S2 enums", reads a header by a relative path and fails outside its own folder).
    **Verified in the game:** the menus and the cutscene run as before (same picture, no new message in the log).
- **Confirmed by the audit and left as they are** (none is reached by this game in a way that shows, or the fix costs
  speed for one unit in the last place): the EE and the VU truncate where the host rounds to nearest (ADD, SUB, MUL
  and int-to-float; the real adder also drops the low bits of the smaller operand, PCSX2's `FPU_ADD_SUB`), so about
  20 per cent of the sums and half of the products differ by one unit in the last place; a `MADD.S` whose product
  overflows; an operand with exponent 0 and mantissa bits in ADD/SUB/MUL; `MAX.S`/`MIN.S` (unused); FCR31's fixed
  bits and flags (`CFC1` unused); `CTC2` to FBRST with a reset bit; the VU0 memory instructions, the R register
  instructions, `VCALLMSR` and VI0 as a destination (all unused).
- **Next.** (1) The frame rate in 3D: translate the three VU1 programs ahead of time, from the user's own ELF at build
  time, with upstream's exact VU1 as the fallback and as the reference the translation is checked against (30,000
  launches of the cutscene were captured for that); then profile again (the VIF unpack, the GS submission and the
  game's own code are next in line). (2) Gameplay after the opening cutscene has not been seen since the camera fix:
  the cutscene takes minutes at this speed and Start does not skip it. (3) Sound: the archived SPU2 and libsd engine
  is ported to upstream's IOP in a worktree (SPU2 core, libsd by ordinal; unit tests and a harness with the game's
  own IRX modules pass); the host audio output and the IRX argument fix remain. The user put it after the 3D work.

## 2026-11-11 (later) — VU1 ahead-of-time translation: the foundation (branch `vu1-native`)

First step of item (1) of "Next" above. Nothing is translated yet and nothing changes in the game: this is the ground
the translation will stand on, all of it outside `scripts\build.cmd` for now. `tools/vu1gen/PROGRESS.md` has the
commands, the full tables and the notes for the steps that follow.

- **Four images, not three** (`config/vu1_images.txt`). The uploader always sends 16 KB from the address it is given,
  and a fourth address is uploaded by EE `0x22B2D8`: `0x49BAE0`, a 17-pair routine followed by the front-end program
  shifted by 0x90 bytes (never launched in the captures). The cutscene program's own code is 1,932 pairs; the 1,937
  above counted the five pairs of a VIF packet that follows it. The list holds 36 MSCAL entry points and the one
  target of the cutscene program's indirect calls (`0x30E0`). The hash the captures are named by is 64-bit FNV-1a
  with the offset basis `1469598103934665603` (the standard basis without its last digit).
- **The generator's decoder is the interpreter's** (`tools/vu1gen/src/vu_decode.*`). Its listing of all 8,192 pairs
  of the four images, usage tables included, equals the Python reference of the study byte for byte; and against
  `VU1Interpreter::decodeInstructionPair` itself (reached without a patch) it agrees on 97.5 million pairs: every
  upper word, every word of the lower-1 group, every register field of the other lower opcodes, 20 million random
  pairs.
- **A build tree of its own** (`tools/vu1gen/devbuild.cmd`): configured from a patched PS2Recomp with the quick
  profile's arguments plus `-DCMAKE_PROJECT_PS2Runtime_INCLUDE=tools/vu1gen/cmake/ps2recomp.cmake`, which adds the
  targets without a patch. The interpreter's compile commands there equal those of `out\build-quick` token for token.
- **The interpreter as the oracle** (`vu1difftest`). It replays the 30,000 captured launches on a `PS2Memory` and `GS`
  fixture, with the PATH1 packets taken by the GIF callback. **Verified:** the state after a launch equals the input
  of the next record in 11,964 of 11,997 consecutive pairs (front end 3,999 of 3,999, gameplay 3,994, cutscenes
  3,971), every field and the cycle count included; in the 33 others the captured clock shows that another program's
  launch ran in between. No captured launch starts with `ebit`, `haltAfterDelaySlot` or `branchPending` set. Entering
  the interpreter with `execute(startPC = state.pc)` for an MSCNT gives what `resume()` gives on all 17,942 captured
  MSCNT launches (state, data memory, packets, `mac`, `status`, cycles), 5,830 of them against a `resume()` on the
  very interpreter that ran the launch before: a translated launch can therefore be followed by an interpreted
  MSCNT entered that way.
- **The runtime interface of the translated programs** (`tools/vu1gen/runtime/`): the packets of a launch are copied
  at each XGKICK into a list and submitted after it; the packet walk equals the interpreter's transfer on 20,000
  random packets (tag chains of every format, 4,448 that wrap at 16 KB, 455 of exactly 64 KB, 2,324 that both
  refuse). `vu1nRun` sets MXCSR for the launch and puts the caller's back.
- **What the interpreter costs** (host time of `execute()` alone, quick-profile build, idle machine, best of three
  runs of each captured launch, no GS work): 255 us a launch and 207 ns a VU cycle on average. Front end `MSCAL 0`:
  489 us (1,613 cycles, 131 packets a launch). Gameplay MSCNT: 240-353 us (192-206 ns a cycle, three packets a
  launch). Cutscene MSCNT: 152-426 us (135-181 ns a cycle, one packet). The init launches (MSCAL, 10-25 cycles):
  3-5 us. The same measurement during another build was up to four times slower on the short launches.
- **What a launch executes.** Stepped one pair a call (which gives the same result as the launch run at once, in all
  30,000 launches), the interpreter executes 1,036 pairs a launch on average (gameplay MSCNT 1,040-1,500, cutscene
  MSCNT 650-1,920, front end 1,612): the rest of the 1,232 cycles are stalls and the flush of the XGKICK transfer.
  That is 246 ns of host time for each executed pair. The captured launches execute 50 distinct pairs of the
  front-end program, 664 of the gameplay program and 795 of the cutscene program (of 1,388, 1,930 and 1,924
  reachable ones): the rest has to be reached by constructed launches before the translation can be trusted there.
- **A packet can be copied at its XGKICK.** The interpreter transfers a packet two cycles a qword while the program
  goes on, so a later store could still change it. In the same stepped run the packet was also read whole from data
  memory at each XGKICK, as translated code will do: for all 731,100 XGKICKs of the captured launches (696,996 in
  the front end's sprite routine, 24,243 gameplay, 9,861 cutscene) it is byte for byte the packet the interpreter
  sent.
- **Next.** Control flow from the entry points, the timing exploration that resolves each Q, P, MAC and clip read,
  emission, the arithmetic helpers, then the comparison of every captured launch with the interpreter.

## 2026-11-11 (night) — the VU1 programs run translated; where the frame time goes now; the 3D picture

Item (1) of "Next" above is done: the game's VU1 microcode runs as C++ generated at build time, checked against
upstream's interpreter launch by launch, in the tests and in the game. `tools/vu1gen/PROGRESS.md` has the design, the
commands and the full tables; this section records what was verified and what the game shows now.

- **The translator** (`tools/vu1gen`). A port of the study's timing exploration (`src/vu_flow.*`) explores every
  launch start of the four images and resolves each Q, P, MAC and clip read to a count of producers in flight (its
  counts equal the study's table: Q reads 51/55, 46/15, 72/30; MFP 9/5; FMAND 1/2; FCAND 29; fixed stalls 37/89/105);
  the emitter (`src/vu_emit.*`) writes one C++ function per entry point (one function per image took MSVC up to 583 s a
  unit and left the gameplay program slow); `runtime/vu1n.h` has the arithmetic twice: the interpreter's code lane by
  lane (the reference), and SSE4.1 under the interpreter's own MXCSR (round toward zero, no flush to zero) that is
  exact by construction, with the reference taking the product-sum lanes it must (a nonzero product below 2^-79, a
  product or result at or above 2^127). Every entry of the front-end, gameplay and cutscene programs is translated
  (26, 36, 42).
- **Verified against the interpreter outside the game** (`vu1difftest`): the 30,000 captured launches, 0 mismatches,
  with the fast and with the reference arithmetic (state, the 16 KB of data memory, the PATH1 packets); 60,000
  synthetic launches (captured states with their float words changed, every handled entry), 0 mismatches, which raise
  the executed pairs from 50 / 664 / 795 to 927 / 1,873 / 1,616 of the 1,388 / 1,930 / 1,924 reachable ones; the fast
  helpers against the reference on 20 million fuzzed vectors per operation, 0 mismatches. Each check was shown to fail
  when it should (a swapped Q choice: 9,623 of 12,166 cutscene launches differ; the exact-lane detection switched
  off: 227,206 fuzz mismatches).
- **Reviewed.** Four independent reviewers compared the lower instructions, the pair order and control flow, the
  timing exploration and the fast arithmetic with the interpreter's sources (reports in
  `D:\Projects\Recomps\mksm-vu1-build\review\`): no wrong result for any operand values or path of the game's
  programs. Closed from their notes: a MAC read from before the launch is refused (no program has one); a JR is
  followed only through the link register of its own launch (the generated code did not change); the glue reports a
  dropped XGKICK packet; the fast arithmetic's argument is now the complete one, with the cases that pin its bounds
  as tests (2^-79 is tight: at 2^-80 one MADD lane would give +0 for -0). Left as documented differences: a launch
  that passes the interpreter's 65536-cycle budget runs to its end in the translation, and `mac` / `status` of
  `VU1State` are not kept across translated launches (nothing the game runs can observe either).
- **In the game** (`src/vu1_runtime.cpp`). The override module replaces the VIF1 MSCAL / MSCNT callbacks (no upstream
  patch); `scripts\build.cmd` passes `tools/vu1gen/cmake/ps2recomp.cmake` and the ELF to the runtime's configure
  step, the images are translated during the build into `out\build-quick\generated-vu1` and linked into the runner
  as their own library. The first build with it recompiled all 425 unity batches (the runner got an include
  directory); later builds take about a minute. `MKSM_VU1=0` runs upstream's interpreter, `MKSM_VU1=verify` runs the
  interpreter and compares the translation on a copy at every launch, `MKSM_VU1=verify:<n>` runs the translation and
  has a second interpreter check one launch in n on a copy (`[mksm-vu1]` lines; `MKSM_VU1_DUMP=<folder>` writes the
  launches that differ in the capture format; `MKSM_VU1_STATS=1` prints the counts every 10 s in the normal mode).
  **Verified:** `verify`, 240 s through the menus into the first level: 346,246 launches, 0 differ, none left to the
  interpreter; `verify:20` at full speed, 115 s: 70,677 launches checked, 0 differ; `verify:1`, 124 s: 0 differ.
- **Speed.** The opening cutscene of Goro's Lair plays at 30 to 36 frames a second and drops to 20 when the
  characters are on screen; gameplay after it (now reached) stays near 20. It was 2 to 8, and 4 with the characters.
  The game makes 18,000 to 30,000 launches a second there, about 3 us each. The frame rate sits on divisors of 60
  because the game flips at a vertical blank; its animation keeps real time down to about 15 frames a second and
  slows below that (the interpreter's run needed 50 s of wall time for what the translated run plays in 23 s).
- **Scripted pad, tighter.** With the menus at 60 frames a second:
  `MKSM_PAD_SCRIPT=6:cross,9:cross,16.5:start,19.5:cross,22.5:cross,26:cross,29.5:cross` reaches the level's loading
  at 30 s and the cutscene at 42 s (150 s with the 15 s spacing). The autosave notice is up 5 s after start and every
  screen follows its press within 2.5 s, except the legal screen before "Press START button", which loads for about
  5 s.
- **The 3D picture has artifacts, and they are not the translation's.** In the cutscene and in gameplay the
  characters are right, but the environment shows blocky stair-stepped areas, hard white wedges and light shafts as
  solid slabs. The cutscene advances mostly per frame, so captures of two runs have to be compared at the same frame
  count, not at the same second: about 110 frames into the scene the interpreter (`MKSM_VU1=0`) shows the same white
  wedge and the same broken left side as the translated run (the picture was only "right" in the first seconds, which
  is all anybody saw at 4 to 8 frames a second). `MKSM_GS_DUMP=<file>` with `MKSM_GS_DUMP_TICKS=<a>:<b>`
  (`src/trace_gs_dump.cpp`) records the GIF packets of a range of vertical blanks, and `tools/gs_dump.py` lists the
  uploads and the draw batches with their state. **First look at the opening shot** (ticks 2706-2709, not analysed
  further): the scene is drawn into FBP 0x46 with the depth buffer at 0x8C (about 90 batches of textured strips,
  88 uploads of 911 KB in a frame), and then comes a post-processing chain: the depth buffer read as a Z24 texture
  (TBP 0x1180) into FBP 0x12C at 320x128; frame buffers with a Z24 pixel format (FBP 0x12C and 0x14C); a sprite that
  writes colour into FBP 0x8C, the depth buffer's own memory; thirteen 320x128 sprites into FBP 0x8C that sample
  TBP 0x08C0 (the scene) and TBP 0x1180 (the very buffer they draw into), each shifted by a few pixels, blended
  `Cs*FIX+Cd`: an iterated blur with the render target as its own texture; and 64x64 textures drawn as sprites
  thousands of pixels wide with FBMSK 0xFF000000. Stair-steps and hard-edged bright areas are what that chain would
  give if it is not rendered as on the console. **Not verified which step fails**; the candidates are in "Next".
- **Where the game thread's time goes now** (`MKSM_PROFILE=host`, which now lists 70 leaf and 60 inclusive entries;
  3D scene with characters, about 20 frames a second, the thread 93 per cent busy; inclusive shares): the VIF1
  transfer of a frame 37 per cent, of which paraLLEl-GS's work on the PATH1 packets is 19 (state and texture updates
  per batch, flushes; 15 inside the Vulkan driver), the translated VU1 code 11 and the VIF unpack 5; the IOP
  emulation 23 (it uses the whole allowance of patch 0006 all the time, so IOP code is spinning: probably the sound
  modules without an SPU2, to be measured again with the sound work); VU0 microprograms through the interpreter 10
  (`PS2Runtime::executeVU0Microprogram`); clock queries (`steady_clock::now`, 4.5), heap allocation and release
  (about 5: a vector per GIF packet, a 1.8 MB buffer per DMA chain) and scheduler bookkeeping; the rest is the game's
  own code, spread over hundreds of functions at the quick profile's `/O1`. A frame's VIF1 chain is about 111,000
  quadwords (1.8 MB): 1 MB of texture data in 68 uploads through DIRECT, 770 KB of vertex data, 563 launches; the
  game sends such a chain about 37 times a second in the opening shot. No MSKPATH3 is used.
- **One crash seen.** With `PS2X_GS_BACKEND=cpu` (and the translated VU1, the 15 s pad script) the game stopped during
  the level's loading with `[guest-branch:missing-target] ... target=0x1dfd520 ... trace=0x414638 -> 0x480010 ->
  0x47ffb0 -> 0x414ca0 -> ...` (a jump into data from the job system). One occurrence, not investigated; the GPU
  backend has not shown it in any run.
- **Sound** was taken up again in its own worktree (`D:/Projects/Recomps/ps2recomp-snd`, notes in
  `D:/Projects/Recomps/snd-notes/PROGRESS.md`): the SPU2 core and the libsd HLE are moved onto patches 0001-0032 and
  IOP modules are started with argc and argv (three commits there, each with tests). The host audio output is half
  written and not committed. Still to do: the output, an offline render to a WAV file as a proof, the patch files
  (from 0040 up) with the configuration change, and a test in the game.
- **Next.** (1) The 3D picture: find which step of the post-processing chain differs. Check how paraLLEl-GS, as the
  backend configures it (`DebugMode::disable_sampler_feedback`), treats a texture that is the current render target,
  a FRAME with a Z pixel format, and the depth buffer sampled as a texture; a capture of the same frame from another
  GS (PCSX2's software renderer from a GS dump, or upstream's CPU backend once its crash is understood) tells whether
  the stream or the backend is at fault. (2) The frame rate: a GS thread (the packets queued to a worker that owns the
  backend) takes about a fifth of the game thread; the IOP share after the sound work; VU0 microprograms through the
  translator; fewer clock queries and allocations; the release profile for the game's own code. (3) A pacing option
  that does not wait for the next vertical blank when a frame is late (the frame rate then stops snapping to 30 and
  20): the vertical blank is real time here and the game's animation follows it, so the display swap must be made
  when the frame is ready instead; needs the game's swap protocol (frame sync `0x4158D8`, vsync callback `0x26F2A0`,
  swap request `0x4FC09C`). (4) Sound: finish the output, make the patches and test in the game. (5) Smaller: a
  run-time check of the XGKICK assumption (a store that can still reach the packet being transferred hands the launch
  back to the interpreter from a snapshot), the software backend's loading crash, the audit leftovers above.

## 2026-11-12 — sound: an SPU2 in the IOP, libsd, the output to the audio device (branch `sound`; not run in the game)

Patches 0040 to 0045 and a configuration change, made in a PS2Recomp worktree of its own (`ps2recomp-snd`, one commit a
patch on top of 0001-0032) and tested there. **The game was not run with them**: everything below that is called
verified was verified by unit tests, by generating the game's code, and by an offline render with the game's IOP
modules; the list of what is not verified is at the end.

- **`patches/0040-iop-spu2-core.patch`** (`ps2xIOP/src/emulator/core/iop_spu2.*`). A sound unit with no register file:
  2 MB of sound RAM, two cores of 24 ADPCM voices (decoding and ADSR after psx-spx and PCSX2, 4.12 pitch with linear
  interpolation, KON/KOFF/ENDX/NAX, loop flags), the VMIX dry mix, the master volumes, and for each core the input a
  block transfer feeds (a two-block FIFO filled one block ahead of playback, as on the hardware, so a half of the
  driver's ring is complete when its last block has been fetched). Not modelled: reverb (its parameters are stored),
  noise, pitch modulation, volume sweeps (the target is applied at once), the gaussian interpolation, IRQA, the
  digital output. Tests: `ps2_iop_spu2_tests` (9 cases), now also run by `scripts\build.cmd -Tests`.
- **`patches/0041-iop-libsd-hle.patch`** (`ps2xIOP/src/emulator/imports/iop_libsd.*`). The sound drivers reach the
  hardware only through LIBSD.IRX's exports, whose table is the same in every build, so each `libsd` import is
  answered natively by ordinal with the arguments, results and error codes of LIBSD.IRX's own functions (init,
  parameters, switches, addresses, core attributes, voice and block transfers with their status, the transfer and
  SPU2 interrupt handlers, the effect area); LIBSD.IRX still loads, and the ordinals without a native form
  (`sceSdNote2Pitch`, `sceSdProcBatch`...) run its code, logged once each (`[IOP] libsd #n ... has no native form`).
  A transfer's interrupt handler is called with (channel, argument) when the transfer completes. `MKSM_TRACE_LIBSD=1`
  logs the first 256 calls, the first 64 transfer interrupts and a count per ordinal every 5 s of IOP time
  (`[mksm-libsd]`). The debug panel's IOP page has an `spu2` row (frames, interrupts, active voices, fallbacks...).
- **`patches/0042-iop-module-argv.patch`** and **`patches/0045-iop-ctype-table.patch`**: why CRI's driver ignored
  its options. The game loads `CRI_ADXI.IRX` with the argument block `sdinit=0\0spucore=2\0`. (1) The emulator called
  a module's start function with (byte count, block) where the IOP kernel's modload passes (argc, argv): argv[0] the
  module's path (`LBF` for a module loaded from a buffer), one pointer per string of the block, then a null pointer.
  (2) With a proper argv the options were still not taken: the driver (`0x18C`) accepts a number only after
  `look_ctype_table` (sysclib #8) says its first character is a digit, and upstream returned 0 for every character.
  Left at its default the driver's main thread calls `sceSdInit(SD_INIT_COLD)`, which resets the sound unit the game
  set up through SDRDRV before. Seen in the offline render: before 0045 the driver prints "sceSdInit(SD_INIT_COLD)
  call.", after it "sceSdInit Call = %d" at its start and no cold init. SDRDRV reads its own options the same way.
- **The SPU2's clock and the output** (`patches/0043-iop-spu2-sound-sink.patch`, `patches/0044-spu2-audio-output.patch`).
  The emulated IOP's clock cannot feed an audio device: it gets its cycles from the EE's accounting, in bursts (one
  step of up to a vertical blank after an idle wait, the catch-up steps of patch 0005), loses what exceeds 20 ms of
  debt or its quarter of the host time (patch 0006), and runs RPC servers outside those grants. A sound unit that
  makes one frame every 768 of those cycles (which is what 0040 and 0041 do on their own, and what the unit tests use)
  would have to be resampled or cut to fit the device. Instead the SPU2 is paced by its output: `PS2Spu2Output`
  (`ps2xRuntime/include/runtime/ps2_spu2_output.h`) is a ring of stereo frames with one writer (the thread that runs
  the IOP) and one reader (the device's callback) and no lock, kept at a level of 60 ms (`PS2X_AUDIO_LATENCY=<ms>`,
  10 to 500); at the IOP's service points (a scheduler pass or a libsd call, not more often than every 2048 IOP
  cycles) the SPU2 mixes exactly what the ring is short of that level. So the sound unit advances as fast as the
  device plays, 48,000 frames for each second of the device's own clock, with nothing resampled or dropped, and the
  block-transfer interrupts follow the same clock because they are raised by the frames. The callback (a 48 kHz
  stereo raylib stream, started after `InitAudioDevice`) only copies frames out. While no device reads (none exists,
  `PS2X_AUDIO=0`, or it has not asked for 250 ms) the host's steady clock drains the ring at 48,000 frames a second,
  so the drivers see the same sound unit with or without a device. When the IOP thread is away longer than the ring
  covers, the device gets silence, entered and left with a 64-frame ramp, and the missed time is not made up: the
  sound unit stood still and goes on where it was.
- **The wait after a transfer interrupt.** Mixing a burst must not read a half of the driver's ring before the driver
  has refilled it. CRI's driver refills from a thread: its transfer handler (`0x59E0`) only signals a semaphore, the
  thread (`0x5530`, priority 39) wakes, asks `sceSdBlockTransStatus` which half the DMA is on, hands the other half's
  chunks back to its stream (`0x5298`) and calls the routine that copies PCM into them (`0x1D18`). On the console that
  is a matter of a few thousand IOP cycles, and the DMA comes back to that half only after the other one has been
  fetched (1,280 frames later with the 1,024-frame halves the archived port saw). Here the SPU2 stops mixing when a
  block-transfer handler has been called and goes on when no IOP thread is ready any more (the refill has run), or
  after 3 ms of IOP time if the IOP never goes idle. A test stream driver built the same way (handler, semaphore,
  refill thread) plays a counting signal through twelve rings in order when the output asks for all of it at once,
  and plays blocks twice when the wait is taken out.
- **Diagnostics of the output**: `MKSM_TRACE_AUDIO=1` logs every 5 s the frames mixed, played, discarded (host
  clock), missing (silence the device got), the gaps and the ring's lowest and highest level (`[mksm-audio]`);
  `MKSM_AUDIO_WAV=<file>` records what the SPU2 mixed as a WAV file (valid at any moment; the gaps' silence is not in
  it). At start the log says `[audio] SPU2 output: 48 kHz stereo, 60 ms buffered`, or why there is no stream.
- **libsdr is the game's code again** (`config/recomp.toml`; the build regenerates). `sceSdRemoteInit` (`0x382C70`),
  `sceSdTransToIOP` (`0x382D20`) and `sceSdRemote` (`0x382DB0`) leave the stubs list and are entry points named
  `mksm_libsdr_*` (a function named after a runtime handler is stubbed whatever the list says). The path their code
  takes: `sceSifBindRpc(client 0x757640, 0x80000701)` and `sceSifCallRpc` are the runtime's (`Kernel/Syscalls/RPC.cpp`)
  and end in `IopSubsystem::handleRpc`, which asks the emulator's registered servers first: the 0x40-byte send
  buffer is copied into SDRDRV's, its server function (`0x588`) runs on the IOP with (command, buffer, size), calls
  libsd by import, and its result is copied back (the command is the RPC function number, `0x8000` init, `0x8010`
  set parameter, `0x80D0` voice transfer...; the arguments are words 1 and up, the result word 0). `sceSdTransToIOP`
  uses the syscalls SifSetDma (the runtime copies EE memory into IOP RAM at once) and SifDmaStat (done). Verified by
  generating the code with the old and the new configuration (outside the repository): 13,596 files each, five
  differ (the three functions are translated code instead of calls of `ps2_stubs::sceSd*`, the stubs header, the
  registration), and `tools/missing_targets.py` reports the same single target (`0x46F700`) for both. The game calls
  its libsdr at 17 places: the boot (`0x21352C`: `sceSdRemoteInit`, a cold `rSdInit`, the four master volumes at
  `0x3FFF`), the MMIX of both cores read and set (`0x2722F0` to `0x272338`), and the sample uploads (`0x463C6C`,
  `0x463D64`, `0x4648B4`, `0x464A60`, `0x464C28` with `rSdVoiceTrans` and its status; `0x464678` and `0x4649A8` with
  `sceSdTransToIOP`). The commands are `0x8000`, `0x8010`, `0x8020`, `0x80D0` and `0x80F0` only, which SDRDRV passes
  to libsd ordinals 4, 5, 6, 17 and 19, all answered natively. SNDF's and CRI's own libsd imports are all answered
  natively too (SNDF: 4 to 9, 11, 17, 19 and 23, the effect attributes, which are stored and not mixed).
- **Verified.** In the worktree, on patches 0001-0045: `ps2x_tests` 500 of 500 (run from the PS2Recomp folder; 11 of
  them are new: module arguments, the ctype flags, libsd by ordinal, eight of the output ring, its pacing with and
  without a device, the gap ramps and the IOP's SPU2 making 48,000 frames a host second at a ninth and at twice the
  IOP clock), `ps2_iop_spu2_tests` 9 of 9, `ps2_iop_emulator_tests` (nine sound cases), `ps2_iop_import_tests`,
  `ps2_iop_compatibility_tests` 18 of 18, `ps2_iop_import_version_tests` 6 of 6. The module-argument test and the
  refill test were seen to fail without their fix. The 38 patches apply in name order with
  `git apply --ignore-whitespace` on `e563698` and give the worktree's tree exactly.
- **An offline render with the game's modules** (a program kept outside the repository; nothing of the game is in
  it or in its output). `LIBSD.IRX`, `SDRDRV.IRX`, `CRI_ADXI.IRX` (with the game's arguments) and `SNDFI.IRX` load into
  the IOP emulator (start results 0, 2, 2, 2; five threads, four RPC servers), the output ring is the SPU2's sink on a
  clock of the program's, and a stand-in device reads 48 frames a millisecond. SDRDRV is called as the game's libsdr
  calls it (22 RPCs, all served by the module, none by upstream's HLE libsd service): `rSdInit`, the master volumes
  (read back `0x3FFF`), a synthetic looping ADPCM block uploaded with `rSdVoiceTrans` and keyed on with
  `rSdSetAddr`/`rSdSetParam`/`rSdSetSwitch`, then a looping `rSdBlockTrans` of a PCM sine. The 2.5 s WAV file has
  silence, then the voice (peak 7168, RMS 4948, 1714.4 Hz for an expected 1714.3; its first frame 60.0 ms after the
  key-on, the ring's level), silence after the key-off, the streamed sine (peak 8000, RMS 5656, 750.0 Hz) and silence
  after the stop; 122,880 frames mixed, 120,000 played, no gap. The IOP was idle through it: 134,318 instructions in
  92.5 million cycles with the four sound modules loaded.
- **The device side, silently.** A second program outside the repository makes the runtime's raylib calls
  (`InitAudioDevice`, a 48 kHz 16-bit stereo stream whose callback reads the ring) with the master volume at 0 and a
  loop that writes what the ring is short of every millisecond. On the development machine the device asks for 480
  frames at a time (10 ms), 301 times in 3 s (48,000 frames a second within one callback), the ring stays between
  2,400 and 2,880 frames, no gap; a 300 ms pause of the writer gives one gap of 11,520 frames (the pause less the 60 ms
  the ring held); the stream stops and unloads cleanly.
- **Not verified, because the game was not run.** (1) Sound heard from the game: `PS2Runtime`'s own start and stop of
  the stream have only been compiled. (2) The game's libsdr reaching SDRDRV in the game (the sample uploads of
  `0x463BF8` and `0x464238`, the boot's `rSdInit`). (3) Music: CRI's stream needs the game's EE side (it decodes the
  ADX data and hands the PCM to the IOP module, which serves RPC `0x90000200`; the module's PSM thread refills the
  block transfer); only the module's start and the refill mechanism with a test driver are covered. (4) SNDF's
  voices and its 200 Hz tick. (5) Whether the IOP still uses its whole time allowance (23 % of the game thread on
  2026-11-11; possibly LIBSD.IRX's own code waiting on registers that were a plain store, which the drivers no longer
  reach): to be measured again. (6) The sound at low frame rates: a pause of the game thread of more than 60 ms
  between two checkpoints is a gap. (7) The movies' sound (Sofdec) is another path.
- **Open risks.** An early libsdr call made before SDRDRV's thread has registered its server would be answered by
  upstream's HLE libsd service without an error (in the render the server was there 20 ms after the load;
  `MKSM_TRACE_LIBSD=1` shows whether the calls arrive: `#4 sceSdInit` for the game's `rSdInit`). The SPU2's mixing
  counts in the IOP's quarter of the host time (patch 0006), and while that allowance is used up the SPU2 is not
  served for up to 7.5 ms. If IOP code spins, the IOP never goes idle and every transfer interrupt costs 3 ms of IOP
  time before mixing goes on. `sceSdBlockTrans` in read mode is refused; reverb is not mixed. With `spucore=2` CRI's
  driver starts a block transfer on both cores (`0x1FD8`: the loop over its two streams), with one handler and one
  refill thread for the two; the SPU2 mixes both inputs, but two transfers side by side are not covered by a test
  yet (each core's input is tested on its own). The driver can also switch core 1 to the digital output and silence
  its AVOL (`0x5D6C`); neither is modelled (core 0 always passes through core 1).
- **To test in the game.** `scripts\build.cmd -Regenerate -Tests`, then a run with the pad script of 2026-11-05:
  the menu music, the announcer and the select sounds are what the archived port had (its notes of 2026-10-28 and
  2026-11-03). `MKSM_TRACE_AUDIO=1` tells an underrun (missing, gaps) from a driver problem, `MKSM_AUDIO_WAV` keeps
  the sound for inspection, `MKSM_TRACE_LIBSD=1` shows what the drivers ask of the sound unit.
