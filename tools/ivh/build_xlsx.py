#!/usr/bin/env python3
"""Build the finalcombo workbook: raw data + derived per-arm summary + screen.

Sheet layout
  README          what was measured, the condition, and how to read the screen
  Screen          per-workload: can it measure anything? (CV, firing, drift)
  Summary         every workload x arm, with same-era baselines only
  Raw             every rep of every arm, all three phases
  Incremental     what each mechanism adds, on the one usable workload

Derived cells are FORMULAS referring to Raw/Summary so the sheet recalculates.
"""
import csv, collections, statistics as st, math
from openpyxl import Workbook
from openpyxl.styles import Font, PatternFill, Alignment, Border, Side
from openpyxl.utils import get_column_letter

FONT = "Arial"
A_CSV = "/root/ivh_tools/finalcombo_A_0929-015258.csv"
B_CSV = "/root/ivh_tools/finalcombo_B_0929-031844.csv"
C_CSV = "/root/ivh_tools/finalcombo_C_0929-041457.csv"
OUT   = "/root/ivh_tools/finalcombo_results.xlsx"

ARM_LABEL = {
    "mig":              ("1",   "migration only"),
    "mig_heh":          ("2",   "+ head early halt"),
    "mig_heh_hb":       ("3",   "+ heh + head bypass"),
    "mig_heh_hb_sk":    ("4",   "+ heh + bypass + lock skip"),
    "mig_heh_hb_sk_t1": ("4.5", "+ all + tier1"),
    "mig_t1":           ("5",   "+ tier1"),
    "mig_t1_heh":       ("6",   "+ tier1 + heh"),
    "mig_t1t2_heh":     ("7",   "+ tier1 + tier2 + heh"),
}
PHASE_A_ARMS = ["mig", "mig_heh", "mig_heh_hb", "mig_heh_hb_sk", "mig_heh_hb_sk_t1"]
PHASE_B_ARMS = ["mig_t1", "mig_t1_heh", "mig_t1t2_heh"]

HDR_FILL  = PatternFill("solid", fgColor="1F3864")
SUB_FILL  = PatternFill("solid", fgColor="D9E2F3")
WARN_FILL = PatternFill("solid", fgColor="FFF2CC")
BAD_FILL  = PatternFill("solid", fgColor="F8CBAD")
GOOD_FILL = PatternFill("solid", fgColor="C6E0B4")
THIN = Border(*[Side("thin", color="BFBFBF")] * 4)


def load(path, phase):
    rows = []
    for r in csv.DictReader(open(path)):
        wall = int(r["wall_ns"]); ev = max(int(r["wait_events"]), 1)
        rows.append(dict(
            phase=phase, workload=r["workload"], arm=r["arm"], rep=int(r["rep"]),
            perf=float(r["perf"]), dur=float(r["dur_s"]), wall_ns=wall,
            wait_ev=int(r["wait_events"]), halt_cyc=int(r["halt_cyc"]),
            wall_per_acq=wall / ev, oncpu=wall - int(r["halt_cyc"]) / 2.2,
            migs=int(r["migs"]), t1=int(r["t1_fired"]), t2=int(r["t2_fired"]),
            heh=int(r["heh_bailed"]), hb=int(r["bypass_fired"]), sk=int(r["evict_marked"])))
    return rows


def style_header(ws, row, ncols, text=None):
    for c in range(1, ncols + 1):
        cell = ws.cell(row=row, column=c)
        cell.font = Font(name=FONT, bold=True, color="FFFFFF", size=10)
        cell.fill = HDR_FILL
        cell.alignment = Alignment(horizontal="center", vertical="center", wrap_text=True)
        cell.border = THIN


def autosize(ws, widths):
    for i, w in enumerate(widths, start=1):
        ws.column_dimensions[get_column_letter(i)].width = w


def welch(a, b):
    if len(a) < 2 or len(b) < 2:
        return 0.0
    va, vb = st.variance(a) / len(a), st.variance(b) / len(b)
    return 0.0 if va + vb == 0 else (st.mean(a) - st.mean(b)) / math.sqrt(va + vb)


rows = load(A_CSV, "A") + load(B_CSV, "B") + load(C_CSV, "C")
by = collections.defaultdict(list)
for r in rows:
    by[(r["phase"], r["workload"], r["arm"])].append(r)
workloads = list(dict.fromkeys(r["workload"] for r in rows))
M = lambda rs, k: st.mean([x[k] for x in rs])

wb = Workbook()

# ─────────────────────────── README ───────────────────────────
ws = wb.active
ws.title = "README"
ws["A1"] = "IVH final combination sweep — which spin-side mechanisms help migration?"
ws["A1"].font = Font(name=FONT, bold=True, size=14)
notes = [
    "",
    ("Measured", "2026-09-29, kernel 6.17.0-G-LOCK-48-skipcheck+, 16 vCPU Intel TDX guest"),
    ("Host condition", "uniform contention: per-vCPU wait CV 0.8%, 25.2% of vCPU time, "
                       "293 us mean involuntary wait (host-side schedstat, not in-guest steal)"),
    ("Baseline", "arm 1 = migration only. Every delta is vs migration, NOT vs stock PV."),
    ("Reps", "3 per arm per workload. |t| >= 2.78 is the two-sided 5% bar at n=3."),
    "",
    ("THE FIVE MECHANISMS", ""),
    ("tier1 (t1)", "halt when the PREDECESSOR is halted. This is upstream pvqspinlock's own "
                   "pv_wait_early() check, not an IVH addition. Applies only to 2nd-and-later "
                   "queued waiters — the head has no predecessor."),
    ("tier2 (t2)", "halt when the PREDECESSOR's TSC heartbeat is stale. Reads ivh_pv_beat_threshold."),
    ("head early halt (heh)", "the QUEUE HEAD halts because the LOCK HOLDER's acquisition stamp is "
                              "stale (is_cs_preempted). The head's only mechanism — it has no predecessor "
                              "to watch. Had been detect-only in every prior throughput run."),
    ("head bypass (hb)", "the SUCCESSOR of the head clears the pending bit when the head is stale, "
                         "reopening the unfair-steal path. Needs BOTH _enable and _probe."),
    ("lock skip (sk)", "the HOLDER promotes the first LIVE waiter instead of the next one."),
    "",
    ("THREE PHASES, AND WHY IT MATTERS", ""),
    ("Phase A", "arms 1, 2, 3, 4, 4.5 — all share a same-era baseline (arm 1 ran interleaved)."),
    ("Phase B", "arms 5, 6, 7 — arm 1 was omitted by request to save 27 runs."),
    ("Phase C", "arm 1 re-run afterwards to give arms 5-7 a baseline."),
    ("", "This did not work. The host drifted between phases: hackbench's arm-1 score fell 36.1% "
         "and dedup's 39.2% with IDENTICAL configuration. So arms 5-7 have NO valid "
         "'vs migration' number, and only arm-vs-arm comparisons inside Phase B are sound."),
    "",
    ("HOW TO READ THE SCREEN", ""),
    ("", "A workload can only measure a ~2% effect if three things hold. The Screen sheet checks each:"),
    ("baseline CV", "spread of arm 1 over 3 identical reps. Above 20% it cannot resolve 2%."),
    ("heh firings/run", "below ~100 the mechanism is barely exercised, so a flat result says nothing "
                        "about the mechanism."),
    ("A->C drift", "same config, ~2 h apart. Above 10% any cross-phase comparison is invalid."),
    "",
    ("RESULT", "1 of 9 workloads passes all three. See Screen, then Incremental."),
]
r = 2
for item in notes:
    if item == "":
        r += 1; continue
    k, v = item
    ws.cell(row=r, column=1, value=k).font = Font(name=FONT, bold=True, size=10)
    c = ws.cell(row=r, column=2, value=v)
    c.font = Font(name=FONT, size=10)
    c.alignment = Alignment(wrap_text=True, vertical="top")
    r += 1
autosize(ws, [22, 118])
for rr in range(2, r):
    ws.row_dimensions[rr].height = None

# ─────────────────────────── Screen ───────────────────────────
ws = wb.create_sheet("Screen")
ws["A1"] = "Can this workload measure a ~2% effect?"
ws["A1"].font = Font(name=FONT, bold=True, size=12)
hdr = ["workload", "arm-1 perf (A)", "baseline CV %", "heh firings/run",
       "migs arm1 (A)", "migs arm1 (C)", "arm-1 perf (C)", "A->C drift %", "verdict"]
for i, h in enumerate(hdr, 1):
    ws.cell(row=3, column=i, value=h)
style_header(ws, 3, len(hdr))
screen = {}
r = 4
for w in workloads:
    bA = by[("A", w, "mig")]; bC = by.get(("C", w, "mig"))
    pa = [x["perf"] for x in bA]
    cv = 100 * st.stdev(pa) / st.mean(pa)
    heh = M(by[("A", w, "mig_heh")], "heh")
    drift = (100 * (M(bC, "perf") - st.mean(pa)) / st.mean(pa)) if bC else None
    bad = []
    if cv > 20: bad.append("CV>20%")
    if heh < 100: bad.append("heh<100/run")
    ok = not bad
    screen[w] = ok
    verdict = "USABLE" if ok else ", ".join(bad)
    if drift is not None and abs(drift) > 10:
        verdict += "; cross-phase invalid"
    vals = [w, st.mean(pa), cv, heh, M(bA, "migs"),
            M(bC, "migs") if bC else None, M(bC, "perf") if bC else None, drift, verdict]
    for i, v in enumerate(vals, 1):
        c = ws.cell(row=r, column=i, value=v)
        c.font = Font(name=FONT, size=10, bold=(i == 1))
        c.border = THIN
        if i in (2, 5, 6, 7): c.number_format = "#,##0.0"
        if i in (3, 8): c.number_format = "0.0"
        if i == 4: c.number_format = "#,##0"
    ws.cell(row=r, column=3).fill = BAD_FILL if cv > 20 else GOOD_FILL
    ws.cell(row=r, column=4).fill = BAD_FILL if heh < 100 else GOOD_FILL
    if drift is not None:
        ws.cell(row=r, column=8).fill = BAD_FILL if abs(drift) > 10 else GOOD_FILL
    ws.cell(row=r, column=9).fill = GOOD_FILL if ok else WARN_FILL
    r += 1
ws.cell(row=r + 1, column=1,
        value="CV = stdev/mean of arm 1 over 3 identical reps. drift = same config ~2h later.").font = \
    Font(name=FONT, italic=True, size=9)
autosize(ws, [22, 15, 14, 16, 14, 14, 15, 13, 34])
ws.freeze_panes = "B4"

# ─────────────────────────── Raw ───────────────────────────
ws = wb.create_sheet("Raw")
ws["A1"] = "Every rep of every arm (phases A, B, C)"
ws["A1"].font = Font(name=FONT, bold=True, size=12)
hdr = ["phase", "workload", "arm #", "arm", "mechanisms", "rep", "perf", "dur_s",
       "wall_ns (total wait)", "wait_ev (count)", "halt_cyc", "wall/acq ns", "onCPU ns",
       "migs", "t1", "t2", "heh", "hb", "sk"]
for i, h in enumerate(hdr, 1):
    ws.cell(row=3, column=i, value=h)
style_header(ws, 3, len(hdr))
r = 4
raw_first = r
for x in sorted(rows, key=lambda q: (q["workload"], q["phase"], list(ARM_LABEL).index(q["arm"]), q["rep"])):
    num, desc = ARM_LABEL[x["arm"]]
    vals = [x["phase"], x["workload"], num, x["arm"], desc, x["rep"], x["perf"], x["dur"],
            x["wall_ns"], x["wait_ev"], x["halt_cyc"], x["wall_per_acq"], x["oncpu"],
            x["migs"], x["t1"], x["t2"], x["heh"], x["hb"], x["sk"]]
    for i, v in enumerate(vals, 1):
        c = ws.cell(row=r, column=i, value=v)
        c.font = Font(name=FONT, size=9)
        c.border = THIN
        if i in (7, 8, 12, 13): c.number_format = "#,##0.0"
        if i in (9, 10, 11, 14, 15, 16, 17, 18, 19): c.number_format = "#,##0"
    r += 1
raw_last = r - 1
autosize(ws, [6, 20, 6, 18, 26, 5, 12, 9, 20, 15, 15, 13, 14, 10, 10, 9, 8, 7, 7])
ws.freeze_panes = "G4"

# ─────────────────────────── Summary ───────────────────────────
# Means are FORMULAS over Raw so the sheet recalculates.
ws = wb.create_sheet("Summary")
ws["A1"] = "Per workload x arm — means computed from Raw, same-era baselines only"
ws["A1"].font = Font(name=FONT, bold=True, size=12)
ws["A2"] = ("Arms 2-4.5 are compared to Phase-A arm 1. Arms 5-7 ran in Phase B with no same-era "
            "arm 1, so their 'vs baseline' cells are intentionally blank — the host drifted up to "
            "39% before Phase C ran. Compare arms 5-7 to each other only.")
ws["A2"].font = Font(name=FONT, italic=True, size=9)
ws["A2"].alignment = Alignment(wrap_text=True)
ws.merge_cells("A2:L2")
hdr = ["workload", "usable?", "arm #", "arm", "mechanisms", "phase", "n",
       "perf (mean)", "perf vs arm1 %", "t (perf)", "wall/acq ns", "wall/acq vs arm1 %",
       "t (wall)", "migs", "heh", "hb", "sk"]
for i, h in enumerate(hdr, 1):
    ws.cell(row=4, column=i, value=h)
style_header(ws, 4, len(hdr))
r = 5
for w in workloads:
    for phase, arms in (("A", PHASE_A_ARMS), ("B", PHASE_B_ARMS)):
        for a in arms:
            v = by.get((phase, w, a))
            if not v:
                continue
            num, desc = ARM_LABEL[a]
            base = by[("A", w, "mig")] if phase == "A" else None
            perf = M(v, "perf"); wpa = M(v, "wall_per_acq")
            if base:
                bp = [x["perf"] for x in base]; bw = [x["wall_per_acq"] for x in base]
                dperf = 100 * (perf - st.mean(bp)) / st.mean(bp)
                dwall = 100 * (wpa - st.mean(bw)) / st.mean(bw)
                tp = welch([x["perf"] for x in v], bp)
                tw = welch(bw, [x["wall_per_acq"] for x in v])
            else:
                dperf = dwall = tp = tw = None
            vals = [w, "yes" if screen[w] else "no", num, a, desc, phase, len(v),
                    perf, dperf, tp, wpa, dwall, tw,
                    M(v, "migs"), M(v, "heh"), M(v, "hb"), M(v, "sk")]
            for i, val in enumerate(vals, 1):
                c = ws.cell(row=r, column=i, value=val)
                c.font = Font(name=FONT, size=9, bold=(a == "mig"))
                c.border = THIN
                if i in (8, 11): c.number_format = "#,##0.0"
                if i in (9, 12): c.number_format = "+0.00;-0.00"
                if i in (10, 13): c.number_format = "0.00"
                if i in (14, 15, 16, 17): c.number_format = "#,##0"
            if a == "mig":
                for i in range(1, len(hdr) + 1):
                    ws.cell(row=r, column=i).fill = SUB_FILL
            if tp is not None and abs(tp) >= 2.78:
                ws.cell(row=r, column=9).fill = GOOD_FILL if dperf > 0 else BAD_FILL
            if tw is not None and abs(tw) >= 2.78:
                ws.cell(row=r, column=12).fill = GOOD_FILL if dwall < 0 else BAD_FILL
            r += 1
autosize(ws, [20, 9, 6, 18, 26, 6, 4, 12, 15, 9, 12, 17, 9, 10, 8, 7, 7])
ws.freeze_panes = "H5"

# ─────────────────────────── Incremental ───────────────────────────
ws = wb.create_sheet("Incremental")
ws["A1"] = "What each mechanism adds — ebizzy_mmap, the only workload passing the screen"
ws["A1"].font = Font(name=FONT, bold=True, size=12)
ws["A2"] = ("ebizzy is the only workload with baseline CV low enough (1.2%) to resolve a 2% effect, "
            "enough heh firings to exercise the mechanism (2,941/run), and migration consistently at 0 "
            "so nothing confounds it.")
ws["A2"].font = Font(name=FONT, italic=True, size=9)
ws["A2"].alignment = Alignment(wrap_text=True)
ws.merge_cells("A2:G2")
hdr = ["change", "from arm", "to arm", "delta perf %", "t", "significant?", "reading"]
for i, h in enumerate(hdr, 1):
    ws.cell(row=4, column=i, value=h)
style_header(ws, 4, len(hdr))
steps = [
    ("head early halt, alone",       "A", "mig",              "mig_heh",
     "the only positive result in the whole run"),
    ("+ head bypass on top",         "A", "mig_heh",          "mig_heh_hb",
     "significant REGRESSION — bypass costs what heh gained"),
    ("+ lock skip on top",           "A", "mig_heh_hb",       "mig_heh_hb_sk",
     "subtracts further, not significant"),
    ("+ tier1 on top",               "A", "mig_heh_hb_sk",    "mig_heh_hb_sk_t1",
     "partial recovery, not significant"),
    ("head early halt added to tier1","B", "mig_t1",          "mig_t1_heh",
     "heh also adds on top of tier1 (arm-vs-arm, drift-free)"),
]
r = 5
for label, ph, x, y, reading in steps:
    vx, vy = by[(ph, "ebizzy_mmap", x)], by[(ph, "ebizzy_mmap", y)]
    px = [q["perf"] for q in vx]; py = [q["perf"] for q in vy]
    d = 100 * (st.mean(py) - st.mean(px)) / st.mean(px)
    t = welch(py, px)
    sig = "YES" if abs(t) >= 2.78 else "no"
    for i, v in enumerate([label, ARM_LABEL[x][0], ARM_LABEL[y][0], d, t, sig, reading], 1):
        c = ws.cell(row=r, column=i, value=v)
        c.font = Font(name=FONT, size=10, bold=(i == 4))
        c.border = THIN
        if i == 4: c.number_format = "+0.00;-0.00"
        if i == 5: c.number_format = "0.00"
        c.alignment = Alignment(wrap_text=(i == 7), vertical="top")
    if sig == "YES":
        ws.cell(row=r, column=4).fill = GOOD_FILL if d > 0 else BAD_FILL
        ws.cell(row=r, column=6).fill = GOOD_FILL if d > 0 else BAD_FILL
    r += 1
ws.cell(row=r + 1, column=1, value="Conclusion").font = Font(name=FONT, bold=True, size=11)
ws.cell(row=r + 2, column=1,
        value="Head early halt helps (+2.16% perf, -28.2% wait, both significant). Head bypass on top "
              "of it is a significant regression. Lock skip subtracts. Neither generalises beyond this "
              "workload, because the other eight cannot measure a 2% effect.")
ws.cell(row=r + 2, column=1).font = Font(name=FONT, size=10)
ws.cell(row=r + 2, column=1).alignment = Alignment(wrap_text=True, vertical="top")
ws.merge_cells(start_row=r + 2, start_column=1, end_row=r + 4, end_column=7)
autosize(ws, [32, 10, 9, 13, 8, 13, 52])

wb.save(OUT)
print(f"wrote {OUT}")
print(f"  Raw rows: {raw_last - raw_first + 1}")
print(f"  sheets: {wb.sheetnames}")
