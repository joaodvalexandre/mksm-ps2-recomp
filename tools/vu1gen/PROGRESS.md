# vu1gen: state of the work

Ahead-of-time translation of the game's VU1 programs to C++ (branch `vu1-native`). The design brief and the studies
it rests on are outside the repository, in `D:/Projects/Recomps/vu1-study/` (`DESIGN.md` first). This file says
what exists, how to build and run it, what was measured and what comes next. Keep it current: it and the commits are
all a successor has.

Rules that are easy to break: nothing derived from the game goes into the repository (listings, generated sources,
captures: they go to the build tree); the main repository tree `D:/Projects/Recomps/mksm-ps2-recomp` and the
PS2Recomp tree `D:/Projects/Recomps/ps2recomp-pgs` are read only; never run `scripts/build.cmd`, `scripts/run.cmd`
or the game from here.

## State (2026-11-11): the foundation stage is done, nothing is translated yet

| Piece | State |
|---|---|
| development build (own tree, oracle compiled as in the quick profile) | works |
| `config/vu1_images.txt` and its parser | done |
| decoder and usage tables | done; equal to `vudis.py` on the four images and to the interpreter's own decoder on 97.5 million pairs |
| runtime interface (`vu1n_api.h`, `vu1n.h`, `vu1n_runtime.cpp`) | done as far as the interface goes; `kick` equal to the interpreter's transfer on 20,000 random packets |
| `vu1difftest`, oracle side | done: 30,000 captured launches replayed; sanity, `execute` against `resume`, comparison machinery, bench, pair coverage |
| control flow, timing exploration, emission, arithmetic helpers, translation side of `vu1difftest`, synthetic launches | not started |

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
| `tools/vu1gen/difftest/captures.*` | reader of `logs/vu1cap/launches_<hash>.bin` |
| `tools/vu1gen/difftest/oracle.*` | upstream's interpreter on a `PS2Memory` + `GS` fixture, PATH1 packets to a memory callback, the three ways to enter it |
| `tools/vu1gen/difftest/launch.*` | `LaunchResult`, the comparison of two results, `prepareLaunch` |
| `tools/vu1gen/difftest/checks.*` | `--decode-check` and `--kick-check` |
| `tools/vu1gen/difftest/vu1difftest.cpp` | the passes over the captures and the reports |
| `tools/vu1gen/difftest/no_programs.cpp` | empty `vu1nPrograms()` until the generator emits the table |
| `tools/vu1gen/tests/` | `vu1n_tests`: decoder, float notation, image list, ELF reader, hash, runtime |
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
cycle: `Oracle::runStepping`), which tells which pairs it executes. Result (29 s):

| | front | game | cut |
|---|---|---|---|
| distinct pairs executed by the captured launches | 50 | 664 | 795 |
| launches whose stepped run equals the run at once (state with `mac` and `status`, cycles, data memory, packets) | 5,314 of 5,314 | 12,520 of 12,520 | 12,166 of 12,166 |

50, 664 and 795 are the study's numbers (of 1,388, 1,930 and 1,924 reachable pairs). The same pass is what the
synthetic launches of the brief need to report the coverage they reach.

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

## For the next stages

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
- **Benchmarks need an idle machine** (see above).
- The hash of an image is not standard FNV-1a (see `imageHash`).
- `vudis.py` in the study folder says 1932 own pairs for the cut image; the study's `raw_listing_cut.txt` still shows
  the earlier 1937.
