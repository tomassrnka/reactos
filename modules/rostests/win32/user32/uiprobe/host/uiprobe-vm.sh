#!/bin/bash
# uiprobe-vm.sh idle|firefox VM LABEL [SITE]: run uiprobe in a rosdev test VM (host side, vultr-kvm-2).
#   idle:    settle, then the probe for IDLE_SECS (default 30) in rounds of N samples.
#   firefox: the probe in a loop; after PRE secs (default 30) a cold Firefox start with a fresh profile
#            (the ffrun.sh pattern); the probe keeps running AFTER_SECS (default 120) after the launch.
# VM comes from `rosdev up` under ROSDEV_HOME. Results: $OUT/LABEL.{raw.txt,agg.txt,md,*.png,meta}.
# $L.vmstat is the host vmstat (1 s) over the run: compare stalls with host steal and load.
# Probe binary: $PROBE_DIR/<arch>/uiprobe.exe, arch from `rosdev info`, else PROBE_ARCH.
set -u
MODE=${1:?mode idle|firefox}; V=${2:?vm}; L=${3:?label}; SITE=${4:-https://example.com/}
export ROSDEV_HOME=${ROSDEV_HOME:-/root/nvme/sprobe/rosdev}
R=${ROSDEV:-/root/nvme/rosdev/host/rosdev}
HERE=$(cd "$(dirname "$0")" && pwd)
PROBE_DIR=${PROBE_DIR:-/root/nvme/sprobe/bin}
OUT=${OUT:-/root/nvme/sprobe/results}
N=${N:-15}; SETTLE=${SETTLE:-60}; IDLE_SECS=${IDLE_SECS:-30}; PRE=${PRE:-30}; AFTER_SECS=${AFTER_SECS:-120}
PROBE_EXTRA=${PROBE_EXTRA--q}   # detached probes write the -o file only: console output from a detached console process stalls the writer
FF=${FF:-C:\\Program Files\\Mozilla Firefox\\firefox.exe}
ARCH=${PROBE_ARCH:-$(python3 -c "import json;print(json.load(open('$ROSDEV_HOME/$V/vm.json'))['arch'])")}
mkdir -p "$OUT"; cd "$OUT" || exit 1
G="C:\\uiprobe\\$L.txt"
vmstat -n 1 > "$L.vmstat" 2>&1 & VMSTAT=$!
trap 'kill $VMSTAT 2>/dev/null' EXIT
step() { echo "$(date -u +%T) $*"; }

$R wait-ready "$V" --timeout 900 >/dev/null || { echo "VM $V not ready"; exit 1; }
step "ready; settling ${SETTLE}s"; sleep "$SETTLE"
$R push "$V" "$PROBE_DIR/$ARCH/uiprobe.exe" 'C:\uiprobe\' | tail -1
$R run "$V" "del $G & echo ok" >/dev/null 2>&1

wait_end() {
  for i in $(seq 1 90); do
    $R get "$V" "$G" "$L.raw.txt" >/dev/null 2>&1 && grep -q '^UIPROBE-END' "$L.raw.txt" && return 0
    sleep 10
  done
  echo "WARNING: probe did not end; partial data"; return 1
}

if [ "$MODE" = idle ]; then
  if [ -n "${DETACH:-}" ]; then
    $R run "$V" "C:\\uiprobe\\uiprobe.exe -n $N -loop $IDLE_SECS -raw -label $L -o $G $PROBE_EXTRA" --raw --detach > /dev/null 2>&1
    sleep "$IDLE_SECS"; wait_end
  else
    $R run "$V" "C:\\uiprobe\\uiprobe.exe -n $N -loop $IDLE_SECS -raw -label $L -o $G" --raw --timeout $((IDLE_SECS + 600)) > "$L.raw.txt" 2>&1
  fi
  python3 "$HERE/uiprobe-agg.py" "$L.raw.txt" --label "$L" --md "$L.md" > "$L.agg.txt"
  $R qshot "$V" "$L-end.png" >/dev/null 2>&1
  step "idle done: $OUT/$L.agg.txt"; exit 0
fi

# Firefox: fresh profile and a policy that sets the homepage (as ffrun.sh)
$R run "$V" 'taskkill /f /im firefox.exe & taskkill /f /im plugin-container.exe & echo ok' >/dev/null 2>&1; sleep 3
$R run "$V" "rmdir /s /q C:\\ffprof\\$L & echo ok" >/dev/null 2>&1
printf '{"policies":{"DisableAppUpdate":true,"DisableTelemetry":true,"DontCheckDefaultBrowser":true,"OverrideFirstRunPage":"","OverridePostUpdatePage":"","NoDefaultBookmarks":true,"Homepage":{"URL":"%s","StartPage":"homepage"}}}' "$SITE" > "pol-$L.json"
$R push "$V" "pol-$L.json" 'C:\Program Files\Mozilla Firefox\distribution\policies.json' | tail -1 | cut -c1-80
LOOP=$((PRE + AFTER_SECS))
$R run "$V" "C:\\uiprobe\\uiprobe.exe -n 5 -loop $LOOP -budget 30000 -raw -label $L -o $G $PROBE_EXTRA" --raw --detach > /dev/null 2>&1
P0=$(date +%s.%N); step "probe started"
sleep "$PRE"
F0=$(date +%s.%N); step "launching Firefox"
$R run "$V" "$FF -no-remote -profile C:\\ffprof\\$L" --raw --detach > /dev/null 2>&1
F1=$(date +%s.%N)
sleep 60; $R qshot "$V" "$L-60s.png" >/dev/null 2>&1
sleep 55; $R qshot "$V" "$L-115s.png" >/dev/null 2>&1
step "waiting for the probe to end"
wait_end
$R qshot "$V" "$L-end.png" >/dev/null 2>&1
$R run "$V" 'taskkill /f /im firefox.exe & taskkill /f /im plugin-container.exe & echo ok' >/dev/null 2>&1
OFF=$(python3 - "$P0" "$F0" "$F1" "$L.raw.txt" <<'PY'
import sys, re, datetime
p0, f0, f1, fn = float(sys.argv[1]), float(sys.argv[2]), float(sys.argv[3]), sys.argv[4]
off = f0 - p0
for line in open(fn, errors="replace"):
    m = re.match(r"UIPROBE-INFO .* started=(\S+)", line)
    if m:
        st = datetime.datetime.strptime(m.group(1), "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=datetime.timezone.utc).timestamp()
        o2 = f0 - st
        if 0 <= o2 <= 120:
            off = o2
        break
print("%.1f" % off)
PY
)
echo "launch_offset_s=$OFF host_launch_epoch=$F0 launch_call_returned_after_s=$(python3 -c "print(round($F1-$F0,1))") site=$SITE" > "$L.meta"
python3 "$HERE/uiprobe-agg.py" "$L.raw.txt" --label "$L" --launch "$OFF" --start-len 60 --after-len 60 --md "$L.md" > "$L.agg.txt"
step "firefox done: offset ${OFF}s, $OUT/$L.agg.txt"
