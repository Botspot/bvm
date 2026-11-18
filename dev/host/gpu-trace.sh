#!/bin/bash
# Capture V3D kernel job events for a few seconds while the benchmark runs.
# Usage: sudo ~/bvm/dev/host/gpu-trace.sh [seconds=5]
# Output: ~/bvm/dev/host/gpu-trace-YYYYmmdd-HHMMSS.txt          (ftrace, clock = CLOCK_MONOTONIC seconds,
#                                            same clock as python time.monotonic() and the
#                                            NPT-Q-RETIRE *_us fields)
#         ~/bvm/dev/host/gpu-trace-threads-YYYYmmdd-HHMMSS.txt  (tid tgid comm cmdline, snapshot of all threads)
SECS=${1:-5}
T=/sys/kernel/tracing
[ -d $T ] || T=/sys/kernel/debug/tracing
H=${SUDO_USER:+/home/$SUDO_USER}; H=${H:-$HOME}
STAMP=$(date +%Y%m%d-%H%M%S)
OUT=$H/bvm/scratchpad/gpu-trace-$STAMP.txt
TH=$H/bvm/scratchpad/gpu-trace-threads-$STAMP.txt

snap() {
  for d in /proc/[0-9]*; do
    p=${d#/proc/}
    cmd=$(tr '\0' ' ' < $d/cmdline 2>/dev/null | cut -c1-60)
    for t in $d/task/*; do
      echo "${t##*/} $p $(cat $t/comm 2>/dev/null | tr ' ' _) $cmd"
    done
  done
}

echo 0 > $T/tracing_on
echo mono > $T/trace_clock
echo > $T/trace
echo 65536 > $T/buffer_size_kb
for ev in v3d gpu_scheduler dma_fence; do
  if [ -d $T/events/$ev ]; then echo 1 > $T/events/$ev/enable && echo "enabled: $ev"
  else echo "missing: $ev"; fi
done
snap > "$TH"
echo "trace start (monotonic): $(python3 -c 'import time;print("%.6f"%time.monotonic())')"
echo 1 > $T/tracing_on
sleep "$SECS"
echo 0 > $T/tracing_on
echo "trace end   (monotonic): $(python3 -c 'import time;print("%.6f"%time.monotonic())')"
for ev in v3d gpu_scheduler dma_fence; do
  [ -d $T/events/$ev ] && echo 0 > $T/events/$ev/enable
done
snap >> "$TH"
cp $T/trace "$OUT"
echo local > $T/trace_clock
chown ${SUDO_USER:-$USER} "$OUT" "$TH" 2>/dev/null
echo "saved $(wc -l < "$OUT") lines to $OUT, threads to $TH"
