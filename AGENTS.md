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
| `tools/ee_disasm.py` | disassembler for the game ELF (capstone), with the names of the generated code and of the configuration |
| `tools/missing_targets.py` | lists the direct calls and tail jumps whose target has no generated function (run after a build) |

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
3. `scripts\build.cmd`: the **quick profile** (default; RelWithDebInfo, `/O1`, debug info, no whole-program
   optimization, 8 jobs) builds the tools, generates the C++ from the ELF when the configuration or the ELF
   changed, and builds the runner into `PS2Recomp\out\build-quick`. `scripts\build.cmd -Release` builds the
   optimized profile into `out\build-release` (still without LTO: `patches/0002-lto-option.patch`). Other
   switches: `-Jobs n`, `-ToolsOnly`, `-Regenerate`, `-Reset` (check out and patch the submodule again), `-Tests`.
   Only what changed is rebuilt: the submodule is left alone while it is the pinned commit plus the patches, and
   regenerated functions are copied only when their content differs. Logs in `logs\build.log`.
4. `scripts\run.cmd [seconds]` runs the quick build (`-Release` for the other); the game's output goes to
   `logs\run.log` and `logs\run_err.log`. Set `MKSM_ISO` to the disc image path (kept outside the repository)
   for the sector reads the game does outside the extracted files; the image is never part of the build.
   `scripts\screenshot.ps1` saves a PNG of the game window.
   The window title shows the displayed frames and their rate (`patches/0003`); `F1` hides and shows upstream's
   runtime debug panel (registers, threads, IOP, RPC history, GS, logs), which opens over the game by default.
   The game's movies do not play yet and are skipped by default (`MKSM_SKIP_MOVIES=0` plays them). Diagnostics
   of the override module (`MKSM_TRACE_*`, `MKSM_PROFILE`) are described in `docs/MKSM.md`.
