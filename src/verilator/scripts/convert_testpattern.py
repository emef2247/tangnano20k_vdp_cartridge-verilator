#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# convert_testpattern.py
#
# Convert [TESTPATTERN] logs to C++ calls using vdp_cartridge_write_io().
# - Control-port writes are treated as 2-byte pairs: low then high -> 16-bit address.
# - Frame filter accepts: single (50), range (34-35) or comma list (10,12-14,20).
# - For writes/changes: skip printing step_cycles(0).
# - For VSYNC: always print step_cycles(delta).
# - When scheduleDisplayStart (current format "[DISPLAY][RESET]VDP.scheduleDisplayStart:")
#   is seen, reset the internal time counter so deltas inside the frame measure from 0.
#
# NOTE (updated): multiply printed step_cycles(...) argument by 4 so that
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

DATA_PORTS = (0x88, 0x98)
ADDR_PORTS = (0x89, 0x99)

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

    # Internal state maintained across all lines (updated even when not printed)
    address = 0                 # 16-bit composed address used for data writes
    control_low = None          # holds low byte when waiting for next control write
    last_time = 0               # last observed time for delta calculation

    with path.open('r', encoding='utf-8', errors='replace') as f:
        for raw in f:
            line = raw.rstrip('\n')

            # Only current format: detect scheduleDisplayStart and reset time base.
            if line.startswith(SCHEDULE_DISPLAY_PREFIX):
                last_time = 0
                continue

            parsed = parse_line(line)
            if parsed is None:
                continue

            is_match = frame_match(parsed.get('frame'))

            # compute delta from last_time (always, before updating last_time)
            cur_time = parsed['time']
            delta = cur_time - last_time
            if delta < 0:
                delta = 0
            last_time = cur_time

            # convert delta (in log units / emu-cycles) to wrapper main cycles by *4
            step_cycles_count = delta * 4

            # print header once if we're about to produce any printed output
            if is_match and not printed_header:
                print('// Converted test pattern (auto-generated)')
                print('// address variable holds VDP target address; initialized to current log state')
                print(f'uint16_t address = {format_hex(address,4)};')
                printed_header = True

            if parsed['type'] == 'vsync':
                if is_match:
                    print(f'std::cout << "frame={parsed["frame"]} time={parsed["time"]}" << std::endl;')
                    # For VSYNC we always print the step_cycles delta (even if zero)
                    print(f'step_cycles({step_cycles_count});')
                    printed_any = True
                continue

            if parsed['type'] == 'change':
                # convert changeRegister to cout + step_cycles (no internal register emulation)
                if is_match:
                    reg_text = parsed.get('reg_text', format_hex(parsed['reg'],2))
                    val_text = parsed.get('val_text', format_hex(parsed['val'],2))
                    print(f'std::cout << "frame={parsed["frame"]} time={parsed["time"]} reg={reg_text} val={val_text}" << std::endl;')
                    if delta > 0:
                        print(f'step_cycles({step_cycles_count});')
                    printed_any = True
                # no internal state change in this converter
                continue

            # write event
            port = parsed['port']
            value = parsed['value']
            port_text = parsed.get('port_text', hex(port))
            value_text = parsed.get('value_text', hex(value))

            # If matched, print debug line and step_cycles delta (skip delta==0)
            if is_match:
                print(f'std::cout << "frame={parsed["frame"]} time={parsed["time"]} port={port_text} value={value_text}" << std::endl;')
                if delta > 0:
                    print(f'step_cycles({step_cycles_count});')
                printed_any = True

            # Handle control (address) writes as LOW/HIGH pair
            if port in ADDR_PORTS:
                if control_low is None:
                    # store as low byte, do not update address yet
                    control_low = value & 0xFF
                else:
                    # This write is treated as high byte; compose 16-bit address
                    high = value & 0xFF
                    new_addr = ((high << 8) | control_low) & 0xFFFF
                    if is_match:
                        print(f'address = {format_hex(new_addr,4)};  // set address (high={format_hex(high,2)} low={format_hex(control_low,2)})')
                    address = new_addr
                    control_low = None
                continue

            # Data port: do write using current internal address, then increment
            if port in DATA_PORTS:
                if is_match:
                    print(f'vdp_cartridge_write_io(address, {format_hex(value,2)});  // data={format_hex(value,2)}')
                    print('address++;')
                address = (address + 1) & 0xFFFF
                continue

            # Unknown port
            if is_match:
                print(f'// UNHANDLED-PORT: {parsed["orig_line"]}')

    if not printed_header:
        # nothing printed (no matching lines)
        return 1
    return 0

if __name__ == '__main__':
    sys.exit(main(sys.argv))