import struct, importlib.util
spec = importlib.util.spec_from_file_location("dr", r"D:\claude video game stuff\github-backups-pd\flat-to-vr-RE-toolkit\tools\dxbc-reflect.py")
dr = importlib.util.module_from_spec(spec); spec.loader.exec_module(dr)

blob = open("tew_all_shaders.bin","rb").read()
def reflect(b):
    for base,_ in dr.find_dxbc(b):
        for fourcc, coff, csize in dr.chunks(b, base):
            if fourcc == b"RDEF":
                return dr.parse_rdef(b, coff)
    return []
# mirror mvptable.c's reflect_find_cb0(): prefer "constantBufferV", else the
# cbuffer bound at b0.  dxbc-reflect.py's parse_rdef gives buffers in RDEF
# order; bindings come from parse_bindings.
def bindings(b):
    for base,_ in dr.find_dxbc(b):
        for fourcc, coff, csize in dr.chunks(b, base):
            if fourcc == b"RDEF":
                return dr.parse_bindings(b, coff)
    return []

out = open("tew_expected.txt","w")
i = 0; n = 0
while i < len(blob):
    sz = struct.unpack_from("<I", blob, i+24)[0]
    s = blob[i:i+sz]; i += sz; n += 1
    cbs = reflect(s)
    cb = None
    for c in cbs:
        if c["name"] == "constantBufferV": cb = c
    if cb is None:
        b0 = [nm for nm,bp in bindings(s) if bp == 0]
        if b0:
            for c in cbs:
                if c["name"] == b0[0]: cb = c
    if cb is None:
        out.write("0 -1 -1 -1 -1\n"); continue
    v = {x["name"]: x["offset"] for x in cb["vars"]}
    r = [v.get("mvpmatrix"+ch, -1) for ch in "xyzw"]
    out.write("%d %d %d %d %d\n" % (cb["size"], r[0], r[1], r[2], r[3]))
out.close()
print("expectations written for %d shaders" % n)
