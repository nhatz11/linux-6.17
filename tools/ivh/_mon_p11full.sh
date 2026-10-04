L=/root/ivh_logs/p11full_run.log
until grep -qE "P11FULL_DONE|FATAL" "$L" 2>/dev/null; do sleep 300; done
echo "########## P11 FULL SWEEP COMPLETE ##########"
sed -n '/FULL RESULT/,$p' "$L"
