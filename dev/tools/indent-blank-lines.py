#!/usr/bin/env python3
# one-off: give empty lines inside a block the indentation of the block (min of previous and next non-blank line), only above the final "exit 0"
# usage: indent-blank-lines.py FILE OUT   (writes OUT, prints the changed line numbers)
import sys
src, out = sys.argv[1], sys.argv[2]
L = open(src).read().split('\n')
exit_i = max(i for i, l in enumerate(L) if l == 'exit 0')
ind = lambda s: len(s) - len(s.lstrip(' '))
changed = []
import re
heredoc = None   # the terminator of a here-document that is being read: its lines are data, never touched
for i in range(exit_i):
    if heredoc is not None:
        if L[i].strip() == heredoc:
            heredoc = None
        continue
    m = re.search(r"<<-?\s*['\"]?([A-Za-z_][A-Za-z_0-9]*)['\"]?", L[i])
    if m and not L[i].lstrip().startswith('#'):
        heredoc = m.group(1)
        continue
    if L[i] != '':
        continue
    p = next(j for j in range(i - 1, -1, -1) if L[j].strip())
    n = next(j for j in range(i + 1, len(L)) if L[j].strip())
    w = min(ind(L[p]), ind(L[n]))
    if w >= 2:
        L[i] = ' ' * w
        changed.append(i + 1)
open(out, 'w').write('\n'.join(L))
print(len(changed), 'lines:', changed)
