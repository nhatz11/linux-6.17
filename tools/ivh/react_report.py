#!/usr/bin/env python3
"""Report one arm of the G-LOCK-46b react measurement.

ivh_cs_react[had_tail][state][bucket] is flattened by the counter reader into
6 rows of 32 buckets: row index = had_tail * 3 + state.

  rows 0,1,2 -> had_tail = 0 (no queue at release: there was no head to react,
                so these holds are NOT in the denominator)
  rows 3,4,5 -> had_tail = 1, states spinning / halted-on-its-own / self-set

Only row 4 is evidence. Row 5 is the pv_kick_node self-set case, excluded.
"""
import re
import sys

LO, HI = 20, 32          # buckets >= 476.6us at 2200 MHz: the preemption mode


def rows(s):
    return [{int(x): int(y) for x, y in re.findall(r'\((\d+),\s*(\d+)\)', p)}
            for p in s.split('|') if p.strip()]


def main():
    label, a_raw, b_raw, sa_raw, sb_raw = sys.argv[1:6]
    a, b = rows(a_raw), rows(b_raw)
    while len(a) < 6:
        a.append({})
    while len(b) < 6:
        b.append({})
    d = [{k: b[i].get(k, 0) - a[i].get(k, 0) for k in set(a[i]) | set(b[i])}
         for i in range(6)]

    def S(idx, lo, hi):
        return sum(max(d[idx].get(k, 0), 0) for k in range(lo, hi))

    spin, halted, self_ = S(3, LO, HI), S(4, LO, HI), S(5, LO, HI)
    real = spin + halted                       # self-set excluded entirely
    pct = (100.0 * halted / real) if real else float('nan')

    noq = S(0, LO, HI) + S(1, LO, HI) + S(2, LO, HI)
    sh_spin, sh_halt = S(3, 0, LO), S(4, 0, LO)
    sh = (100.0 * sh_halt / (sh_spin + sh_halt)) if (sh_spin + sh_halt) else float('nan')

    sa = [int(x) for x in sa_raw.split()]
    sb = [int(x) for x in sb_raw.split()]
    dl = [sb[i] - sa[i] for i in range(min(len(sa), len(sb)))] + [0, 0, 0, 0]
    exhaust, cs, suppressed, overwrote = dl[0], dl[1], dl[2], dl[3]

    print(f"{label:18} long+queued={real:5d}  halted={halted:5d}  {pct:5.1f}%"
          f"  | self-set excl={self_:<5d} no-queue={noq:<5d}"
          f" short-hold={sh:5.1f}%"
          f" | EXHAUST={exhaust} CS={cs} suppressed={suppressed} nested={overwrote}")


if __name__ == "__main__":
    main()
