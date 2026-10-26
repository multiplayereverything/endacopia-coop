#!/usr/bin/env bash
# Full Endacopia co-op suite on the current build: every scenario, the bad-network profiles (fresh guest each),
# then the mutation twins. Prints one line per scenario and a SUITE line at the end.
cd "$(dirname "$0")/.."
PY="${MP_PYTHON:-python}"
pass=0; fail=0
run() {  # run <name> <scenario> [fresh]
  [ "$3" = fresh ] && rm -rf "$LOCALAPPDATA/EndacopiaMP/lab-guest/saves"
  timeout 900 node tools/mptest.js "$2" > "tools/out/suite-$1.log" 2>&1
  r=$(tail -1 "tools/out/suite-$1.log")
  h=$(grep -oE "HOST   OK tick=[0-9]+" "tools/out/suite-$1.log" | tail -1 | grep -oE "[0-9]+$")
  g=$(grep -oE "GUEST  OK tick=[0-9]+" "tools/out/suite-$1.log" | tail -1 | grep -oE "[0-9]+$")
  lag=""; [ -n "$h" ] && [ -n "$g" ] && lag="lag $((h - g)) frames"
  echo "$1: $r $lag"
  case "$r" in "RESULT: PASS"*) pass=$((pass + 1));; *) fail=$((fail + 1));; esac
}
node tools/mptest.js --selftest | tail -1
run smoke tools/scenarios/smoke.txt
run probes tools/scenarios/probes.txt
run join-holding tools/scenarios/join-holding.txt
run steam tools/scenarios/steam.txt
for f in tools/scenarios/saves/*.txt; do run "$(basename "$f" .txt)" "$f"; done
for p in good_wifi bad_wifi transatlantic mobile_tether hell; do run "net-$p" "tools/scenarios/net/$p.txt" fresh; done
echo "SUITE: $pass passed, $fail failed"
"$PY" -I coop-anything/tools/mutants/run_twins.py tools/twins.json > tools/out/suite-twins.log 2>&1
grep -E "caught|MISSED|WRONG|twin\(s\) not caught|ALL OK" tools/out/suite-twins.log
