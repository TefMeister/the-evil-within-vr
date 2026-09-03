import struct, zlib, collections
p = r"D:\Program Files (x86)\Steam\steamapps\common\TheEvilWithin\base\common.tangoresource"
d = open(p,'rb').read()
T = 0x2F77AB8
recs=[]; at=T
while at+16 <= len(d):
    off,c,u,i = struct.unpack_from(">IIII", d, at)
    if recs and recs[-1][0]+recs[-1][1] != off: break
    if off < 0x1902E8 or off+c > len(d) or c == 0: break
    recs.append((off,c,u,i)); at += 16
print("entries in the offset table: %d ; table 0x%X..0x%X" % (len(recs), T, at))
print("last entry: off=0x%X csize=%d usize=%d id=%d -> ends 0x%X" % (recs[-1][0],recs[-1][1],recs[-1][2],recs[-1][3],recs[-1][0]+recs[-1][1]))
print("id range %d..%d" % (recs[0][3], recs[-1][3]))
print("total compressed %d, total uncompressed %d" % (sum(r[1] for r in recs), sum(r[2] for r in recs)))

magics = collections.Counter(); dxbc=[]
fails=0
for k,(off,c,u,i) in enumerate(recs):
    blob = d[off:off+c]
    try:
        out = zlib.decompressobj(-15).decompress(blob, u+64)
    except zlib.error:
        fails += 1; continue
    magics[bytes(out[:4])] += 1
    if out[:4] == b'DXBC' or b'DXBC' in out[:4096]:
        dxbc.append((k,off,c,u,i,len(out)))
print("decompress failures: %d / %d" % (fails, len(recs)))
print("entries whose first 4 KB contain DXBC: %d" % len(dxbc))
print("most common first-4-bytes:")
for m,n in magics.most_common(12):
    print("   %-24r %d" % (m,n))
