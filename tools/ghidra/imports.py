# List a PE file's imports as "<IAT address> <dll>!<name>".
# Usage: python3 imports.py <file>
import struct, sys
d = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]
nsec = struct.unpack_from('<H', d, pe + 6)[0]
optsz = struct.unpack_from('<H', d, pe + 20)[0]
opt = pe + 24
base = struct.unpack_from('<I', d, opt + 28)[0]
imp_rva = struct.unpack_from('<I', d, opt + 96 + 8)[0]
secs = [struct.unpack_from('<8sIIII', d, opt + optsz + i * 40) for i in range(nsec)]
def off(rva):
    for n, vs, va, rs, ro in secs:
        if va <= rva < va + max(vs, rs): return rva - va + ro
i = off(imp_rva)
while True:
    oft, _, _, name, ft = struct.unpack_from('<5I', d, i)
    if not name: break
    dll = d[off(name):].split(b'\0')[0].decode()
    j = 0
    while True:
        e = struct.unpack_from('<I', d, off(oft or ft) + j * 4)[0]
        if not e: break
        fn = d[off(e) + 2:].split(b'\0')[0].decode() if not e & 0x80000000 else '#%d' % (e & 0xffff)
        print('%#x %s!%s' % (base + ft + j * 4, dll, fn))
        j += 1
    i += 20
