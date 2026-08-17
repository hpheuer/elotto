#!/usr/bin/env python3
"""Analyze the live unattended full-pass download."""
import json
import math
import csv
from pathlib import Path
from collections import defaultdict

ROOT = Path(__file__).resolve().parent
st = json.loads((ROOT / "_live_status.json").read_text(encoding="utf-8"))
lp = json.loads((ROOT / "_live_loops.json").read_text(encoding="utf-8"))

# --- CSV (German: ; and , decimal) ---
rows = []
header_meta = []
with (ROOT / "_live_results_all.csv").open(encoding="utf-8") as f:
    for line in f:
        line = line.rstrip("\n")
        if line.startswith("#"):
            header_meta.append(line)
            continue
        if line.startswith("order;") or not line:
            if line.startswith("order;"):
                cols = line.split(";")
            continue
        parts = line.split(";")
        # order;item;n1..n6;e1;e2;z_raw;z_adj;block;k;z0;z1;z2;z3

        def de(s):
            s = s.strip()
            if s == "":
                return float("nan")
            return float(s.replace(",", "."))

        rows.append(
            {
                "order": int(parts[0]),
                "item": int(parts[1]),
                "z": de(parts[10]),
                "z_adj": de(parts[11]),
                "block": int(parts[12]),
                "k": int(parts[13]),
                "z0": de(parts[14]) if len(parts) > 14 else float("nan"),
                "z1": de(parts[15]) if len(parts) > 15 else float("nan"),
                "z2": de(parts[16]) if len(parts) > 16 else float("nan"),
                "z3": de(parts[17]) if len(parts) > 17 else float("nan"),
            }
        )

print("=== META ===")
for h in header_meta:
    print(h)

print("\n=== SESSION ===")
keys = [
    "state", "mode", "focus", "score_dir", "fw_version", "fw_built", "fw_sha", "fw_slot",
    "completed", "total", "pass_n_valid", "pass_n_void", "pass_mean", "pass_sigma",
    "pass_chi2", "pass_stouffer", "v_eff", "null_flags", "best_z", "p_corr", "comparisons",
    "loops_done", "drift_slope", "drift_t", "off_first", "off_last", "sigma_lo", "sigma_hi",
    "pair_r", "pair_n", "pair_i", "pair_j", "pair_count", "elapsed_ms", "paused_ms",
    "pool_auto", "baseline_mean", "focus_win_ms", "focus_gap_ms", "run_s", "gap_s",
    "cal_budget_ms", "cal_interval_ms", "nodes_total", "nodes_ok",
    "net_retries", "net_lost", "net_stale",
]
for k in keys:
    print(f"  {k}: {st.get(k)}")

nf = int(st.get("null_flags") or 0)
print("\n=== NULL FLAGS ===")
print(f"  raw={nf}  SIGMA={bool(nf&1)} DRIFT={bool(nf&2)} CHI2={bool(nf&4)} PAIR={bool(nf&8)}")

print("\n=== NODES (session) ===")
for n in st.get("nodes", []):
    zn = n.get("z_n") or 0
    zm = n.get("z") or 0
    stou = zm * math.sqrt(zn) if zn else 0
    print(
        f"  id={n['id']} ip={n.get('ip')} ok={n.get('ok')} soft={n.get('soft_down')} "
        f"meanZ={zm:.4f} n={zn} Stouffer={stou:.2f} sigma={n.get('sigma'):.4f} "
        f"exp={n.get('cam_exp')} fold={n.get('cam_fold')} cal={n.get('cam_cal')} "
        f"bias={n.get('cam_bias')} mbit={n.get('cam_mbit')} lost={n.get('lost')} stalls={n.get('cam_stalls')}"
    )

print("\n=== PAIR MATRIX ===")
n_valid = st.get("pass_n_valid") or len(rows)
for p in st.get("pairs", []):
    r = p["r"]
    # use pair_n only for worst; approx n_valid for flag scale
    t = abs(r) * math.sqrt(n_valid)
    flag = " FLAG" if t > 3 else ""
    print(f"  n{p['i']}-n{p['j']}: r={r:+.4f}  |r|√N≈{t:.2f}{flag}")

print("\n=== TOP / LOW / NEAR (device) ===")
for label in ("top", "low", "near"):
    print(f"  -- {label} --")
    for t in st.get(label, []):
        print(f"    item={t.get('run')} z={t.get('z'):.4f} z_adj={t.get('z_adj')} k={t.get('k')} nums={t.get('nums')}")

# --- CSV-level stats ---
valid = [r for r in rows if r["k"] > 0]
voids = [r for r in rows if r["k"] == 0]
zs = [r["z"] for r in valid]
N = len(zs)
mean = sum(zs) / N if N else 0
var = sum((z - mean) ** 2 for z in zs) / (N - 1) if N > 1 else 0
sig = math.sqrt(var) if var > 0 else 0
chi2 = sum(z * z for z in zs)
stouffer = mean * math.sqrt(N) if N else 0
print("\n=== PASS FROM CSV ===")
print(f"  rows={len(rows)} valid={N} void={len(voids)}")
print(f"  mean={mean:.4f} sigma={sig:.4f} chi2={chi2:.2f} chi2/n={chi2/N if N else 0:.4f}")
print(f"  Stouffer={stouffer:.3f}")
# expected max |Z*| under studentized null ~ something; under raw N(0,1)
absz = sorted((abs(z) for z in zs), reverse=True)
print(f"  max|z| raw top5: {absz[:5]}")
zstar = [(z - mean) / sig for z in zs] if sig > 0 else zs
abszs = sorted((abs(z) for z in zstar), reverse=True)
print(f"  max|Z*| top5: {abszs[:5]}")
# Bonferroni on max |Z*|
zmax = abszs[0] if abszs else 0
p1 = math.erfc(zmax / math.sqrt(2))
pc = min(1.0, N * p1)
print(f"  best |Z*|={zmax:.3f} p1={p1:.3e} p_bonf={pc:.4f}")

# fraction |z|>1.96 etc on studentized
for thr, name in [(1.96, "1.96"), (2.58, "2.58"), (3.29, "3.29")]:
    frac = sum(1 for z in zstar if abs(z) > thr) / N if N else 0
    exp = 2 * (1 - 0.5 * (1 + math.erf(thr / math.sqrt(2))))  # two-sided normal approx
    # better: erfc
    exp = math.erfc(thr / math.sqrt(2))
    print(f"  frac |Z*|>{name}: {frac:.4f} (null expect {exp:.4f})")

# per block from CSV
print("\n=== PER-BLOCK FROM CSV ===")
by_b = defaultdict(list)
for r in valid:
    by_b[r["block"]].append(r["z"])
for b in sorted(by_b):
    zz = by_b[b]
    n = len(zz)
    m = sum(zz) / n
    s = math.sqrt(sum((z - m) ** 2 for z in zz) / (n - 1)) if n > 1 else 0
    print(f"  block {b}: n={n} mean={m:+.4f} sigma={s:.4f}")

# per-node from CSV (z0..z3)
print("\n=== PER-NODE FROM CSV (all valid) ===")
for i, key in enumerate(("z0", "z1", "z2", "z3")):
    vals = [r[key] for r in valid if not math.isnan(r[key])]
    if not vals:
        print(f"  node{i}: no data")
        continue
    n = len(vals)
    m = sum(vals) / n
    s = math.sqrt(sum((z - m) ** 2 for z in vals) / (n - 1)) if n > 1 else 0
    print(f"  node{i}: n={n} mean={m:+.4f} sigma={s:.4f} Stouffer={m*math.sqrt(n):.2f}")

# block x node means
print("\n=== BLOCK x NODE MEAN (CSV) ===")
for b in sorted(by_b):
    parts = []
    for i, key in enumerate(("z0", "z1", "z2", "z3")):
        vals = [r[key] for r in valid if r["block"] == b and not math.isnan(r[key])]
        if vals:
            m = sum(vals) / len(vals)
            parts.append(f"n{i}:{m:+.3f}(n={len(vals)})")
        else:
            parts.append(f"n{i}:—")
    print(f"  block {b}: " + " ".join(parts))

# soft-down inference: k < 4 often?
print("\n=== k DISTRIBUTION ===")
from collections import Counter
kc = Counter(r["k"] for r in rows)
print(dict(sorted(kc.items())))

# loops table compact
print("\n=== LOOPS TABLE ===")
print(f"  loops_done={lp.get('loops_done')} drift_slope={lp.get('drift_slope')} drift_t={lp.get('drift_t')}")
print(f"  sigma_lo={lp.get('sigma_lo')} sigma_hi={lp.get('sigma_hi')}")
for L in lp.get("loops", []):
    print(
        f"  B{L['loop']}: t={L['t_s']}s base={L['base']:+.3f} raw_m={L['raw_m']:+.3f} "
        f"mean={L['mean']:+.3f} sig={L['sigma']:.3f} cal_ms={L['cal_ms']} "
        f"win={L['win_ms']:.0f} gap={L['gap_ms']:.0f}"
    )
    for i, nn in enumerate(L.get("n", [])):
        print(
            f"      n{i}: mean={nn['mean']:+.3f} sig={nn['sigma']:.3f} "
            f"exp={nn['cam_exp']} cal={nn['cam_cal']} bias={nn['cam_bias']:.6f} "
            f"mbit={nn['cam_mbit']:.2f}"
        )

# correlation of node series (session-level, simple)
print("\n=== NODE PAIR r FROM CSV (raw, not block-centered) ===")
import itertools
node_series = []
for i, key in enumerate(("z0", "z1", "z2", "z3")):
    node_series.append([r[key] for r in valid])

def pearson(a, b):
    pairs = [(x, y) for x, y in zip(a, b) if not math.isnan(x) and not math.isnan(y)]
    if len(pairs) < 3:
        return float("nan"), 0
    xs = [p[0] for p in pairs]
    ys = [p[1] for p in pairs]
    n = len(xs)
    mx = sum(xs) / n
    my = sum(ys) / n
    num = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    dx = math.sqrt(sum((x - mx) ** 2 for x in xs))
    dy = math.sqrt(sum((y - my) ** 2 for y in ys))
    if dx == 0 or dy == 0:
        return float("nan"), n
    return num / (dx * dy), n

for i, j in itertools.combinations(range(4), 2):
    r, n = pearson(node_series[i], node_series[j])
    if n:
        print(f"  n{i}-n{j}: r={r:+.4f} n={n} |r|√n={abs(r)*math.sqrt(n):.2f}")

# block-centered pair r (like firmware)
print("\n=== NODE PAIR r BLOCK-CENTERED ===")
for i, j in itertools.combinations(range(4), 2):
    cxx = cyy = cxy = 0.0
    cn = 0
    for b in sorted(by_b):
        pairs = []
        for r in valid:
            if r["block"] != b:
                continue
            x, y = r[f"z{i}"], r[f"z{j}"]
            if math.isnan(x) or math.isnan(y):
                continue
            pairs.append((x, y))
        if len(pairs) < 2:
            continue
        n = len(pairs)
        mx = sum(p[0] for p in pairs) / n
        my = sum(p[1] for p in pairs) / n
        cxx += sum((p[0] - mx) ** 2 for p in pairs)
        cyy += sum((p[1] - my) ** 2 for p in pairs)
        cxy += sum((p[0] - mx) * (p[1] - my) for p in pairs)
        cn += n
    if cxx > 0 and cyy > 0:
        r = cxy / math.sqrt(cxx * cyy)
        print(f"  n{i}-n{j}: r={r:+.4f} n={cn} |r|√n={abs(r)*math.sqrt(cn):.2f}")

print("\n=== ELAPSED ===")
ems = st.get("elapsed_ms") or 0
print(f"  elapsed_ms={ems} = {ems/3600000:.2f} h")
print(f"  items/h = {N / (ems/3600000) if ems else 0:.1f}")
print("DONE")
