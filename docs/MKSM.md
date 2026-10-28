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

Startup. The first two fixes live only in the build tree (`mksm-work\PS2Recomp`) for now: a full `2_build.cmd`
discards them until they become patches with their tests.

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
| `MKSM_THREAD_WATCH=<s>` | Every `<s>` seconds, print the EE threads (status, wait, pc, ra) and, for now, a `[mksm-gs]` line with the GS counters and display registers |
| `MKSM_WATCH_WORDS=<addr>:<n>,...` | With `MKSM_THREAD_WATCH`, also print `<n>` guest RAM words at each `<addr>` (hex) |
| `MKSM_IOP_MODULOS` | Folder of dumped IRX modules for the native IOP build |

## Next steps

1. Confirm the display fix in the game: pink should turn into black (display on, nothing drawn yet) or a picture.
2. Turn the IOP heap and `EndOfHeap` fixes into patches with their tests.
3. First movie (`MIDWAY.SFD`/`ESRB.SFD`) and the main menu; then pad, audio and gameplay.
4. Make the VBlank rate configurable (PAL 50 Hz).

## Language

The repository is in English: documentation, scripts, CI helpers and file names. Remaining Spanish text lives only
inside the bodies of `patches/*.patch`, in some code comments and in environment variable names those patches define
(for example `MKSM_VU1_MICROCODIGO`); they are translated whenever a patch is regenerated.
