#!/usr/bin/env python3
"""Idle/live entropy-rate profile from /diag snapshots (no firmware change)."""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent


def load(name):
    return json.loads((ROOT / name).read_text(encoding="utf-8"))


def cam(d):
    return d.get("cam") or d


def row(label, a, b, dt_hint=8.0):
    c1, c2 = cam(a), cam(b)
    bits1 = int(c1.get("bits") or c1.get("bits_extracted") or 0)
    bits2 = int(c2.get("bits") or c2.get("bits_extracted") or 0)
    # Prefer reported mbit_s; also estimate from bit delta if counters move
    mbit = float(c2.get("mbit_s") or c2.get("mbit_per_sec") or 0)
    db = bits2 - bits1
    mbit_delta = (db / 1e6) / dt_hint if db > 0 else None
    return {
        "label": label,
        "ready": c2.get("ready"),
        "mbit_s": mbit,
        "mbit_delta_est": mbit_delta,
        "bits": bits2,
        "d_bits": db,
        "bias": float(c2.get("bias") or 0),
        "sigma": float(c2.get("sigma") or 0),
        "sigma_n": c2.get("sigma_n") or c2.get("sigma_samples"),
        "mean_px": float(c2.get("mean_pixel") or c2.get("mean_px") or 0),
        "zero_diff": float(c2.get("zero_diff") or c2.get("zero_diff_frac") or 0),
        "drops": int(c2.get("drops") or c2.get("ring_drops") or 0),
        "waits": int(c2.get("waits") or c2.get("consumer_waits") or 0),
        "d_drops": int(c2.get("drops") or 0) - int(c1.get("drops") or c1.get("ring_drops") or 0),
        "d_waits": int(c2.get("waits") or 0) - int(c1.get("waits") or c1.get("consumer_waits") or 0),
        "stalls": int(c2.get("stalls") or 0),
        "stuck": int(c2.get("stuck_frames") or 0),
        "pairs": int(c2.get("frame_pairs") or 0),
        "d_pairs": int(c2.get("frame_pairs") or 0) - int(c1.get("frame_pairs") or 0),
        "exp": c2.get("exposure"),
        "gain": c2.get("gain"),
        "fold": c2.get("fold"),
        "autocorr": c2.get("autocorr") or c2.get("autocorr_lag"),
        "measuring": a.get("measuring") if isinstance(a, dict) else None,
        "fw": a.get("fw_version") or b.get("fw_version"),
        "slot": a.get("fw_slot") or b.get("fw_slot"),
    }


def main():
    st = load("status2.json")
    print("=== SESSION / MASTER STATUS ===")
    for k in (
        "state",
        "phase",
        "focus",
        "completed",
        "total",
        "nodes_total",
        "nodes_ok",
        "focus_win_ms",
        "focus_gap_ms",
        "run_s",
        "gap_s",
        "fw_version",
        "fw_built",
        "fw_sha",
        "fw_slot",
        "pass_sigma",
        "pass_mean",
        "null_flags",
        "v_eff",
    ):
        if k in st:
            print(f"  {k}: {st[k]}")
    print("  nodes:")
    for n in st.get("nodes", []):
        print(
            f"    id={n.get('id')} ip={n.get('ip')} ok={n.get('ok')} soft={n.get('soft_down')} "
            f"exp={n.get('cam_exp')} fold={n.get('cam_fold')} cal={n.get('cam_cal')} "
            f"bias={n.get('cam_bias')} mbit={n.get('cam_mbit')} stalls={n.get('cam_stalls')} "
            f"sigma={n.get('sigma')}"
        )

    specs = [
        ("master .100", "master1.json", "master2.json"),
        ("slave  .103", "n103_1.json", "n103_2.json"),
        ("slave  .145", "n145_1.json", "n145_2.json"),
        ("slave  .155", "n155_1.json", "n155_2.json"),
    ]
    rows = []
    print("\n=== PER-NODE CAM (/diag), ~8 s apart ===")
    print(
        f"{'node':12} {'mbit_s':>7} {'ΔMbit~':>7} {'bias':>9} {'σ':>6} {'mean_px':>8} "
        f"{'zeroΔ':>6} {'fold':>5} {'exp':>5} {'dropsΔ':>8} {'waitsΔ':>8} {'pairsΔ':>7}"
    )
    for label, f1, f2 in specs:
        a, b = load(f1), load(f2)
        r = row(label, a, b)
        rows.append(r)
        md = f"{r['mbit_delta_est']:.2f}" if r["mbit_delta_est"] is not None else "—"
        print(
            f"{r['label']:12} {r['mbit_s']:7.3f} {md:>7} {r['bias']:9.6f} {r['sigma']:6.3f} "
            f"{r['mean_px']:8.2f} {r['zero_diff']:6.3f} {str(r['fold']):>5} {str(r['exp']):>5} "
            f"{r['d_drops']:8d} {r['d_waits']:8d} {r['d_pairs']:7d}"
        )

    print("\n=== INTERPRETATION HINTS ===")
    mbits = [r["mbit_s"] for r in rows if r["mbit_s"] > 0]
    if mbits:
        print(f"  reported mbit_s range: {min(mbits):.3f} … {max(mbits):.3f}")
        print(f"  sum over 4 nodes (if all streaming): {sum(mbits):.2f} Mbit/s aggregate")
    folds = {r["label"]: r["fold"] for r in rows}
    print(f"  xor_fold: {folds}")
    for r in rows:
        # ring full while consumer idle → producer starved by priority/duty
        # drops rising, waits~0 → classic starve signature in reverse: consumer behind
        # waits rising → consumer hungry, producer slow
        note = []
        if r["d_drops"] > 1000 and r["d_waits"] == 0:
            note.append("many drops, no waits → excess entropy / consumer not keeping up OR idle discard")
        if r["d_waits"] > 0 and r["d_drops"] == 0:
            note.append("waits without drops → producer slower than consumer wants")
        if r["d_drops"] > 1000 and r["d_waits"] > 0:
            note.append("drops+waits both moving → mixed load / windowed stats")
        if r["mbit_s"] < 2.0 and r["mbit_s"] > 0:
            note.append("below ~2 Mbit/s gate (historical Phase-0 floor)")
        if abs(r["bias"] - 0.5) >= 1e-3:
            note.append("bias outside 1e-3 of 0.5")
        if r["sigma"] and abs(r["sigma"] - 1.0) > 0.05:
            note.append("|σ-1| > 0.05")
        if note:
            print(f"  {r['label']}: " + "; ".join(note))
        else:
            print(f"  {r['label']}: no red flags in this idle snapshot")

    print("\n=== AUTO CORR (sample 2) ===")
    for r in rows:
        print(f"  {r['label']}: {r['autocorr']}")

    print("\n=== FW (from diag where present) ===")
    for label, f1, f2 in specs:
        b = load(f2)
        print(
            f"  {label}: fw={b.get('fw_version')} slot={b.get('fw_slot')} "
            f"built={b.get('fw_built')} measuring={b.get('measuring')}"
        )

    print("\nDONE — idle profile. For load profile: start a short unattended session and re-sample /diag mid-measure.")


if __name__ == "__main__":
    main()
