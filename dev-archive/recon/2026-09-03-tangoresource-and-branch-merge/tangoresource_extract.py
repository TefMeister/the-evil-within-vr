import struct, zlib
p = r"D:\Program Files (x86)\Steam\steamapps\common\TheEvilWithin\base\common.tangoresource"
d = open(p,'rb').read(); T = 0x2F77AB8
recs=[]; at=T
while at+16 <= len(d):
    off,c,u,i = struct.unpack_from(">IIII", d, at)
    if recs and recs[-1][0]+recs[-1][1] != off: break
    if off < 0x1902E8 or off+c > len(d) or c == 0: break
    recs.append((off,c,u,i)); at += 16
n_entries = 0; n_blobs = 0
with open("tew_all_shaders.bin","wb") as w:
    for off,c,u,i in recs:
        try: out = zlib.decompressobj(-15).decompress(d[off:off+c], u+64)
        except zlib.error: continue
        if b'DXBC' not in out: continue
        n_entries += 1
        j = 0
        while True:
            j = out.find(b'DXBC', j)
            if j < 0: break
            sz = struct.unpack_from("<I", out, j+24)[0]
            if 64 <= sz <= 4*1024*1024 and j+sz <= len(out):
                w.write(out[j:j+sz]); n_blobs += 1
            j += 4
print("entries yielding shaders: %d ; DXBC containers written: %d" % (n_entries, n_blobs))
