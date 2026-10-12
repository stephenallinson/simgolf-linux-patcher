# Group sghooks draw-log lines by the golf.exe functions that issued them.
# Usage: python3 drawmap.py <decompiled golf.exe.c> <sghooks.log>
import re, sys, bisect, collections
starts = sorted(int(m.group(1), 16) for m in re.finditer(r'^// ==== \S+ @ ([0-9a-f]+)', open(sys.argv[1], encoding='latin1').read(), re.M))
def fn(a):
    i = bisect.bisect_right(starts, a) - 1
    return f"{starts[i]:06x}" if i >= 0 and a else "-"
groups = collections.defaultdict(list)
for line in open(sys.argv[2]):
    m = re.match(r'draw (\S+) obj \S+ at (-?\d+),(-?\d+) size (\d+)x(\d+) callers (\S+) (\S+) (\S+) (\S+)', line)
    if not m: continue
    kind, x, y, w, h = m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5))
    chain = tuple(fn(int(c, 16)) for c in m.groups()[5:])
    groups[chain[1:3]].append((kind, x, y, w, h, chain[0]))
for chain, l in sorted(groups.items(), key=lambda kv: -len(kv[1])):
    xs = [d[1] for d in l]; ys = [d[2] for d in l]
    kinds = collections.Counter(d[0] for d in l)
    print(f"{len(l):4d} draws via {' <- '.join(chain)}: x {min(xs)}..{max(xs)} y {min(ys)}..{max(ys)} {dict(kinds)}")
