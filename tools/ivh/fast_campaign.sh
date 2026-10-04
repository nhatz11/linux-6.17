#!/bin/bash
# Quick regression check: 5 fast workloads across all 3 phases, THEN dedup.
# Not an investigation -- just "do we still win, and roughly by the recorded amount".
set -u
T=/root/ivh_tools
R="${1:-2}"; P="${2:-2}"
for ph in A B C; do
	echo "######## FAST PHASE $ph ########"
	bash $T/csmin_campaign.sh "$ph" "$R"
	echo "FAST_${ph}_COMPLETE"
done
echo "ALL_FAST_DONE"
for ph in A B C; do
	echo "######## DEDUP PHASE $ph ########"
	bash $T/dedup_phase.sh "$ph" "$P"
	echo "DEDUP_${ph}_COMPLETE"
done
echo "FULL_CAMPAIGN_DONE"
