#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# convert_testpattern.py
#
# Convert [TESTPATTERN] logs to C++ calls using vdp_cartridge_write_io().
# - For write events, emit direct port writes: vdp_cartridge_write_io(port, value)
# - Control-port/address assembly is NOT performed here (the CSV path handles finer control)
# - Frame filter accepts: single (50), range (34-35) or comma list (10,12-14,20).
# - For writes/changes: skip printing step_cycles(0).
# - For VSYNC: always print step_cycles(delta).
#
# NOTE: printed step_cycles(...) argument is multiplied by 4 so that
# the generated main.cpp uses wrapper cycles (main clock) = 4 * openMSX emu-cycles.
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

# Only current scheduleDisplayStart format is supported:
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
            'time': time_val,        # raw log units
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

    printed_header = False
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

            # convert delta (in log units / emu-cycles) to wrapper main cycles by *4
            step_cycles_count = delta * 4

            if is_match and not printed_header:
                print('// Converted test pattern (auto-generated)')
                printed_header = True

            if parsed['type'] == 'vsync':
                if is_match:
                    print(f'std::cout << "frame={parsed["frame"]} time={parsed["time"]}" << std::endl;')
                    print(f'step_cycles({step_cycles_count});')
                    printed_any = True
                continue

            if parsed['type'] == 'change':
                if is_match:
                    reg_text = parsed.get('reg_text', format_hex(parsed['reg'],2))
                    val_text = parsed.get('val_text', format_hex(parsed['val'],2))
                    print(f'std::cout << "frame={parsed["frame"]} time={parsed["time"]} reg={reg_text} val={val_text}" << std::endl;')
                    if delta > 0:
                        print(f'step_cycles({step_cycles_count});')
                    printed_any = True
                continue

            # write event: output direct port write
            if parsed['type'] == 'write':
                port = parsed['port']
                value = parsed['value']
                port_text = parsed.get('port_text', format_hex(port,2))
                value_text = parsed.get('value_text', format_hex(value,2))

                if is_match:
                    print(f'std::cout << "frame={parsed["frame"]} time={parsed["time"]} port={port_text} value={value_text}" << std::endl;')
                    if delta > 0:
                        print(f'step_cycles({step_cycles_count});')
                    # write using port directly
                    print(f'vdp_cartridge_write_io({port_text}, {value_text});')
                    printed_any = True
                continue

    if not printed_header:
        return 1
    return 0

if __name__ == '__main__':
    sys.exit(main(sys.argv))