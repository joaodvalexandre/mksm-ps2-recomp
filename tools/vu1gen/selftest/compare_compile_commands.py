"""Checks that the oracle of vu1difftest is compiled as in the game's build (development only).

    python -I tools/vu1gen/selftest/compare_compile_commands.py <build tree A> <build tree B> [source suffix ...]

Compares, for each source, the compile command of the two trees' compile_commands.json: every flag and definition,
in order, with the paths left out (include directories, output files). Without suffixes it takes the sources the
oracle is made of: upstream's VU1 interpreter and the memory, VIF1 and GIF arbiter code it runs on. The exit code is
0 when all of them are compiled with the same flags.
"""
import json
import os
import re
import sys

DEFAULT_SOURCES = [
    "lib/vu/ps2_vu1_core.cpp",
    "lib/vu/ps2_vu1_upper.cpp",
    "lib/vu/ps2_vu1_lower.cpp",
    "lib/ps2_memory.cpp",
    "lib/ps2_vif1_interpreter.cpp",
    "lib/gs/ps2_gif_arbiter.cpp",
]


def commands(tree, suffixes):
    found = {}
    with open(os.path.join(tree, "compile_commands.json"), encoding="utf-8") as fh:
        for entry in json.load(fh):
            name = entry["file"].replace(chr(92), "/")
            for suffix in suffixes:
                if name.endswith(suffix):
                    found[suffix] = entry["command"]
    return found


def flags(command):
    kept = []
    for token in command.split()[1:]:
        plain = token.replace(chr(92), "/")
        if plain.startswith(("-I", "/Fo", "/Fd", "-external:I")) or re.match(r"^[A-Za-z]:/", plain):
            continue
        kept.append(token)
    return kept


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    suffixes = sys.argv[3:] or DEFAULT_SOURCES
    first = commands(sys.argv[1], suffixes)
    second = commands(sys.argv[2], suffixes)
    same = True
    for suffix in suffixes:
        if suffix not in first or suffix not in second:
            same = False
            print("MISSING  %s" % suffix)
            continue
        a, b = flags(first[suffix]), flags(second[suffix])
        if a == b:
            print("SAME     %-32s %s" % (suffix, " ".join(a)))
        else:
            same = False
            print("DIFFERENT %s" % suffix)
            print("   only in the first tree:  %s" % " ".join(t for t in a if t not in b))
            print("   only in the second tree: %s" % " ".join(t for t in b if t not in a))
    print("ALL SAME" if same else "NOT THE SAME")
    sys.exit(0 if same else 1)


if __name__ == "__main__":
    main()
