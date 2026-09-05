#!/usr/bin/env python3
"""Paired-per-round analysis for hackbench_tier2_isolation.sh.

Every arm runs once per round, so a round is a matched block: whatever drift
the guest suffered in that round hit all arms. All comparisons below are
paired WITHIN round (per-round difference, then a one-sample t on the
differences), which is what makes an n of 5-8 rounds usable against a
workload with hackbench's ~57% run-to-run spread.

Reads the CSV written by the shell script; column set is discovered from the
header so arms can be added or dropped without editing this file.
"""
import csv
import statistics
import sys

CSV = sys.argv[1] if len(sys.argv) > 1 else None
rows = list(csv.DictReader(open(CSV)))
if len(rows) < 2:
    print("Not enough clean rounds for paired stats (need >= 2).")
    sys.exit(0)

arms = []
for c in rows[0]:
    if c.endswith("_iters"):
        arms.append(c[: -len("_iters")])
n = len(rows)

# Derived per-round column: mean node-path spin iterations PER PASS.
# node_iters_total is a raw sum whose two factors move for different reasons
# (how long each pass spun -- tier-2's lever -- and how many passes there were
# -- contention, which external drift dominates). perpass isolates the first.
for r in rows:
    for a in arms:
        for num, den, out in (
            (f"{a}_iters", f"{a}_att", f"{a}_perpass"),
            # bailpp is the sharpest metric available: mean iterations spent by
            # a pass that gave up without acquiring. Tier-2's entire mechanism
            # is "end such a pass sooner", and it has a hard known ceiling of
            # SPIN_THRESHOLD = 32768, so a move here is directly interpretable
            # as a fraction of the budget saved.
            (f"{a}_bailit", f"{a}_bailatt", f"{a}_bailpp"),
            (f"{a}_okit", f"{a}_okatt", f"{a}_okpp"),
        ):
            if num in r and den in r and float(r[den]) > 0:
                r[out] = str(float(r[num]) / float(r[den]))


# Mean real-HLT cost per halt in microseconds (ivh_lock_halt.hlt_cycles /
# hlt_events / 2200MHz TSC). This -- not wall-clock or halt count -- is the
# metric the "sticky vs lossy wake" hypothesis needs: see the 2026-09-05
# test plan. Covers ALL halt sites (head+node) and both real-halt mechanisms
# (mech 0's PV_UNHALT halt, mech 2's scoped halt both populate this struct).
for r in rows:
    for a in arms:
        hltc, hlte = f"{a}_hltc", f"{a}_hlte"
        if hltc in r and hlte in r and float(r[hlte]) > 0:
            r[f"{a}_usperhalt"] = str(float(r[hltc]) / float(r[hlte]) / 2200.0)


def col(arm, metric):
    return f"{arm}_{metric}"


def have(arm, metric):
    return col(arm, metric) in rows[0]


def paired(a, b, metric, label):
    """b minus a, paired per round."""
    if not (have(a, metric) and have(b, metric)):
        return
    diffs = [float(r[col(b, metric)]) - float(r[col(a, metric)]) for r in rows]
    base = statistics.mean(float(r[col(a, metric)]) for r in rows)
    mean = statistics.mean(diffs)
    sd = statistics.stdev(diffs) if len(diffs) > 1 else 0.0
    t = mean / (sd / len(diffs) ** 0.5) if sd > 0 else (float("inf") if mean else 0.0)
    pct = (mean / base * 100) if base else 0.0
    nneg = sum(1 for d in diffs if d < 0)
    print(
        f"  {label:<34} n={n} mean_diff={mean:>14,.0f} ({pct:+6.2f}%)"
        f"  sd={sd:>13,.0f}  t={t:>7.2f}  rounds_negative={nneg}/{n}"
    )


def section(title):
    print("\n" + title)
    print("-" * len(title))


order = [a for a in ("D", "G0", "G1", "H1", "H2") if a in arms]
extra = [a for a in arms if a not in order]
arms = order + extra

print(f"=== hackbench tier-2 isolation: {n} clean rounds, arms {arms} ===")

section("Per-arm means")
hdr = (f"  {'arm':<5}{'node_iters_total':>20}{'attempts':>14}{'bail/pass':>11}"
       f"{'ok/pass':>9}{'head_iters':>16}{'wall_s':>9}{'t2fire':>12}"
       f"{'node_halts':>12}{'head_halts':>12}{'us/halt':>9}{'tier1':>11}")
print(hdr)
for a in arms:
    def m(metric, _a=a):
        if not have(_a, metric):
            return 0.0
        return statistics.mean(float(r[col(_a, metric)]) for r in rows)
    print(
        f"  {a:<5}{m('iters'):>20,.0f}{m('att'):>14,.0f}{m('bailpp'):>11,.0f}"
        f"{m('okpp'):>9,.0f}{m('head'):>16,.0f}{m('wall'):>9.2f}{m('t2f'):>12,.0f}"
        f"{m('nhalt'):>12,.0f}{m('hhalt'):>12,.0f}{m('usperhalt'):>9.1f}{m('tier1'):>11,.0f}"
    )

section("STICKY-WAKE TEST: mean cycle cost per real HLT, us (hlt_cycles/hlt_events/2200MHz)")
print("  D's wake latches (KVM_HC_KICK_CPU sets pv_unhalted); mechanism 2's wake is a")
print("  plain IPI and does NOT latch -- if it arrives before the target halts, it's")
print("  lost, and the halt is rescued by the next 1ms tick instead. G0K/G0N restore")
print("  the latching hypercall wake without touching tier-2. If G0K's us/halt")
print("  collapses toward D's, the wake vehicle is the fix -- a default-value change,")
print("  not a new feature. If it doesn't move, halts are genuinely long and idea 2/3")
print("  are where the remaining budget should go.")
for a in arms:
    if not have(a, "usperhalt"):
        continue
    us = statistics.mean(float(r[col(a, "usperhalt")]) for r in rows)
    print(f"  {a:<4} us/halt={us:8.1f}")
for base, fixed in (("G0", "G0K"), ("G0", "G0N"), ("H1", "H1K")):
    if base in arms and fixed in arms:
        paired(base, fixed, "usperhalt", f"us/halt {fixed}-{base}")

section("HEAD vs NODE HALT SPLIT (bounds max benefit of a head-scoped-halt gate)")
print("  ivh_two_enhancement_designs_2026-09-04.md idea 2 needs this before building")
print("  anything: what fraction of ALL halts happen at the queue head")
print("  (pv_wait_head_or_lock, currently ungated -- no wait_early check exists")
print("  there at all in this tree) vs mid-queue (pv_wait_node, which already has")
print("  mechanism 2's scoped-halt gate). If head's share is small, most of the")
print("  5-8% regression lives in pv_wait_node's policy instead, not here.")
for a in arms:
    if not (have(a, "nhalt") and have(a, "hhalt")):
        continue
    nh = statistics.mean(float(r[col(a, "nhalt")]) for r in rows)
    hh = statistics.mean(float(r[col(a, "hhalt")]) for r in rows)
    tot = nh + hh
    pct = 100 * hh / tot if tot else 0.0
    print(f"  {a:<4} node_halts={nh:>14,.0f}  head_halts={hh:>14,.0f}  head_share={pct:6.2f}%")

section("VALIDATION (these must be near zero for anything below to mean anything)")
paired("D", "G0", "iters", "node_iters G0-D  (mech only)")
paired("G0", "G1", "iters", "node_iters G1-G0 (instr. cost)")
paired("D", "G0", "wall", "wall_s     G0-D")
paired("G0", "G1", "wall", "wall_s     G1-G0")

section("TIER-2 MARGINAL EFFECT on the primary metric (negative = less spin work)")
for h in ("H1", "H2"):
    if h in arms:
        paired("G1", h, "iters", f"node_iters {h}-G1 (cost-matched)")
        paired("G0", h, "iters", f"node_iters {h}-G0 (optimistic)")

section("SHARPEST: mean iterations of a BAIL/EXHAUST pass (max possible 32768)")
print("  This is the population tier-2 acts on, and the only one where its")
print("  mechanism can show up first-order. Ceiling of the effect is 32768.")
for h in ("H1", "H2"):
    if h in arms:
        paired("G1", h, "bailpp", f"bail_iters/pass {h}-G1")
        paired("G0", h, "bailpp", f"bail_iters/pass {h}-G0")
paired("D", "G0", "bailpp", "bail_iters/pass G0-D")
paired("G0", "G1", "bailpp", "bail_iters/pass G1-G0")

section("SECOND PLACEBO: mean iterations of a SUCCESS pass (tier-2 cannot end one)")
for h in ("H1", "H2"):
    if h in arms:
        paired("G1", h, "okpp", f"ok_iters/pass   {h}-G1")
        paired("G0", h, "okpp", f"ok_iters/pass   {h}-G0")

section("TIER-2 on iterations PER PASS (raw sum divided by node spin attempts)")
for h in ("H1", "H2"):
    if h in arms:
        paired("G1", h, "perpass", f"iters/pass {h}-G1")
        paired("G0", h, "perpass", f"iters/pass {h}-G0")
paired("D", "G0", "perpass", "iters/pass G0-D")
paired("G0", "G1", "perpass", "iters/pass G1-G0")

section("CONTENTION CO-MOVE CHECK: node spin attempts (the raw sum's denominator)")
for h in ("H1", "H2"):
    if h in arms:
        paired("G1", h, "att", f"attempts   {h}-G1")
        paired("G0", h, "att", f"attempts   {h}-G0")
paired("D", "G0", "att", "attempts   G0-D")

section("PLACEBO: head_iters -- tier-2 cannot reach this path; it must NOT move")
for h in ("H1", "H2"):
    if h in arms:
        paired("G1", h, "head", f"head_iters {h}-G1")
        paired("G0", h, "head", f"head_iters {h}-G0")
paired("D", "G0", "head", "head_iters G0-D")

section("SECONDARY: wall-clock / hackbench Time (expect this to be far noisier)")
for h in ("H1", "H2"):
    if h in arms:
        paired("G1", h, "wall", f"wall_s   {h}-G1")
        paired("G1", h, "hbtime", f"hb_time  {h}-G1")

section("Tier-2 engagement and its THEORETICAL CEILING on node_iters_total")
SPIN_THRESHOLD = 32768
for a in arms:
    if not have(a, "t2f"):
        continue
    t2f = statistics.mean(float(r[col(a, "t2f")]) for r in rows)
    t2c = statistics.mean(float(r[col(a, "t2c")]) for r in rows)
    nh = statistics.mean(float(r[col(a, "nhalt")]) for r in rows)
    it = statistics.mean(float(r[col(a, "iters")]) for r in rows)
    if t2f == 0:
        print(f"  {a:<4} tier-2 never fired (as designed for this arm)")
        continue
    if a == "G1":
        # t2fire at src==1 is NOT comparable with src==2 and must not be read
        # as "how often tier-2 would have fired". At src==1 the verdict is
        # discarded, so the waiter keeps spinning and re-fires on the SAME
        # stale predecessor every 256 iterations; at src==2 the first fire
        # ends the pass, so one stale predecessor yields exactly one count.
        # The 2026-09-01 pilot measured 1.76M (G1) vs 26.6k (H1) at the same
        # threshold for exactly this reason.
        print(
            f"  G1   t2fire={t2f:,.0f} at src==1 -- NOT comparable to an src==2 arm "
            f"(verdict discarded, so the same stale predecessor is re-counted every "
            f"256 iterations). Diagnostic only."
        )
        continue
    print(
        f"  {a:<4} t2fire/t2chk={100*t2f/t2c if t2c else 0:6.3f}%   "
        f"t2fire/node_halts={100*t2f/nh if nh else 0:6.2f}%   "
        f"ceiling = t2fire*{SPIN_THRESHOLD}/node_iters = {100*t2f*SPIN_THRESHOLD/it if it else 0:6.2f}%"
    )

section("Per-round raw (primary metric), to eyeball consistency not just the mean")
print("  round " + "".join(f"{a:>18}" for a in arms))
for r in rows:
    print(f"  {r['round']:>5} " + "".join(f"{float(r[col(a,'iters')]):>18,.0f}" for a in arms))

print(
    "\nNote on t: with n<10 rounds this is a rough guide, not a p-value. "
    "Read it together with rounds_negative (a real effect should be consistent "
    "in sign round to round, not one outlier round carrying the mean)."
)
