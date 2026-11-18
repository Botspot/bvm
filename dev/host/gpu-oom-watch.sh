#!/bin/bash
# Trace the V3D kernel IRQ / binner-overflow-memory path until the next GPU hang reset.
# Hypothesis (README item 108): the binner (PTB) runs out of tile-list memory, raises the one-shot
# V3D_INT_OUTOMEM IRQ, and the kernel's v3d_overflow_mem_work (schedule_work -> allocate a 256 KB BO
# -> MMU flush -> write PTB_BPOA/BPOS) is late, loses a race ("no active bin job") or fails, which
# leaves the binner stuck for ~1 s until the kernel resets the GPU.
# Uses the function_graph tracer on v3d_irq and v3d_overflow_mem_work (with durations and the
# calls they make), plus the v3d + gpu_scheduler events, all on the monotonic clock.
# Usage: sudo ~/bvm/dev/host/gpu-oom-watch.sh [max_seconds=300] [after_reset_seconds=3]
# Output: ~/bvm/scratchpad/gpu-oom-<stamp>.txt  (+ threads file gpu-oom-threads-<stamp>.txt)
MAXS=${1:-300}
AFTER=${2:-3}
T=/sys/kernel/tracing
[ -d $T ] || T=/sys/kernel/debug/tracing
H=${SUDO_USER:+/home/$SUDO_USER}; H=${H:-$HOME}
STAMP=$(date +%Y%m%d-%H%M%S)
OUT=$H/bvm/scratchpad/gpu-oom-$STAMP.txt
TH=$H/bvm/scratchpad/gpu-oom-threads-$STAMP.txt

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

for f in v3d_irq v3d_overflow_mem_work v3d_mmu_flush_all; do
  if grep -qw "$f" $T/available_filter_functions; then echo "traceable: $f"; else echo "NOT traceable: $f"; fi
done
grep -qw function_graph $T/available_tracers || { echo "no function_graph tracer in this kernel"; exit 1; }

echo 0 > $T/tracing_on
echo nop > $T/current_tracer
echo mono > $T/trace_clock
echo > $T/trace
echo 131072 > $T/buffer_size_kb
echo > $T/set_graph_function
for f in v3d_overflow_mem_work v3d_irq; do
  echo $f >> $T/set_graph_function
done
echo 1 > $T/options/funcgraph-abstime
echo 1 > $T/options/funcgraph-proc
echo function_graph > $T/current_tracer
for ev in v3d gpu_scheduler; do
  [ -d $T/events/$ev ] && echo 1 > $T/events/$ev/enable
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
# copy the buffer BEFORE changing the tracer: switching current_tracer clears it
cp $T/trace "$OUT"
for ev in v3d gpu_scheduler; do
  [ -d $T/events/$ev ] && echo 0 > $T/events/$ev/enable
done
echo nop > $T/current_tracer
echo > $T/set_graph_function
echo 0 > $T/options/funcgraph-abstime
echo 0 > $T/options/funcgraph-proc
snap >> "$TH"
echo local > $T/trace_clock
chown ${SUDO_USER:-$USER} "$OUT" "$TH" 2>/dev/null
echo "saved $(wc -l < "$OUT") lines to $OUT"
echo "threads: $TH"
echo "resets now: $(count)"
