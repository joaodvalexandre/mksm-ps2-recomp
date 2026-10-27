# Guide for agents and contributors

Port of *Mortal Kombat: Shaolin Monks* (PS2, PAL `SLES-53524`) by static recompilation with
[PS2Recomp](https://github.com/ran-j/PS2Recomp), on top of a native runtime kept as a patch stack. This repository
does not contain game files or the generated C++: only what is specific to the game. Research log:
[`docs/MKSM.md`](docs/MKSM.md). Architecture and CI: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

Several agents work on `master` at the same time. `git pull` before starting; before pushing, fetch `master` again
and integrate the changes (merge, never rewrite published history).

## Where each change goes

| Change | Where |
|---|---|
| Replacements and diagnostics for game functions | `src/mksm_overrides.cpp` (`applyMksmOverrides`) |
| Changes to the PS2Recomp runtime/IOP | one patch per topic in `patches/` |
| Recompiler functions, stubs and entry points | `config/recomp.template.toml`, `config/funcmap.csv` (after a map change, `python tools/funcmap_fixup.py game/SLES_535.24`) |
| Game-independent tests | `tests/` (runtime tests go inside the patch, in `ps2xTest/`) |
| Port status (README map) | `docs/status/data.toml` |
| Research log | `docs/MKSM.md` |

## PS2Recomp patches

- They are applied on top of the commit pinned in `scripts/common.ps1` (`$PS2RecompCommit`), **in the order of
  `scripts/build.ps1`**, with `git apply --ignore-whitespace`. Each patch is generated against the tree with the
  previous ones already applied.
- **New patch:** add its variable to `scripts/build.ps1` (`$xxxPatch = Join-Path $RepoRoot
  'patches\ps2recomp-xxx.patch'`) and its `Run $git @('apply', ...)` line. CI reads the list from there
  (`tools/ci/patches.py`) and fails if a `patches/*.patch` does not apply.
- Prefer a new patch per topic over extending `ps2recomp-runtime.patch`: changes from other agents conflict less.
- **Editing an existing patch** (`N` = the one you change), from the repository root (`PS2Recomp/` is in
  `.gitignore`):

  ```sh
  git clone https://github.com/ran-j/PS2Recomp.git PS2Recomp   # first time only
  git -C PS2Recomp checkout -qf <pinned commit> && git -C PS2Recomp clean -fdq
  # apply, in order, the patches before N and make a "base" commit
  git -C PS2Recomp apply --ignore-whitespace "$PWD/patches/<previous patch>"   # one per patch
  git -C PS2Recomp -c user.name=x -c user.email=x commit -qam base
  git -C PS2Recomp apply --ignore-whitespace "$PWD/patches/<N>"
  # ... edit ...
  git -C PS2Recomp diff > patches/<N>
  ```

  Then check that the patches after `N` still apply.
- Mark changes with a `// MKSM-Port:` comment to tell them apart from the original code.
- A runtime behavior change comes with a regression test in `ps2xTest/` that fails without the change. If you
  deliberately change something an existing test checked, adjust the test and explain why.
- Patch changes require `scripts\2_build.cmd`; `2_rebuild_fast.cmd` only copies `src/mksm_overrides.cpp` and
  `src/*.h`.

## Overrides (`src/mksm_overrides.cpp`)

- Call the original function for everything you do not replace, and honor resumptions after a checkpoint
  (`ctx->pc` different from the entry address: just call the original).
- Diagnostics are optional: enable them with an environment variable (`MKSM_*_DIAG`) or with a message limit, and
  use a `[mksm-xxx]` tag in the log. Document the variable in `docs/MKSM.md`, and remove the diagnostic once the
  problem is solved.
- If you use a new runtime function, it must exist in some patch: CI compiles `src/*.cpp` against the patched
  runtime.

## Checks (CI: `.github/workflows/tests.yml`)

On every push to `master` and every PR, without the game. They must stay green:

- `python3 tools/ci/validate_config.py`: `funcmap.csv` and `recomp.template.toml`.
- `pwsh -File tools/ci/analyze_ps1.ps1`: syntax of the `.ps1` files.
- The Python tool tests in `tests/` (`python -m unittest discover -s tests -p <file>`).
- The patches apply, every stub has a handler in the runtime, `src/*.cpp` compiles against the patched runtime
  and the whole `ps2x_tests` suite passes. `tests/known_failures.txt` is empty: do not add a test there to hide a
  regression, fix it.

The suite can be reproduced on Linux (or WSL): apply the patches as above and then

```sh
cmake -S PS2Recomp -B PS2Recomp/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPS2X_BUILD_STUDIO=OFF \
  -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2
ninja -C PS2Recomp/build ps2x_tests
(cd PS2Recomp && ./build/ps2xTest/ps2x_tests 2>/dev/null) | python3 tools/ci/check_tests.py tests/known_failures.txt
```

The suite runs from `PS2Recomp/`, as in CI: some tests read sources through relative paths (for example
`VU0 macro mappings cover all S1/S2 enums`) and fail if launched from the root.

(Ubuntu packages: the ones under "Install dependencies" in `tests.yml`.)

## Testing with the game (Windows)

What CI cannot check: it needs the ELF, the IRX modules and the ISO (see the README).

1. `scripts\2_build.cmd` (or `2_rebuild_fast.cmd` if you only changed `src/`).
2. `scripts\3_run.cmd` (60 s) or `scripts\3_run_manual.cmd` (no limit).
3. Review `logs\run_err.log`. A clean log does not prove the game is playable: record in `docs/MKSM.md` exactly
   what was verified.

## Documentation and status

- `docs/MKSM.md` is the research log: add what was tried, with what result, and the next steps. Do not delete
  findings; correct them if they turn out to be wrong.
- If a component or library changes state (working / partial / pending), update `docs/status/data.toml` and run
  `python tools/status/generate.py` (the "Status map" action also does it on pushes to `master`).
- Documentation and comments are written in English; commits too.

## Legal

Never commit game files (ELF, IRX, ISO, `.WAD`, `.SFD`), code generated from them, RAM/VRAM captures or
microcode dumps. `game/` and `logs/` are in `.gitignore`.
