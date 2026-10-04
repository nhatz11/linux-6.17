#!/bin/bash
# All three phases, both harnesses. Usage: full_campaign.sh [REPS] [PAIRS]
#   A  THRESH=4000000 gate2_ref=0   (documented shipped state)
#   B  THRESH=2500000 gate2_ref=0   (1.5ms head budget + 1ms delta)
#   C  THRESH=2500000 gate2_ref=1   (same, Gate 2 reads min_cs_ns)
set -u
T=/root/ivh_tools
R="${1:-4}"; P="${2:-3}"
for ph in A B C; do
	echo "################################ PHASE $ph ################################"
	bash $T/csmin_campaign.sh "$ph" "$R"
	bash $T/dedup_phase.sh    "$ph" "$P"
	echo "PHASE_${ph}_COMPLETE"
done
echo "FULL_CAMPAIGN_DONE"
