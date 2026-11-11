# vu1gen: state of the work

Ahead-of-time translation of the game's VU1 programs to C++ (branch `vu1-native`). The design brief and the studies
it rests on are outside the repository, in `D:/Projects/Recomps/vu1-study/` (`DESIGN.md` first). This file says
what exists, how to build and run it, what was measured and what comes next. Keep it current: it and the commits are
all a successor has.

Rules that are easy to break: nothing derived from the game goes into the repository (listings, generated sources,
captures: they go to the build tree); the main repository tree `D:/Projects/Recomps/mksm-ps2-recomp` and the
PS2Recomp tree `D:/Projects/Recomps/ps2recomp-pgs` are read only; never run `scripts/build.cmd`, `scripts/run.cmd`
or the game from here.

## State (2026-11-11, night): stages 2 and 3 done: exact on every captured launch, 83 times the interpreter's speed

| Piece | State |
|---|---|
| development build (own tree, oracle compiled as in the quick profile) | works |
| `config/vu1_images.txt` and its parser | done |
| decoder and usage tables | done; equal to `vudis.py` on the four images and to the interpreter's own decoder on 97.5 million pairs |
| runtime interface (`vu1n_api.h`, `vu1n.h`, `vu1n_runtime.cpp`) | done; `kick` equal to the interpreter's transfer on 20,000 random packets and on the 731,100 XGKICKs of the captures |
| `vu1difftest`, oracle side | done: 30,000 captured launches replayed; sanity, `execute` against `resume`, comparison machinery, bench, pair coverage, packets read at the XGKICK |
| control flow and timing exploration (`src/vu_flow.*`, port of `vutiming.py`) | done; its counts equal the study's timing table (below) |
| emitter (`src/vu_emit.*`) and reference arithmetic (`runtime/vu1n.h`, `VU1N_MATH_REFERENCE`) | done: **30,000 of 30,000 captured launches translated and compared, 0 mismatches**; every entry of the three programs handled |
| build integration (`VU1GEN_ELF`: vu1gen runs in the build, the generated library is linked into `vu1difftest`) | done |
| fast SSE arithmetic (stage 3, `vu1n.h` default build) and one function per entry | done: 0 mismatches on the captures, 0 in 600 million fuzzed lanes against the reference; 83 times faster than the interpreter on the captures (below) |
| synthetic launches (`difftest/synthetic.*`, `vu1difftest --synthetic`) | done: 60,000 launches, 0 mismatches; reachable pairs executed 927 / 1,873 / 1,616 of 1,388 / 1,930 / 1,924 (captures: 50 / 664 / 795) |
| glue in the override module (`src/mksm_vu1.cpp`) | not started |

## Layout

| Path | What |
|---|---|
| `config/vu1_images.txt` | the four images: ELF address, hash, own pairs, MSCAL entries, subroutine (indirect jump) targets |
| `tools/vu1gen/src/vu_decode.*` | decoder and usage tables, a port of `vudis.py` checked against the interpreter's sources |
| `tools/vu1gen/src/images.*` | parser of `config/vu1_images.txt`, ELF segment reader, the image hash |
| `tools/vu1gen/src/listing.*` | the two text listings of an image (the self-test compares them with `vudis.py`) |
| `tools/vu1gen/src/vu_flow.*` | control flow and timing exploration: launch starts, per-pair timing classes, refusals, handled entries |
| `tools/vu1gen/src/vu_emit.*` | emission of `vu1n_<image>.cpp` and `vu1n_table.cpp` |
| `tools/vu1gen/src/main.cpp` | `vu1gen` |
| `tools/vu1gen/runtime/vu1n_api.h` | the interface of the translated programs: `Vu1nStatus`, `Vu1nHost` (packet arena, `kick`), `Vu1nProgram`, `vu1nRun` |
| `tools/vu1gen/runtime/vu1n.h` | what generated code includes: the interface, `kVu1nMxcsr`, `Vu1nRegs`, the reference arithmetic, memory and ring helpers |
| `tools/vu1gen/runtime/vu1n_runtime.cpp` | the packet walk of XGKICK, `vu1nRun` with the MXCSR guard, `vu1nFindProgram` |
| `tools/vu1gen/difftest/captures.*` | reader of `logs/vu1cap/launches_<hash>.bin` |
| `tools/vu1gen/difftest/oracle.*` | upstream's interpreter on a `PS2Memory` + `GS` fixture, PATH1 packets to a memory callback, the three ways to enter it |
| `tools/vu1gen/difftest/launch.*` | `LaunchResult`, the comparison of two results, `prepareLaunch` |
| `tools/vu1gen/difftest/checks.*` | `--decode-check` and `--kick-check` |
| `tools/vu1gen/difftest/vu1difftest.cpp` | the passes over the captures and the reports |
| `tools/vu1gen/difftest/no_programs.cpp` | empty `vu1nPrograms()` for a build without `VU1GEN_ELF` |
| `tools/vu1gen/tests/` | `vu1n_tests`: decoder, float notation, image list, ELF reader, hash, runtime |
| `tools/vu1gen/selftest/check_all.sh` | runs every check below in one go (development only) |
| `tools/vu1gen/selftest/vudis_listing.py` | reference side of the decoder self-test (development only) |
| `tools/vu1gen/selftest/compare_compile_commands.py` | checks that the oracle is compiled with the flags of the game's build (development only) |
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
./tools/vu1gen/devbuild.cmd                 # configures when needed; builds vu1gen, vu1n_tests, vu1difftest
./tools/vu1gen/devbuild.cmd vu1difftest     # one target
./tools/vu1gen/devbuild.cmd -configure      # runs the configure step again
```

`MKSM_VU1_DEPS` and `MKSM_VU1_PARALLEL_GS` make CMake use dependency sources that are already on disk
(`FETCHCONTENT_SOURCE_DIR_*`, read in place); FFmpeg's prebuilt package (35 MB) is downloaded into the build tree on
the first build of `ps2_runtime`, and its DLLs are staged next to `vu1difftest.exe`. Measured: configure 11 s,
`ps2_runtime` with its dependencies (169 steps) 43 s with 8 jobs, the targets of this folder a few seconds.

The oracle is compiled as in the game's quick build: the compile commands of `vu/ps2_vu1_core.cpp`,
`ps2_vu1_upper.cpp`, `ps2_vu1_lower.cpp`, `ps2_memory.cpp`, `ps2_vif1_interpreter.cpp` and `gs/ps2_gif_arbiter.cpp`
in `mksm-vu1-build/compile_commands.json` have the same flags and definitions, token for token, as in
`mksm-ps2-recomp/PS2Recomp/out/build-quick/compile_commands.json` (`/EHsc /Zi /DNDEBUG -std:c++20 -MD /O2 /Ob2`,
no `/fp` option, so `/fp:precise`, and no `/arch`); only the paths differ. To repeat the check (exit code 0 and
`ALL SAME`):

```
python -I tools/vu1gen/selftest/compare_compile_commands.py D:/Projects/Recomps/mksm-vu1-build \
    D:/Projects/Recomps/mksm-ps2-recomp/PS2Recomp/out/build-quick
```

Those six sources and their headers are byte for byte the same in `ps2recomp-pgs` and in the game's patched
`mksm-ps2-recomp/PS2Recomp` (compared with `cmp` on 2026-11-11). And the build behaves like the game's on real data:
see the harness sanity below.

A build from nothing (the tree deleted) with the commands above took 73 s: configure 19 s, build 52 s.

The folder also configures on its own (no PS2Recomp, no oracle): `cmake -S tools/vu1gen -B <dir> -G Ninja` inside a
Visual Studio environment gives `vu1gen` and `vu1n_tests`.

## Everything at once

After a build, one script runs every check described below and says PASS or FAIL for each (1 min 45 s to 2 min 15 s;
`--quick` takes the first 1000 launches of each capture file, 20 s):

```
export MKSM_VU1_BUILD=D:/Projects/Recomps/mksm-vu1-build
export MKSM_VU1_ELF=D:/Projects/Recomps/mksm-ps2-recomp/game/SLES_535.24
export MKSM_VU1_CAPTURES=D:/Projects/Recomps/mksm-ps2-recomp/logs/vu1cap
export MKSM_VU1_GAME_BUILD=D:/Projects/Recomps/mksm-ps2-recomp/PS2Recomp/out/build-quick
export MKSM_VU1_STUDY=D:/Projects/Recomps/vu1-study
bash tools/vu1gen/selftest/check_all.sh [--quick]
```

Its output on 2026-11-11 (exit code 0; the reports are in `mksm-vu1-build/check/`):

```
PASS  unit-tests         vu1n_tests: 30 cases, 1069 checks, 0 failed checks in 0 cases
PASS  images             image cut    ELF 0x4A2370  hash 8A34F4A8F0CE0620  pairs 2048 (own 1932)  reserved 1 (own 0)  MSCAL entries 14  subroutines 1
PASS  listing            the listings of vu1gen and vudis.py are identical (32780 lines)
PASS  oracle-flags       ALL SAME
PASS  decoder            decode check: 97518848 pairs, 0 different: PASSED
PASS  kick               kick check: 20000 random packets: 17676 sent by both with the same bytes (4448 wrap at 16 KB, 455 of exactly 64 KB, the largest 65536 bytes), 2324 ref
PASS  captures           sanity 11964 of 11997 consecutive records; MSCNT by execute() = by resume() in 5830 of 5830 and 12112 of 12112 launches
PASS  self                  all            30000    1232.2    24.37     260.0      30000    30000          0           0        0
PASS  self-fault            comparison machinery: PASSED
PASS  coverage              all            30000    1232.2    24.37     260.0          0        0          0       30000        0 |   1036.2
0 failed, 0 skipped; outputs in D:/Projects/Recomps/mksm-vu1-build/check
```

Run it again after any change to the decoder, the runtime or the difftest. Once translated programs are linked, the
`captures` line is where their mismatches show (its exit code is then the comparison's).

## Tools

In the commands below `B=D:/Projects/Recomps/mksm-vu1-build`, `ELF=D:/Projects/Recomps/mksm-ps2-recomp/game/SLES_535.24`,
`CAP=D:/Projects/Recomps/mksm-ps2-recomp/logs/vu1cap`, run from the worktree.

### vu1gen

```
$B/vu1gen/vu1gen.exe --elf $ELF --images config/vu1_images.txt [--listing $B/selftest/cpp]
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

### Decoder self-test against vudis.py (repeatable)

The generator's decoder must list every pair of the four images exactly as `vudis.py` does, the usage tables
included:

```
$B/vu1gen/vu1gen.exe --elf $ELF --images config/vu1_images.txt --listing $B/selftest/cpp
python -I tools/vu1gen/selftest/vudis_listing.py D:/Projects/Recomps/vu1-study $ELF $B/selftest/py
diff -r $B/selftest/py $B/selftest/cpp && echo IDENTICAL
```

Result: the eight files are identical (4 x 2051 lines of listing, 4 x 6144 lines of usage: 8192 pairs, 16384
instructions, every field). `listing_front.txt` and `listing_game.txt` also equal the study's
`raw_listing_front.txt` and `raw_listing_game.txt` byte for byte; `raw_listing_cut.txt` differs only in its header
and in the place of the end marker (it was written when `vudis.py` still said 1937 own pairs; all 2048 pair lines
are equal). Only two immediates occur in the images (`8388863.0`, `1.0`), so the port of Python's float notation is
covered by the unit tests rather than by this comparison.

### Decoder check against the interpreter itself

```
$B/vu1gen/vu1difftest.exe --decode-check [random pairs, 20000000 by default] [--captures $CAP]
```

Compares `vu1gen::decodePair` with `VU1Interpreter::decodeInstructionPair` (private; reached through an explicit
template instantiation, no change to upstream): every usage field of both instructions, the I E M D T bits, the
shadow register and the suppressed lower write. Result (7 s):

```
   upper words, bits 24..0 exhaustive                 33554432 pairs, 0 different so far
   lower-1 group, bits 24..0 exhaustive               33554432 pairs, 0 different so far
   other lower opcodes, bits 24..11 exhaustive        10403840 pairs, 0 different so far
   random pairs                                       20000000 pairs, 0 different so far
   code_61FEBE6F1C6A31CC.bin                              2048 pairs, 0 different so far
   code_65ACC9AE7D1E0A07.bin                              2048 pairs, 0 different so far
   code_8A34F4A8F0CE0620.bin                              2048 pairs, 0 different so far
decode check: 97518848 pairs, 0 different: PASSED
```

(Checked that it can fail: with RSQRT's latency changed to 12 and the suppressed write dropped in the generator's
decoder it reported 16792 differences, and `vu1n_tests` two failed checks.)

### vu1n_tests

```
$B/vu1gen/vu1n_tests.exe [substring of a case name]
```

Needs no game file. Result: `vu1n_tests: 30 cases, 1069 checks, 0 failed checks in 0 cases`.

### vu1difftest on the captures

```
$B/vu1gen/vu1difftest.exe --captures $CAP --images config/vu1_images.txt
        [--only game] [--limit n] [--self | --self-fault n] [--bench] [--repeat n] [--coverage] [--max-report n]
```

For every record of every `launches_<hash>.bin` (30,000 launches: 5,314 front, 12,520 game, 12,166 cut; the header of
`vu1difftest.cpp` and `captures.h` describe the options and the format) it runs:

1. the oracle: `execute()` at the entry pc on the record's state and data memory, also for an MSCNT (entry pc =
   `state.pc`): the way the glue will enter the interpreter;
2. the launch as upstream's runtime runs it: MSCAL by `execute()`, MSCNT by `resume()`, on a second interpreter that
   keeps its private state from record to record while the records are consecutive;
3. the translation, when the build has a program for the image and the entry: `prepareLaunch` (what `execute()` does
   to the state), then `vu1nRun`, then `compareLaunch` against 1.

Results (16 to 21 s without `--bench`; exit code 0):

| | front | game | cut | all |
|---|---|---|---|---|
| launches (MSCAL + MSCNT) | 5,314 + 0 | 4,439 + 8,081 | 2,305 + 9,861 | 30,000 |
| launches that start with `ebit`, `haltAfterDelaySlot` or `branchPending` set | 0 | 0 | 0 | 0 |
| oracle launches that ran out of cycles or stopped on an error | 0 | 0 | 0 | 0 |
| **harness sanity**: consecutive records compared | 3,999 | 3,999 | 3,999 | 11,997 |
| ... oracle's state after the launch = next record's input, compared fields | 3,999 | 3,994 | 3,971 | 11,964 |
| ... also `mac`, `status`, `top`, `itop`, the enables, `vf00`, `vi00` and the cycle count | 3,999 | 3,994 | 3,971 | 11,964 |
| ... the same for upstream's way (MSCNT by `resume()`) | 3,999 | 3,994 | 3,971 | 11,964 |
| ... pairs that differ / of them with another launch between the two records | 0 / 0 | 5 / 5 | 28 / 28 | 33 / 33 |
| **MSCNT: `execute(startPC = state.pc)` against `resume()`**, true continuation: launches / equal | - | 2,555 / 2,555 | 3,275 / 3,275 | 5,830 / 5,830 |
| ... `resume()` after loading the state (zero-cycle `execute`, branch state put back): launches / equal | - | 5,526 / 5,526 | 6,586 / 6,586 | 12,112 / 12,112 |

- The 11,964 of 11,997 are the numbers of the study's Python model. The 33 pairs that differ do so first in `vf01`
  (29) or `vf04` (4), and in all 33 the captured clock (`state.cycles`) advanced by more cycles than the launch took:
  another program's launch ran between the two records (the index of a record counts its own program's launches).
- "Equal" for the MSCNT rows is: the compared fields of the state, the 16 KB of data memory, the packets, and also
  `mac`, `status` and the number of cycles. So entering the interpreter with `execute(startPC = state.pc)` for an
  MSCNT gives what `resume()` gives on all 17,942 captured MSCNT launches, 5,830 of them against a `resume()` that
  continued the very interpreter that ran the launch before.
- `--self` (the translation is a second interpreter behind `Vu1nProgram` / `vu1nRun`): 30,000 launches translated,
  30,000 compared, 0 mismatches.
- `--self-fault 7` (one result in seven damaged, the kind of damage rotating over every field of the state, a byte
  of data memory, a byte of a packet, the packet count, a packet's size: 172 kinds): 4,285 faults, 3,989 in compared
  fields, all 3,989 reported with the damaged field, the 296 in fields that are not compared not reported, no
  difference without a fault: `comparison machinery: PASSED`.

What the comparison covers (`launch.h`): `vf01..vf31`, `vi01..vi15`, `acc`, `q`, `p`, `i`, `r`, `pc`, `clip`, `ebit`,
`haltAfterDelaySlot`, `stoppedByD/T`, `branchPending`, `branchTarget`, `branchDelay`, bit for bit; the whole data
memory; the packets (count, sizes, bytes). Not `mac`, `status`, `cycles`, `vf00`, `vi00`, `top`, `itop`, the enables.
A launch whose oracle did not end (cycle budget, error) is counted and not compared.

### vu1difftest --bench: the oracle's host time

`--bench` times `execute()` alone with the time stamp counter (best of `--repeat` runs of each launch, 3 by
default); the oracle's packets go to a memory callback, so no GS work is in the numbers. The machine is shared with
another session, and the numbers follow its load: the table is the first of two consecutive runs that agreed within
1 % (all images: 255.3 and 255.8 us a launch); two later runs with other processes at work gave 290.9 and 280.4 us,
and one run during a build was up to 4 times slower on the short launches. Compare the oracle and a translation
within one run.

| image, entry | launches | pairs a launch | cycles a launch | kicks a launch | qwords a launch | us a launch | ns a cycle |
|---|---|---|---|---|---|---|---|
| front `MSCAL 0000` | 5,314 | 1,611.6 | 1,612.6 | 131.16 | 655.8 | 489.0 | 303.3 |
| game `MSCAL 0000` / `06F0` / `0E50` | 1,362 / 1,852 / 1,225 | 21 / 23 / 20 | 23 / 25 / 22 | 0 | 0 | 5.04 / 5.32 / 4.80 | 219 / 213 / 218 |
| game `MSCNT 00A8` | 1,350 | 1,214.4 | 1,427.3 | 3 | 302.9 | 277.0 | 194.1 |
| game `MSCNT 06D8` | 2,432 | 1,426.1 | 1,673.9 | 3 | 355.3 | 322.8 | 192.9 |
| game `MSCNT 07A8` | 1,867 | 1,381.9 | 1,584.7 | 3 | 283.3 | 325.8 | 205.6 |
| game `MSCNT 0E38` | 1,176 | 1,496.8 | 1,715.0 | 3 | 306.4 | 352.8 | 205.7 |
| game `MSCNT 0EF0` | 1,219 | 1,041.5 | 1,186.1 | 3 | 185.3 | 239.5 | 201.9 |
| game `MSCNT 14C0` | 37 | 1,242.1 | 1,409.5 | 3 | 219.6 | 283.3 | 201.0 |
| game, all | 12,520 | 867.3 | 1,004.5 | 1.94 | 191.4 | 200.3 | 199.4 |
| cut `MSCAL 04A0` / `0790` / `0B40` / `0EF0` / `36F0` | 39 / 214 / 1,625 / 408 / 19 | 9 / 13 / 13 / 17 / 17 | 10 / 14 / 15 / 19 / 18 | 0 | 0 | 3.11 / 3.81 / 3.77 / 4.41 / 4.30 | 311 / 272 / 251 / 232 / 239 |
| cut `MSCNT 04E8` | 51 | 816.6 | 1,306.8 | 1 | 191.4 | 183.6 | 140.5 |
| cut `MSCNT 0778` | 89 | 826.0 | 1,332.8 | 1 | 159.5 | 179.4 | 134.6 |
| cut `MSCNT 07F8` | 207 | 646.3 | 923.0 | 1 | 122.2 | 152.5 | 165.2 |
| cut `MSCNT 0B28` | 552 | 964.9 | 1,413.7 | 1 | 163.1 | 221.1 | 156.4 |
| cut `MSCNT 0BA8` | 1,623 | 1,061.3 | 1,482.3 | 1 | 186.6 | 226.0 | 152.5 |
| cut `MSCNT 0ED8` | 3,075 | 1,025.7 | 1,425.2 | 1 | 183.5 | 219.5 | 154.0 |
| cut `MSCNT 0F78` | 352 | 1,342.9 | 1,774.2 | 1 | 204.7 | 297.6 | 167.7 |
| cut `MSCNT 1298` | 3,802 | 1,391.2 | 1,829.5 | 1 | 215.0 | 308.0 | 168.3 |
| cut `MSCNT 36D8` | 1 | 1,204.0 | 1,517.0 | 1 | 148.0 | 266.3 | 175.6 |
| cut `MSCNT 3778` | 17 | 1,881.8 | 2,317.1 | 1 | 196.4 | 419.8 | 181.2 |
| cut `MSCNT 3C50` | 92 | 1,920.7 | 2,366.7 | 1 | 199.7 | 425.7 | 179.9 |
| cut, all | 12,166 | 958.5 | 1,300.3 | 0.81 | 157.6 | 209.7 | 161.3 |
| all images | 30,000 | 1,036.2 | 1,232.2 | 24.37 | 260.0 | 255.3 | 207.2 |

The cycles of a launch include its stalls and the flush of the XGKICK transfer (2 cycles a qword), so "ns a cycle"
is not the cost of an executed pair: the pairs a launch executes are in the second column (from `--coverage`,
below), and the oracle costs 246 ns for each of them on average (255.3 us for 1,036.2 pairs). With a translated
program linked, the same table gets the translation's time and the ratio.

### vu1difftest --coverage: the pairs the captures execute

```
$B/vu1gen/vu1difftest.exe --captures $CAP --images config/vu1_images.txt --coverage
```

Runs every launch a second time on the oracle one pair a call (`execute()` then `resume()` with a budget of one
cycle: `Oracle::runStepping`), which tells which pairs it executes, and lets the tool do at each XGKICK what
generated code will do: `Vu1nHost::kick` on data memory as it is at the instruction. Result (29 s, exit code 0):

| | front | game | cut |
|---|---|---|---|
| distinct pairs executed by the captured launches | 50 | 664 | 795 |
| launches whose stepped run equals the run at once (state with `mac` and `status`, cycles, data memory, packets) | 5,314 of 5,314 | 12,520 of 12,520 | 12,166 of 12,166 |
| XGKICKs executed | 696,996 | 24,243 | 9,861 |
| ... whose packet read whole at the instruction is the packet the interpreter transferred | 696,996 | 24,243 | 9,861 |

- 50, 664 and 795 are the study's numbers (of 1,388, 1,930 and 1,924 reachable pairs). The same pass is what the
  synthetic launches of the brief need to report the coverage they reach.
- **The brief's fact 4 holds on every captured launch**: for all 731,100 XGKICKs the packet copied at the instruction
  equals, byte for byte, the packet the interpreter read two cycles a qword while the program went on. No store
  lands in a packet that is still being transferred, and `kick` sizes and copies the game's real packets as the
  interpreter does. (Checked that it can fail: a wrong qword address at one XGKICK site made 50 of 50 launches
  differ.) This is data dependent: it has to be repeated on the synthetic launches.

### vu1difftest --kick-check

```
$B/vu1gen/vu1difftest.exe --kick-check [packets, 20000 by default]
```

A three-pair program (`XGKICK vi01`, the E bit, its delay slot) on random data memory with a chain of GIF tags of
every format; the interpreter's packet against `Vu1nHost::kick` on the same memory. It first checks that the harness
tells the three ends of an oracle launch apart (`OracleEnd`). Result (1 s):

```
oracle ends: a program that ends (2 cycles), one that never does (65536 cycles: the budget) and a reserved instruction (0 cycles, an error) are told apart: PASSED
kick check: 20000 random packets: 17676 sent by both with the same bytes (4448 wrap at 16 KB, 455 of exactly 64 KB, the largest 65536 bytes), 2324 refused by both, 0 different: PASSED
```

## The translation (stage 2)

### Building it

The translation is part of the development build when the configure step gets the ELF:

```
export MKSM_VU1_CMAKE_ARGS="-DVU1GEN_ELF=D:/Projects/Recomps/mksm-ps2-recomp/game/SLES_535.24"
./tools/vu1gen/devbuild.cmd -configure vu1difftest     # once; later builds keep the cache variable
```

`vu1gen --elf ... --images config/vu1_images.txt --out <build>/generated-vu1 --report <build>/generated-vu1/report.txt`
then runs as a build step (whenever `vu1gen`, the configuration or the ELF changes) and writes `vu1n_small.cpp`,
`vu1n_front.cpp`, `vu1n_game.cpp`, `vu1n_cut.cpp` and `vu1n_table.cpp` there (outside the repository: they quote the
microcode). `vu1n_programs` compiles them `/fp:precise /bigobj` with `VU1N_MATH_REFERENCE` (generated code refuses to
compile without it until the fast helpers exist). The analysis takes 0.1 s, compiling the four units about a minute.
Without `VU1GEN_ELF` the empty table of `difftest/no_programs.cpp` is linked as before.

### How it works

- `vu_flow.cpp` explores every launch start: the MSCAL entries of the configuration with empty pipelines, then,
  transitively, the continuation of every E bit with the pipelines flushed. A continuation carries the VI register a
  branch could still read the old value of and the last JALR's return address, so the refusal of a branch reading
  the VI register written by the pair before counts the E-bit delay slot, as stage 1 asked. The state is the timing
  state of `vutiming.py` without the producer tags: pc, pending branch, E state, cycles left for every VF lane and
  VI register, the divider, the EFU queue and its busy time, the MAC and clip entries in flight, "an XGKICK may be
  active", the link. A pair's stall is calculatePairReadyCycle's; at an XGKICK that may find a transfer active every
  extra wait up to the longest pending latency is tried.
- What the clock decides is a count, taken when the pair executes: divisions in flight for a Q read (0 or 1), EFU
  results in flight for MFP, MAC writers in flight for FMAND / FMEQ / FMOR, clip writers in flight for FCAND / FCEQ /
  FCOR / FCGET. The emitted code keeps rings (`Vu1nRegs::q`, `p`, `mac`, `clip`: element k is the value k producers
  ago, pushed by each producer) and each read site reads a constant element. A count that differs between two states
  at one pair refuses the pair.
- Refused pairs (DESIGN.md fact 5, all checked during the exploration): reserved instruction, outside the image's own
  code, D or T bit, OPMULA / OPMSUB with a mask other than xyz, an upper writing VF00 lanes its lower reads, status
  flag instructions, E bit on a branch or in a delay slot, a branch in a delay slot or in the pair after an E bit, a
  branch reading a VI register the pair before wrote (delaysNextBranchRead), JR without a known return address, a
  full EFU queue or flag pipeline. An entry is handled when no launch start at its pc reaches a refused pair. Upper
  and lower writing the same VF register is supported (the lower's write is dropped, as in the interpreter).
- Emission (`vu_emit.cpp`): per image one function with a switch on the entry pc (NotHandled before anything is
  read), a label per pair reached in sequence, branch pairs followed by a copy of their delay slot and a goto (a
  switch over the known targets for JR / JALR, `UnknownTarget` otherwise), E-bit pairs followed by a copy of the last
  pair and the end of the launch. Each pair: the upper computed into a temporary from the state before the pair, the
  lower executed (its VF write dropped for VF00 and when the upper writes the same register), then the upper's
  result, its MAC flags and its clip value committed. Backward jumps count down `kVu1nLoopBudget`.
- Reference arithmetic (`vu1n.h`): the interpreter's code lane by lane (normalised operands, float32 under round
  toward zero, product-sums as two roundings, the binary64 fix-up of `normalizeFmacExactResult`, DIV / SQRT / RSQRT /
  EFU and FTOI as coded). `std::signbit` is replaced by a bit test: MSVC calls the CRT for it.

### Results (2026-11-11)

`report.txt` (the analysis) against the study's timing table (`tpl_20_common.md` 5.4): equal in every count.

| | front | game | cut |
|---|---|---|---|
| launch starts / states explored | 27 / 2,703 | 36 / 3,671 | 49 / 5,537 |
| reachable pairs (all emitted) | 1,388 | 1,930 | 1,924 |
| pairs with a fixed stall | 37 | 89 | 105 |
| Q reads: last division / the one before / both | 51 / 55 / 0 | 46 / 15 / 0 | 72 / 30 / 0 |
| MFP: last EFU result / the one before | - | - | 9 / 5 |
| FMAND: 0 / 1 writers in flight | - | - | 1 / 2 |
| FCAND: 0 writers in flight | - | - | 29 |
| entries (MSCAL + continuations) / handled | 26 / 26 | 36 / 36 | 42 / 42 |

(`small`: its continuation runs past the 17 own pairs into the shifted front code, so 1 of its 2 entries is refused.)

`vu1difftest --captures ...` with the translation linked: **30,000 launches translated, 30,000 compared, 0
mismatches** (front 5,314, game 12,520, cut 12,166; every MSCAL and MSCNT entry the captures use; no NotHandled, no
UnknownTarget). `check_all.sh` passes as before, its `coverage` line now with the translation compared too.

Checked that it can fail: replacing every "the division before" Q read of the cut program by "the last division"
(31 sites) gave 9,623 mismatches in 12,166 cut launches (first differences in data memory and vf14).

`--bench` (reference arithmetic; the machine is shared, so compare ratios within one run only):

| | oracle us a launch | translation us a launch | speed-up |
|---|---|---|---|
| front | 478.7 | 13.1 | 36.5 |
| game | 200.2 | 77.1 | 2.6 |
| cut | 217.0 | 93.3 | 2.3 |
| all | 256.3 | 72.3 | 3.5 |

The game and cut launches cost about 75 ns a pair: the reference helpers inline to about 330 instructions a pair
(641,849 lines of disassembly for `vu1n_cut.cpp`, a 40 KB stack frame), so a vertex loop does not fit the
instruction cache. That is what stage 3 replaces.

## Fast arithmetic and entry functions (stage 3)

### What changed

- **One function per handled entry** (`vu1n_<image>_<pc>`, `vu1n_run_<image>` dispatches): each holds the pairs the
  launches from that entry reach (the routines only meet at the end of a launch). With one function per image MSVC
  took up to 583 s for `vu1n_game.cpp` and left the game program at 2.7 times the interpreter; now each unit takes
  about 85 s and the code is about twice as large (pairs shared by several entries are emitted in each).
- **No flush to zero.** DESIGN.md 4.1 planned MXCSR 0xFFC0 (flush to zero, denormals are zero) for the fast build.
  It is 0x7F80 in both builds instead, the interpreter's own environment: with the operands normalised explicitly
  (integer operations), every SSE operation then rounds as the interpreter's float code, and only the binary64
  fix-up is left to handle. Flush to zero would change a product-sum with a denormal product by one unit (1.0 -
  2^-140 truncates to 0x3F7FFFFF; the test has it). A denormal result costs a microcode assist; forcing FTZ/DAZ made
  no measurable difference on the captures.
- **The fast helpers** (`vu1n.h`, "the public names of the upper helpers", with the argument written there): ADD,
  SUB, MUL equal the fix-up once a result below FLT_MIN becomes the zero of its sign; MADD, MSUB, OPMSUB take the
  reference for the lanes with a nonzero product below 2^-79 or a product or result at or above 2^127, and are exact
  elsewhere; MAX / MINI are MAXPS / MINPS on normalised operands; ABS, ITOF (multiplication by the exact reciprocal),
  FTOI (CVTTPS2DQ with the positive saturation fixed). Dest is a template parameter; `vu1nCommit<Dest>` blends. The
  lower instructions, DIV and the EFU are the same scalar code in both builds.
- **MAC flags only where they are read**: the exploration keeps, with MAC readers in the image, the pcs of the last
  flag writers (`TState::macHist`), so a read with k writers in flight names its producer (`PairFlow::macNeeded`).
  Only those producers compute flags (with the reference helper); the others push a dummy so the ring stays aligned.
  The cut program has 3 such producers. (The tracking adds states: game 3,671 to 4,565, cut 5,537 to 7,594.)
- Targets: `vu1n_programs` is the fast build, `vu1n_programs_ref` the reference one, linked into `vu1difftest_ref`
  (`devbuild.cmd vu1difftest vu1difftest_ref`).

### Results (2026-11-11)

- `vu1difftest` (fast) and `vu1difftest_ref`: **30,000 launches translated and compared, 0 mismatches** each.
- `vu1n_tests` (`tests/math_tests.cpp`): every fast helper against the reference, bit for bit, on operands built to
  part them (exponent 0 and 255, tiny and huge exponents, products near 2^-79, 2^-126 and 2^127, product-sums that
  cancel to within three units, integers as floats, FTOI near its limits; dest masks xyzw, xz, w; vector and
  broadcast forms): 0 mismatches with `VU1N_MATH_FUZZ=20000000` (14 FMAC runs, OPMULA / OPMSUB, MAX, MINI, ABS, ITOF
  and FTOI in all four forms; 60 s). Checked that it can fail: with the exact-lane detection switched off it reported
  227,206 FMAC and 46,774 OPMSUB mismatches at the default 200,000 vectors.
- `--bench` (one run, the machine shared):

| | oracle us a launch | fast us a launch | speed-up | reference build us a launch |
|---|---|---|---|---|
| front | 485.9 | 2.86 | 170 | |
| game | 191.5 | 3.18 | 60 | |
| cut | 200.8 | 2.80 | 72 | |
| all | 247.4 | 2.97 | 83 | 8.37 (30 times) |

  About 2.4 ns a cycle of the interpreter's clock, 2.9 ns an executed pair.

## Synthetic launches

```
$B/vu1gen/vu1difftest.exe --captures $CAP --images config/vu1_images.txt --coverage --synthetic 20000 \
        [--seed n] [--synthetic-out $B/synthetic]
```

After the captures of each image, `--synthetic n` runs n launches built from up to 1,024 of its records (`synthetic.cpp`):
one in 64, 16, 6 or 3 float-like words (exponent 1..254) of VF01..VF31, ACC, Q, P, I and of the 16 KB of data memory
changed (sign, exponent by up to 3, a large mantissa change, a few units in the last place, or one of a list of
special values: zeros, exponent 255, FLT_MAX, FLT_MIN, a denormal, 1.0, 2^-70, the colour constant). VI registers and
words with exponent 0 (counts, pointers, flags, the low words of GIF tags) are kept, so the loops keep their counts.
Three launches in four start at the base's entry, the fourth at every handled entry in turn (entries the captures
never launched included). Each launch: the oracle at once, the oracle one pair a call (coverage, and its packets
read at each XGKICK), the translation; compared as a captured launch. A mismatching launch is written to
`--synthetic-out` as `launches_<hash>.bin` with `code_<hash>.bin`, which `--captures <that folder>` replays.

Fact 4 (the packet read whole at the XGKICK is the packet the interpreter transfers) is a property of the data, and
changed data breaks it: a float-like GIF tag word changed makes a packet run into memory the program writes next.
Such launches are counted ("transfer") and their packets compared with the read at the instruction (what the
translation promises); state and data memory are still compared with the oracle. The first run, before that rule,
reported 64 mismatches, all of this kind and only in the packets; replayed with `--coverage` every one of them showed
"read at the instruction" differing from "transferred".

Result (2026-11-11, seed 1, 20,000 a image, 1 min 56 s with the captures and `--coverage`):

| | front | game | cut |
|---|---|---|---|
| launches / oracle ended / out of cycles / oracle error | 20,000 / 16,930 / 2,981 / 89 | 20,000 / 19,097 / 48 / 855 | 20,000 / 18,748 / 395 / 857 |
| translated and compared / mismatches | 16,930 / 0 | 19,097 / 0 | 18,748 / 0 |
| of them "transfer" (fact 4 broken by the changed data) | 182 | 118 | 0 |
| reachable pairs executed (captures alone) | 927 of 1,388 (50) | 1,873 of 1,930 (664) | 1,616 of 1,924 (795) |

The launches that run out of cycles start mostly at continuations with a base state of another routine (their input
headers do not fit). The pairs still not executed are mostly in the front end's routines the captures never launch.

## The runtime interface (`runtime/`)

What is decided, and why, where the brief left room:

- `Vu1nHost::kick(data, qword)` copies the packet at the XGKICK into the host's arena (`kicks` holds offset and size
  of each, in order). The size comes from the same tag walk as the interpreter's transfer (`progressXgkick`): NLOOP,
  EOP, FLG and NREG of each tag, PACKED / REGLIST / IMAGE (FLG 3 like IMAGE), tags up to the first EOP, addresses
  wrapping at 16 KB.
- Where the interpreter reports an error and sends nothing (a packet that would pass 64 KB, which includes memory
  without any EOP tag), `kick` records nothing, returns false and counts the XGKICK in `Vu1nHost::dropped`. Generated
  code goes on (it need not look at the result): the launch still returns `Done`. The interpreter itself stops the
  launch there, so such a launch is not comparable: `vu1difftest` sees the oracle's error (a new entry in upstream's
  runtime log) and puts the launch aside.
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
- `vu1nPrograms()` is defined by the generated table (not written yet; `difftest/no_programs.cpp` stands in);
  `vu1nFindProgram()` compares all 16 KB.
- `vu1n.h` does not include upstream's `runtime/ps2_vu1.h` (`VU1State` is only declared), so the runtime and its
  tests build without PS2Recomp; generated code includes that header itself, before `vu1n.h`.

## Next

1. **Glue** (`src/mksm_vu1.cpp` in the main repository, `MKSM_VU1=0|1|verify`), the build integration of the generated
   library into the game (its own library, `/fp:precise`, no `/GL`), then a game run. The verify mode is where fact 4
   is checked on the game's real data beyond the captures.
2. Coverage of the remaining pairs: synthetic bases fitted to each entry (a captured state of the same routine), or
   records captured from the front end's other routines.
3. Possible later speed: VF registers kept in `__m128` locals instead of `Vu1nRegs` in memory; operand normalisation
   skipped for registers known to hold normalised values (FMAC results); the code duplication of the entry functions.

## Notes from stage 1 for the next stages

Control flow (port `vucfg.py`), timing exploration (`vutiming.py`), emission, the arithmetic helpers, the translation
side of the difftest. Things this stage found or fixed that they should know:

- **The decoder is the interpreter's**, proven on every encoding (above), not only on the game's. `Instr` has the
  usage fields under the interpreter's names and a normalised form for the emitter (`Op`, `Operand`, `toAcc`, `bc`,
  `fixedPoint`, `imm`, `memBase`/`memStep`, `linkReg`, `jumpReg`, `kickReg`).
- **Plugging the translation into `vu1difftest`**: replace `difftest/no_programs.cpp` in the `vu1n_programs` target
  (`cmake/targets.cmake`) by the generated sources (they go to `D:/Projects/Recomps/mksm-vu1-build/generated-vu1/`,
  never into the repository; compile them `/O2 /fp:precise`). Nothing else changes: the tool finds the program with
  `vu1nFindProgram`, asks `handles(entryPc)`, runs `vu1nRun` and compares. An `UnknownTarget` from an entry without
  `kVu1nEntrySnapshot` is reported as an error.
- **How a translated launch must be prepared** (`prepareLaunch` in `difftest/launch.cpp`, and so the glue): as
  `execute()` does, for an MSCNT too: `pc` = entry, `ebit`, `haltAfterDelaySlot`, `stoppedByD/T` false,
  `branchPending` false, `branchTarget` = `branchDelay` = 0, `top`, `itop`, `vf00`. The oracle of the comparison is
  `execute()` at the entry pc, which was shown equal to `resume()` on every captured MSCNT.
- **What `execute()` at the saved pc cannot keep** is the interpreter's remembered VI value for a branch in the
  first pair of the continuation (the E-bit delay slot wrote a VI register with an instruction that delays the next
  branch read, and the continuation starts with a branch that reads it; `resume()` keeps it). No captured MSCNT
  shows a difference, and a one-off scan with `vudis.py` over all 2048 pairs of the four images found no such
  continuation (the continuations that start with a branch start with `B`, which reads no register; the delay slots
  are NOP, IADDIU or the I-bit immediate). The generator's static refusals (brief, fact 5: a branch reading a VI
  register written by the pair before it) must count the E-bit delay slot as the pair before a continuation.
- **`branchTarget` and `branchDelay` are compared.** After a launch the interpreter leaves `branchPending` false,
  `branchDelay` 0 and `branchTarget` = the target of the last branch taken in the launch (0 when none was taken; the
  value is dead, nothing reads it while `branchPending` is false). Generated code has to store it (a constant store
  at each taken branch is enough), or the brief's list of compared fields has to lose it.
- **A launch that runs away**: the interpreter stops after 65536 cycles in the middle of the program without a flush
  (`OracleEnd::Budget`: counted, not compared). The brief asks for a pair limit in the translation; no status was
  added for it. A possibility: return `Done` with `state.pc` at the pair it stopped before, which is not an entry, so
  the next MSCNT goes to the interpreter.
- **Synthetic launches** (brief, section 6) are not started. `vu1difftest` processes one `LaunchRecord` at a time
  (`processFile`); a generator of synthetic records can feed the same code (the sanity and `resume()` passes only
  make sense for captured sequences), and `--coverage` already tells which pairs a launch executes: 50, 664 and 795
  distinct pairs for the captures, so 1,338, 1,266 and 1,129 reachable pairs of the three programs still wait for a
  launch that runs them.
- **Only the quick profile gives the oracle.** A configure-only test of the hook with `-DCMAKE_BUILD_TYPE=Release`
  (and `-DPS2X_ENABLE_LTO=OFF`, as `scripts/build.ps1 -Release` passes) showed the interpreter's sources with
  `/GL /Oi /Gy /Gw /GF /Zc:inline /fp:fast /arch:AVX2 /GS- /Qspectre-`: `/fp:fast` as the studies said, and `/GL`
  still there although the option of patch 0002 is off (not traced further; upstream's `ReleaseMode.cmake` also
  sets `INTERPROCEDURAL_OPTIMIZATION_RELEASE`, which makes CMake add it). `targets.cmake` warns when `vu1difftest` is configured in a Release tree; the
  targets of this folder get no `/fp:fast` there (`vu1n_runtime`: `/O2 /Ob2 /DNDEBUG /fp:precise`). Generated code
  must get the same treatment when it joins the game's build: its own library, `/fp:precise`, no `/GL`.
- **Benchmarks need an idle machine** (see above).
- The hash of an image is not standard FNV-1a (see `imageHash`).
- `vudis.py` in the study folder says 1932 own pairs for the cut image; the study's `raw_listing_cut.txt` still shows
  the earlier 1937.
