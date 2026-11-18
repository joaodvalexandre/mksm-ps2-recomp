# Instructions for Claude

The project conventions are in AGENTS.md, shared with the other agents:

@AGENTS.md

## Quick start for a session

- Read `docs/STATUS.md` (where the project stands), then the end of `docs/MKSM.md` and `tools/vu1gen/PROGRESS.md`.
- On the owner's Windows machine you can build and run the game (see AGENTS.md); in the cloud you cannot: say what
  could not be verified, and put unverified code behind an `MKSM_*` toggle that is off by default.
- Open problem at the time of writing: wrong vertex colours in 3D environments (VIF1 / VU1 timing hypothesis in
  `docs/STATUS.md`). Next step: trace the VIF1 commands (`MKSM_TRACE_VIF1`).
- Use all the CPU threads that make sense (the owner has 16): profile first, keep the guest-visible order of GS/GIF
  writes and vertical blanks exactly as it is, and put threading behind an `MKSM_*` toggle.
