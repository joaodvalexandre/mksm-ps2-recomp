# Mortal Kombat: Shaolin Monks (PAL) — PS2 static recompilation

An attempt to run *Mortal Kombat: Shaolin Monks* (PlayStation 2, PAL release `SLES-53524`) natively on Windows by
static recompilation of its MIPS executable to C++ with [PS2Recomp](https://github.com/ran-j/PS2Recomp).

**Status: the front end runs at 60 fps and the first level is reached; 3D runs at 20 to 36 fps with a picture that is
not right yet, and there is no sound.** The project restarted on 2026-11-03 on upstream PS2Recomp. The PAL build boots,
runs its menus at 60 frames a second in the game's own NTSC mode with the GS drawn on the GPU (2026-11-06), loads the
first level and plays its opening cutscene and the gameplay after it (2026-11-11). The game's VU1 microcode runs
translated to C++ ahead of time and checked against upstream's exact interpreter (`tools/vu1gen`), which took 3D from
2-8 to 20-36 frames a second. Open: artifacts in the 3D environment (the vertex colours of a mesh's later chunks reach
the GS wrong: the VIF1 side, found on 2026-11-12, not fixed yet), the frame rate, and sound (written and tested outside
the game, on the branch `sound`, not merged). Movies are skipped. The previous attempt (NTSC-U
release on a heavily patched runtime) is kept in the branch `archive/ntsc-waffle-runtime`; what it taught about the
game is summarized in the [research log](docs/MKSM.md).

You need your own copy of the game. No game files, and no code generated from them, are in this repository.

## How it works

- `PS2Recomp` (submodule, pinned) parses the ELF, translates every function to C++ and provides the runtime (EE
  kernel, GS, VU, IOP emulation for the disc's IRX modules, pad, file system).
- `config/recomp.toml` tells the recompiler which functions are runtime stubs, entry points and patches for this ELF.
- `src/mksm_overrides.cpp` is the game override module: address bindings and replacements for this build.
- `patches/` holds the few recompiler fixes the game needed, each with a test, meant to go upstream.

## Building and running

See [AGENTS.md](AGENTS.md): extract the disc into `game\`, run `scripts\build.cmd`, then `scripts\run.cmd`.

## License

GPL, see [LICENSE](LICENSE). PS2Recomp has its own license in the submodule.
