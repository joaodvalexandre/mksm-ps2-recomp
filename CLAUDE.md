# Active Session Focus: Mortal Kombat: Shaolin Monks (PS2 Static Recomp)

Read `@AGENTS.md` and `docs/MKSM.md` first for all repository rules, build commands, and past findings.

## 1. Current Verified State
- **Running Build (`pal-restart`):** PAL (`SLES-53524`) boots in native NTSC 60Hz mode at **60 FPS** through the front-end menus and difficulty select on `paraLLEl-GS` (Vulkan). Goro's Lair opening cutscene loads with valid camera, characters, and lighting.
- **Confirmed 3D Bottleneck (2–8 FPS):** Profiling via `MKSM_PROFILE` proved **~80% of frame time goes to upstream's cycle-exact VU1 interpreter**.
- **In-Progress Solution (`vu1-native` branch):** Stage 1 of the ahead-of-time (AOT) VU1-to-C++ translator is built (decoder, runtime interface, comparison harness against the interpreter) but **not yet wired into the live game**.
- **Separate Track (Do Not Touch):** SPU2 / `libsd` sound work is isolated. Leave audio stubs untouched unless an IOP callback deadlocks the main thread.

## 2. Immediate Priority Queue
1. **Wire `vu1-native` into the live build:** Integrate AOT VU1-to-C++ execution with a per-program hash whitelist/blacklist (or toggle) so verified VU1 microprograms run natively while any mismatched microprogram safely falls back to the cycle-exact interpreter.
2. **Validate via Comparison Harness:** Test translated VU1 microprograms against the interpreter on captured launches before full gameplay runs to catch flag/register mismatches (`MAC`, `CLIP`, `STATUS`, `Q`/`P` stalls) early.
3. **Fix Remaining 3D Visual Glitches:** Once 3D speed reaches playable framerates, use `scripts\screenshot.ps1`, `MKSM_PAD_SCRIPT`, and GS packet diagnostics to isolate any remaining Z-buffer, blending, or multi-pass artifacts.

## 3. Anti-Loop & Handoff Rules
- **2-Strike Circuit Breaker:** If an attempted fix fails twice, **STOP**, revert the change, record the failed attempt in `docs/MKSM.md`, and switch hypotheses.
- **Follow Patch & Override Rules Strictly:** Put game-specific hooks in `src/mksm_overrides.cpp` and any `PS2Recomp/` submodule fixes in `patches/NNNN-topic.patch` with a `ps2xTest` regression test (see `@AGENTS.md`).
- **Session Handoff:** Before context compaction or stopping, update `docs/MKSM.md` and Section 1 above with exact build/FPS results, modified files, and the single next action.

@AGENTS.md
