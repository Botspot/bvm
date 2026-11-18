#!/usr/bin/env python3
"""Add or remove our startup script in Windows' local Group Policy files (used by gpu_inject_guest and gpu_transition in bvm).

  gpu-policy.py add|remove <existing gpt.ini or ''> <existing scripts.ini or ''> <outdir>

Writes <outdir>/gpt.ini and <outdir>/scripts.ini (the same encodings as Windows' own: gpt.ini Windows-1252, scripts.ini UTF-16 with a BOM) and prints one word:
  add:    'changed' or 'unchanged'. Existing entries are kept; our entry (cmd.exe /c C:\\bvm\\guest-autologon.cmd) is added once under [Startup], an older version of it
          (the PowerShell setup, guest-setup.ps1) is updated in place; the machine policy version is raised only when something changed, so Windows re-reads the
          local policy at the next boot.
  remove: 'removed' (our entry is gone, others are kept), 'empty' (removed and no entry is left: the caller can delete scripts.ini) or 'unchanged'.
The computer startup script runs as SYSTEM, whether somebody logs in or not (that is the point: it repairs a broken automatic login at the login screen).
"""
import os, re, sys

CMD = 'C:\\Windows\\System32\\cmd.exe'
PARAMS = '/c C:\\bvm\\guest-autologon.cmd'
OURS = ('guest-autologon.cmd', 'guest-setup.ps1')   # what marks an entry as ours (the second one is from the PowerShell versions)
EXT = '[{42B5FAAE-6536-11D2-AE5A-0000F87571E3}{40B6664F-4972-11D1-A7CA-0000F87571E3}]'


def read_text(path):
    if not path or not os.path.exists(path): return ''
    b = open(path, 'rb').read()
    if b[:2] in (b'\xff\xfe', b'\xfe\xff'): return b.decode('utf-16')
    if b[:3] == b'\xef\xbb\xbf': return b[3:].decode('utf-8')
    try: return b.decode('utf-8')
    except UnicodeDecodeError: return b.decode('cp1252')


def split_lines(text):
    lines = text.replace('\r\n', '\n').replace('\r', '\n').split('\n')
    while lines and not lines[-1].strip(): lines.pop()
    return lines


def startup_section(lines):
    """(index of [Startup] or None, index after the last line of that section)"""
    start = next((i for i, l in enumerate(lines) if l.strip().lower() == '[startup]'), None)
    if start is None: return None, len(lines)
    end = next((i for i in range(start + 1, len(lines)) if lines[i].strip().startswith('[')), len(lines))
    return start, end


def entries_of(lines, start, end):
    entries = {}
    for l in lines[start + 1:end]:
        m = re.match(r'\s*(\d+)(CmdLine|Parameters)\s*=\s*(.*)$', l, re.I)
        if m: entries.setdefault(int(m.group(1)), {})[m.group(2).lower()] = m.group(3).strip()
    return entries


def is_ours(entry):
    return any(o in entry.get('parameters', '').lower() for o in OURS)


def add_scripts(text):
    """Return (new text, changed)."""
    lines = split_lines(text)
    start, end = startup_section(lines)
    if start is None:
        lines += ['[Startup]']; start = len(lines) - 1; end = len(lines)
    entries = entries_of(lines, start, end)
    for n, e in entries.items():
        if e.get('cmdline', '').lower() == CMD.lower() and e.get('parameters', '').lower() == PARAMS.lower():
            return '\r\n'.join(lines) + '\r\n', False
        if is_ours(e):
            # an older version of our own entry: update it in place instead of adding a second one
            for i in range(start + 1, end):
                m = re.match(r'\s*%d(CmdLine|Parameters)\s*=' % n, lines[i], re.I)
                if m: lines[i] = '%d%s=%s' % (n, m.group(1), CMD if m.group(1).lower() == 'cmdline' else PARAMS)
            return '\r\n'.join(lines) + '\r\n', True
    n = max(entries) + 1 if entries else 0
    lines[end:end] = ['%dCmdLine=%s' % (n, CMD), '%dParameters=%s' % (n, PARAMS)]
    return '\r\n'.join(lines) + '\r\n', True


def remove_scripts(text):
    """Return (new text, state) with state 'unchanged', 'removed' or 'empty'."""
    lines = split_lines(text)
    start, end = startup_section(lines)
    if start is None: return '\r\n'.join(lines) + '\r\n', 'unchanged'
    entries = entries_of(lines, start, end)
    ours = [n for n, e in entries.items() if is_ours(e)]
    if not ours: return '\r\n'.join(lines) + '\r\n', 'unchanged'
    keep = lines[:start + 1]
    for l in lines[start + 1:end]:
        m = re.match(r'\s*(\d+)(CmdLine|Parameters)\s*=', l, re.I)
        if not (m and int(m.group(1)) in ours): keep.append(l)
    keep += lines[end:]
    left = [l for l in keep if re.match(r'\s*\d+CmdLine\s*=', l, re.I)]
    return '\r\n'.join(keep) + '\r\n', ('removed' if left else 'empty')


def merge_gpt(text, force_bump):
    lines = split_lines(text)
    if not any(l.strip().lower() == '[general]' for l in lines): lines.insert(0, '[General]')
    gi = next(i for i, l in enumerate(lines) if l.strip().lower() == '[general]')
    changed = False
    def find(key):
        for i in range(gi + 1, len(lines)):
            if lines[i].strip().startswith('['): break
            if re.match(r'\s*%s\s*=' % key, lines[i], re.I): return i
        return None
    i = find('gPCMachineExtensionNames')
    guids = re.findall(r'\{[0-9A-Fa-f-]+\}', EXT)
    if i is None:
        lines.insert(gi + 1, 'gPCMachineExtensionNames=' + EXT); changed = True
    else:
        cur = lines[i].split('=', 1)[1]
        if not all(g.upper() in cur.upper() for g in guids):
            lines[i] = lines[i].rstrip() + EXT; changed = True   # each CSE entry is [{cse}{tool}...]: append ours as another group
    if changed or force_bump:
        j = find('Version')
        if j is None:
            lines.insert(gi + 1, 'Version=1'); changed = True
        else:
            v = int(lines[j].split('=', 1)[1].strip() or 0)
            lines[j] = 'Version=%d' % ((v & 0xffff0000) | (((v & 0xffff) + 1) & 0xffff)); changed = True
    elif find('Version') is None:
        lines.insert(gi + 1, 'Version=1'); changed = True
    return '\r\n'.join(lines) + '\r\n', changed


def main():
    action, gpt_in, scr_in, out = sys.argv[1:5]
    os.makedirs(out, exist_ok=True)
    if action == 'add':
        scr, scr_changed = add_scripts(read_text(scr_in))
        gpt, gpt_changed = merge_gpt(read_text(gpt_in), scr_changed)
        result = 'changed' if (scr_changed or gpt_changed) else 'unchanged'
    elif action == 'remove':
        scr, result = remove_scripts(read_text(scr_in))
        gpt = '\r\n'.join(split_lines(read_text(gpt_in))) + '\r\n'
    else:
        sys.exit('usage: gpu-policy.py add|remove GPT_IN SCRIPTS_IN OUTDIR')
    open(os.path.join(out, 'scripts.ini'), 'wb').write(b'\xff\xfe' + scr.encode('utf-16-le'))
    open(os.path.join(out, 'gpt.ini'), 'wb').write(gpt.encode('cp1252'))
    print(result)


if __name__ == '__main__':
    main()
