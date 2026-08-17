import math
from collections import Counter
from pathlib import Path

rows = []
with open(Path(__file__).parent / "_live_results_all.csv", encoding="utf-8") as f:
    for line in f:
        if line.startswith("#") or line.startswith("order") or not line.strip():
            continue
        p = line.strip().split(";")

        def de(s):
            return float(s.replace(",", ".")) if s.strip() else float("nan")

        rows.append(
            dict(
                order=int(p[0]),
                item=int(p[1]),
                z=de(p[10]),
                block=int(p[12]),
                k=int(p[13]),
                z0=de(p[14]),
                z1=de(p[15]),
                z2=de(p[16]),
                z3=de(p[17]),
            )
        )

print("k counts", Counter(r["k"] for r in rows))
zs = [r["z"] for r in rows]
mean = sum(zs) / len(zs)
sig = math.sqrt(sum((z - mean) ** 2 for z in zs) / (len(zs) - 1))
for r in rows:
    r["zs"] = (r["z"] - mean) / sig
sb = sorted(rows, key=lambda r: r["zs"])

print("BOTTOM 10 by Z*:")
for r in sb[:10]:
    print(
        f"  item={r['item']} Z*={r['zs']:.3f} z={r['z']:.3f} blk={r['block']} k={r['k']} "
        f"z0={r['z0']:.2f} z1={r['z1']:.2f} z2={r['z2']:.2f} z3={r['z3']:.2f}"
    )
print("TOP 10 by Z*:")
for r in sb[-10:][::-1]:
    print(
        f"  item={r['item']} Z*={r['zs']:.3f} z={r['z']:.3f} blk={r['block']} k={r['k']} "
        f"z0={r['z0']:.2f} z1={r['z1']:.2f} z2={r['z2']:.2f} z3={r['z3']:.2f}"
    )

bot, rest = sb[:50], sb[50:]
print("mean z0 bottom50", sum(r["z0"] for r in bot) / 50)
print("mean z0 rest", sum(r["z0"] for r in rest) / len(rest))
print("blocks bottom20", Counter(r["block"] for r in sb[:20]))
print("blocks top20", Counter(r["block"] for r in sb[-20:]))

# slaves-only recombine
for r in rows:
    r["z3n"] = (r["z1"] + r["z2"] + r["z3"]) / math.sqrt(3)
z3 = [r["z3n"] for r in rows]
m3 = sum(z3) / len(z3)
s3 = math.sqrt(sum((z - m3) ** 2 for z in z3) / (len(z3) - 1))
print("slaves-only: mean", round(m3, 4), "sigma", round(s3, 4), "Stouffer", round(m3 * math.sqrt(len(z3)), 3))
print("slaves chi2/n", round(sum(z * z for z in z3) / len(z3), 4))
abs3 = sorted(abs((z - m3) / s3) for z in z3)
zmax = abs3[-1]
print("slaves max|Z*|", round(zmax, 3), "bonf", round(min(1, len(z3) * math.erfc(zmax / math.sqrt(2))), 4))
for thr in (1.96, 2.58, 3.29):
    frac = sum(1 for z in abs3 if z > thr) / len(abs3)
    exp = math.erfc(thr / math.sqrt(2))
    print(f"slaves |Z*|>{thr}: {frac:.4f} (expect {exp:.4f})")

# master-only
z0s = [r["z0"] for r in rows]
m0 = sum(z0s) / len(z0s)
s0 = math.sqrt(sum((z - m0) ** 2 for z in z0s) / (len(z0s) - 1))
print("master-only: mean", round(m0, 4), "sigma", round(s0, 4), "Stouffer", round(m0 * math.sqrt(len(z0s)), 2))

# k=3 vs k=4 pass stats
for kk in (3, 4):
    sub = [r["z"] for r in rows if r["k"] == kk]
    if len(sub) < 2:
        continue
    m = sum(sub) / len(sub)
    s = math.sqrt(sum((z - m) ** 2 for z in sub) / (len(sub) - 1))
    print(f"k={kk}: n={len(sub)} mean={m:.4f} sigma={s:.4f}")

# block combined sigma distribution
from collections import defaultdict

bb = defaultdict(list)
for r in rows:
    bb[r["block"]].append(r["z"])
sigs = []
for b, zz in bb.items():
    if len(zz) < 4:
        continue
    m = sum(zz) / len(zz)
    s = math.sqrt(sum((z - m) ** 2 for z in zz) / (len(zz) - 1))
    sigs.append(s)
print(
    "block sigma: mean",
    round(sum(sigs) / len(sigs), 4),
    "min",
    round(min(sigs), 4),
    "max",
    round(max(sigs), 4),
    "n_blocks",
    len(sigs),
)
print("blocks with |mean|>1.5:", sum(1 for b, zz in bb.items() if abs(sum(zz) / len(zz)) > 1.5))
print("blocks with |mean|>2.5:", sum(1 for b, zz in bb.items() if abs(sum(zz) / len(zz)) > 2.5))
