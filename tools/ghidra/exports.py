# List a PE file's named exports as "<address> <name>".
# Usage: python3 exports.py <file>
import sys, struct
d = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]; opt = pe + 24
base = struct.unpack_from('<I', d, opt + 28)[0]
exp_rva = struct.unpack_from('<I', d, opt + 96)[0]
nsec = struct.unpack_from('<H', d, pe + 6)[0]; optsz = struct.unpack_from('<H', d, pe + 20)[0]
secs = [struct.unpack_from('<8sIIII', d, opt + optsz + i * 40) for i in range(nsec)]
def off(r):
    for n, vs, va, rs, ro in secs:
        if va <= r < va + max(vs, rs): return r - va + ro
e = off(exp_rva)
nfun, nnames, af, an, ao = struct.unpack_from('<5I', d, e + 20)
for i in range(nnames):
    nm = d[off(struct.unpack_from('<I', d, off(an) + 4 * i)[0]):].split(b'\0')[0].decode('latin1')
    o = struct.unpack_from('<H', d, off(ao) + 2 * i)[0]
    print('%08x %s' % (base + struct.unpack_from('<I', d, off(af) + 4 * o)[0], nm))
