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
