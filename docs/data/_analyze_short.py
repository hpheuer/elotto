#!/usr/bin/env python3
"""Compare short/post-hardware-fix run vs previous full unattended pass."""
import json
import math
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent


def load_csv(path):
    rows = []
    meta = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("#"):
                meta.append(line)
                continue
            if line.startswith("order;") or not line.strip():
                continue
            p = line.split(";")

            def de(s):
                s = s.strip()
                if not s:
                    return float("nan")
                return float(s.replace(",", "."))

            rows.append(
                {
                    "order": int(p[0]),
                    "item": int(p[1]),
                    "z": de(p[10]),
                    "block": int(p[12]),
                    "k": int(p[13]),
                    "z0": de(p[14]),
                    "z1": de(p[15]),
                    "z2": de(p[16]),
                    "z3": de(p[17]),
                }
            )
    return meta, rows


def stats(vals):
    n = len(vals)
    if n == 0:
        return None
    m = sum(vals) / n
    if n < 2:
        return dict(n=n, mean=m, sigma=0.0, stouffer=0.0, chi2n=0.0)
    var = sum((v - m) ** 2 for v in vals) / (n - 1)
    s = math.sqrt(var) if var > 0 else 0.0
    return dict(
        n=n,
        mean=m,
        sigma=s,
        stouffer=m * math.sqrt(n),
        chi2n=sum(v * v for v in vals) / n,
    )


def summarize(label, st, lp, rows):
    print(f"\n{'='*60}\n{label}\n{'='*60}")
    print(
        f"  state={st.get('state')} mode={st.get('mode')} focus={st.get('focus')} "
        f"completed={st.get('completed')}/{st.get('total')} void={st.get('pass_n_valid')} "
        f"pass_n_void={st.get('pass_n_void')}"
    )
    print(
        f"  pass_mean={st.get('pass_mean'):.4f} pass_sigma={st.get('pass_sigma'):.4f} "
        f"chi2={st.get('pass_chi2'):.1f} Stouffer={st.get('pass_stouffer'):.2f} "
        f"v_eff={st.get('v_eff'):.3f} null_flags={st.get('null_flags')}"
    )
    nf = int(st.get("null_flags") or 0)
    print(
        f"  flags: SIGMA={bool(nf&1)} DRIFT={bool(nf&2)} CHI2={bool(nf&4)} PAIR={bool(nf&8)}"
    )
    print(
        f"  drift_t={st.get('drift_t')} slope={st.get('drift_slope')} "
        f"sigma_lo/hi={st.get('sigma_lo')}/{st.get('sigma_hi')} "
        f"off_first/last={st.get('off_first')}/{st.get('off_last')}"
    )
    print(
        f"  pair_r_max={st.get('pair_r')} n={st.get('pair_n')} "
        f"i-j={st.get('pair_i')}-{st.get('pair_j')} net={st.get('net_lost')}/{st.get('net_retries')}/{st.get('net_stale')}"
    )
    print(f"  loops_done={st.get('loops_done')} elapsed_h={(st.get('elapsed_ms') or 0)/3.6e6:.2f}")

    print("  --- nodes ---")
    for n in st.get("nodes", []):
        zn = n.get("z_n") or 0
        zm = n.get("z") or 0
        print(
            f"    id={n['id']} ip={n.get('ip')} soft={n.get('soft_down')} "
            f"meanZ={zm:+.4f} n={zn} Stouffer={zm*math.sqrt(zn) if zn else 0:+.1f} "
            f"sig={n.get('sigma'):.4f} exp={n.get('cam_exp')} cal={n.get('cam_cal')} "
            f"bias={n.get('cam_bias')} mbit={n.get('cam_mbit')}"
        )

    valid = [r for r in rows if r["k"] > 0]
    print(f"  --- csv: rows={len(rows)} valid={len(valid)} k={dict(Counter(r['k'] for r in rows))}")
    if valid:
        zs = [r["z"] for r in valid]
        ps = stats(zs)
        print(
            f"  pass csv: mean={ps['mean']:+.4f} sigma={ps['sigma']:.4f} "
            f"chi2/n={ps['chi2n']:.4f} Stouffer={ps['stouffer']:.2f}"
        )
        for i, key in enumerate(("z0", "z1", "z2", "z3")):
            vals = [r[key] for r in valid if not math.isnan(r[key])]
            s = stats(vals)
            if s:
                print(
                    f"  node{i}: n={s['n']} mean={s['mean']:+.4f} sigma={s['sigma']:.4f} "
                    f"Stouffer={s['stouffer']:+.1f}"
                )
        # slaves-only
        z3 = [
            (r["z1"] + r["z2"] + r["z3"]) / math.sqrt(3)
            for r in valid
            if not any(math.isnan(r[k]) for k in ("z1", "z2", "z3"))
        ]
        s3 = stats(z3)
        if s3:
            print(
                f"  slaves-only: mean={s3['mean']:+.4f} sigma={s3['sigma']:.4f} "
                f"Stouffer={s3['stouffer']:+.2f} chi2/n={s3['chi2n']:.4f}"
            )

        by_b = defaultdict(list)
        by_b0 = defaultdict(list)
        for r in valid:
            by_b[r["block"]].append(r["z"])
            if not math.isnan(r["z0"]):
                by_b0[r["block"]].append(r["z0"])

        print("  --- per-block (combined mean/sigma, master mean) ---")
        bad_master = 0
        for b in sorted(by_b):
            zz = by_b[b]
            s = stats(zz)
            m0 = sum(by_b0[b]) / len(by_b0[b]) if by_b0[b] else float("nan")
            flag = ""
            if not math.isnan(m0) and abs(m0) > 1.5:
                flag = "  << MASTER |mean|>1.5"
                bad_master += 1
            elif s and abs(s["mean"]) > 1.0:
                flag = "  << block mean large"
            if s:
                print(
                    f"    B{b:02d}: n={s['n']:3d} mean={s['mean']:+.3f} sig={s['sigma']:.3f} "
                    f"master_mean={m0:+.3f}{flag}"
                )
        print(f"  blocks with |master mean|>1.5: {bad_master} / {len(by_b)}")

        # first vs rest half master
        n = len(valid)
        half = n // 2
        m1 = stats([r["z0"] for r in valid[:half] if not math.isnan(r["z0"])])
        m2 = stats([r["z0"] for r in valid[half:] if not math.isnan(r["z0"])])
        if m1 and m2:
            print(
                f"  master first half: mean={m1['mean']:+.4f} sig={m1['sigma']:.4f} n={m1['n']}"
            )
            print(
                f"  master second half: mean={m2['mean']:+.4f} sig={m2['sigma']:.4f} n={m2['n']}"
            )

    if lp and lp.get("loops"):
        print("  --- loop table master mean (first 5 / last 5) ---")
        loops = lp["loops"]
        for L in loops[:5] + (loops[-5:] if len(loops) > 5 else []):
            nn = L.get("n") or []
            m0 = nn[0]["mean"] if nn else float("nan")
            print(
                f"    loop{L['loop']}: comb_mean={L['mean']:+.3f} sig={L['sigma']:.3f} "
                f"master={m0:+.3f} exp0={nn[0].get('cam_exp') if nn else '?'}"
            )


def main():
    st = json.loads((ROOT / "_short_status.json").read_text(encoding="utf-8"))
    lp = json.loads((ROOT / "_short_loops.json").read_text(encoding="utf-8"))
    meta, rows = load_csv(ROOT / "_short_results_all.csv")
    print("META short:")
    for h in meta:
        print(" ", h)
    summarize("CURRENT (post master HW change)", st, lp, rows)

    # compare to previous full if present
    prev = ROOT / "_live_status.json"
    prev_csv = ROOT / "_live_results_all.csv"
    if prev.exists() and prev_csv.exists():
        st0 = json.loads(prev.read_text(encoding="utf-8"))
        lp0 = json.loads((ROOT / "_live_loops.json").read_text(encoding="utf-8")) if (ROOT / "_live_loops.json").exists() else {}
        _, rows0 = load_csv(prev_csv)
        summarize("PREVIOUS full unattended (before HW change)", st0, lp0, rows0)

        print("\n" + "=" * 60)
        print("COMPARISON (master mean z session / pass sigma / soft / k=3 share)")
        print("=" * 60)

        def master_mean(rows_):
            vals = [r["z0"] for r in rows_ if r["k"] > 0 and not math.isnan(r["z0"])]
            return stats(vals)

        def k3_share(rows_):
            c = Counter(r["k"] for r in rows_)
            tot = sum(c.values()) or 1
            return c.get(3, 0) / tot, c

        for name, st_, rows_ in (
            ("PREV", st0, rows0),
            ("NOW", st, rows),
        ):
            m = master_mean(rows_)
            sh, kc = k3_share(rows_)
            print(
                f"  {name}: pass_mean={st_.get('pass_mean'):+.4f} pass_sig={st_.get('pass_sigma'):.4f} "
                f"null={st_.get('null_flags')} master_mean={m['mean']:+.4f} master_sig={m['sigma']:.4f} "
                f"soft0={st_['nodes'][0].get('soft_down')} k3_share={sh:.1%} kdist={dict(kc)}"
            )


if __name__ == "__main__":
    main()
