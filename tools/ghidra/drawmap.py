# Group sghooks draw-log lines by the golf.exe functions that issued them.
# Usage: python3 drawmap.py <decompiled golf.exe.c> <sghooks.log>
import re, sys, bisect, collections
starts = sorted(int(m.group(1), 16) for m in re.finditer(r'^// ==== \S+ @ ([0-9a-f]+)', open(sys.argv[1], encoding='latin1').read(), re.M))
def fn(a):
    i = bisect.bisect_right(starts, a) - 1
    return f"{starts[i]:06x}" if i >= 0 and a else "-"
groups = collections.defaultdict(list)
for line in open(sys.argv[2], errors='replace'):
    m = re.match(r'draw (\S+) obj \S+ at (-?\d+),(-?\d+) size (\d+)x(\d+) callers ((?:[0-9a-f]{8} ?)+)', line)
    if not m: continue
    kind, x, y, w, h = m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5))
    chain = [fn(int(c, 16)) for c in m.group(6).split()]
    # skip the generic draw/text helpers so groups name the code that decided the position
    helpers = {'473f60', '473df0', '473e60', '475c60', '475c90', '475d00', '4775b0', '477a50', '477da0', '477c30', '477e30', '404b70', '404bc0', '4049d0', '470140', '421b90', '473cb0', '40cc00', '4767a0', '476700', '476750', '477fc0', '478140'}
    useful = [c for c in chain if c not in helpers and c != '-'] or chain
    groups[tuple(useful[:2])].append((kind, x, y, w, h, chain[0]))
for chain, l in sorted(groups.items(), key=lambda kv: -len(kv[1])):
    xs = [d[1] for d in l]; ys = [d[2] for d in l]
    kinds = collections.Counter(d[0] for d in l)
    print(f"{len(l):4d} draws via {' <- '.join(chain)}: x {min(xs)}..{max(xs)} y {min(ys)}..{max(ys)} {dict(kinds)}")
