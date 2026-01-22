#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# generate_testpattern_csv.py
#
# Convert [TESTPATTERN] logs to a simple CSV instruction stream.
# - For write events, emit: IO,port,value
# - For VSYNC events, emit: CYCLE,<cycles*4>
# - For changeRegister events, emit: INFO,"VDP.changeRegister: frame=... time=... reg=... val=..."
#
# NOTE: step cycle counts are multiplied by 4 to map openMSX emu-cycles -> wrapper main cycles.
#
import sys
import re
from pathlib import Path

RE_WRITE = re.compile(
    r'^\[IO\]VDP\.writeIO:\s*frame=(\d+)\s+time=(\d+)\s+port=((?:0x[0-9A-Fa-f]+)|\d+)\s+value=((?:0x[0-9A-Fa-f]+)|\d+)\s+\[TESTPATTERN\]$')
RE_VSYNC = re.compile(
    r'^\[VSYNC\]VDP\.execVSync:\s*frame=(\d+)\s+time=(\d+)\s+\[TESTPATTERN\]$')
RE_CHANGE = re.compile(
    r'^\[INFO\]VDP\.changeRegister:\s*frame=(\d+)\s+time=(\d+)\s+reg=(0x[0-9A-Fa-f]+|\d+)\s+val=(0x[0-9A-Fa-f]+|\d+)\s+\[TESTPATTERN\]$',
    re.IGNORECASE)

SCHEDULE_DISPLAY_PREFIX = '[DISPLAY][RESET]VDP.scheduleDisplayStart:'

def format_hex(value, width=2):
    return f'0x{(value & 0xFFFF):0{width}x}'

def parse_line(line):
    line = line.rstrip('\n')
    m = RE_WRITE.match(line)
    if m:
        frame_s, time_s, port_s, value_s = m.groups()
        try:
            port_val = int(port_s, 0)
            value_val = int(value_s, 0)
            time_val = int(time_s, 0)
        except ValueError:
            return None
        return {
            'type': 'write',
            'frame': int(frame_s),
            'time': time_val,
            'port': port_val,
            'value': value_val,
            'port_text': port_s,
            'value_text': value_s,
            'orig_line': line,
        }
    m = RE_VSYNC.match(line)
    if m:
        frame_s, time_s = m.groups()
        try:
            time_val = int(time_s, 0)
        except ValueError:
            return None
        return {
            'type': 'vsync',
            'frame': int(frame_s),
            'time': time_val,
            'orig_line': line,
        }
    m = RE_CHANGE.match(line)
    if m:
        frame_s, time_s, reg_s, val_s = m.groups()
        try:
            reg_v = int(reg_s, 0)
            val_v = int(val_s, 0)
            time_val = int(time_s, 0)
        except ValueError:
            return None
        return {
            'type': 'change',
            'frame': int(frame_s),
            'time': time_val,
            'reg': reg_v,
            'val': val_v,
            'reg_text': reg_s,
            'val_text': val_s,
            'orig_line': line,
        }
    return None

def parse_frame_spec(spec):
    if spec is None:
        return lambda f: True
    spec = spec.strip()
    if spec == '':
        return lambda f: True
    parts = [p.strip() for p in spec.split(',') if p.strip()]
    ranges = []
    for p in parts:
        if '-' in p:
            a,b = p.split('-', 1)
            lo = int(a, 0); hi = int(b, 0)
            if lo > hi:
                raise ValueError(f"invalid range (lo>hi): {p}")
            ranges.append((lo, hi))
        else:
            v = int(p, 0)
            ranges.append((v, v))
    def matcher(frame):
        for lo,hi in ranges:
            if lo <= frame <= hi:
                return True
        return False
    return matcher

def main(argv):
    if len(argv) < 2:
        print("Usage: {} <input-file> [frame-spec]".format(argv[0]), file=sys.stderr)
        print("frame-spec examples: 50, 34-35, 10,12-14,20", file=sys.stderr)
        return 2

    frame_spec = None
    if len(argv) >= 3:
        frame_spec = argv[2]

    try:
        frame_match = parse_frame_spec(frame_spec)
    except ValueError as e:
        print("Frame spec error:", e, file=sys.stderr)
        return 2

    path = Path(argv[1])
    if not path.exists():
        print("Warning: file not found: {}".format(argv[1]), file=sys.stderr)
        return 1

    printed_any = False
    last_time = 0

    with path.open('r', encoding='utf-8', errors='replace') as f:
        for raw in f:
            line = raw.rstrip('\n')

            if line.startswith(SCHEDULE_DISPLAY_PREFIX):
                last_time = 0
                continue

            parsed = parse_line(line)
            if parsed is None:
                continue

            is_match = frame_match(parsed.get('frame'))

            cur_time = parsed['time']
            delta = cur_time - last_time
            if delta < 0:
                delta = 0
            last_time = cur_time

            step_cycles_count = delta * 4

            if parsed['type'] == 'vsync':
                if is_match:
                    # Output informational line and cycle advance
                    print(f'INFO,"frame={parsed["frame"]} time={parsed["time"]}"')
                    print(f'CYCLE,{step_cycles_count}')
                    printed_any = True
                continue

            if parsed['type'] == 'change':
                if is_match:
                    # Emit change register as INFO with descriptive text
                    txt = f'VDP.changeRegister: frame={parsed["frame"]} time={parsed["time"]} reg={format_hex(parsed["reg"],2)} val={format_hex(parsed["val"],2)}'
                    print(f'INFO,"{txt}"')
                    printed_any = True
                continue

            if parsed['type'] == 'write':
                if is_match:
                    # Emit IO line: IO,port,value
                    port_text = parsed.get('port_text', format_hex(parsed['port'],2))
                    value_text = parsed.get('value_text', format_hex(parsed['value'],2))
                    print(f'IO,{port_text},{value_text}')
                    # Also emit timing if delta>0 (optional; keep separate CYCLE lines for VSYNC)
                    if delta > 0:
                        print(f'CYCLE,{step_cycles_count}')
                    printed_any = True
                continue

    if not printed_any:
        return 1
    return 0

if __name__ == '__main__':
    sys.exit(main(sys.argv))