import re, sys, math
from collections import defaultdict

LOG = r"C:\Users\Tefa\AppData\Local\TEWVR\cbdump.log"

rec_re = re.compile(r'^\[.*?\]\s+res=(0x[0-9A-Fa-f]+)\s+size=(\d+)')
row_re = re.compile(r'^\s+r\d+:\s*(.*)$')

# res -> list of float-lists (each record's floats, up to 32)
data = defaultdict(list)
size_of = {}

cur_res = None
cur_floats = []
def flush():
    global cur_res, cur_floats
    if cur_res is not None and cur_floats:
        data[cur_res].append(cur_floats)
    cur_res = None
    cur_floats = []

with open(LOG, 'r', errors='replace') as fh:
    for line in fh:
        m = rec_re.match(line)
        if m:
            flush()
            cur_res = m.group(1)
            size_of[cur_res] = int(m.group(2))
            cur_floats = []
            continue
        rm = row_re.match(line)
        if rm and cur_res is not None:
            for tok in rm.group(1).split():
                try:
                    cur_floats.append(float(tok))
                except ValueError:
                    cur_floats.append(float('nan'))
flush()

def ortho_err(m16):
    # upper-left 3x3 rows (row-major): indices 0,1,2 / 4,5,6 / 8,9,10
    try:
        rows = [(m16[0],m16[1],m16[2]),(m16[4],m16[5],m16[6]),(m16[8],m16[9],m16[10])]
    except IndexError:
        return 1e9
    for r in rows:
        for v in r:
            if math.isnan(v) or math.isinf(v): return 1e9
    def dot(a,b): return sum(x*y for x,y in zip(a,b))
    err = 0.0
    for i in range(3):
        err += abs(math.sqrt(max(dot(rows[i],rows[i]),0))-1.0)
    for i in range(3):
        for j in range(i+1,3):
            err += abs(dot(rows[i],rows[j]))
    return err

def ortho_err_cols(m16):
    # upper-left 3x3 columns (column-major rotation): cols 0,1,2
    try:
        cols = [(m16[0],m16[4],m16[8]),(m16[1],m16[5],m16[9]),(m16[2],m16[6],m16[10])]
    except IndexError:
        return 1e9
    for c in cols:
        for v in c:
            if math.isnan(v) or math.isinf(v): return 1e9
    def dot(a,b): return sum(x*y for x,y in zip(a,b))
    err = 0.0
    for i in range(3):
        err += abs(math.sqrt(max(dot(cols[i],cols[i]),0))-1.0)
    for i in range(3):
        for j in range(i+1,3):
            err += abs(dot(cols[i],cols[j]))
    return err

def sparsity(m16):
    z = sum(1 for v in m16[:16] if abs(v) < 1e-6)
    return z/16.0

def variance(records, off):
    # mean per-element stddev over the 16 floats starting at off
    n = len(records)
    if n < 2: return 0.0
    cols = [[] for _ in range(16)]
    for r in records:
        if len(r) >= off+16:
            for k in range(16):
                v = r[off+k]
                if not (math.isnan(v) or math.isinf(v)):
                    cols[k].append(v)
    tot=0.0; cnt=0
    for c in cols:
        if len(c)>=2:
            mu=sum(c)/len(c)
            var=sum((x-mu)**2 for x in c)/len(c)
            tot+=math.sqrt(var); cnt+=1
    return tot/cnt if cnt else 0.0

results=[]
for res, recs in data.items():
    n=len(recs)
    if n < 20: continue
    # use a representative record with full 16 floats
    rep=None
    for r in recs:
        if len(r)>=16 and not any(math.isnan(x) or math.isinf(x) for x in r[:16]):
            rep=r; break
    if rep is None: continue
    for off,label in [(0,'m0'),(16,'m1')]:
        if len(rep) < off+16: continue
        sub=rep[off:off+16]
        oe=min(ortho_err(sub), ortho_err_cols(sub))
        sp=sparsity(sub)
        var=variance(recs, off)
        results.append((res, size_of[res], n, label, oe, sp, var, sub))

# View candidates: low ortho_err AND nonzero variance (changes as camera moved)
print("=== VIEW-matrix candidates (orthonormal 3x3, ranked by ortho_err; must vary) ===")
view=[r for r in results if r[4] < 0.15 and r[6] > 1e-3]
view.sort(key=lambda r:(r[4], -r[6]))
for res,size,n,label,oe,sp,var,sub in view[:12]:
    print(f"res={res} size={size} n={n} {label} ortho_err={oe:.4f} sparsity={sp:.2f} var={var:.3f}")

print()
print("=== PROJECTION-matrix candidates (sparse, static) ===")
proj=[r for r in results if r[5] >= 0.5 and r[6] < 1e-2]
proj.sort(key=lambda r:(-r[5], r[6]))
for res,size,n,label,oe,sp,var,sub in proj[:12]:
    print(f"res={res} size={size} n={n} {label} sparsity={sp:.2f} var={var:.4f} ortho_err={oe:.3f}")
    # print the matrix
    for i in range(4):
        print("     ", " ".join(f"{sub[i*4+j]:9.4f}" for j in range(4)))
