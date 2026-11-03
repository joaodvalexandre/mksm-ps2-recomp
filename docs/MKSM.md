# Mortal Kombat: Shaolin Monks — port log

Research log of the port of *Mortal Kombat: Shaolin Monks* (MKSM, NTSC-U `SLUS-21087`; the port started on the PAL
`SLES-53524` executable and switched on November 3, 2026, see that entry). Current goal: **get the game to boot and run
smoothly on Windows**; everything else (improvements, higher frame rates, etc.) comes later.

_Last updated: November 3, 2026_

## The disc

Image used: `SLES_535.24.Mortal Kombat Shaolin Monks.iso` (PAL, 3.36 GB), placed in `game/` (ignored by git).
`SYSTEM.CNF`: `BOOT2 = cdrom0:\SLES_535.24;1`, `VER = 1.02`, `VMODE = PAL`. It is a **single-layer** DVD: the
extractor reports `layer1=0` (an earlier note here said dual layer; that was wrong).

Full extraction, needed to run (27 files, ~3.4 GB):

```sh
python tools/iso_extract.py "game/SLES_535.24.Mortal Kombat Shaolin Monks.iso" game --layout
```

Only what is needed to recompile:

```sh
python tools/iso_extract.py "<image>.iso" game --only SLES_535.24,SYSTEM.CNF,IOP/CDVDSTM.IRX,IOP/CRI_ADXI.IRX,IOP/IOPRP300.IMG,IOP/LIBSD.IRX,IOP/MCMAN.IRX,IOP/MCSERV.IRX,IOP/PADMAN.IRX,IOP/SDRDRV.IRX,IOP/SIO2MAN.IRX,IOP/SNDFI.IRX
```

Relevant disc contents:

| File | Size | Purpose |
|---|---|---|
| `SLES_535.24` | 4.9 MB | Main executable (ELF) |
| `IOP/*.IRX`, `IOP/IOPRP300.IMG` | ~0.7 MB | IOP modules: `CDVDSTM`, `CRI_ADXI`, `LIBSD`, `MCMAN`, `MCSERV`, `PADMAN`, `SDRDRV`, `SIO2MAN`, `SNDFI` |
| `GAMEDATA.WAD/.WAE/.WAF/.WAG/.WAH/.WAI/.WAJ` | ~0.4 GB each | Game data (7 volumes), with `WADCRC.BIN` and `FILELIST.DIR` |
| `FRONT/MOVIES/*.SFD` | 1–225 MB | Sofdec movies (opening, endings, ESRB, Midway) |

The ELF builds its paths as `cdrom0:` + `\IOP\PADMAN.IRX`, `\GAMEDATA.WAD`, `\FRONT\MOVIES\OPENING.SFD`… so the
modules load straight from the extracted tree. It also names `\GAMEDATA.WAK`, `\IOP\SNPROFIL.IRX`,
`\IOP\SULPHASOUND.IRX` and `\IOP\SULPHACOMM.IRX`, which are not on the disc (probably development leftovers).

## The executable

- MIPS R5900 ELF, **entry `0x0011C038`**, a single `PT_LOAD` segment at `0x00100000` (`filesz 0x4B2294`,
  `memsz 0x680B98`).
- `.text` `0x00100000–0x0049A9C8` (~3.7 MB), `.vutext` `0x0049A9D0` (0xB630 bytes), `.data` `0x004A6400`,
  `.bss` `0x005B2380`.
- **No symbol table**: there is no `.symtab`; `.mdebug` is empty. The function map is generated with PS2Recomp's
  analyzer.
- Middleware visible in the strings: **CRI ADX** (`ADXCS/ADXF/ADXT`, build June 2005) and **Sofdec/SFD** for audio
  and movies; Sony SDK: `sceGs*`, `sceCd*`, `sceMc*` (classic libmc), `scePad*` (classic libpad), `sceSd*`,
  `sceMpeg*`.
- The SDK is linked in several runs with game code in between (`docs/status/data.toml` lists them): C++ EH, libpad,
  libm around `0x201380`; libsdr, libgraph, libdma, libvu0, libcdvd, libmc around `0x382C70`; libmpeg, libipu,
  libc around `0x467BF0`; libkernel, sif, fileio, loadfile around `0x4805B0`.

## What the runtime has to handle

| Topic | MKSM | Consequence |
|---|---|---|
| Region/video | **PAL**, 50 Hz | The `vblank-ntsc` patch fixes the VBlank at 59.94 Hz; the field rate has to become configurable |
| Pad | `PADMAN.IRX` + `SIO2MAN.IRX` (classic libpad) | The runtime does not model SIO2: `scePad*` are stubbed to its EE-side handlers |
| Memory card | `MCMAN.IRX` + `MCSERV.IRX` (classic libmc) | `sceMc*` stubbed to the runtime's EE-side handlers |
| Audio | **CRI ADX** (`CRI_ADXI.IRX`) + `SNDFI.IRX` over `LIBSD`/`SDRDRV` | The native SPU2 engine handles every `libsd` import by ordinal: ADX music plays; the EE's libsdr (sample upload through `SDRDRV.IRX`) is recompiled, so the SNDF effects sound, with a pitch/tempo problem still open |
| Movies | **SFD** (Sofdec, MPEG-2 through the IPU) | `sceMpeg*` stubbed to the native decoder; the SFD demultiplexer has to be checked |
| Disc | 7 `GAMEDATA.WA*` + `FILELIST.DIR` | The extractor's `.disc_layout` works as is |

The runtime's tailor-made IOP services (`989snd`, `mc2_d`, `sio2d`, `dbcman`) are only activated by module name,
so with this game's IRX modules they should stay inert and the IOP runs the modules in the R3000 interpreter. This
is a hypothesis to confirm once the game gets past its startup.

## Function map and configuration

1. `ps2_analyzer game/SLES_535.24 <out>.toml` with `PS2X_ANALYZER_FUNCMAP=config/funcmap.csv`
   (`patches/ps2recomp-analyzer-funcmap.patch`): 13,627 functions, 476 with SDK names from the embedded symbol
   database. Overlapping functions are clamped to the next start and repeated names get their address as a
   suffix.
2. The TOML becomes `config/recomp.template.toml` (paths replaced by `@ELF@`, `@MAP@`, `@OUT@`). 156 stubs; the
   software double `floor`, `sqrt` and `log` are removed (their ABI does not match the runtime's float handlers),
   which leaves 153.
3. `python tools/funcmap_fixup.py game/SLES_535.24`: 3 gaps with code added to the map (13,630 functions) and 247
   jump targets that are not function starts added to `entry_points` (567 in total). It also adds every word outside
   `.text` that points into `.text` right after a `jr $ra` + delay slot but not at a function start: a function pointer
   (vtables, callback tables) to a function the map merged into the one before. 687 more, 1,254 entry points in all.

Some SDK names are false matches of the symbol database (`_InitSys` and `eh_context_free` appear at a dozen
addresses in game code). They are only names: none of them is stubbed.

## Test log

### 2026-10-27 — first build and first run

- Build: the full pipeline builds `ps2EntryRunner.exe` with MSVC 14.51 (Visual Studio 18 Build Tools, picked by
  `2_build.cmd`). A build folder configured with another Visual Studio fails with `STL1001` (the cached compiler
  and the loaded headers differ): delete `mksm-work\PS2Recomp\out\build` when switching.
- Run (map without the fix-up of step 3): the window stays pink (nothing presented) and there is no sound. The log
  shows the runtime set up (GS renderer, SPU2 output at 48 kHz, the `SLES_535.24` override) and then:
  `[guest-branch:missing-target] kind=DirectJump op=J source=0x486534 target=0x487890`. The main thread stops inside
  `_InitSys` (`0x4864E8`, called from the entry at `0x11C218`): `0x487890–0x4878C8` was a gap of the map, code only
  reached by `J`. Fixed by `tools/funcmap_fixup.py` (step 3 above); verified on the next run (the boot gets past
  `_InitSys` and loads the 9 IRX modules).

### 2026-10-28 — startup fixes and the pink screen

Startup. The first two fixes started in the build tree only; they are now `patches/ps2recomp-iop-module-arena.patch`
and `patches/ps2recomp-ee-end-of-heap.patch`, each with its `ps2xTest` test.

- `SNDFI.IRX` did not load (`module arena exhausted`): the 9 modules span `0x76F00` bytes from `0x10000` and the
  IOP heap started at `0x70000`. `IopMemory::HeapBase` moved to `0x90000`.
- Every `malloc` failed: `EndOfHeap` (syscall `0x3E`) returned the end of the runtime's private heap (`0xFF000`)
  instead of the limit the game gave `SetupHeap`, so newlib's `sbrk` (`0x4807F0`) refused every request and the GS
  packets were built at address 0. `EndOfHeap` now returns the game's limit.
- `sceCdLayerSearchFile` (`0x385F90`) retried forever to bind the IOP search server `0x80000597`; it is bound to the
  runtime's native `sceCdSearchFile` (`src/mksm_overrides.cpp`).
- `sceCdSearchFile failed: \\FRONT\MOVIES\BLITZ.SFD;1` (and `GAUNTLET.SFD`) is expected: the PAL disc has no such
  movies (it has `ESRB`, `MIDWAY`, `OPENING`, `FINAL`, `END006`, `END011`), the game only probes for them.

The pink screen, analysed from the output side:

- The host window shows its magenta placeholder (`GSHostPresentation::blankFallback`) whenever the backend's
  `Present` returns no frame, and `Present` returns none when PMODE enables no read circuit or DISPLAY1/2 is zero
  (`gs_hw_backend.cpp`). A display that is on with nothing drawn would be black: pink means the display registers
  were never turned on.
- The game has no direct stores to PMODE/DISPFB/DISPLAY (only CSR accesses and a debug BGCOLOR store at
  `0x414A34`). It uses libgraph: `sceGsResetGraph` at boot (`0x274224`, `0x275484`), then `0x275170` seeds two
  display envs with `sceGsSetDefDispEnv` and patches their FBP, and the flip `0x275870` applies one of them with
  `sceGsPutDispEnv` (5 callers, the frame loop at `0x274898` among them).
- Those libgraph functions are stubs. The runtime's `sceGsSetDefDispEnv` only wrote DISPFB and DISPLAY and left
  PMODE, SMODE2 and BGCOLOR as they were (zero), while libgraph's own (`0x3831F0`) writes PMODE `0x66` (circuit 2,
  CRTMD 1, MMOD, AMOD), SMODE2 from the reset mode and BGCOLOR 0. Every flip therefore loaded PMODE 0. God of War
  used `sceGsSetDefDBuffDc`, whose handler does set PMODE, so the gap never showed there.
- Fix: `patches/ps2recomp-gs-def-disp-env.patch`, with a `ps2xTest` regression test that fails without it. In the
  game: pending the next run.

Black screen after the display fix (pink turned black: display on, nothing drawn). The GS counters stayed still for
the whole run (7 DMA, 3 GIF, 4 VIF): the game never started rendering. The thread watch and the branch ring showed
the main thread looping in `0x211DB0` (start a read, then every frame: poll it, run the ADX server `0x319CB8`, wait
for vsync), called from `0x211750` → `0x2F8240` → the boot `0x2131E8`: it waits for file 29, `\GAMEDATA.WAD`, read
through CRI's file system (ADXF → CVFS → the `SRD` device, which reads with `sceCdRead`/`sceCdSync`). The SRD server
only runs in CRI ADX's threads, and there were none: `ADXPS2_SetupThrd` (`0x414FF0`) creates four (idle counter
`0x414858`, servers `0x4148B0`, `0x4149B8`, `0x414AA8`; ids at `0x534478`, `0x534480`, `0x534484`, `0x53448C`) at the
main thread's current priority, read with `ReferThreadStatus`, and lowers them afterwards. The main thread runs at
priority 0, as on the console, but the runtime's `CreateThread` only took 1-127: all four ids were `-403`
(`KE_ILLEGAL_PRIORITY`). Fix: `patches/ps2recomp-ee-thread-priority.patch` (0-127 for `CreateThread` and
`ChangeThreadPriority`, like the EE kernel; regression test that fails without it).

With the threads created the load of `GAMEDATA.WAD` completed and the boot went on, to stop again (still black): the
main thread spun at priority 0 in `0x3794B0` (loop while bit 0 of `0x755DB8`, a loading flag, is set) while CRI's two
servers stayed Ready at priority 1 and never ran. CRI sets the main thread to `0x18` at the end of its setup, but its
lock (`0x414638`: raise the caller to priority 1 and keep what `ChangeThreadPriority` returned) and unlock (`0x4146C0`:
restore that value) rely on the EE kernel returning the previous priority; the runtime returned `KE_OK` (0), so every
unlock moved the caller to priority 0 (before the first fix that was refused and the caller stayed at 1, the
`prio=1` of the earlier runs). Same patch: `ChangeThreadPriority` returns the previous priority (test extended, fails
without it).

With that the boot reached the first movie (skipped) and the frame loop: the display flips every frame between FBP 0
and `0x50` (640x256 each), the game draws ~660 primitives a frame with 4 textures (`MKSM_GS_HW_STATS`: nothing
skipped), and the main thread waits for each field on CSR at `0x274A48` at priority `0x18`. Still black, and the
displayed buffer was entirely black (`MKSM_GS_HW_DISPDUMP`). The game keeps one block per buffer at `*(0x4FC1F8)`
(display envs at `+0x00`/`+0x40`, then GIF tag, draw env 1, draw env 2 and clear packet at `+0x80`/`+0x1F0`) and calls
`sceGsPutDrawEnv(tag)` (`0x275430`, `0x275444`); libgraph's (`0x383918`) DMAs the tag and its NLOOP quadwords to the
GS. The runtime's read 8 register pairs from the tag itself: the tag became a register, TEST_1 and context 2 were
never set and the clear never ran. Fix: `patches/ps2recomp-gs-put-draw-env.patch` (sends the tag and its pairs through
the GIF path; regression test that fails without it). In the game: pending the next run.

Movies. The file table at `0x4D3D00` names every file the game opens by index: the movies 0-7 (`MIDWAY`, `OPENING`,
`FINAL`, `END006`, `END011`, `ESRB`, `BLITZ`, `GAUNTLET`), the IOP modules 8-20 (with `IOPRP300.IMG`) and the data
21-29. `0x270038` plays one movie (path in `a0`): it creates a CRI Sofdec player (`mwPlyCreateSofdec` `0x43B920`,
through `0x26FBD0`) and loops on `0x26FEA8` until the player reports its end (returns 4) or a button skips it (3);
its caller `0x32ECE0` turns 4 into 0. Sofdec (`MWSFD/PS2EE Ver.3.50`, CRI MPV/M2V) decodes through the IPU itself,
and the runtime has no IPU: the IPU DMA channels are accepted and ignored (`ps2_memory.cpp`), because its MPEG
support replaces Sony's libmpeg, which is what God of War used. A movie therefore never ends and the game waits in
that loop (vsync plus the movie's sound update at `0x271568`, as the thread watch showed). Until movies are supported,
`src/mksm_overrides.cpp` makes `0x270038` return 4 at once (`[mksm-movie] skipped ...`); `MKSM_MOVIES=1` plays them.

Still black after the draw-env fix: the draws are PSMT8 quads with CLUTs whose textures (`0x3F2C`, `0x3F70`,
`0x3F84`, `0x3F98`) were zero in GS memory. Two causes:

- `sceGsSetDefLoadImage`/`sceGsExecLoadImage` kept a private record whose DBP was multiplied by 8, so the only image
  the game loads that way landed outside GS memory. Fix: `patches/ps2recomp-gs-load-image.patch` (libgraph's own
  0x60-byte packet, sent through the GIF path; regression test).
- The game's other uploads go in GIF chains built with libpkt, and the chain before them was corrupt: a cnt tag of
  QWC 8 over 4 quadwords, so the GIF read a stale quadword as a tag (NLOOP 15844) and swallowed everything after it,
  texture uploads included. The config stubs five libpkt functions (`sceGifPkCloseGifTag`, `sceGifPkAddGsAD`,
  `sceVif1PkOpenDirectCode`, `sceVif1PkCloseGifTag`, `sceVif1PkAddGsAD`) and the rest runs as game code. The
  runtime's handlers rewrite the open DMA tag's QWC (`sceGifPacket` +8) each time they move the packet pointer
  (`refreshPacketBuilderPendingCount`), and the game's terminate (`0x384C58`) adds the QWC when it closes the tag:
  every tag closed after a stubbed call counted its quadwords twice. Fix: `src/mksm_overrides.cpp` replaces the five
  stubs with the game's own code (`libpkt=1` in `[mksm-override]`). The analyzer would add the stubs back on a
  regenerated config, so the override is the durable place. **Verified in the game:** the frame shows the game's
  framed text panel (background, corners, glow); the user saw it too.

No text in that panel: each frame also sends 10 GIF chains from address 0, which are empty. They come from
`0x37E5B4`-`0x37E828`, which builds a packet with `sceGifPkAddGsData` in a loop, closes it and calls
`sceDmaSend(gif, (pkt & 0x3FF0) | 0x80000000)`: the packet is in the scratchpad, and bit 31 of a DMA address selects
the scratchpad (libdma's `CheckAddress`, `0x384480`, turns `0x7xxxxxxx` into that form too). The runtime only treated
`0xF000xxxx` (`0x80000000 | 0x7000xxxx`) that way and read main memory at the offset. Fix:
`patches/ps2recomp-dma-scratchpad-address.patch` (`ps2DmaAddress`: the SPR bit in `sceDmaSend`'s argument, D_TADR,
D_MADR and a tag's ADDR; regression test that fails without it). **Verified in the game:** the panel shows its text,
the autosave notice ("MKSM uses an Autosave feature ... Press ✕ to continue").

The notice ignored ✕ (the keyboard, the user's Xbox controller and `MKSM_PAD_SCRIPT` alike). The game polls the pad
with libpad's state machine (`0x273200`), but `0x212808` takes the buttons and sticks straight from the buffer given to
`scePadPortOpen` (pad object `0x652200 + port * 0x700`, `+2..+7`), not from what `scePadRead` returns. That buffer is
libpad's: two 128-byte halves (frame count at `+0x58`, the game's `scePadGetDmaStr` `0x206AF0` picks the newer one),
each starting with the 32 bytes `scePadRead` returns; the IOP's pad driver keeps it up to date. The runtime zeroed it
at open and never wrote it again, so every button looked held and no press was ever new. Fix:
`patches/ps2recomp-pad-dma-buffer.patch` (`scePadRead` also writes its data into both halves; regression test that
fails without it). **Verified in the game:** ✕ continues to "No MKSM save data present on memory card ... create save
data now? No / Yes" (the emulated card is seen, empty), then the title screen ("Press START button"), the first-boot
brightness screen and the main menu (Single Player, Ko-op, Versus Mode, Load Game, Options, Kontent).

The main menu showed leftovers of earlier screens (the brightness bar's end, its panel border, trails of the menu
labels sliding in, dark blotches by the logo). The menu is ~42 pre-rendered sprites a frame, layered with Z
`0x199999A0`, `0x40000000` and `0x7FFFFFFF` over a Z24 buffer with ZTST GEQUAL, and nothing clears Z between frames.
The hardware renderer kept only the low 24 bits of Z (depths 0.6, 0 and 1), so the lower layers failed against the
previous frame's and the old pixels stayed. The GS clamps Z to the format's maximum (PCSX2's `zclamp`): all three are
`0xFFFFFF` and every layer passes. Fix: `patches/ps2recomp-gs-depth-clamp.patch` (`gsHwDepthToUnit`; regression test
that fails with the old wrapping). **Verified in the game:** the main menu is complete and clean.

The picture shook slightly and the 2D screens and their text flickered, at times losing the text for seconds. Three
causes, three fixes (each with a regression test that fails without it):

- The flip went ahead of the drawing. The game waits for the next field (CSR FIELD, `0x274A48`) and flips with
  `sceGsPutDispEnv`; the runtime switched DISPFB at once while the frame's last draws (the text, sent last) could still
  be queued on the VIF1/GIF worker, and the window showed the buffer without them.
  `patches/ps2recomp-gs-flip-after-queued-work.patch`: the flip waits for the queued GS work, like the other GS writes
  from the EE (the frame rate stays at 60 in the menus).
- Field rendering. The game draws 640x256 fields (SMODE2 interlaced, FFMD) and puts every other field half a line
  lower (a sprite at y 133.6 in one buffer, 133.1 in the other), which a TV cancels by showing that field half a line
  lower; the window showed the fields one after the other, so the picture moved a line every frame and thin strokes
  came and went. Forty consecutive window frames of the menu: each differed from the one before in ~20,000 pixels
  over the whole picture, and a one-row shift made them identical. The offset is in the game's own vertices, not in
  XYOFFSET (both buffers use OFY `0x7800`): a first try that dropped OFY's half line (as libgraph's
  `sceGsSetHalfOffset` sets it) changed nothing here and was removed. `patches/ps2recomp-gs-field-blend.patch`: in
  field mode the backend shows the mean of each field and the one before (PCSX2's "blend" deinterlacing,
  `MKSM_DEINTERLACE=0` turns it off). **Verified in the game:** consecutive menu frames now differ only where the
  fire and the selection sparkle move (300-500 pixels).
- The window stretched the 4:3 picture to 16:9 (God of War's port forced it; the dragon emblem's ring was 1.36 times
  wider than tall). `patches/ps2recomp-host-aspect.patch`: 4:3 by default, `MKSM_ASPECT=16:9` for a widescreen TV;
  `scripts/run.ps1` now opens a 720x540 window.

`scripts\5_scripted_run.cmd` runs the game with a pad script (`-Pad`, by default the sequence to the difficulty
select), captures the window every three seconds (`-Shots` for other frames) and writes `logs\scripted-<tag>\`
with the logs, the captures and a summary (presses, captures, last thread snapshot). `scripts\4_diagnose.cmd` runs the game for 30 s with the GS and DMA diagnostics on and moves everything to
`logs\diag-<date>-<time>\`: `run.log`, `run_err.log`, PNG captures (display, render targets after each batch, host
window) and a `summary.txt` with the last thread snapshot, GS counters, render targets and likely problems.

### 2026-10-28 (evening) — sound, widescreen and the character select

- **Freeze after choosing a character.** The game called `0x38C730` through a function pointer in data; the map had
  merged that function into the one before it, so the runtime had no entry for the address and the EE stopped.
  `tools/funcmap_fixup.py` now adds such pointers as entry points (step 3 of the function map, test in
  `tests/test_funcmap_fixup.py`, run by CI). **Verified in the game:** character and difficulty select no longer freeze.
- **Widescreen.** With `MKSM_ASPECT` wider than 4:3, `src/mksm_overrides.cpp` (`applyMksmAspect`) also writes
  `(4/3) / aspect` over the game's horizontal 3D scale at `0x4F4E08` (1.0 in the ELF; the value PCSX2's 16:9 patch
  for this game changes) and logs `[mksm-override] widescreen`. The menus are 2D and stay stretched; the 3D view is
  not reached yet, so the result is unverified.
- **Sound.** `LIBSD.IRX` on the disc is not the build whose offsets the native LIBSD overrides name, so its own code
  ran in the interpreter and wrote SPU2 registers the runtime does not have: no sound at all.
  `patches/ps2recomp-iop-libsd-imports.patch`: every `libsd` import (CRI ADX, SNDF, SDRDRV) goes to the native
  sound engine by export ordinal (`iopNativeLibsdImport`; GetCoreAttr, SetSpu2IntrHandler,
  GetTransIntrHandlerArgument and StopTrans added), and a looping block transfer calls its interrupt handler at each
  half (`iopNativeLibsdStreamInterrupts`, what CRI's stream refill waits for). Regression test "libsd imports drive
  the native sound engine by ordinal". **Verified in the game:** the menu music and the announcer play.
- **Open: the sound crackles.** The device never runs short (`[mksm-audio]` with `MKSM_THREAD_WATCH`: asked = got
  every period), so it is not an underrun. A capture of the output shows steps in the right channel at the 256-sample
  block boundaries of CRI's stream: the refill of a 1024-sample half lands late relative to the engine's reads.
  Next: model the ADMA read-ahead the way PCSX2's SPU2 does.
- **Open: no menu sound effects** (resolved on 2026-11-03: the EE uploads the samples itself through libsdr, which was stubbed; see that entry). `SNDFI.IRX` keys on core 1 voices, but never uploads their samples: its only
  `sceSdVoiceTrans` call (`0xE158`, in `0xE09C`) is skipped behind two capacity checks (`[s2+0x10] <= [v1+0x4A8]`,
  `[s2+0x18] <= [v1+0x628]`). SNDF does no file I/O, so the bank comes from the EE through SIF commands or RPC;
  `PS2X_TRACE_RPC` prints nothing for it, so that path bypasses `IopSubsystem::handleRpc`.
- **Open: the wait after difficulty select.** The game reads 19 sectors of `GAMEDATA.WAG` and then waits on an empty
  panel. It may wait on the same SNDF bank load.

### 2026-11-03 — the fight never loads: D_STAT

- **Interrupted builds.** The build of October 28 (after un-stubbing libsdr, below) stopped with eight unity objects
  "failed" and no compiler message: the build had been interrupted (console closed), each `cl.exe` in flight exited
  with code 1 and left a **zero-length `.obj`**, and ninja took those for up to date on the next build. `scriptsuild.ps1`
  and `2_rebuild_fast.cmd` now delete zero-length objects before building.
- **libsdr un-stubbed.** `sceSdRemoteInit`, `sceSdTransToIOP` and `sceSdRemote` (`0x382C70`-`0x383130`) are no longer
  stubs: the runtime's EE-side handlers (`Stubs/Audio.cpp`) faked the transfers in a private record and `sceSdTransToIOP`
  was a TODO, while the game's own libsdr talks to `SDRDRV.IRX` (RPC `0x80000701`; the IOP tries the module's server
  before its HLE service), whose libsd calls reach the native sound engine by ordinal. The map names them `libsdr_*` so
  that a regenerated config does not stub them again, and `tools/ci/validate_config.py` rejects those stubs. The game
  calls them at 17 sites: the boot (`0x2131E8`: `sceSdRemoteInit`, then `rSdInit` 0x8000 and four `rSdSetParam` 0x8010
  that set MVOLL/MVOLR of both cores to `0x3FFF`), the MMIX setup (`0x2722B0`) and, above all, **the sound-effect
  sample upload**: `0x463BF8` copies a block to the IOP with `sceSifSetDma` and asks `rSdVoiceTrans` (0x80D0) to move
  it into SPU2 RAM, polling `rSdVoiceTransStatus` (0x80F0); `0x464238` does the same with `sceSdTransToIOP`. That is
  why the menu effects were silent: `SNDFI.IRX` never uploads samples itself (its "capacity checks" of October 28 were
  a red herring), the EE does, and the stubs dropped the data. **Verified in the game:** the menu select sound plays
  (the user hears it slowed down or wrong, see below). SDRDRV serves RPC `0x80000701` (the IOP tries the module's
  server before its HLE service); its callback-client thread (bind to an EE server `0x80000704`, which the runtime
  cannot serve) is only created by command `0xE620`, which the game never sends.
- **`MKSM_HOST_SHOT` takes a list** of host frames (`patches/ps2recomp-host-shot-list.patch`, `host_shot_<n>.png` each),
  so a scripted run saves every screen it goes through. A run with `MKSM_PAD_SCRIPT=12:cross,16:cross,20:start,24:cross,
  28:cross,33:cross,38:cross` goes notice (9 s) → save prompt (15 s) → title → brightness (21 s) → main menu (27 s) →
  character select (33 s) → difficulty select, then the opening movie is skipped (`[mksm-movie] skipped OPENING.SFD` at
  ~40 s) and the picture freezes on a stretched copy of the last menu frame (the display switched to a 512-wide buffer,
  `fbp=32 fbw=8`, that nothing draws into).
- **The wait after difficulty select is a DMAC status poll.** With the thread watch, the main thread spins at priority
  `0x18` in `0x40E4D8` (called from `0x3D5080` ← `0x3935F0` ← `0x395500` ← `0x395200` ← `0x3350F0`, the level loader):
  the routine clears `D_STAT` CIS0 (`sw 1, 0x1000E010`), sets `D_PCR`, points `D0_MADR`/`D0_QWC` at
  `0x49A9D0`-`0x49B430` (the start of `.vutext`: a VIF0 packet with two `MPG`s, 256 and 74 instructions, the game's
  **VU0 microprogram**), starts channel 0 with `D0_CHCR = 0x100` and loops until `D_STAT` bit 0 (CIS0) reads 1. The
  runtime ran the VIF0 transfer (the MPG lands in VU0 micro memory) but dropped every `D_STAT` store and read it as 0,
  so the loop never ended. God of War only cleared the bits after its scratchpad copies, which is why no status was kept.
  Fix: `patches/ps2recomp-dmac-stat.patch`: `D_STAT` keeps the channel status: `queueCompletedDmacCause` sets CISn when a
  transfer completes (still at the store that starts it), a write of 1 clears the status bits (0-9, 13-15) and toggles
  the mask bits (16-25, 29, 30), as on the DMAC. Regression test "D_STAT reports a finished VIF0 transfer and clears on
  write-one" (fails without the patch). **Verified in the game:** the main thread leaves `0x40E4D8`, loads for a few
  seconds (`0x3404BC`, callers `0x343518` ← `0x2592A0` ← `0x2596DC`) and reaches the level loader's wait
  (`0x3794B0`, which loops while bit 0 of `0x755DB8` is set, running `0x378D08` each pass). After the upload the game
  calls its VU0 microprogram with `vcallms` at 16 sites (`0x0`, `0x8`, `0x38`, `0x60`...; the runtime's VU0
  interpreter runs them synchronously) and polls `VPU_STAT` (`cfc2 $vi29`) at four.
- **Then the sync path spins on VU0.** The next stop is `0x24E910` (called from `0x27D6D0`, which sends one GS
  register through VIF1 synchronously, here `TEST_1 = 0x30000` and `ALPHA_1 = 0x44` from `0x22E6B8`, the GS setup of
  the fight): it waits for VIF1 idle (`VIF1_STAT & 0x1F000003`), then for VU0 idle with **`bc2t .`** (`0x24EA10`,
  the only COP2 branch in the game), then for the GIF (`GIF_STAT & 0xC00`). The recompiler translated `bc2t`/`bc2f`
  as a test of bit 0 of the VU0 **STATUS** register (the Z flag of the last VU0 result) instead of the COP2 condition,
  which is VU0 busy (`VPU_STAT` bit 0, as the runtime's own EE reference `eeref.cpp` says); any zero result left the
  loop spinning forever. Fix: `patches/ps2recomp-bc2-vpu-stat.patch` (`control_flow_emitter.cpp` tests
  `ctx->vu0_vpu_stat`; recompiler test "COP2 branches test VU0 busy, not the STATUS flags"). It changes the generated
  code, so it needs a full `2_build.cmd`. **Verified in the game:** the loader goes on: with `PS2X_TRACE_RPC=80000701`
  the EE uploads the level's sound banks through libsdr (58 `rSdVoiceTrans` of `0x8000` bytes to SPU2 RAM `0xD0240`
  onwards, each followed by `rSdVoiceTransStatus`), then the main thread stops running at `0x17C218` (ra `0x30FD48`,
  callers `0x2F4534` ← `0x1D3538` ← `0x1D5264` ← `0x379FB8` ← `0x37A16C`), the scheduler shows no running thread
  and the GS counters stay still: the next wait, not yet analysed. The picture is still the stretched menu frame.
- **IOP clock.** `MKSM_IOP_DRIFT=1` shows the IOP granted and advancing 184.32 M cycles per 5.00 s of wall time
  (36.864 MHz exactly), so the slowed-down select sound is not a slow IOP clock. SNDFI's tick is a 200 Hz hardware
  timer (`AllocHardTimer(SYSCLOCK, 32 bits, prescale 256)`, compare 720, handler `+0xA3F4` that wakes the tick thread
  and skips the tick if the previous one was not consumed); whether ticks are lost to scheduling, or the pitch is
  wrong, is the next thing to measure (`MKSM_SND_CALLLOG=1` logs the pitch and volume writes).
- Seen by the user during the scripted run: the menu select sound plays but sounds slowed down or wrong, the text
  flickers, and the music crackles. The sound engine's pitch model is the SPU2's (4.12, `0x1000` = 48 kHz), so a wrong
  select sound most likely plays data that is not its sample: SNDF's bank upload is skipped (the capacity checks of the
  evening entry of October 28), and its voices point at whatever is in sound RAM, such as the ADX stream buffers.

### 2026-11-03 (evening) — the NTSC-U executable replaces the PAL one

Decision: the port targets the NTSC-U release **SLUS-21087** from now on; PAL support is dropped (git history keeps it).
Everything found on PAL is engine-level and carries over; only the addresses change.

- **Disc.** `S-MKSM.ISO` (3.49 GB, single layer, 68 files): `SYSTEM.CNF` boots `cdrom0:\SLUS_210.87;1`, `VER = 1.02`,
  `VMODE = NTSC`. Same `GAMEDATA.WAD`-`.WAJ` volumes, `FILELIST.DIR`, `WADCRC.BIN`, `IOP\` (same nine modules) and
  `FRONT\MOVIES` (with `BLITZ.SFD` and `GAUNTLET.SFD`, which the PAL disc lacked), plus `MODULES\` (the IRX again and
  `MTAPMAN.IRX`), `JARVOS.ELF`, `CLASSIC\MK2.SR` and `MAT2.SR` (the classic MK II), and `SURREAL\ENGLISH\LEVELS_STRTH`
  (one level as loose `.ADU/.CDU/.LDU/...` files, probably a development leftover). Extracted into `game\` with
  `tools\iso_extract.py --layout`.
- **Executable.** `SLUS_210.87`: entry **`0x0011C070`**, `.text` `0x00100000`-`0x0049A708`, `.vutext` `0x0049A710`,
  `.data` `0x004A6180`, `.bss` `0x005B2080`-`0x00780898`, no symbols. The analyzer finds 13,685 functions (PAL: 13,627);
  with `tools/funcmap_fixup.py` (3 gaps, 908 jump targets) the map has 13,688 and 1,228 entry points; 150 stubs after
  removing the software double libm and the EE's libsdr, as on PAL.
- **Addresses.** `tools/port_addresses.py` ports an address from one build to the other by fingerprinting its function
  (instruction words with the J/JAL targets and the address immediates masked) and reading the data addresses the
  matched function builds. The key PAL addresses of this log map to:

  | What | PAL | NTSC-U |
  |---|---|---|
  | Entry | `0x11C038` | `0x11C070` |
  | Boot function | `0x2131E8` | `0x212F70` |
  | Movie player (skipped) | `0x270038` | `0x26FD80` |
  | Frame loop / flip | `0x274898` / `0x275870` | `0x2745E0` / `0x2755B8` |
  | libpkt: GifPkCloseGifTag, GifPkAddGsAD, Vif1PkOpenDirectCode, Vif1PkCloseGifTag, Vif1PkAddGsAD | `0x384F10`, `0x384FE0`, `0x385388`, `0x385428`, `0x385610` | `0x384C60`, `0x384D30`, `0x3850D8`, `0x385178`, `0x385360` |
  | sceCdLayerSearchFile / sceCdRead / scePadRead | `0x385F90` / `0x467BF0` / `0x206C48` | `0x385CE0` / `0x467940` / `0x2069D0` |
  | Pad reader / pad object | `0x212808` / `0x652200` | `0x212590` / `0x651F00` |
  | Widescreen scale float / its setter | `0x4F4E08` / `0x2268B0` | `0x4F4B38` / `0x226620` |
  | File table | `0x4D3D00` | `0x4D3A10` |
  | CRI thread setup / ADX server | `0x414FF0` / `0x319CB8` | `0x414D40` / `0x3199E0` |
  | Loading flag / its loop | `0x755DB8` / `0x3794B0` | `0x755AB8` / `0x3791D8` |
  | VU0 upload wait (D_STAT) | `0x40E4D8` | `0x40E228` |
  | Sync path (bc2t) / GS register send | `0x24E910` / `0x27D6D0` | `0x24E680` / `0x27D418` |
  | Fight GS setup | `0x22E6B8` | `0x22E428` |
  | Last wait seen on PAL | `0x17C218` | `0x17C2B8` |
  | Sample upload through libsdr / SNDF client / bank DMA | `0x463BF8` / `0x4629F0` / `0x462FE8` | `0x463948` / `0x462740` / `0x462D38` |
  | libsdr (recompiled) | `0x382C70`-`0x383130` | `0x3829C0`-`0x382E90` |

  The earlier entries of this log keep their PAL addresses.
- **Repository.** `config/` regenerated for `SLUS_210.87`; `src/mksm_overrides.cpp` registered for `SLUS_210.87`, entry
  `0x11C070`, with the addresses above; `scripts/common.ps1` looks for `game\SLUS_210.87`; the status map's library
  boundaries ported by name; the God of War state word the optional GS replay capture waited for (`0x29E560`) removed.
  The PAL disc files were moved to `game_pal\` (git-ignored) and will be deleted once the NTSC port is at parity.
- **First NTSC runs.** The boot, the menus, the sound and the character and difficulty select match the PAL build at
  the first attempt (`scripts_scripted_run.cmd`, default pad script). The level loader then called `0x17C2B8`
  through a vtable: a function the map had merged into `0x17C0E8` because the return of the function before it is
  followed by a padding `nop` (the same thing stopped the PAL build at `0x17C218`; the thread watch showed it as a
  thread that no longer ran). `tools/funcmap_fixup.py` now skips padding nops after a return (651 more entry points,
  test added). **Verified:** the level loads, the fight HUD ("LIU KANG", EXP, health bar) is drawn at 60 fps.
- **The 3D world is white (open).** With `MKSM_GS_HW_DUMPAT` every batch of a level frame is a sprite (the full-screen
  passes, a 320x128 blur chain into `fbp=140`, the particle sprites, the HUD) or the 1024x256 backdrop strip; no
  triangle geometry reaches the renderer. The 180 draws per frame the renderer refuses (`skippedPsm`, now logged with
  `[gs-hw-skip] fpsm`) are only the game copying its depth buffer into a PSMZ24 target in 32x32 tiles. The VU1 runs
  (`MKSM_VIF_LAUNCH_HIST=1`, new): per frame 32 launches of the program at `0x0` (2-vertex particle packets, XGKICK
  at `0x90`/`0xE8`/`0x138`/`0x148`), one launch of the mesh program at `0x790` and two `MSCNT` continuations, and
  about 25 VIF commands: the EE submits almost no mesh work, so the world is culled or never queued on the EE side.
  The VU0 microprograms (`MKSM_VU0_DIAG=1`, new) run `0x8`, `0x4B8` and `0x588` about 180 times a frame without hitting
  the cycle budget; `MKSM_XGKICK_IMMEDIATE=1`, `MKSM_VU1_COLAS=1` and `PS2X_VU1_LEGACY=1` change nothing; the disc keeps
  streaming during the level. The user sees the characters' place as a void.
- **The sound effects played 17 semitones low (fixed, pending the rebuild).** `MKSM_SND_CALLLOG=1` shows every menu
  effect keyed with pitch `0x02B4` (a third of a 22 kHz sample's `0x758`). SNDF computes its pitch in cents and
  subtracts a base the game sends in its init command; the base read from IOP memory (`MKSM_WATCH_IOP_WORDS`, new)
  was 1731 = 1200 / ln 2, what `1200 * log(x) / log(2)` gives when `log(x)` returns its argument (1.0). The game's
  init (`0x462918`) calls the **software double `log`** (`0x470B38`), which the generated code still routed to the
  runtime's float handler: the recompiler stubs any function whose *name* is a runtime handler, whatever the stubs
  list says (`ps2_recompiler.cpp isStubFunction` → `isStubName`), so removing `floor`/`sqrt`/`log` from the list
  never had an effect, on PAL either. The three are now `libm_dp_*` in `config/funcmap.csv` (like `libsdr_*`), and
  `tools/ci/validate_config.py` rejects any map name that has a runtime handler without being in the stubs list.
- **Sound effects: verified.** After the rebuild the menu effects key with pitch `0x0759` and the SNDF base is 0;
  the user confirms the effects sound right. The remaining crackle is in the **ADX music**, to be looked at after
  the 3D rendering (user's priority).
- **Every object was culled: the recompiler's VCLIPw set the clip flags in the wrong order (fixed, patch pending the
  full build).** The faithful DMA capture (`MKSM_DMA_CHAINDUMP`, decoded by `tools/gs/vifdump.py`) showed an intro
  frame as 284 KB of 2D GIF data (54 sprites, texture uploads) plus a single mesh launch with three small vertex
  chunks; the per-frame call histogram (`MKSM_CALL_HIST=55:0.05`, 3 frames) pointed at `0x22C838`, called 290 times
  a frame: it transforms a bounding sphere, runs `vclipw` twice and returns 0 (invisible) if `cfc2 vi18 & 0xAAA` is
  not zero, i.e. if the sphere is behind any of the six frustum planes (the "< -w" bits of the last two clips;
  `& 0x555 == 0x555` means fully inside). `vu_translator.cpp` emitted bit 0 for "< -w" and bit 1 for "> +w", the
  reverse of the hardware order (VU manual, PCSX2 `_vuCLIP`), so every object comfortably inside the frustum
  tested as behind a plane and only huge meshes crossing all planes survived. `patches/ps2recomp-vu0-clip-flags.patch`
  swaps the bits (test in `code_generator_tests.cpp`); it is registered in `scripts/build.ps1` but the executable
  tested so far has the five generated clip functions (`0x213980`, `0x22C630`, `0x22C838`, `0x24D178`, `0x268218`)
  hand-patched: with it the intro submits thousands of VU1 launches a frame (`0x6F0`, `0xB40`, `0xE50`, `0xEF0`,
  28000 MSCNT per 5 s) and the renderer draws textured triangle strips into `fbp=0x70` (`MKSM_GS_HW_DUMPAT=70`:
  three passes per object, PSMT8 128x128 textures plus a 512x256 one, dark vertex colors). **The screen is still
  white**: the display is filled by untextured full-screen sprites (`rgba 128,128,128,0`, `abe=1`, 2940 in a
  fog-diag window, `fb=0`) and the renderer reports `unsupportedBlend` about 37,000 times per 5 s, so the final
  composite / flash passes are the next suspect. Batch dump: `logs/scripted-ntsc-clipfix-dump/run_err.log`.
- **Ruled out on the way.** The DMA chain code handles every tag, TTE and the SPR bit; the disc reads of the level
  are healthy (`MKSM_CD_READ_TRACE=200000`: 1 MB chunks, `ret=1`, non-zero data); `fmod`/`rint` are software double
  but not stubbed (no runtime handler); the character select is 2D (no VU1 work at all in a 0.1 s window).
- **Tools.** `tools/ee_disasm.py` (capstone) disassembles any function or range of the ELF with map names and the
  COP2/MMI instructions decoded; `tools/gs/vifdump.py` decodes a chain dump (VIFcodes, launches, DIRECT GIF packets).
- **Session end (23:30).** The user asked for a frame counter (pending) and then to stop and rethink the approach.

## Work folders

Everything lives inside `D:\Projects\Recomps\mksm-ps2-recomp`, never in the root of a drive:

- the repository itself.
- `mksm-work\PS2Recomp\` — PS2Recomp clone at the pinned commit with the patches applied. It is git-ignored, so it
  is never pushed. `scripts\common.ps1` (`Get-MksmWorkDir`) points there by default; `MKSM_WORK` overrides it.

Any other repository that is needed must also be cloned inside `D:\Projects\Recomps`.

## Environment variables

| Variable | Effect |
|---|---|
| `MKSM_ELF` | Path of the game ELF (default `game\SLES_535.24`) |
| `MKSM_ISO` | Read this disc image instead of the extracted files |
| `MKSM_WORK` | Work folder (default `mksm-work`) |
| `MKSM_WINDOW=<w>x<h>` | Window size; `scripts/run.ps1` sets `720x540` unless it is already defined (without it the runtime starts in borderless fullscreen) |
| `MKSM_THREAD_WATCH=<s>` | Every `<s>` seconds, print the EE threads (status, wait, pc, ra), a `[mksm-audio]` line (frames the audio device asked for and frames the sound engine had) and a `[mksm-gs]` line with the GS counters and display registers |
| `MKSM_THREAD_WATCH_RING=<n>` | With `MKSM_THREAD_WATCH`, the tick that dumps the last 256 inter-function branches (default 3) |
| `MKSM_MOVIES=1` | Play the movies instead of skipping them (they need the IPU, which the runtime does not emulate yet) |
| `MKSM_WATCH_WORDS=<addr>:<n>,...` | With `MKSM_THREAD_WATCH`, also print `<n>` guest RAM words at each `<addr>` (hex) |
| `MKSM_WATCH_IOP_WORDS=<addr>:<n>,...` | With `MKSM_THREAD_WATCH`, also print `<n>` IOP RAM words at each `<addr>` (hex): a driver's state, a bank uploaded to the IOP |
| `MKSM_VIF_LAUNCH_HIST=1` | Every 5 s, the VU1 microprograms launched (start pc -> launches, `0xFFFF` = MSCNT) and the first 96 microprogram uploads; with `MKSM_VIF_DIAG=1`, `MKSM_VIF_LAUNCH_FROM=<n>` dumps 40 launches from the n-th |
| `MKSM_VU0_DIAG=1` | Every 5 s, the VU0 microprograms run (start pc -> runs / runs that hit the cycle budget) |
| `MKSM_DMA_CHAINDUMP=<file>:<from s>:<len s>` | Appends every DMA transfer delivered in the window as the VIF/GIF receives it (`[u32 path][u32 source][u32 bytes][data]`); decode with `python tools/gs/vifdump.py <file>` |
| `MKSM_DMA_BLOCKDUMP_MAXQWC=<n>` | Raises the 2000-qword limit of `MKSM_DMA_BLOCKDUMP` (the game sends 16384-qword display lists) |
| `MKSM_CALL_HIST=<from s>:<len s>` | Histogram of the function calls made in the window (`[mksm-call-hist] pc:calls`, 400 at most) |
| `MKSM_SND_CALLLOG=1` | The sound engine's voice volume and pitch writes that change a value, and the key-on/off switches |
| `MKSM_CD_READ_TRACE=<n>` | Log the first `<n>` `sceCdRead` calls: sector, count, destination, caller, result, non-zero bytes |
| `MKSM_PAD_SCRIPT=<s>:<button>[+<button>],...` | Hold those pad buttons for 0.25 s at `<s>` seconds after start, on top of the real pad (`start`, `select`, `cross`, `circle`, `square`, `triangle`, `up`, `down`, `left`, `right`, `l1`, `r1`, `l2`, `r2`), e.g. `12:start,15:cross` |
| `MKSM_ASPECT=16:9` | Show the picture at 16:9 instead of the game's 4:3 |
| `MKSM_DEINTERLACE=0` | Show the fields of field rendering one after the other instead of blended (the picture then shakes a line every frame) |
| `MKSM_IOP_MODULOS` | Folder of dumped IRX modules for the native IOP build |
| `MKSM_HOST_SHOT=<n>[,<n>...]` | Save the window at those host frames (about 60 per second) as `host_shot_<n>.png` next to the executable; with `MKSM_PAD_SCRIPT`, one capture per screen of a scripted run |

## Next steps

1. Gameplay: verify the D_STAT fix in the game and follow the level load (VU0 microprograms, the first 3D frame).
2. Audio: the SNDF bank upload (the capacity checks), the crackle (ADMA read-ahead).
3. Translate the Spanish runtime messages (`[SPU2] salida de audio ...`, `[stack] pilas ...`) and comments.
4. Movies: IPU emulation or a native Sofdec player (SFD: MPEG-2 video and ADX audio).
5. Make the VBlank rate configurable (PAL 50 Hz).

## Language

The repository is in English: documentation, scripts, CI helpers and file names. Remaining Spanish text lives only
inside the bodies of `patches/*.patch`, in some code comments and in environment variable names those patches define
(for example `MKSM_VU1_MICROCODIGO`); they are translated whenever a patch is regenerated.
