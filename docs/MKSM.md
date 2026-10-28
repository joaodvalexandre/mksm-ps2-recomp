# Mortal Kombat: Shaolin Monks — port log

Research log of the port of *Mortal Kombat: Shaolin Monks* (MKSM, PAL `SLES-53524`). Current goal: **get the game
to boot and run smoothly on Windows**; everything else (improvements, higher frame rates, etc.) comes later.

_Last updated: October 27, 2026_

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
| Audio | **CRI ADX** (`CRI_ADXI.IRX`) + `SNDFI.IRX` over `LIBSD`/`SDRDRV` | The native SPU2 engine is driven by LIBSD calls; ADX should work on top, to be verified |
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
   jump targets that are not function starts added to `entry_points` (567 in total).

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

`scripts\4_diagnose.cmd` runs the game for 30 s with the GS and DMA diagnostics on and moves everything to
`logs\diag-<date>-<time>\`: `run.log`, `run_err.log`, PNG captures (display, render targets after each batch, host
window) and a `summary.txt` with the last thread snapshot, GS counters, render targets and likely problems.

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
| `MKSM_THREAD_WATCH=<s>` | Every `<s>` seconds, print the EE threads (status, wait, pc, ra) and, for now, a `[mksm-gs]` line with the GS counters and display registers |
| `MKSM_MOVIES=1` | Play the movies instead of skipping them (they need the IPU, which the runtime does not emulate yet) |
| `MKSM_WATCH_WORDS=<addr>:<n>,...` | With `MKSM_THREAD_WATCH`, also print `<n>` guest RAM words at each `<addr>` (hex) |
| `MKSM_CD_READ_TRACE=<n>` | Log the first `<n>` `sceCdRead` calls: sector, count, destination, caller, result, non-zero bytes |
| `MKSM_LOADIMAGE_TRACE=<n>` | Log the first `<n>` `sceGsExecLoadImage` uploads: DBP, DBW, PSM, position, size, source, caller |
| `MKSM_PAD_SCRIPT=<s>:<button>[+<button>],...` | Hold those pad buttons for 0.25 s at `<s>` seconds after start, on top of the real pad (`start`, `select`, `cross`, `circle`, `square`, `triangle`, `up`, `down`, `left`, `right`, `l1`, `r1`, `l2`, `r2`), e.g. `12:start,15:cross` |
| `MKSM_ASPECT=16:9` | Show the picture at 16:9 instead of the game's 4:3 |
| `MKSM_DEINTERLACE=0` | Show the fields of field rendering one after the other instead of blended (the picture then shakes a line every frame) |
| `MKSM_IOP_MODULOS` | Folder of dumped IRX modules for the native IOP build |

## Next steps

1. Audio: nothing is heard yet (CRI ADX and the `SNDF` driver run in the IOP interpreter over the SPU2 emulation).
2. Gameplay: Single Player and beyond.
3. Translate the Spanish runtime messages (`[SPU2] salida de audio ...`, `[stack] pilas ...`) and comments.
4. Movies: IPU emulation or a native Sofdec player (SFD: MPEG-2 video and ADX audio).
5. Make the VBlank rate configurable (PAL 50 Hz).

## Language

The repository is in English: documentation, scripts, CI helpers and file names. Remaining Spanish text lives only
inside the bodies of `patches/*.patch`, in some code comments and in environment variable names those patches define
(for example `MKSM_VU1_MICROCODIGO`); they are translated whenever a patch is regenerated.
