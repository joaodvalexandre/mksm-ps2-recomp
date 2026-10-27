#!/usr/bin/env python3
"""Summarizes windows of the optional profile. Times are elapsed and exclusive per thread."""
from __future__ import annotations
import argparse
from bisect import bisect_left, bisect_right
import json
import math
import re
import sys
from pathlib import Path

TIME_KEYS = ('ee_ms', 'vu_ms', 'gs_ms', 'iop_ms', 'guest_wait_ms', 'upload_ms', 'host_wait_ms')
REQUIRED_KEYS = ('t', 'window', 'guest_flip_hz', 'host_hz')


def parse_window(line: str) -> dict:
    if line.count('[gow-perf]') != 1:
        raise ValueError('interleaved profile record')
    fields = re.findall(r'(\w+)=([^\s]+)', line.split('[gow-perf]', 1)[1])
    row = {}
    for key, value in fields:
        if key not in (*REQUIRED_KEYS, *TIME_KEYS):
            continue
        if key in row:
            raise ValueError(f'repeated profile field: {key}')
        try:
            number = float(value)
        except ValueError as error:
            raise ValueError(f'invalid profile value: {key}') from error
        if not math.isfinite(number) or number < 0:
            raise ValueError(f'invalid profile value: {key}')
        row[key] = number
    if not all(key in row for key in REQUIRED_KEYS) or row['window'] <= 0:
        raise ValueError('incomplete profile record or empty window')
    if row['t'] - row['window'] < -0.011:
        raise ValueError('profile window earlier than its origin')
    return row


def select_windows(text: str, start_time: float, end_time: float, in_game: bool = False) -> list[dict]:
    """With in_game, requires loaded states that enclose each profile window.

    The controller clock starts at its first read and the profile clock at the
    presentation. The order of the lines is used; those clocks are never equated.
    """
    if '[gow-perf:invalid]' in text:
        raise ValueError('profile invalidated during the run; repeat without external load')
    rows = []
    positions = []
    runs = []
    current = []
    for position, line in enumerate(text.splitlines()):
        if '[gow-perf]' in line:
            row = parse_window(line)
            # Both numbers are printed separately to hundredths. Gaps are allowed
            # without inventing samples, but overlaps/restarts are not.
            if rows and (row['t'] <= rows[-1]['t'] or
                         row['t'] - row['window'] < rows[-1]['t'] - 0.011):
                raise ValueError('overlapping profile windows or restarted clock')
            rows.append(row)
            positions.append(position)
        elif '[gow-pad2:state]' in line:
            state = {key: int(value) for key, value in re.findall(r'(state|pending|levelReady)=(\d+)', line)}
            if state == {'state': 11, 'pending': 0, 'levelReady': 1}:
                current.append(position)
            else:
                if len(current) >= 2:
                    runs.append(current)
                current = []
    if len(current) >= 2:
        runs.append(current)

    spans = []
    if in_game:
        for run in runs:
            # The first report after the initial state bounds the start from
            # above; the last one before the final state bounds the end from
            # below. This excludes the windows that touch loading or transition.
            first = bisect_right(positions, run[0])
            last = bisect_left(positions, run[-1]) - 1
            if first <= last and first < len(rows):
                spans.append((rows[first]['t'], rows[last]['t']))

    selected = []
    rounding = 0.011 if in_game else 0
    for row in rows:
        start, end = row['t'] - row['window'], row['t']
        if start < start_time - rounding or end > end_time + rounding:
            continue
        # t and window are printed rounded to hundredths separately.
        if in_game and not any(start >= low - 0.011 and end <= high + 0.011 for low, high in spans):
            continue
        selected.append(row)
    return selected


def summarize_windows(rows: list[dict]) -> dict:
    if not rows:
        raise ValueError('there are no profile windows')
    seconds = sum(row['window'] for row in rows)
    # Do not replace absent scopes by a zero measurement. Times accumulate
    # per scope from several threads and do not add up to a single game thread.
    totals = {key: sum(row[key] for row in rows) if all(key in row for row in rows) else None
              for key in TIME_KEYS}
    measured = sum(value for value in totals.values() if value is not None)
    return {
        'windows': len(rows), 'seconds': seconds,
        'from': rows[0]['t']-rows[0]['window'], 'to': rows[-1]['t'],
        'guest_flip_hz': sum(row['guest_flip_hz']*row['window'] for row in rows)/seconds,
        'host_hz': sum(row['host_hz']*row['window'] for row in rows)/seconds,
        'time_ms': totals if measured > 0 else None,
        'instrumented_time_share_pct': {
            key: 100*value/measured if value is not None else None for key, value in totals.items()
        } if measured > 0 else None,
        'metrics': {
            'guest_flip_hz': 'Entries to vid::Flip per second; not unique frames shown.',
            'host_hz': 'Host iterations completed per second; can repeat a texture.',
        },
        'time_limits': ('Elapsed time exclusive within each thread; threads are added up and '
                        'can overlap. Not CPU use nor a split of a single game thread.')
                       if measured > 0 else
                       ('No positive times recorded: timers disabled or scopes '
                        'without enough samples. Does not prove absence of work or waits.'),
    }


def main() -> int:
    # JSON output and errors reproducible on Windows too, with non-English paths/texts.
    sys.stdout.reconfigure(encoding='utf-8')
    sys.stderr.reconfigure(encoding='utf-8')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--from', type=float, default=100, dest='start_time')
    parser.add_argument('--to', type=float, default=145, dest='end_time')
    parser.add_argument('--in-game', action='store_true',
                        help='require state=11, pending=0 and levelReady=1 before and after the windows')
    parser.add_argument('--json', type=Path, dest='output')
    args = parser.parse_args()
    try:
        rows = select_windows(args.log.read_text(encoding='utf-8', errors='replace'),
                              args.start_time, args.end_time, args.in_game)
    except ValueError as error:
        parser.error(str(error))
    if not rows:
        qualifier = ' of the loaded game' if args.in_game else ''
        parser.error(f'there are no complete windows{qualifier} in the chosen interval')
    result = summarize_windows(rows)
    if args.in_game:
        result['selection'] = 'observed game: state=11, pending=0, levelReady=1'
    print(json.dumps(result, ensure_ascii=False, indent=2))
    if args.output:
        args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
