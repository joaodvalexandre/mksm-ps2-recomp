# Guide for agents and contributors

Port of *Mortal Kombat: Shaolin Monks* (PS2, PAL `SLES-53524`) by static recompilation with upstream
[PS2Recomp](https://github.com/ran-j/PS2Recomp), used as a pinned submodule. The repository holds only what is
specific to the game: its recompiler configuration, its override module, a few small recompiler patches, the
scripts and the research log ([`docs/MKSM.md`](docs/MKSM.md)). No game files and no generated code are committed.

## Layout

| Path | What |
|---|---|
| `PS2Recomp/` | upstream PS2Recomp, submodule pinned at the commit in `scripts/common.ps1` (`$PS2RecompCommit`) |
| `patches/` | small patches applied on the submodule at build time, in name order (`NNNN-topic.patch`), each with a test in `ps2xTest` |
| `config/recomp.toml` | the recompiler configuration for `SLES_535.24` (`@ELF@`, `@MAP@`, `@OUT@` are filled by the build) |
| `src/` | the game override module (`mksm_overrides.cpp`), copied next to the generated code |
| `scripts/` | `build.cmd` (everything), `run.cmd` (run with a time limit) |
| `game/` | the extracted PAL disc (git-ignored): `SLES_535.24`, `SYSTEM.CNF`, `IOP\`, the data files |
| `docs/MKSM.md` | research log: what was tried, with what result, and the next steps |

## Rules

- **PS2Recomp stays upstream.** Game-specific behavior goes to `src/mksm_overrides.cpp` (upstream's game override
  hooks: `PS2_REGISTER_GAME_OVERRIDE`, `bindAddressHandler`, `replaceFunction`). A change to the recompiler or the
  runtime is a patch in `patches/`, as small as possible, marked `// MKSM:` in the code, with a regression test, so
  that it can be submitted upstream. Prefer fixing upstream over working around it in the override module.
- **Editing or adding a patch**: `git -C PS2Recomp checkout -f <pinned>` and `git -C PS2Recomp clean -fdq -e out`,
  apply the earlier patches with `git -C PS2Recomp apply --ignore-whitespace`, make the change, then
  `git -C PS2Recomp diff > patches/NNNN-topic.patch`. `scripts\build.cmd` re-applies all of them from the pinned
  tree on every build.
- **Diagnostics** are optional, enabled by an environment variable (`MKSM_*`), tagged `[mksm-...]` in the log,
  documented in `docs/MKSM.md` and removed once the problem is solved.
- **Research log**: add what was tried and the result to `docs/MKSM.md`; a clean log does not prove the game is
  playable, record exactly what was verified.
- English only, in code, comments, commits and documentation.
- Never commit game files (ISO, ELF, IRX, `.WAD`, `.SFD`), generated code, memory captures or microcode dumps.
  `game/` and `PS2Recomp/out/` are ignored.

## Building and running (Windows)

1. Visual Studio 2022 or later with the C++ tools, CMake and Ninja (the Visual Studio installer provides them).
2. Extract the PAL disc into `game\` (7-Zip: `7z x "<image>.iso" -ogame`).
3. `scripts\build.cmd` (first run: builds the tools, generates the C++ from the ELF, builds the runner;
   `-ToolsOnly`, `-NoGenerate`, `-Tests` as needed). Logs in `logs\build.log`.
4. `scripts\run.cmd [seconds]`; the output is in `logs\run.log` and `logs\run_err.log`.
