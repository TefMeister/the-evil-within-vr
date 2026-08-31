"""Analyze seqdump.log ordering invariants for the K_eye patch-point decision (Task 5)."""
import re, sys, collections

path = sys.argv[1]
ev_re = re.compile(r"^\[(\d+)\]\[tid=(\d+)\] (\w+) (.*)$")

tids = collections.Counter()
types = collections.Counter()
maptypes = collections.Counter()

# state
cur_vs = None            # ptr string at time of event
slot0 = None             # buffer ptr bound to VS slot 0
last_unmap_seq = {}      # res -> seq of last UNMAP
last_map_seq = {}
vs_set_seq = 0           # seq when cur_vs was set
slot0_set_seq = 0
draws = 0
pattern = collections.Counter()
updatesub_draws = 0
vscb1 = 0
frames = set()
draws_per_frame = collections.Counter()
cur_frame = None
examples = collections.defaultdict(list)

for line in open(path, errors="replace"):
    m = ev_re.match(line)
    if not m:
        continue
    seq, tid, typ, rest = int(m.group(1)), m.group(2), m.group(3), m.group(4)
    tids[tid] += 1
    types[typ] += 1
    if typ == "PRESENT":
        cur_frame = rest
        frames.add(rest)
    elif typ == "VSSETSHADER":
        cur_vs = rest.split()[0].split("=")[1]
        vs_set_seq = seq
    elif typ == "MAP":
        res = rest.split()[0].split("=")[1]
        last_map_seq[res] = seq
        mt = rest.split("maptype=")[1]
        maptypes[mt] += 1
    elif typ == "UNMAP":
        res = rest.split()[0].split("=")[1]
        last_unmap_seq[res] = seq
    elif typ == "UPDATESUB":
        res = rest.split()[0].split("=")[1]
        last_unmap_seq[res] = seq  # treat as a fill
    elif typ == "VSSETCB":
        d = dict(kv.split("=") for kv in rest.split() if "=" in kv)
        if d.get("start") == "0" and "slot0" in d:
            slot0 = d["slot0"].split(":")[0]
            slot0_set_seq = seq
        elif d.get("start") == "0" and d.get("num", "0") != "0":
            # slot0 rebound but ptr formatting differs; count separately
            pattern["VSSETCB_slot0_unparsed"] += 1
    elif typ == "VSSETCB1":
        vscb1 += 1
    elif typ == "DRAW":
        draws += 1
        draws_per_frame[cur_frame] += 1
        fill = last_unmap_seq.get(slot0, -1)
        # classify ordering for this draw
        if slot0 is None:
            key = "no-slot0"
        else:
            fresh = fill > 0 and fill > (prev_draw_seq if 'prev_draw_seq' in dir() else 0)
            # relations
            rel_vs_fill = "VSfirst" if vs_set_seq < fill else "FILLfirst"
            rel_fill_bind = "fill<bind" if fill < slot0_set_seq else "bind<fill"
            key = f"{rel_vs_fill},{rel_fill_bind}"
        pattern[key] += 1
        if len(examples[key]) < 2:
            examples[key].append(seq)
        prev_draw_seq = seq

print("tids:", dict(tids))
print("event types:", dict(types))
print("maptypes:", dict(maptypes))
print("VSSETCB1 events:", vscb1)
print("draws:", draws, "frames:", len(frames))
if frames:
    dpf = sorted(draws_per_frame.values())
    print("draws/frame: min", dpf[0], "median", dpf[len(dpf)//2], "max", dpf[-1])
print("draw ordering patterns:")
for k, v in pattern.most_common():
    print(f"  {k}: {v}  (e.g. seq {examples.get(k, [])})")
