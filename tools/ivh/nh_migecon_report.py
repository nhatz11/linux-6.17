#!/usr/bin/env python3
import sys
CS={'5000':'11us','25000':'55us','50000':'110us','100000':'220us',
    '200000':'440us','300000':'660us','600000':'1.3ms'}
print(f"\n{'loop_spin':>9} {'CS':>6} {'migs':>8} {'mig/it':>7} {'cost':>8} {'delay':>9} "
      f"{'PV wait':>9} {'IVH wait':>9} {'saved':>9} {'spend':>8} {'ratio':>7} {'thr':>8}")
for ln in open(sys.argv[1]):
    f=ln.split()
    sp,pvi,pvw,ivi,ivw,nhm,nmig,tot,dly,nrun = f[0],int(f[1]),float(f[2]),int(f[3]),float(f[4]),int(f[5]),int(f[6] or 0),int(f[7] or 0),int(f[8] or 0),int(f[9] or 1)
    cost_us = tot/nmig/1000 if nmig else 0
    delay_us= dly/nrun/1000 if nrun else 0
    spend   = (tot+dly)/1e9
    # scale system-wide probe counts to NHextend's own migration count
    scale   = nhm/nmig if nmig else 0
    spend_s = spend*scale
    saved   = pvw-ivw
    ratio   = spend_s/saved if saved else float('nan')
    thr     = 100*(ivi-pvi)/pvi
    rs = f"{ratio:>7.2f}" if saved else f"{'n/a':>7}"
    print(f"{sp:>9} {CS.get(sp,'?'):>6} {nhm:>8,} {nhm/ivi:>7.2f} {cost_us:>7.1f}us {delay_us:>8.1f}us "
          f"{pvw:>8.1f}s {ivw:>8.1f}s {saved:>+8.1f}s {spend_s:>7.1f}s {rs} {thr:>+7.1f}%")
print("\n  cost  = decision -> arrived on destination rq")
print("  delay = on destination rq -> actually running")
print("  spend = (cost+delay) totals, scaled to NHextend's OWN migration count")
print("  ratio = spend / wait saved.  negative saved = wait got WORSE")
