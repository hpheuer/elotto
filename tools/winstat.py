import sys, math, statistics as st
names = {"192.168.178.145": "slave1", "192.168.178.103": "slave0",
         "192.168.178.155": "slave2", "192.168.178.100": "master"}
rows = [l.rstrip("\n").split(";") for l in open(sys.argv[1]) if not l.startswith("i;")]
by = {}
for r in rows:
    if r[3] == "":
        by.setdefault(r[2], {"void": 0}).setdefault("void", 0)
        by[r[2]]["void"] = by[r[2]].get("void", 0) + 1
        continue
    d = by.setdefault(r[2], {"void": 0})
    d.setdefault("t", []).append(float(r[1]))
    d.setdefault("z", []).append(float(r[3]))
    d.setdefault("h1", []).append(float(r[4]) if r[4] else None)
    d.setdefault("h2", []).append(float(r[5]) if r[5] else None)
    d.setdefault("w", []).append(float(r[6]) if r[6] else None)

def ac(x, lag):
    m = st.mean(x); v = sum((a - m) ** 2 for a in x)
    return sum((x[i] - m) * (x[i + lag] - m) for i in range(len(x) - lag)) / v

def slope_t(t, z):
    n = len(z); mt = st.mean(t); mz = st.mean(z)
    sxx = sum((a - mt) ** 2 for a in t); sxy = sum((a - mt) * (b - mz) for a, b in zip(t, z))
    b = sxy / sxx
    res = [zz - (mz + b * (tt - mt)) for tt, zz in zip(t, z)]
    s2 = sum(r * r for r in res) / (n - 2)
    return b, b / math.sqrt(s2 / sxx)

for ip, d in by.items():
    z = d.get("z", []); n = len(z)
    if n < 10:
        print(ip, "too few", n); continue
    m = st.mean(z); sd = st.stdev(z); se_sd = sd / math.sqrt(2 * (n - 1))
    c = [a - m for a in z]
    k = sum(a ** 4 for a in c) / n / (sd ** 4) - 3
    b, tb = slope_t(d["t"], z)
    q = n // 4
    qm = [round(st.mean(z[i * q:(i + 1) * q]), 3) for i in range(4)]
    qs = [round(st.stdev(z[i * q:(i + 1) * q]), 3) for i in range(4)]
    h1 = [x for x in d["h1"] if x is not None]; h2 = [x for x in d["h2"] if x is not None]
    same = r12 = None
    if len(h1) == n:
        m1, m2 = st.mean(h1), st.mean(h2)
        same = sum(1 for a, bb in zip(h1, h2) if (a - m1) * (bb - m2) > 0) / n
        r12 = sum((a - m1) * (bb - m2) for a, bb in zip(h1, h2)) / math.sqrt(
            sum((a - m1) ** 2 for a in h1) * sum((bb - m2) ** 2 for bb in h2))
    w = [x for x in d["w"] if x is not None]
    print(f"== {names.get(ip, ip)}  n={n} void={d['void']}")
    print(f"   z mean {m:+.3f} (SE {sd/math.sqrt(n):.3f})   z sigma {sd:.3f} ± {se_sd:.3f}   excess kurtosis {k:+.2f}")
    print(f"   |z-mean|>2: {sum(1 for a in c if abs(a) > 2*1)/n:.3f} (ideal 0.046)  >3: {sum(1 for a in c if abs(a) > 3)/n:.3f} (ideal 0.003)")
    print(f"   drift slope {b*60:+.4f} z/min  t={tb:+.2f}   quarter means {qm}   quarter sigma {qs}")
    print(f"   autocorr lag1..5 {[round(ac(z, L), 3) for L in range(1, 6)]}  (SE ~{1/math.sqrt(n):.3f})")
    if same is not None:
        print(f"   halves: corr {r12:+.3f}  same sign {same:.3f}")
    if w:
        print(f"   wsig mean {st.mean(w):.4f} sd {st.stdev(w):.4f}")

# ── Pairwise: the same window measured by two nodes. Independent nodes give
# r ~ 0 with SE 1/sqrt(n); a shared disturbance shows here and nowhere else.
zi = {}
for r in rows:
    if r[3] != "":
        zi.setdefault(r[2], {})[int(r[0])] = float(r[3])
ips = sorted(zi)
if len(ips) >= 2:
    print("== pairwise correlation of window z (same trigger)")
    for a in range(len(ips)):
        for b in range(a + 1, len(ips)):
            common = sorted(set(zi[ips[a]]) & set(zi[ips[b]]))
            x = [zi[ips[a]][i] for i in common]; y = [zi[ips[b]][i] for i in common]
            mx, my = st.mean(x), st.mean(y)
            num = sum((p - mx) * (q - my) for p, q in zip(x, y))
            den = math.sqrt(sum((p - mx) ** 2 for p in x) * sum((q - my) ** 2 for q in y))
            r_ = num / den
            print(f"   {names.get(ips[a], ips[a])} x {names.get(ips[b], ips[b])}: "
                  f"r {r_:+.3f}  (n {len(common)}, |r|*sqrt(n) {abs(r_)*math.sqrt(len(common)):.2f})")
    # combined z over all nodes, as the master would form it: sum / sqrt(k)
    common = sorted(set.intersection(*[set(zi[i]) for i in ips]))
    comb = [sum(zi[i][w] for i in ips) / math.sqrt(len(ips)) for w in common]
    print(f"   combined z over {len(ips)} nodes: mean {st.mean(comb):+.3f}  sigma {st.stdev(comb):.3f} "
          f"± {st.stdev(comb)/math.sqrt(2*(len(comb)-1)):.3f}  (ideal 1,00)")
