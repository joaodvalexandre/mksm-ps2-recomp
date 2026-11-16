# Instructions for Claude

# Project Context: Mortal Kombat: Shaolin Monks (PS2 Static Recomp)

## 1. Core Architecture & Goals
- **Base Framework:** `ran-j/PS2Recomp` (`ps2xAnalyzer`, `ps2xRecomp`, `ps2xRuntime`, `ps2xIOP`), referencing existing PS2 static recomp runtime implementations (e.g., *God of War* PS2 recomp) for GS/VU1/runtime patterns.
- **Target Platform:** Native Windows C++20.
- **Multi-Region Goal:** Support both **PAL (SLES-53524/53525)** and **NTSC (SLUS-21087)** discs at a locked **60 FPS**.
  - Keep region-specific recompiled ELF code/addresses isolated from the shared `ps2xRuntime` hardware/rendering layer.
  - Ensure PAL builds adjust internal delta-time / VBlank pacing from 50Hz to 60Hz without altering simulation speed.

## 2. Current Priorities & Known Bottlenecks
- **Priority 1 — 60 FPS Gameplay Performance:** Menus run at 60 FPS, but active 3D gameplay drops to **20–30 FPS**.
  - Suspects: VU1 microcode execution overhead, VIF/GIF DMA transfer stalls, or synchronous GS render-target/texture uploads.
- **Priority 2 — 3D Visual Artifacts:** Fix Z-buffer sorting, alpha blending, and multi-pass post-processing corruption in 3D stages.
- **Upstream `PS2Recomp` Patch:** Note that we have local fixes/workarounds for a bug in upstream `PS2Recomp`—do NOT overwrite or revert custom runtime patches when syncing or refactoring.
- **Isolated Track — Audio (SPU2/IOP):** Handled by a separate research agent. Do not modify audio stubs unless a missing IOP/SPU2 callback deadlocks the main thread.

## 3. Strict Engineering Rules (Anti-Loop Protocol)
- **Measure First:** Never guess at FPS optimizations. Instrument frame loops (`R5900` vs `VU1` vs `GS Backend`) and verify millisecond timings before changing code.
- **2-Strike Circuit Breaker:** If an attempted fix for a rendering bug or FPS drop fails twice, **STOP**, revert the change, log the failure in `PROGRESS_HANDOFF.md`, and switch hypotheses.
- **Single-Target Iterations:** Fix one bottleneck or visual bug per cycle, verify compilation and runtime FPS, and commit/log before moving on.
- **Session Handoff:** Always update `PROGRESS_HANDOFF.md` before context compaction with modified files, confirmed root causes, upstream bug notes, and the exact next step.

The rest of the project conventions are in AGENTS.md, shared with the other agents:

@AGENTS.md
