# Architecture

## Build pipeline

`scripts/build.ps1` runs these steps:

1. **Tool check** — `git`, `cmake`, `ninja` (the MSVC environment is loaded by `2_build.cmd`).
2. **PS2Recomp** — clones `ran-j/PS2Recomp` into `<repository>\mksm-work\PS2Recomp` (inside the repository folder
   and git-ignored; override with `MKSM_WORK`), checks out commit
   `c5a9d02573410a2085a4b4b831b0b68ba3515440` and initializes the submodules.
3. **Patches** — applies the full list in the order defined by [`scripts/build.ps1`](../scripts/build.ps1).
   [`tools/ci/patches.py`](../tools/ci/patches.py) reads that same list for CI and checks that no file in
   `patches/` is missing. From the root, `python tools/ci/patches.py` prints the patches and their order; that is
   the reference for preparing a change on the pinned runtime.

   The base is Waffle565's native runtime: `ps2recomp-waffle-runtime.patch` is its `c5a9d02..runtime` diff.
   Small per-topic patches go on top. See "Runtime base".

   Before reapplying the list, the script removes the files that some patch creates (`new file mode`), keeping a
   common cleanup for new patches. Every patch change requires the full build with `scripts\2_build.cmd`; the fast
   build only updates the overrides.

4. **CMake tweaks** — adds `src/runner` to the includes of `ps2EntryRunner` and disables `/GL` and `/LTCG` so it
   builds in parallel (with LTCG, linking ~13,600 generated files is unworkable).
5. **Recompiler** — builds the `ps2_recomp` target.
6. **C++ generation** — fills in `config/recomp.template.toml` (`@ELF@`, `@MAP@`, `@OUT@`) and runs `ps2_recomp`
   on the game ELF (`SLES_535.24` for Mortal Kombat: Shaolin Monks).
7. **Native IOP (optional)** — if `MKSM_IOP_MODULOS` points to a folder with `modules.txt`,
   `tools/iop/iopn_gen.py` compiles the game's IRX modules ahead of time to C++ in `ps2xIOP/src/native/`. The
   folder is obtained by running the game once with `PS2X_IOP_DUMP_MODULES=<folder>`. Without it the IRX run in
   the IOP interpreter.
8. **Runtime** — copies the generated code and `src/mksm_overrides.cpp` to `ps2xRuntime/src/runner` and builds
   `ps2EntryRunner` (unity build + PCH). With `MKSM_VU1_MICROCODIGO`, CMake also generates the native VU1
   (`tools/vu1/vu1n_gen.py`, with the extra entries of `tools/vu1/vu1n_heads.txt`) and the compiled VU1.

The `PS2X_ENABLE_RUNTIME_LOGS`, `PS2X_ENABLE_AGRESSIVE_LOGS` and `PS2X_ENABLE_IOP_RPC_TRACE` traces default to
`OFF`; `scripts\2_build.cmd -Traces` enables them for investigation. The `MKSM_PERF_DIAG` profile is independent of
these build options.

## Recompiler configuration

`config/recomp.template.toml` contains:

- **`stubs`** — functions of Sony libraries (`sceMpeg*`, `sceIpu*`, `sceGs*`, `sceDma*`, `sceCd*`, libc…) that the
  runtime implements natively.
- **`entry_points`** — addresses reached by a jump that are not function starts: the recompiler turns them into
  resume entries of the owning function, or into standalone functions.
- **`skip`** and instruction patches, adjusted by hand.

`config/funcmap.csv` (`Name,Start,End,Size`) defines the function boundaries of the ELF.

`SLES_535.24` has no symbols, so both files come from PS2Recomp's analyzer:

```sh
PS2X_ANALYZER_FUNCMAP=config/funcmap.csv ps2_analyzer game/SLES_535.24 <out>.toml   # map + stubs
python tools/funcmap_fixup.py game/SLES_535.24                                      # gaps and jump targets
```

The analyzer's TOML becomes the template by replacing its paths with `@ELF@`, `@MAP@` and `@OUT@`, and removing the
software double `floor`/`sqrt`/`log` stubs (`tools/ci/validate_config.py` rejects them: the runtime's float
handlers do not follow their ABI). `PS2X_ANALYZER_FUNCMAP` comes from `ps2recomp-analyzer-funcmap.patch`; the
analyzer finds functions from `JAL` targets, so `tools/funcmap_fixup.py` adds the code only reached by `J` (tail
calls) and the jumps into the middle of other functions.

## Overrides (`src/mksm_overrides.cpp`)

They are registered with `PS2_REGISTER_GAME_OVERRIDE` for a given ELF name and entry address: `SLES_535.24`,
entry `0x0011C038`. For now the file only does game-independent setup (disc files, the runtime's private heap below
the ELF, the optional `MKSM_THREAD_WATCH` thread dump); game functions are replaced as the first runs show which
ones need it.

## Runtime base (Waffle565)

Waffle565's runtime replaces the PS2 hardware with native code:

| Layer | Native replacement |
|---|---|
| GS | OpenGL render-target renderer (2x internal resolution, 16:9), texture/CLUT store instead of a VRAM model |
| VU1 | Microprograms compiled ahead of time to C++ (`vu1n`); the exact VU1 stays as a fallback |
| IOP | The game's IRX modules in the interpreter, or compiled ahead of time (`tools/iop/iopn_gen.py`) |
| SPU2 | Native sound engine driven by the game's LIBSD calls |
| Pad / memory card | EE-side handlers for the `scePad*`/`sceMc*` stubs, no SIO2 model |
| DMA / VIF / GIF | Packets go straight to VU1 and the GS; the recompiler turns accesses to DMA registers, timers and GS display registers into native calls |
| Disc | Files extracted next to the ELF (`tools/iso_extract.py --layout`, `.disc_layout`), each at its original sector |

Some of the patches on top of the base (the full list and order are in `scripts/build.ps1`):

| Patch | Content |
|---|---|
| `ps2recomp-waffle-runtime.patch` | `c5a9d02..runtime` diff (49ca62c), without the generated registry |
| `ps2recomp-waffle-tests.patch` | Removes from `ps2xTest` the tests of parts replaced by design and adapts `SifInitRpc` to the IOP batching of 512 cycles |
| `ps2recomp-vu0-vf0.patch`, `ps2recomp-vu0-vi0.patch` | Constant VF0 and VI0 of VU0 (#79) |
| `ps2recomp-vu1n-heads.patch` | `-DPS2X_VU1N_HEADS_FILE` to pass `tools/vu1/vu1n_heads.txt` |
| `ps2recomp-waffle-gcc.patch` | Include path of `ee_ref/` and non-`inline` `VUflags`/`VUops` functions for GCC |
| `ps2recomp-vu1n-leak.patch` | Native VU1 `NativeState` in `std::shared_ptr` (it was lost with every `VU1Interpreter`) |
| `ps2recomp-analyzer-funcmap.patch` | `PS2X_ANALYZER_FUNCMAP=<csv>`: `ps2_analyzer` also writes its functions as a function map |

## Continuous integration

`.github/workflows/tests.yml` runs on every push to `master` and every PR, without needing the game:

- **Quick checks** (seconds):
  - `tools/ci/validate_config.py` validates `config/funcmap.csv` (order, overlaps, sizes, names) and
    `config/recomp.template.toml` (`@ELF@/@MAP@/@OUT@` markers, `name@0xADDRESS` format, addresses inside some
    function). Stubs that do not start a function and repeated addresses are warnings.
  - `tools/ci/analyze_ps1.ps1` parses every `.ps1` with the PowerShell parser.
  - The Python tool tests (`tests/test_gif_inspector.py`).
- **Patches and runtime suite**: downloads PS2Recomp at the commit in `scripts/common.ps1`, applies the patches in
  the order of `build.ps1` (`tools/ci/patches.py` reads them from there and fails if any `patches/*.patch` does not
  apply), checks that every stub of `recomp.template.toml` has a handler in `ps2_call_list.h`
  (`validate_config.py --runtime`), builds `ps2x_tests` on Linux and runs the suite. It also compiles `src/*.cpp`
  against the headers of the already patched runtime (declaring the `sub_*` they use), so an override that uses a
  runtime function no patch defines fails here and not only on Windows. `tools/ci/check_tests.py` only fails for
  tests that are not in `tests/known_failures.txt` (empty today: the whole suite passes). When a test in the list
  passes, CI warns so it can be removed. CI includes `tools/ci/gs_tests.cmake` and `tools/ci/vu1_tests.cmake` in
  its temporary PS2Recomp checkout to build and run the GS FINISH checks and the compiled VU1 checks (pairs, DIV,
  FMAC), the latter with `MKSM_VU1_NATIVE_OFF=1` because the native VU1 goes first.

`.github/workflows/status.yml` regenerates the status map (`docs/status/`) when its data changes.

The VU1 microcode and the generated VU1 C++ remain private data, outside the repository.
