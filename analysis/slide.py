import re, math
from collections import defaultdict
LOG=r"C:\Users\Tefa\AppData\Local\TEWVR\cbdump.log"
rec_re=re.compile(r'^\[.*?\]\s+res=(0x[0-9A-Fa-f]+)\s+size=(\d+)')
row_re=re.compile(r'^\s+r\d+:\s*(.*)$')
data=defaultdict(list); size_of={}
cur=None; buf=[]
def flush():
    global cur,buf
    if cur and buf: data[cur].append(buf)
    cur=None; buf=[]
with open(LOG,errors='replace') as f:
    for ln in f:
        m=rec_re.match(ln)
        if m: flush(); cur=m.group(1); size_of[cur]=int(m.group(2)); buf=[]; continue
        r=row_re.match(ln)
        if r and cur:
            for t in r.group(1).split():
                try: buf.append(float(t))
                except: buf.append(float('nan'))
flush()

def finite(xs): return all(not(math.isnan(x) or math.isinf(x)) for x in xs)

def ortho_err_rows(w):
    rows=[(w[0],w[1],w[2]),(w[4],w[5],w[6]),(w[8],w[9],w[10])]
    if not finite([v for r in rows for v in r]): return 1e9
    dot=lambda a,b: sum(x*y for x,y in zip(a,b))
    e=0.0
    for r in rows: e+=abs(math.sqrt(max(dot(r,r),0))-1)
    for i in range(3):
        for j in range(i+1,3): e+=abs(dot(rows[i],rows[j]))
    return e

# collect per-(res,offset) stats over records
view_hits=[]
proj_hits=[]
for res,recs in data.items():
    if len(recs)<30: continue
    maxlen=min(len(r) for r in recs)
    # offsets to try (float granularity, but step 4 for matrix-ish alignment plus a few)
    for off in range(0, maxlen-15):
        # gather this window across records
        wins=[r[off:off+16] for r in recs if len(r)>=off+16 and finite(r[off:off+16])]
        if len(wins)<30: continue
        # temporal variance of the 9 rotation entries (0,1,2,4,5,6,8,9,10)
        rotidx=[0,1,2,4,5,6,8,9,10]
        var=0.0
        for k in rotidx:
            col=[w[k] for w in wins]; mu=sum(col)/len(col)
            var+=sum((x-mu)**2 for x in col)/len(col)
        var=math.sqrt(var/9)
        # ortho error on the mean window
        mean=[sum(w[k] for w in wins)/len(wins) for k in range(16)]
        oe=min(ortho_err_rows(mean), ortho_err_rows([mean[i] for i in [0,4,8,1,5,9,2,6,10,3,7,11,12,13,14,15]]))
        if oe<0.1 and var>0.02:
            # translation candidates = col 3 (w[3],w[7],w[11]) or row3 (w[12..14])
            tr_c=[wins[0][3],wins[0][7],wins[0][11]]
            view_hits.append((oe,var,res,off,size_of[res],len(wins)))
        # projection: static (var tiny), sparse, with a -1/+1 in a w-row/col
        allvar=0.0
        for k in range(16):
            col=[w[k] for w in wins]; mu=sum(col)/len(col)
            allvar+=sum((x-mu)**2 for x in col)/len(col)
        allvar=math.sqrt(allvar/16)
        zeros=sum(1 for v in mean if abs(v)<1e-4)
        # perspective signature: near-zero off-diagonals, |m[2][3]| or |m[3][2]| ~1, m[0][0]&m[1][1] nonzero moderate
        m=mean
        persp = (allvar<1e-3 and zeros>=8 and
                 (abs(abs(m[11])-1)<0.05 or abs(abs(m[14])-1)<0.05) and
                 abs(m[0])>0.3 and abs(m[5])>0.3 and abs(m[1])<1e-3 and abs(m[4])<1e-3)
        if persp:
            proj_hits.append((res,off,size_of[res],len(wins),mean))

print("=== VIEW hits (orthonormal + high temporal variance), by variance desc ===")
seen=set()
for oe,var,res,off,size,n in sorted(view_hits,key=lambda x:-x[1])[:20]:
    print(f"res={res} off={off}(byte {off*4}) size={size} n={n} ortho_err={oe:.4f} rot_var={var:.4f}")

print("\n=== PROJECTION hits (static perspective signature) ===")
for res,off,size,n,mean in proj_hits[:12]:
    print(f"res={res} off={off}(byte {off*4}) size={size} n={n}")
    for i in range(4):
        print("     ", " ".join(f"{mean[i*4+j]:10.5f}" for j in range(4)))
if not proj_hits:
    print("  (none matched the strict perspective signature in first 128 bytes)")
