#!/bin/bash
# Continuous V3D kernel trace that stops itself when the GPU hangs.
# Start it BEFORE doing the thing that freezes (e.g. hover the taskbar search box in the VM),
# leave it running; when a `Resetting GPU for hang` appears in dmesg it waits a few seconds
# (to catch the second reset of a pair), then saves the last part of the ring buffer.
# Usage: sudo ~/bvm/dev/host/gpu-trace-watch.sh [max_seconds=300] [after_reset_seconds=3]
# Output: ~/bvm/dev/host/gpu-trace-hang-YYYYmmdd-HHMMSS.txt and gpu-trace-threads-hang-YYYYmmdd-HHMMSS.txt
#         (same format/clock as gpu-trace.sh; analyse with the scratchpad inflight.py)
# Only the v3d and gpu_scheduler events are traced (no dma_fence) so minutes fit in the buffer.
MAXS=${1:-300}
AFTER=${2:-3}
T=/sys/kernel/tracing
[ -d $T ] || T=/sys/kernel/debug/tracing
H=${SUDO_USER:+/home/$SUDO_USER}; H=${H:-$HOME}
STAMP=$(date +%Y%m%d-%H%M%S)
OUT=$H/bvm/scratchpad/gpu-trace-hang-$STAMP.txt
TH=$H/bvm/scratchpad/gpu-trace-threads-hang-$STAMP.txt

snap() {
  for d in /proc/[0-9]*; do
    p=${d#/proc/}
    cmd=$(tr '\0' ' ' < $d/cmdline 2>/dev/null | cut -c1-60)
    for t in $d/task/*; do
      echo "${t##*/} $p $(cat $t/comm 2>/dev/null | tr ' ' _) $cmd"
    done
  done 2>/dev/null
}
count() { dmesg | grep -c 'Resetting GPU for hang'; }

echo 0 > $T/tracing_on
echo mono > $T/trace_clock
echo > $T/trace
echo 131072 > $T/buffer_size_kb
for ev in v3d gpu_scheduler; do
  [ -d $T/events/$ev ] && echo 1 > $T/events/$ev/enable && echo "enabled: $ev"
done
snap > "$TH"
start=$(count)
echo "tracing; waiting for a GPU reset (resets so far: $start, max ${MAXS}s)... go hover / freeze it now"
echo 1 > $T/tracing_on
t0=$SECONDS
while [ $((SECONDS - t0)) -lt "$MAXS" ]; do
  if [ "$(count)" -gt "$start" ]; then
    echo "reset detected at monotonic $(python3 -c 'import time;print("%.3f"%time.monotonic())'); collecting ${AFTER}s more"
    sleep "$AFTER"
    break
  fi
  sleep 0.2
done
echo 0 > $T/tracing_on
for ev in v3d gpu_scheduler; do
  [ -d $T/events/$ev ] && echo 0 > $T/events/$ev/enable
done
snap >> "$TH"
cp $T/trace "$OUT"
echo local > $T/trace_clock
chown ${SUDO_USER:-$USER} "$OUT" "$TH" 2>/dev/null
echo "saved $(wc -l < "$OUT") lines to $OUT"
echo "threads: $TH"
echo "resets now: $(count)"
