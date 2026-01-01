#!/usr/bin/env python3
# name=analyze_ticks_fixed.py
# usage: python3 analyze_ticks_fixed.py run.log
import sys, re

if len(sys.argv) < 2:
    print("usage: analyze_ticks_fixed.py run.log")
    sys.exit(1)

fn = sys.argv[1]
lines = open(fn, 'r', encoding='utf-8').read().splitlines()

# Only capture a single representative mark per edge: use POS_EXIT and NEG_EXIT
pos_re = re.compile(r'MARK:\s*POS_EXIT\s.*time=(\d+)')
neg_re = re.compile(r'MARK:\s*NEG_EXIT\s.*time=(\d+)')
half_re = re.compile(r'HALF_TICK: level=(\d+) (BEFORE|AFTER) dump at time=(\d+) tick=(\d+)')

pos_times = []
neg_times = []
half_before = []
half_after = []

for ln in lines:
    m = pos_re.search(ln)
    if m:
        pos_times.append(int(m.group(1)))
        continue
    m = neg_re.search(ln)
    if m:
        neg_times.append(int(m.group(1)))
        continue
    mh = half_re.search(ln)
    if mh:
        level = int(mh.group(1))
        when = mh.group(2)
        t = int(mh.group(3))
        tick = int(mh.group(4))
        if when == "BEFORE":
            half_before.append((tick, level, t))
        else:
            half_after.append((tick, level, t))

def diffs(arr):
    return [arr[i+1]-arr[i] for i in range(len(arr)-1)]

MAIN_CYCLE_PS = 23270
HALF_CYCLE_PS = MAIN_CYCLE_PS//2

print("Fixed analysis from", fn)
print("POS edges (POS_EXIT):", len(pos_times), "NEG edges (NEG_EXIT):", len(neg_times))
if len(pos_times) >= 2:
    p_d = diffs(pos_times)
    print("pos->pos intervals (ps): count", len(p_d))
    print("  min", min(p_d), "max", max(p_d), "mean", (sum(p_d)/len(p_d)))
    bad = [(i,d) for i,d in enumerate(p_d) if d != MAIN_CYCLE_PS]
    if bad:
        print("  INTERVALS != MAIN_CYCLE_PS (index,delta):")
        for i,d in bad[:50]:
            print("   ", i, d)
    else:
        print("  All pos->pos intervals == MAIN_CYCLE_PS (%d ps)" % MAIN_CYCLE_PS)
if len(neg_times) >= 2:
    n_d = diffs(neg_times)
    print("neg->neg intervals (ps): count", len(n_d))
    print("  min", min(n_d), "max", max(n_d), "mean", (sum(n_d)/len(n_d)))
    badn = [(i,d) for i,d in enumerate(n_d) if d != MAIN_CYCLE_PS]
    if badn:
        print("  INTERVALS != MAIN_CYCLE_PS (index,delta):")
        for i,d in badn[:50]:
            print("   ", i, d)
    else:
        print("  All neg->neg intervals == MAIN_CYCLE_PS (%d ps)" % MAIN_CYCLE_PS)

if half_before or half_after:
    print("\nHALF_TICK samples (BEFORE):", len(half_before), " (AFTER):", len(half_after))
    before_map = {t[0]:(t[1], t[2]) for t in half_before}
    after_map = {t[0]:(t[1], t[2]) for t in half_after}
    common = sorted(set(before_map.keys()) & set(after_map.keys()))
    mismatches = []
    for tick in common:
        lvl_b, tb = before_map[tick]
        lvl_a, ta = after_map[tick]
        if tb != ta or tb is None or ta is None or tb != lvl_a or tb != lvl_a or tb != tb:
            pass
        if tb != ta:
            mismatches.append((tick, lvl_b, tb, lvl_a, ta))
    if mismatches:
        print("HALF_TICK mismatched BEFORE/AFTER times (tick, lvl_before, t_before, lvl_after, t_after):")
        for mm in mismatches[:50]:
            print(mm)
    else:
        print("HALF_TICK BEFORE/AFTER times match for first", len(common), "ticks.")

print("\nIf pos->pos and neg->neg intervals equal MAIN_CYCLE_PS, timing is consistent.")

