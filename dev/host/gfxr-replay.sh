#!/bin/bash
# Replay a GFXReconstruct capture on the bare host (no VM) and report new GPU resets.
# Usage: ~/bvm/dev/host/gfxr-replay.sh CAPTURE.gfxr [times=1] [extra gfxrecon-replay options...]
#   e.g. ~/bvm/dev/host/gfxr-replay.sh ~/bvm/scratchpad/captures/20261104-2301/ctx7_pid1234.gfxr 5
#        ~/bvm/dev/host/gfxr-replay.sh cap.gfxr 3 --sfa -m rebind
# Run ~/bvm/dev/host/v3d-hangwatch.py in another terminal to see which engine stalls.  VM closed!
# VK_DRIVER_FILES=<vmdir>/gpu/broadcom_icd.json replays against our patched Mesa 25.0.7 (system Mesa otherwise).
f=${1:?usage: gfxr-replay.sh CAPTURE.gfxr [times] [gfxrecon-replay options]}
n=${2:-1}
shift; [ $# -gt 0 ] && shift
count() { dmesg | grep -c 'Resetting GPU for hang'; }
# captures made with the self-built layer must be replayed with the self-built tools (tools/build-gfxr.sh)
PREFIX=$HOME/gfxr/gfxr-install
if [ -x "$PREFIX/bin/gfxrecon-replay" ]; then
  export PATH="$PREFIX/bin:$PATH"
  export LD_LIBRARY_PATH="$PREFIX/lib:$PREFIX/lib/aarch64-linux-gnu:${LD_LIBRARY_PATH:-}"
fi
echo "using $(command -v gfxrecon-replay)"
before=$(count)
echo "resets before: $before"
for i in $(seq 1 "$n"); do
  echo "=== replay $i/$n: $f"
  t0=$(date +%s.%N)
  gfxrecon-replay "$@" "$f"
  rc=$?
  t1=$(date +%s.%N)
  printf 'replay %d exit %d, %.1f s, resets now %d\n' "$i" "$rc" "$(echo "$t1 - $t0" | bc)" "$(count)"
done
after=$(count)
echo "resets: $before -> $after  (+$((after-before)))"
