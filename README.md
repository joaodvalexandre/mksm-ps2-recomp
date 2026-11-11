# Mortal Kombat: Shaolin Monks (PAL) — PS2 static recompilation

An attempt to run *Mortal Kombat: Shaolin Monks* (PlayStation 2, PAL release `SLES-53524`) natively on Windows by
static recompilation of its MIPS executable to C++ with [PS2Recomp](https://github.com/ran-j/PS2Recomp).

You need your own copy of the game. No game files, and no code generated from them, are in this repository.

## Status (2026-11-11)

| Area | State |
|---|---|
| Boot, menus, front end | Works at 60 fps up to the difficulty select, in the game's own NTSC mode, with the GS drawn on the GPU (paraLLEl-GS, Vulkan) |
| Picture | Sharp, no interlacing shake; splash and loading screens fill the 4:3 picture |
| First level | Loads; the opening cutscene (Goro's Lair) plays with the right camera, characters and lighting |
| 3D speed | 2 to 8 fps: four fifths of the time goes to upstream's cycle-exact VU1 interpreter |
| Gameplay | Not checked yet past the opening cutscene |
| Sound | None yet |
| Movies | Skipped |

**In progress:**

- **VU1 translator** (branch `vu1-native`): the game's VU1 programs translated ahead of time to C++ at build time, from
  your own ELF, with the interpreter kept as the fallback and as the reference the translation is checked against.
  The first stage is done (decoder, runtime interface, comparison harness against the interpreter on captured
  launches). It is not used by the game yet.
- **Sound**: a native SPU2 core and libsd layer for the IOP, tested outside the game; the audio output and the module
  arguments remain.

The [research log](docs/MKSM.md) records what was tried, found and verified. The previous attempt (NTSC-U release on a
heavily patched runtime) is kept in the branch `archive/ntsc-waffle-runtime`.

## How it works

- `PS2Recomp` (submodule, pinned) parses the ELF, translates every function to C++ and provides the runtime (EE
  kernel, GS, VU, IOP emulation for the disc's IRX modules, pad, file system).
- `config/recomp.toml` tells the recompiler which functions are runtime stubs, entry points and patches for this ELF.
- `src/mksm_overrides.cpp` is the game override module: address bindings, replacements and diagnostics for this build.
- `patches/` holds the recompiler and runtime fixes the game needed (32 so far), each with a test, meant to go upstream.

## Building and running

See [AGENTS.md](AGENTS.md): extract the disc into `game\`, run `scripts\build.cmd`, then `scripts\run.cmd`. A Vulkan GPU
is needed.

## License

GPL, see [LICENSE](LICENSE). PS2Recomp has its own license in the submodule.
