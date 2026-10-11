# Build a call graph from a Ghidra DecompileAll .c dump and summarise what each
# function in a list reaches (Windows APIs, within N levels).
# Usage: python3 callgraph.py <decompiled .c> <list: lines ending in a hex address> <depth>
import re, sys
src, listfile, depth = sys.argv[1], sys.argv[2], int(sys.argv[3])
bodies, cur, buf = {}, None, []
for line in open(src, encoding='latin1'):
    m = re.match(r'// ==== (\S+) @ ([0-9a-f]+)', line)
    if m:
        if cur: bodies[cur] = ''.join(buf)
        cur = int(m.group(2), 16); buf = []; continue
    buf.append(line)
if cur: bodies[cur] = ''.join(buf)
call = re.compile(r'\b((?:thunk_)?FUN_([0-9a-f]{8})|[A-Z][A-Za-z0-9]+[AW]?)\s*\(')
skip = {'ROUND', 'CONCAT', 'SUB', 'ZEXT', 'SEXT', 'POPCOUNT', 'CARRY', 'SCARRY', 'SBORROW'}
def calls(a):
    out_f, out_api = set(), set()
    for m in call.finditer(bodies.get(a, '')):
        if m.group(2): out_f.add(int(m.group(2), 16))
        elif not any(m.group(1).startswith(k) for k in skip) and len(m.group(1)) > 3: out_api.add(m.group(1))
    return out_f, out_api
thunk_target = {}
for a, b in bodies.items():
    m = re.search(r'thunk_FUN_[0-9a-f]+\(|^\s*\w+ = FUN_([0-9a-f]{8})', b)
def reach(a):
    seen, apis, frontier = {a}, set(), [a]
    for _ in range(depth):
        nxt = []
        for f in frontier:
            fs, ap = calls(f); apis |= ap
            for g in fs:
                if g not in seen: seen.add(g); nxt.append(g)
        frontier = nxt
    return apis, len(seen)
for line in open(listfile):
    parts = line.split()
    idx, a = parts[0], int(parts[-1], 16)
    apis, n = reach(a)
    size = len(bodies.get(a, '').splitlines())
    print(f"{idx:>3} {a:#x} lines={size:<4} reach={n:<4} {' '.join(sorted(apis))[:200]}")
