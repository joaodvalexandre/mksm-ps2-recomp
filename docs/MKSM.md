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
  argument registers and the caller, and a count summary every 300 calls of the first one; it replaces the
  functions in the runtime's dispatch table, so it sees every caller.
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
