import re, math
from collections import defaultdict
LOG=r"C:\Users\Tefa\AppData\Local\TEWVR\cbdump.log"
rec_re=re.compile(r'^\[.*?\]\s+res=(0x[0-9A-Fa-f]+)\s+size=(\d+)')
row_re=re.compile(r'^\s+r\d+:\s*(.*)$')
data=defaultdict(list)
cur=None; buf=[]
def flush():
    global cur,buf
    if cur and buf: data[cur].append(buf)
    cur=None; buf=[]
with open(LOG,errors='replace') as f:
    for ln in f:
        m=rec_re.match(ln)
        if m: flush(); cur=m.group(1); buf=[]; continue
        r=row_re.match(ln)
        if r and cur:
            for t in r.group(1).split():
                try: buf.append(float(t))
                except: buf.append(float('nan'))
flush()

def pm(f16,lbl):
    print(f"  {lbl}:")
    for i in range(4):
        print("    ", " ".join(f"{f16[i*4+j]:10.4f}" for j in range(4)))

for res in ["0x21AD3E186B8","0x21A918DB238"]:
    recs=data.get(res,[])
    print(f"\n===== {res}  ({len(recs)} records) =====")
    # sample records spread across the capture
    idxs=[0, len(recs)//4, len(recs)//2, 3*len(recs)//4, len(recs)-1]
    for k,ix in enumerate(idxs):
        r=recs[ix]
        print(f"-- record #{ix} --  (floats available: {len(r)})")
        if len(r)>=16: pm(r[0:16], "m0 (first matrix)")
        if len(r)>=32: pm(r[16:32], "m1 (second matrix)")
