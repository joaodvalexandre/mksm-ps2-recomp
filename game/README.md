# game/

Put the files of **your own copy** of *Mortal Kombat: Shaolin Monks* (PAL, `SLES-53524`) here. The ISO can sit
in this folder too.

To recompile, only the executable and the IOP modules are needed:

```
game/SLES_535.24
game/SYSTEM.CNF
game/IOP/*.IRX
game/IOP/IOPRP300.IMG
```

To run, extract the whole disc with its layout (`GAMEDATA.WA*`, `FRONT/MOVIES/*.SFD` and the `.disc_layout` index):

```bat
python tools\iso_extract.py "game\SLES_535.24.Mortal Kombat Shaolin Monks.iso" game --layout
```

`docs/MKSM.md` has the `--only` command for the minimal set.

These files are in `.gitignore` and are never committed to the repository. You can also point to another ELF with
the `MKSM_ELF` environment variable; the disc files must sit next to it.
