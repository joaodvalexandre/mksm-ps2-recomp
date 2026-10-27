#!/usr/bin/env python3
"""Compares the output of ps2x_tests with the list of known failures.

Usage:  ps2x_tests | python tools/ci/check_tests.py tests/known_failures.txt

Fails (exit code 1) if any test that is not in the list fails, or if the suite does not print its summary
(it hung or exited early). Warns, without failing, when a test in the list now passes, so it can be removed.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ANSI = re.compile(r"\x1b\[[0-9;]*m")
STATUS = re.compile(r"\[(Passed|Failed|Error)\]")


def parse_results(output: str) -> dict[str, str]:
    """{"suite :: test": "Passed" | "Failed" | "Error"} from the MiniTest output.

    MiniTest writes "  [Run]: <test> " with no newline, then whatever the test prints and afterwards
    "[Passed]", "[Failed]" or "[Error]"; the name ends at the first newline or ANSI code.
    """
    results: dict[str, str] = {}
    text = ANSI.sub("\x00", output)  # marks where color codes were, which delimit the name
    suites = [(m.start(), m.group(1).replace("\x00", "").strip())
              for m in re.finditer(r"^\[Suite\]: (.*)$", text, re.M)]
    chunks = text.split("  [Run]: ")
    pos = len(chunks[0])
    for chunk in chunks[1:]:
        start = pos
        pos += len("  [Run]: ") + len(chunk)
        suite = next((n for p, n in reversed(suites) if p < start), "?")
        name = re.split(r"[\n\x00]", chunk.lstrip("\x00"), maxsplit=1)[0].strip()
        m = STATUS.search(chunk.replace("\x00", ""))
        results[f"{suite} :: {name}"] = m.group(1) if m else "Error"
    return results


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    known = {
        line.strip() for line in Path(sys.argv[1]).read_text(encoding="utf-8").splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    }

    output = sys.stdin.read()
    sys.stdout.write(output)
    sys.stdout.flush()
    results = parse_results(output)
    passing = {n for n, e in results.items() if e == "Passed"}
    failing = {n for n, e in results.items() if e != "Passed"}
    has_summary = "Total Tests:" in output

    new_failures = sorted(failing - known)
    fixed = sorted(known & passing)
    print("\n==== Suite check ====")
    print(f"Passing: {len(passing)}  Failing: {len(failing)}  Known failures: {len(failing & known)}")
    for name in fixed:
        print(f"::notice::Now passes (can be removed from the known failures list): {name}")
    for name in new_failures:
        print(f"::error::New failure: {name}")
    if not has_summary:
        print("::error::The suite did not print its summary: it hung or ended early")
    if not passing and not failing:
        print("::error::No test result was found in the output")
        return 1
    return 1 if new_failures or not has_summary else 0


if __name__ == "__main__":
    sys.exit(main())
