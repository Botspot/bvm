#!/bin/bash
# Sample V3D engine busy time (DRM fdinfo) summed over all virgl_render_server
# processes, over INTERVAL seconds. Run while the benchmark is going.
INTERVAL=${1:-10}
snap() {
  for p in $(pgrep -f "${PATTERN:-virgl_render_server}"); do
    for f in /proc/$p/fdinfo/*; do
      awk -v p="$p" '/^drm-driver:/{d=$2} /^drm-client-id:/{id=$2}
        /^drm-engine-(render|bin|cache-clean|csd|tfu):/{print p, id, $1, $2}' "$f" 2>/dev/null
    done
  done | sort -u
}
a=$(snap); sleep "$INTERVAL"; b=$(snap)
python3 -I - "$INTERVAL" <<E
import sys,collections
iv=float(sys.argv[1])
def parse(s):
    d={}
    for l in s.splitlines():
        p,i,k,v=l.split(); d[(p,i,k)]=int(v)
    return d
a=parse("""$a"""); b=parse("""$b""")
tot=collections.Counter()
for k,v in b.items():
    tot[k[2]]+=v-a.get(k,0)
for k,v in sorted(tot.items()):
    print("%-28s %6.1f%% busy"%(k,v/1e9/iv*100))
E
