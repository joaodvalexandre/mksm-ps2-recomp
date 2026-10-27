# game/

Put the files of **your own copy** of the game here. For *Mortal Kombat: Shaolin Monks* (PAL, `SLES-53524`):

```
game/SLES_535.24
game/SYSTEM.CNF
game/IOP/*.IRX
```

(extract them from the ISO with `python tools/iso_extract.py <iso> game --only ...`; see `docs/MKSM.md`).
For the God of War base the executable is `game/SCUS_973.99` (NTSC-U, `SCUS-97399`).

These files are in `.gitignore` and are never committed to the repository.
You can also point to another path with the `GOW_ELF` environment variable.
