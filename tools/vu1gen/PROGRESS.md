# vu1gen: state of the work

Ahead-of-time translation of the game's VU1 programs to C++ (branch `vu1-native`). The design brief and the studies
it rests on are outside the repository, in `D:/Projects/Recomps/vu1-study/` (`DESIGN.md` first). This file says
what exists, how to build and run it, what was measured and what comes next. Keep it current: it and the commits are
all a successor has.

Rules that are easy to break: nothing derived from the game goes into the repository (listings, generated sources,
captures: they go to the build tree); the main repository tree `D:/Projects/Recomps/mksm-ps2-recomp` and the
PS2Recomp tree `D:/Projects/Recomps/ps2recomp-pgs` are read only; never run `scripts/build.cmd`, `scripts/run.cmd`
or the game from here.

## Layout

| Path | What |
|---|---|
| `config/vu1_images.txt` | the four images: ELF address, hash, own pairs, MSCAL entries, subroutine (indirect jump) targets |
| `tools/vu1gen/src/vu_decode.*` | decoder and usage tables, a port of `vudis.py` checked against the interpreter's sources |
| `tools/vu1gen/src/images.*` | parser of `config/vu1_images.txt`, ELF segment reader, the image hash |
| `tools/vu1gen/src/listing.*` | the two text listings of an image (the self-test compares them with `vudis.py`) |
| `tools/vu1gen/src/main.cpp` | `vu1gen` |
| `tools/vu1gen/runtime/vu1n_api.h` | the interface of the translated programs: `Vu1nStatus`, `Vu1nHost` (packet arena, `kick`), `Vu1nProgram`, `vu1nRun` |
| `tools/vu1gen/runtime/vu1n.h` | what generated code includes: the interface, `kVu1nMxcsr`, `vu1nQwordOffset`; the arithmetic helpers are still to come |
| `tools/vu1gen/runtime/vu1n_runtime.cpp` | the packet walk of XGKICK, `vu1nRun` with the MXCSR guard, `vu1nFindProgram` |
| `tools/vu1gen/tests/` | `vu1n_tests`: decoder, float notation, image list, ELF reader, hash, runtime |
| `tools/vu1gen/selftest/vudis_listing.py` | reference side of the decoder self-test (development only) |
| `tools/vu1gen/cmake/targets.cmake` | the targets; `CMakeLists.txt` makes the folder a project of its own |
| `tools/vu1gen/cmake/ps2recomp.cmake` | hook that adds the targets to a PS2Recomp build without a patch |
| `tools/vu1gen/devbuild.cmd` | configures and builds the development tree |

## Development build

The build tree is `D:/Projects/Recomps/mksm-vu1-build`, configured from the PS2Recomp tree
`D:/Projects/Recomps/ps2recomp-pgs` (pinned commit plus patches 0001-0032) with the arguments of the quick profile
of `scripts/build.ps1` (RelWithDebInfo, `scripts/quick-profile.cmake`: `/O2 /Ob2` for everything but the runner,
`PS2X_ENABLE_AGRESSIVE_LOGS=OFF`, no LTO) plus `-DCMAKE_PROJECT_PS2Runtime_INCLUDE=tools/vu1gen/cmake/ps2recomp.cmake`.
That file defers an `include(targets.cmake)` to the end of `ps2xRuntime/CMakeLists.txt` (a deferred call cannot
`add_subdirectory`), so the targets live in the scope of `ps2xRuntime` and their outputs go to `<build>/vu1gen/`.

From Git Bash (the script loads the Visual Studio environment itself; `cmake`, `ninja` and `cl` are not on the PATH):

```
cd /d/Projects/Recomps/mksm-vu1
export MKSM_VU1_PS2RECOMP=D:/Projects/Recomps/ps2recomp-pgs
export MKSM_VU1_BUILD=D:/Projects/Recomps/mksm-vu1-build
export MKSM_VU1_DEPS=D:/Projects/Recomps/mksm-ps2-recomp/PS2Recomp/out/build-quick/_deps
export MKSM_VU1_PARALLEL_GS=D:/Projects/Recomps/parallel-gs
./tools/vu1gen/devbuild.cmd                 # configures when needed, builds the default targets
./tools/vu1gen/devbuild.cmd vu1gen          # one target
./tools/vu1gen/devbuild.cmd -configure      # runs the configure step again
```

`MKSM_VU1_DEPS` and `MKSM_VU1_PARALLEL_GS` make CMake use dependency sources that are already on disk
(`FETCHCONTENT_SOURCE_DIR_*`, read in place); FFmpeg's prebuilt package (35 MB) is downloaded into the build tree on
the first build of `ps2_runtime`. Measured: configure 11 s, `ps2_runtime` with its dependencies (169 steps) 43 s with
8 jobs, the targets of this folder a few seconds.

The oracle is compiled as in the game's quick build: the compile commands of `vu/ps2_vu1_core.cpp`,
`ps2_vu1_upper.cpp`, `ps2_vu1_lower.cpp`, `ps2_memory.cpp`, `ps2_vif1_interpreter.cpp` and `gs/ps2_gif_arbiter.cpp`
in `mksm-vu1-build/compile_commands.json` have the same flags and definitions, token for token, as in
`mksm-ps2-recomp/PS2Recomp/out/build-quick/compile_commands.json` (`/EHsc /Zi /DNDEBUG -std:c++20 -MD /O2 /Ob2`,
no `/fp` option, so `/fp:precise`, and no `/arch`); only the paths differ.

The folder also configures on its own (no PS2Recomp, no oracle): `cmake -S tools/vu1gen -B <dir> -G Ninja` inside a
Visual Studio environment gives `vu1gen` and `vu1n_tests`.

## Tools

### vu1gen

```
D:/Projects/Recomps/mksm-vu1-build/vu1gen/vu1gen.exe --elf D:/Projects/Recomps/mksm-ps2-recomp/game/SLES_535.24 \
    --images config/vu1_images.txt [--listing D:/Projects/Recomps/mksm-vu1-build/selftest/cpp]
```

Reads the four images from the ELF (through its program headers), refuses an image whose hash is not the one in the
configuration, decodes every pair and prints one line per image. `--listing` writes `listing_<image>.txt` (the
study's raw listing format) and `usage_<image>.txt` (every decoder field, three lines a pair). Measured output:

```
image small  ELF 0x49BAE0  hash F6DCE9B3599C989A  pairs 2048 (own 17)  reserved 0 (own 0)  MSCAL entries 1  subroutines 0
image front  ELF 0x49BB70  hash 61FEBE6F1C6A31CC  pairs 2048 (own 1392)  reserved 0 (own 0)  MSCAL entries 9  subroutines 0
image game   ELF 0x49E6F0  hash 65ACC9AE7D1E0A07  pairs 2048 (own 1936)  reserved 0 (own 0)  MSCAL entries 12  subroutines 0
image cut    ELF 0x4A2370  hash 8A34F4A8F0CE0620  pairs 2048 (own 1932)  reserved 1 (own 0)  MSCAL entries 14  subroutines 1
```

The hash is the one the capture files are named by: 64-bit FNV-1a with the offset basis 1469598103934665603, which is
the standard basis without its last decimal digit (`imageHash` in `images.cpp`). The three `code_<hash>.bin` of
`logs/vu1cap` equal the ELF images byte for byte.

### Decoder self-test (repeatable)

The generator's decoder must list every pair of the four images exactly as `vudis.py` does, the usage tables
included:

```
B=D:/Projects/Recomps/mksm-vu1-build; ELF=D:/Projects/Recomps/mksm-ps2-recomp/game/SLES_535.24
$B/vu1gen/vu1gen.exe --elf $ELF --images config/vu1_images.txt --listing $B/selftest/cpp
python -I tools/vu1gen/selftest/vudis_listing.py D:/Projects/Recomps/vu1-study $ELF $B/selftest/py
diff -r $B/selftest/py $B/selftest/cpp && echo IDENTICAL
```

Result (2026-11-11): the eight files are identical (4 x 2051 lines of listing, 4 x 6144 lines of usage: 8192 pairs,
16384 instructions, every field). `listing_front.txt` and `listing_game.txt` also equal the study's
`raw_listing_front.txt` and `raw_listing_game.txt` byte for byte; `raw_listing_cut.txt` differs only in its header
and in the place of the end marker (it was written when `vudis.py` still said 1937 own pairs; all 2048 pair lines
are equal). Only two immediates occur in the images (`8388863.0`, `1.0`), so the port of Python's float notation is
covered by the unit tests rather than by this comparison.

### vu1n_tests

```
D:/Projects/Recomps/mksm-vu1-build/vu1gen/vu1n_tests.exe [substring of a case name]
```

Needs no game file. Result (2026-11-11): `vu1n_tests: 30 cases, 1069 checks, 0 failed checks in 0 cases`.

## The runtime interface (`runtime/`)

What is decided, and why, where the brief left room:

- `Vu1nHost::kick(data, qword)` copies the packet at the XGKICK into the host's arena (`kicks` holds offset and size
  of each, in order). The size comes from the same tag walk as the interpreter's transfer (`progressXgkick`): NLOOP,
  EOP, FLG and NREG of each tag, PACKED / REGLIST / IMAGE (FLG 3 like IMAGE), tags up to the first EOP, addresses
  wrapping at 16 KB.
- Where the interpreter reports an error and sends nothing (a packet that would pass 64 KB, which includes memory
  without any EOP tag), `kick` records nothing, returns false and counts the XGKICK in `Vu1nHost::dropped`. Generated
  code goes on (it need not look at the result): the launch still returns `Done`. The interpreter itself stops the
  launch there, so such a launch is not comparable; `vu1difftest` must put it aside (the oracle's error shows in
  `ps2_log::runtime_log_next_seq()`).
- A launch records at most 1 MB of packets (`kVu1nMaxLaunchPacketBytes`; the interpreter cannot send more than 512 KB
  in its 65536 cycles): a bound for launches that run away. Beyond it `kick` drops too.
- `Vu1nProgram` is a plain struct the generated table fills: `name`, `image` (16 KB), `entryFlags` (one byte per
  pair: `kVu1nEntry`, `kVu1nEntrySnapshot`), `mxcsr`, `run`. `handles()` and `needsSnapshot()` read the flags; an
  address that is not a pair of micro memory is never an entry.
- `Vu1nProgram::mxcsr` is the MXCSR the program's arithmetic was compiled for: `kVu1nMxcsr` of `vu1n.h`, 0x7F80
  (round toward zero) with `VU1N_MATH_REFERENCE`, 0xFFC0 (plus flush to zero and denormals are zero) otherwise.
  `vu1nRun` sets it around the launch and puts the caller's value back on every way out, an exception included.
- `vu1nRun` returns `NotHandled` without touching anything for an entry the program does not handle; otherwise it
  empties the host's lists first.
- `vu1nPrograms()` is defined by the generated table (not written yet); `vu1nFindProgram()` compares all 16 KB.
- `vu1n.h` does not include upstream's `runtime/ps2_vu1.h` (`VU1State` is only declared), so the runtime and its
  tests build without PS2Recomp; generated code includes that header itself, before `vu1n.h`.

## Next (foundation stage)

`vu1difftest`: the oracle side (capture reader, replay, sanity of consecutive records, `execute` at the saved pc
against `resume`, the comparison function, `--bench`).

Then the later stages of the brief: control flow (port of `vucfg.py`), timing exploration (`vutiming.py`), emission,
the arithmetic helpers, the translation side of `vu1difftest`.
