#!/usr/bin/env python3
"""基准阈值检查：按 bench 段解析，关键数字回归防线。低于阈值 → exit 1。"""
import re, sys

log = open(sys.argv[1]).read()

def section(name):
    """取 '== build/bench_NAME ==' 到下一个 '== build/' 之间的内容"""
    m = re.search(r'== build/bench_%s ==\n(.*?)(?:\n== build/|$)' % name, log, re.S)
    return m.group(1) if m else ""

def grab(text, pat, group=1):
    m = re.search(pat, text)
    return float(m.group(group)) if m else None

fails = []

def check(name, val, limit, should_be_lt=True):
    """should_be_lt=True: 值应 < limit（超标=坏）；False: 值应 > limit"""
    if val is None:
        fails.append(f"{name}: 未找到输出"); return
    bad = val >= limit if should_be_lt else val <= limit
    if bad:
        fails.append(f"{name}: {val:.2f} ({'应<=' if should_be_lt else '应>='} {limit})")

# M5 rbin（bench_data）：1 万消息 load 必须 < 50ms
d = section("data")
check("rbin load (1万消息)", grab(d, r'load\s+:\s+([\d.]+) ms'), 50.0, True)

# M6 SPSC（bench_pipe）：吞吐 > 5 Mops
p = section("pipe")
check("SPSC 吞吐", grab(p, r'throughput\s+:\s+([\d.]+) Mops'), 5.0, False)

# M4a 高亮（bench_highlight）：< 2000 ns/line
h = section("highlight")
check("高亮 ns/line", grab(h, r'per line\s+:\s+(\d+) ns'), 2000.0, True)

# M2 流式累积（bench_message）：> 100x
m = section("message")
check("流式 speedup", grab(m, r'speedup\s+:\s+([\d.]+)x'), 100.0, False)

# M1 JSON 增量（bench_json）：> 1.5x
j = section("json")
check("JSON speedup", grab(j, r'speedup\s+:\s+([\d.]+)x'), 1.5, False)

if fails:
    print("BENCH REGRESSION:")
    for f in fails: print("  ✗", f)
    sys.exit(1)
print("bench thresholds OK: rbin-load/SPSC/highlight/stream/JSON")
