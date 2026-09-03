import struct
p = r"D:\Program Files (x86)\Steam\steamapps\common\TheEvilWithin\base\common.tangoresource"
d = open(p,'rb').read()
cnt = struct.unpack_from(">I", d, 4)[0]

def rdstr(o):
    ln = struct.unpack_from("<I", d, o)[0]
    if ln == 0 or ln > 1024 or o+4+ln > len(d): return None, o
    s = d[o+4:o+4+ln]
    if not all(32 <= c < 127 for c in s): return None, o
    return s.decode(), o+4+ln

off = 8
recs = []
while True:
    a, o2 = rdstr(off)
    if a is None: break
    b, o3 = rdstr(o2)
    if b is None: break
    if o3 + 4 > len(d): break
    tail = struct.unpack_from("<I", d, o3)[0]
    recs.append((a, b, tail, off))
    off = o3 + 4
print("records parsed: %d   (header count = %d)   TOC ends at 0x%X of 0x%X" % (len(recs), cnt, off, len(d)))
print("bytes after TOC:", len(d)-off)
print("next 64 bytes:", d[off:off+64].hex())
sb = [r for r in recs if r[1].endswith('.shaderbin2')]
print("shaderbin2 records:", len(sb))
for r in sb[:5]: print("   ", r[1], "| tail=0x%08X" % r[2])
