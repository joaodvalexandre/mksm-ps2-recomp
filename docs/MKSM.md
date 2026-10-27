# Mortal Kombat: Shaolin Monks — port log

This repository starts from a *God of War* port (base: PS2Recomp runtime with the patch stack in `patches/`).
This document records what is specific to *Mortal Kombat: Shaolin Monks* (MKSM). Current goal: **get the game
to boot and run smoothly on Windows**; everything else (improvements, 120 Hz, etc.) comes later.

_Last updated: October 27, 2026_

## The disc

Image used: `SLES_535.24.Mortal Kombat Shaolin Monks.iso` (PAL, dual-layer DVD, 3.36 GB), placed in `game/`
(ignored by git). `SYSTEM.CNF`: `BOOT2 = cdrom0:\SLES_535.24;1`, `VER = 1.02`, `VMODE = PAL`.

Extracting what is needed to recompile (without the large data):

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

## The executable

- MIPS R5900 ELF, **entry `0x0011C038`**, a single `PT_LOAD` segment at `0x00100000` (`filesz 0x4B2294`,
  `memsz 0x680B98`).
- `.text` `0x00100000–0x0049A9C8` (~3.7 MB), `.vutext` `0x0049A9D0` (0xB630 bytes), `.data` `0x004A6400`,
  `.bss` `0x005B2380`.
- **No symbol table**: there is no `.symtab`; `.mdebug` is empty. The function map has to be generated
  (PS2Recomp's analyzer) instead of imported from Ghidra as in God of War.
- Middleware visible in the strings: **CRI ADX** (`ADXCS/ADXF/ADXT`, build June 2005) and **Sofdec/SFD** for audio
  and movies; Sony SDK: `sceGs*`, `sceCd*`, `sceMc*` (classic libmc), `sceSd*`, `sceMpeg*`.

## Differences from God of War that affect the runtime

| Topic | God of War | MKSM | Consequence |
|---|---|---|---|
| Region/video | NTSC-U, 60 Hz | **PAL**, 50 Hz | The `vblank-ntsc` patch fixes 59.94 Hz; the field rate has to become configurable per game |
| Pad | `libpad2` (DS2U) | `PADMAN.IRX` + `SIO2MAN.IRX` (classic libpad) | The base runtime does not model SIO2; `scePad*` needs an HLE on the EE |
| Memory card | `mc2_d` + `dbcman` | `MCMAN.IRX` + `MCSERV.IRX` (classic libmc) | HLE of `sceMc*` on the EE (or an SIO2 model for MCMAN) |
| Audio | 989snd + smpd (native HLE) | **CRI ADX** (`CRI_ADXI.IRX`) + `SNDFI.IRX` over `LIBSD`/`SDRDRV` | The native SPU2 engine is driven by LIBSD calls; ADX should work on top, to be verified |
| Movies | PSS (MPEG-2 through the IPU) | **SFD** (Sofdec, MPEG-2 through the IPU) | Same hardware (IPU); the demultiplexer has to be checked |
| Disc | `PART1/2.PAK` + TOC | 7 `GAMEDATA.WA*` + `FILELIST.DIR` | The extractor's `.disc_layout` works as is (dual layer) |
| Overrides | `src/gow_overrides.cpp` (God of War addresses) | to be written | No God of War address is useful: the ELF is different |

The runtime base's tailor-made IOP services (`createGow989SndService`, `mc2_d_irx`, `sio2d_irx`, `dbcman`) are only
activated by module name, so with other IRX modules they should stay inert and the IOP will run the modules in the
R3000 interpreter. This is a hypothesis: it will be confirmed on first boot.

## Work folders

Everything lives inside `D:\Projects\Recomps`, never in the root of a drive:

- `mksm-ps2-recomp\` — this repository (and its git worktrees under `.claude\worktrees\`).
- `mksm-work\PS2Recomp\` — PS2Recomp clone at the pinned commit with the 40 patches applied (git-ignored, outside the
  repository). `scripts\common.ps1` (`Get-GowWorkDir`) points there by default; `GOW_WORK` overrides it.

Any other repository that is needed must also be cloned inside `D:\Projects\Recomps`.

## Work route

1. Build `ps2_analyzer` and `ps2_recomp` with the patched runtime (`mksm-work`).
2. Generate the TOML and the function map of `SLES_535.24` with the analyzer.
3. Replace `config/funcmap.csv` / `config/recomp.template.toml` with the MKSM ones; identify the Sony libc/SDK
   *stubs* (the equivalents of `sceSif*`, `sceCd*`, `sceDma*`, `sceMpeg*`, `sceIpu*`).
4. Register a `PS2_REGISTER_GAME_OVERRIDE("Mortal Kombat: Shaolin Monks (SLES-53524)", "SLES_535.24",
   0x0011C038u, ...)` with only what is essential.
5. Rebuild, boot and advance through the log up to the first movie/menu; then pad, audio and gameplay.

## Status

- [x] Image located and ELF/IOP extracted.
- [x] PS2Recomp cloned at the pinned commit with the 40 patches applied (`D:\Projects\Recomps\mksm-work\PS2Recomp`).
- [x] Analyzer and recompiler built once with VS 2022 (the build output was deleted when the clone was moved into
      the Recomps folder, so they need to be rebuilt).
- [ ] MKSM function map and configuration.
- [ ] First boot.

## Language

The repository is fully in English: documentation, scripts, CI helpers and file names. The Spanish and Portuguese
documents and READMEs inherited from the God of War base were removed (they are still in the git history).
Remaining Spanish text lives only inside the bodies of `patches/*.patch` and `patches/anteriores/`, and in some
code comments and environment variable names that those patches define (for example `GOW_VU1_MICROCODIGO`); they
are translated whenever a patch is regenerated.
