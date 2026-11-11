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
| `config/vu1_images.txt` | the game's VU1 program images for `tools/vu1gen`: ELF addresses, hashes, entry points (addresses only) |
| `tools/vu1gen/` | ahead-of-time translator of the game's VU1 programs: generator, runtime helpers, `vu1difftest` against upstream's interpreter. `scripts\build.cmd` translates the images from the ELF during the build (into the build folder, never committed) and links them into the runner; state, commands and measurements in its `PROGRESS.md` |

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
3. `scripts\build.cmd`: the **quick profile** (default; RelWithDebInfo, debug info, the generated game code at `/O1`
   and the runtime at `/O2` (`scripts/quick-profile.cmake`), no whole-program optimization, 8 jobs) builds the
   tools, generates the C++ from the ELF when the configuration or the ELF changed, and builds the runner into
   `PS2Recomp\out\build-quick`. `scripts\build.cmd -Release` builds the
   optimized profile into `out\build-release` (still without LTO: `patches/0002-lto-option.patch`). Other
   switches: `-Jobs n`, `-ToolsOnly`, `-Regenerate`, `-Reset` (check out and patch the submodule again), `-Tests`.
   Only what changed is rebuilt: the submodule is left alone while it is the pinned commit plus the patches, and
   regenerated functions are copied only when their content differs. Logs in `logs\build.log`. The GS is drawn on
   the GPU by upstream's paraLLEl-GS backend (Vulkan, `patches/0026`); the build keeps a paraLLEl-GS checkout in
   `PS2Recomp\out\parallel-gs` (cloned on the first build, about 216 MB).
4. `scripts\run.cmd [seconds]` runs the quick build (`-Release` for the other); the game's output goes to
   `logs\run.log` and `logs\run_err.log`. Set `MKSM_ISO` to the disc image path (kept outside the repository)
   for the sector reads the game does outside the extracted files; the image is never part of the build.
   `scripts\screenshot.ps1` saves a PNG of the game window.
   The window title shows the displayed frames and their rate (`patches/0003`); `F1` hides and shows upstream's
   runtime debug panel (registers, threads, IOP, RPC history, GS, logs), which starts hidden (`patches/0012`).
   The game's movies do not play yet and are skipped by default (`MKSM_SKIP_MOVIES=0` plays them). Diagnostics
   of the override module (`MKSM_TRACE_*`, `MKSM_PROFILE`, `MKSM_PAD_SCRIPT`) are described in `docs/MKSM.md`.
   The game runs in its own NTSC mode, 60 frames a second (`MKSM_VIDEO=pal` keeps the disc's PAL mode, 50 Hz).
   The VU1 microcode runs translated to C++ (`src/vu1_runtime.cpp`, `tools/vu1gen`): `MKSM_VU1=0` runs upstream's
   interpreter instead, `MKSM_VU1=verify` runs the interpreter and compares the translation with it on every launch
   (`[mksm-vu1]` lines; `MKSM_VU1_DUMP=<folder>` saves the launches that differ for `vu1difftest`).
   The picture is drawn on the GPU (a Vulkan GPU is needed; `PS2X_GS_BACKEND=cpu` draws it on the CPU, much slower),
   one field per vertical blank, and the fields are woven into full height on the CPU as with the CPU backend.
   Options of the GPU backend: `PS2X_GS_SSAA=2|4|8|16` super-samples the drawing, and with 4 or more
   `PS2X_GS_PROGRESSIVE=1` scans out a progressive picture at twice the resolution instead of the weave (depends on
   the game's field order; `PS2X_GS_FIELD_PHASE=flip` swaps it). The picture is 4:3 in a resizable window, scaled with
   bilinear filtering; `MKSM_ASPECT=16:9` (any `<w>:<h>`) or `MKSM_ASPECT=window` widens it and the game's 3D view,
   `MKSM_FILTER=nearest` scales with nearest sampling, `MKSM_DEINTERLACE=0` shows the fields one by one.
   Keyboard (upstream's mapping): X or Space is cross, C circle, Z square, V triangle, Enter start, Tab select,
   the arrows or WASD the directional pad, Q and E L1 and R1, the Shift keys L2 and R2; Esc closes the window. A
   gamepad works too.
