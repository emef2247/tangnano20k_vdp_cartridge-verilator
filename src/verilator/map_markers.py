#!/usr/bin/env python3
"""
map_markers.py

Usage:
  python3 map_markers.py markers.txt run.log            # print human mapping
  python3 map_markers.py markers.txt run.log --csv out.csv --threshold 5000

markers.txt format:
  - Each non-empty non-comment line contains either:
      LABEL TIME
      or
      TIME LABEL
    where TIME is integer picoseconds (ps). Comments start with '#'.

Log parsing:
  - Recognizes log lines containing:
      MARK: <TAG> ... time=<NUMBER>
    and also
      DERIVED ... at sim_time=<NUMBER>
    and returns those entries as candidate mapping points.

Output:
  - For each input marker (label, time), finds the nearest log entry by absolute time difference
    and prints a table with delta (marker_time - log_time).
"""
import sys
import re
import argparse
import csv

MARK_RE = re.compile(r"MARK:\s*(\S+).*?time=(\d+)")
DERIVED_RE = re.compile(r"DERIVED\b.*?at sim_time=(\d+)")
# fallback: any "time=<num>" after "MARK" word
MARK_FALLBACK_RE = re.compile(r"MARK:.*?time=(\d+)")

def read_markers(path):
    markers = []
    with open(path, 'r', encoding='utf-8') as f:
        for ln in f:
            s = ln.strip()
            if not s or s.startswith('#'): continue
            parts = s.split()
            if len(parts) == 1:
                # single token: try to parse as integer (time) and make label auto
                try:
                    t = int(parts[0])
                    label = f"M_{t}"
                    markers.append((label, t))
                except:
                    markers.append((parts[0], None))
            elif len(parts) >= 2:
                # either LABEL TIME or TIME LABEL. detect which is numeric
                if parts[0].lstrip('-').isdigit():
                    time = int(parts[0])
                    label = parts[1]
                elif parts[1].lstrip('-').isdigit():
                    label = parts[0]
                    time = int(parts[1])
                else:
                    # fallback: try last token numeric
                    if parts[-1].lstrip('-').isdigit():
                        time = int(parts[-1])
                        label = ' '.join(parts[:-1])
                    else:
                        # no numeric: skip
                        continue
                markers.append((label, time))
    return markers

def read_log_entries(path):
    entries = []  # list of (time:int, tag:str, raw:str)
    with open(path, 'r', encoding='utf-8') as f:
        for ln in f:
            m = MARK_RE.search(ln)
            if m:
                tag = m.group(1)
                time = int(m.group(2))
                entries.append((time, f"MARK:{tag}", ln.rstrip('\n')))
                continue
            m2 = DERIVED_RE.search(ln)
            if m2:
                time = int(m2.group(1))
                entries.append((time, "DERIVED", ln.rstrip('\n')))
                continue
            # fallback: mark lines that contain "MARK:" and a time=...
            if "MARK:" in ln:
                m3 = MARK_FALLBACK_RE.search(ln)
                if m3:
                    time = int(m3.group(1))
                    entries.append((time, "MARK:?", ln.rstrip('\n')))
                    continue
    entries.sort(key=lambda x: x[0])
    return entries

def find_nearest(entries, t):
    """binary search nearest by time"""
    if not entries:
        return None
    lo = 0
    hi = len(entries)-1
    if t <= entries[0][0]:
        return entries[0]
    if t >= entries[-1][0]:
        return entries[-1]
    # binary search for insertion point
    while lo + 1 < hi:
        mid = (lo + hi) // 2
        if entries[mid][0] == t:
            return entries[mid]
        if entries[mid][0] < t:
            lo = mid
        else:
            hi = mid
    # lo < hi and entries[lo].time < t < entries[hi].time
    if abs(entries[lo][0] - t) <= abs(entries[hi][0] - t):
        return entries[lo]
    else:
        return entries[hi]

def main():
    parser = argparse.ArgumentParser(description="Map GTKWave markers (label,time) to MARK/DERIVED log entries.")
    parser.add_argument("markers", help="markers file (label time or time label per line)")
    parser.add_argument("logfile", help="log file to scan for MARK/DERIVED entries")
    parser.add_argument("--csv", help="write CSV output to file")
    parser.add_argument("--threshold", type=int, default=10000, help="warn if delta (ps) > threshold (default 10000 ps)")
    parser.add_argument("--ps", action="store_true", help="treat times as ps (default); no-op, for clarity")
    args = parser.parse_args()

    markers = read_markers(args.markers)
    if not markers:
        print("No markers parsed from", args.markers, file=sys.stderr)
        sys.exit(2)
    entries = read_log_entries(args.logfile)
    if not entries:
        print("No MARK/DERIVED entries found in log:", args.logfile, file=sys.stderr)
        sys.exit(2)

    results = []
    for label, mtime in markers:
        if mtime is None:
            results.append((label, None, None, None, "NO_TIME"))
            continue
        nearest = find_nearest(entries, mtime)
        if nearest is None:
            results.append((label, mtime, None, None, "NO_ENTRY"))
            continue
        ltime, ltag, raw = nearest
        delta = mtime - ltime
        status = "OK" if abs(delta) <= args.threshold else "WARN"
        results.append((label, mtime, ltag, ltime, delta if delta is not None else None, status, raw))

    # Print table
    print("Marker  marker_time(ps)  log_tag        log_time(ps)  delta(ps)  status")
    print("----------------------------------------------------------------------")
    for row in results:
        if len(row) == 4:
            print(f"{row[0]:<7}  {row[1]:>13}  NO_LOG")
            continue
        label, mtime, ltag, ltime, delta, status, raw = row
        print(f"{label:<7}  {mtime:>13}  {ltag:<12}  {ltime:>12}  {delta:>9}  {status}")
    # Optionally write CSV
    if args.csv:
        with open(args.csv, 'w', newline='', encoding='utf-8') as csvf:
            w = csv.writer(csvf)
            w.writerow(["marker_label","marker_time_ps","log_tag","log_time_ps","delta_ps","status","log_line"])
            for row in results:
                if len(row) == 4:
                    w.writerow([row[0], row[1], "", "", "", "NO_LOG", ""])
                else:
                    label, mtime, ltag, ltime, delta, status, raw = row
                    w.writerow([label, mtime, ltag, ltime, delta, status, raw])
        print("Wrote CSV to", args.csv)

if __name__ == "__main__":
    main()

