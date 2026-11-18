# Project status board (snapshot of 18 Nov 2026)

A plain-text copy of the whiteboard https://claude.ai/artifact/Xbd4cBpesF6vWVTPT8XuGX, so the status lives in the
repository too. It summarises `docs/MKSM.md` and `tools/vu1gen/PROGRESS.md` of branch `vu1-native` (commit 2192d22).
If this file and those logs disagree, the logs win. The percentages are rough estimates by the assistant from the
research logs, not measurements; where a log gives a number, the basis column quotes it.

## Pipeline

Game code -> VIF1 / GIF -> VU1 programs -> GS (paraLLEl-GS) -> picture

| Component | Works | Basis |
|---|---|---|
| Boot and menus | 90% | 60 FPS through front end and difficulty select; movies skipped |
| Game code | 80% | Goro's Lair cutscene loads; one loading crash seen on the CPU backend only |
| VIF1 / GIF launch timing | 40% | colour rows land late in later chunks (hypothesis, unverified) |
| VU1 programs | 90% | 0 differences on all launches checked; synthetic launches reach 67-97% of the reachable pairs |
| GS drawing | 85% | textures, mipmaps and blur passes are right; it is fed wrong colours |
| 3D picture | 50% | characters and sprites fine; environments show colour patches |
| Frame rate in 3D | 40% | 20-36 FPS of 60 (was 2-8) |
| Sound | 5% | own branch (`sound`); 2.5 s offline render only, not run in the game |
| Movies | 0% | skipped by default; they do not play yet |
| Fighting / gameplay | not tested | no documented run past the opening cutscene |

## Measured (from the logs)

- Translated VU1 matches the interpreter on 30,000 of 30,000 captured and 60,000 synthetic launches; in the game,
  verify mode: 346,246 launches, 0 differ; 83 times faster than the interpreter.
- Frame rate: 30-36 FPS in the opening shot, 20 with characters (was 2-8).
- Cleared as causes of the 3D artifacts: textures, mipmapping, post-processing, the VU1 translation.

## Open problem: wrong vertex colours in 3D environments

85-95% of shared vertex positions in environment batches disagree on colour. They are already wrong when VU1 starts.

| Launch of a mesh | Colour groups wrong |
|---|---|
| MSCAL (first chunk) | 1% |
| First MSCNT | 29% |
| Second MSCNT | 70% |
| Later MSCNTs | 79-88% |
| Unrelated data | about 80% |

Hypothesis, not verified: the game expects VU1 to run beside the VIF; the runtime finishes each launch inside
MSCAL/MSCNT, so the packet leaves with an older chunk's colours.

## Next, in order

1. Trace the VIF1 commands of one mesh (`MKSM_TRACE_VIF1`): is the colour UNPACK after the MSCNT?
2. If yes: MSCAL/MSCNT only record the start; the launch runs when the VIF next needs VU1. A patch to upstream's
   VIF1 with a test, behind an `MKSM_*` toggle (default off).
3. New GS dump: the colour groups should fall to 0% like the sprites; then check the picture.
4. Later: spread the work over more CPU threads (a GS thread is already in the log; profile first), frame pacing,
   VU0 through the translator. The `sound` branch is not merged and not yet run in the game.

## Test machine

Ryzen 7 5800X (8 cores, 16 threads), RX 6950 XT, 32 GB DDR4-3200, two 2 TB M.2 SSDs, two 1080p 144 Hz monitors.
The GPU is not the limit; the frame rate is bound by software work on the CPU side (the profile must say which part).

## Who works on what (since 18 Nov 2026)

The owner tests and develops on his Windows machine with an interactive session; the weekly scheduled session is a
researcher (no game files, cannot run the game) that answers issues labelled `research` and may push small changes
to a `research/<date>` branch only. Details in AGENTS.md ("Who works on what").
