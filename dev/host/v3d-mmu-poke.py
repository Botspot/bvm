#!/usr/bin/env python3
"""Read or change the V3D MMU control register (V3D_MMU_CTL, hub + 0x1200) through /dev/mem.

Why (README item 109): the kernel enables PT_INVALID_ABORT, WRITE_VIOLATION_ABORT and
CAP_EXCEEDED_ABORT.  In every GPU stall V3D_MMU_VIO_ID/ADDR show the PTB client touching an
unmapped GPU address (0xf0000000).  If the stall is the MMU aborting that access (the client then
never finishes), turning the abort bit off should make the hang disappear (the access then gets
the scratch page instead).  A GPU reset restores the kernel's value, so the change is lost at
the next hang reset (or reboot); `restore` puts it back by hand.

Usage (needs root):
  sudo ~/bvm/dev/host/v3d-mmu-poke.py status
  sudo ~/bvm/dev/host/v3d-mmu-poke.py noabort     # clear PT_INVALID_ABORT (bit 19) only
  sudo ~/bvm/dev/host/v3d-mmu-poke.py noabort-all # clear PT_INVALID/WRITE_VIOLATION/CAP_EXCEEDED abort bits
  sudo ~/bvm/dev/host/v3d-mmu-poke.py restore     # kernel default 0x060d0c01
"""
import mmap, os, re, struct, sys

PT_INVALID_ABORT = 1 << 19
WRITE_VIOLATION_ABORT = 1 << 11
CAP_EXCEEDED_ABORT = 1 << 26
KERNEL_DEFAULT = 0x060d0c01          # ENABLE|PT_INVALID_*|WRITE_VIOLATION_*|CAP_EXCEEDED_* (INT+ABORT)
MMU_CTL = 0x1200
NAMES = {0: 'ENABLE', 16: 'PT_INVALID_ENABLE', 17: 'PT_INVALID_EXC', 18: 'PT_INVALID_INT', 19: 'PT_INVALID_ABORT',
         20: 'PT_INVALID(status)', 9: 'WRITE_VIOL_EXC', 10: 'WRITE_VIOL_INT', 11: 'WRITE_VIOL_ABORT',
         12: 'WRITE_VIOL(status)', 24: 'CAP_EXC', 25: 'CAP_INT', 26: 'CAP_ABORT', 27: 'CAP_EXCEEDED(status)'}

def hub_base():
    for line in open('/proc/iomem'):
        m = re.match(r'\s*([0-9a-f]+)-([0-9a-f]+) : .*v3d hub', line)
        if m:
            return int(m.group(1), 16)
    sys.exit('v3d hub region not found in /proc/iomem (run as root)')

def main():
    if len(sys.argv) != 2 or sys.argv[1] not in ('status', 'noabort', 'noabort-all', 'restore'):
        sys.exit(__doc__)
    if os.geteuid() != 0:
        sys.exit('run with sudo')
    base = hub_base()
    page = (base + MMU_CTL) & ~0xfff
    off = (base + MMU_CTL) & 0xfff
    fd = os.open('/dev/mem', os.O_RDWR | os.O_SYNC)
    mm = mmap.mmap(fd, 0x1000, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=page)
    cur = struct.unpack_from('<I', mm, off)[0]
    print('hub base 0x%x, V3D_MMU_CTL = 0x%08x  [%s]' % (
        base, cur, ' '.join(n for b, n in sorted(NAMES.items()) if cur & (1 << b))))
    cmd = sys.argv[1]
    if cmd == 'status':
        return
    new = {'noabort': KERNEL_DEFAULT & ~PT_INVALID_ABORT,
           'noabort-all': KERNEL_DEFAULT & ~(PT_INVALID_ABORT | WRITE_VIOLATION_ABORT | CAP_EXCEEDED_ABORT),
           'restore': KERNEL_DEFAULT}[cmd]
    struct.pack_into('<I', mm, off, new)
    cur = struct.unpack_from('<I', mm, off)[0]
    print('wrote 0x%08x, now 0x%08x  [%s]' % (new, cur, ' '.join(n for b, n in sorted(NAMES.items()) if cur & (1 << b))))

main()
