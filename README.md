# Mortal Kombat: Shaolin Monks (PAL) — PS2 static recompilation

An attempt to run *Mortal Kombat: Shaolin Monks* (PlayStation 2, PAL release `SLES-53524`) natively on Windows by
static recompilation of its MIPS executable to C++ with [PS2Recomp](https://github.com/ran-j/PS2Recomp).

**Status: boots to its first screen, with text.** The project restarted on 2026-11-03 on upstream PS2Recomp; on 2026-11-04
the PAL build boots, runs its loaders and threads and shows its first screen; on 2026-11-05 the screen shows its text (the autosave notice, waiting for the pad; movies skipped). The previous attempt (NTSC-U
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
