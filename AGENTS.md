# BVM: guide for people and agents working on this repository

BVM (Botspot Virtual Machine) is **one bash script, `bvm`**, that downloads, installs and runs a Windows 11 ARM VM with QEMU/KVM on ARM Linux (Raspberry Pi 5 first, other ARM boards too).
It has a terminal interface (modes such as `boot`, `connect`, `gpu-setup`) and a `yad` GUI that calls the same modes. Everything about one VM lives in its own folder (`$vmdir`, e.g. `~/win11`).

## Before you edit anything: read `dev/CODE-STYLE.md`

The code is written in a specific hand-written style (functions at the top, every mode as one long top-to-bottom recipe in the big `case "$mode"`, comment conventions, `IFS=$'\n'` everywhere,
`error`/`warning`/`status` helpers, variable naming). New code that ignores it gets rewritten. Read the whole file first, then look at a neighbouring piece of `bvm` and match it.
Short version of what bites people most:
- `bvm` sets `IFS=$'\n'` globally: word splitting, `for x in $list` and `${var//,/ }` need a local `IFS` or an array.
- A function exists only when something is used in more than one place (or is a long, self-contained helper). Modes stay inline.
- Blank lines inside a block keep the indentation of the block (`dev/tools/indent-blank-lines.py` enforces it; it skips here-documents).
- Errors go through `error "function_name: what failed"` (it exits), warnings through `warning`, progress through `status`/`status_green`.
- After editing: `bash -n bvm`. Nothing after the final `exit 0` of `bvm` is code.

## Layout of the repository

```
bvm                      the whole program (bash). Sections, top to bottom: helper functions, GPU mode functions (gpu_*), config loading + defaults,
                         then one big  case "$mode" in  ...  esac  with every mode, then  exit 0
README.md                user documentation (what BVM is, how to install and use it, FAQ, changelog)
COPYING                  licence (GPL v3)
AGENTS.md                this file
resources/               everything the script copies, reads or injects at run time
  bvm-config               the commented config file that new-vm copies into the VM folder (every user setting is documented here)
  autounattend.xml         Windows unattended install answers (language, user, drivers, FirstLogonCommands)
  firstlogin.ps1           runs once inside Windows at the end of the install: personal one-time settings (theme, RDP, activation, debloat),
                           then the guest setup below if the host staged it
  guest-gpu-setup.cmd      repeatable guest setup in plain cmd (host folder as Z:, clipboard agent, automatic login, power settings, GPU driver)
  guest-gpu-autologon.cmd  Group Policy startup script that repairs a broken automatic login before anybody logs in
  gpu-policy.py            adds/removes that startup script in the local Group Policy files of a mounted Windows disk
  guest-gpu-revert.ps1     debugging aid: undoes the guest effects of gpu-firstboot inside Windows (never injected, one-liner is in `bvm help`)
  gpu-patches/             our patches for Mesa, DXVK, dxbc-spirv, virglrenderer, QEMU (README.md explains how to work on them)
  graphics/                icons, wallpaper, the firmware boot logo (png + svg)
  connect.remmina          Remmina profile template for the RDP viewer
  rdp-detector.nse         nmap script that detects when the VM's RDP port is up
  terminal-run             helper that runs a command in whatever terminal emulator exists (used by the GUI)
dev/                     developer-only material; none of it is used at run time
  CODE-STYLE.md            the code style (read it before editing)
  GPU-MODE-CHECKLIST.md    what GPU mode is made of, what is done, what is left, the decisions taken
  DEV-NOTES.md             big data dump: the GPU experiment's development log, old notes, where everything went
  host/ guest/             monitoring, capture/replay and Windows diagnostic scripts for debugging the GPU stack
  v3d-repro/               standalone Vulkan reproducers of the V3D GPU hang (for bug reports)
  kernel-report/ patches/  notes for the kernel report; diagnostics patches and the upstream-candidates review
  webtest/                 minimal web pages for isolating rendering bugs in the VM
  tools/                   maintenance helpers (indent-blank-lines.py)
.github/                 funding file only
.claude/                 local assistant settings (not part of the program)
```

## What is in a VM folder (`$vmdir`)

`disk.qcow2` (Windows), `bvm-config` (this VM's settings, sourced by the script), `installer.iso`, `unattended/` and `unattended.iso` (install-time files, including `unattended/bvm/` = the staged guest setup),
`qemu.pid`/`qemu.sock` (running VM), `gui-steps-complete` (1-5, written by the terminal modes too), and **`gpu/`** for GPU mode:
`src/` (downloaded sources, deleted after a successful build), `sysroot/` and `mesa/` (our QEMU, virglrenderer, DXVK, Mesa), `firmware/`, `drivers/`, `spice-agent/`, `done/<step>` (hash markers),
`logs/`, `state` (`injected` or `transitioned`), `location` (detects a moved folder). `gpu/` is always safe to delete; `gpu-setup` rebuilds it.

## The modes of `bvm`

Setup, in order: `new-vm` (makes the folder and config), `download` (Windows ISO, drivers, debloat script, GPU driver + clipboard agent), `prepare` (builds `unattended.iso`), `firstboot` (installs Windows).
Everyday: `boot` and `boot-gtk`/`boot-ramfb` (a QEMU window), `boot-nodisplay` + `connect` (RDP through Remmina), `boot-gpu` (GPU mode), `mount`, `expand`, `gui`, `help`.
GPU mode: `gpu-setup` (host side: downloads, patches, compiles; always run first, skips finished steps by hash), `gpu-firstboot` (guest side for an existing VM: always boots Windows once and runs the guest setup),
`boot-gpu` (refuses unless `gpu/state` is `transitioned`). The `gpu_*` functions near the top of `bvm` implement them: `gpu_hash`, `gpu_run_step`, `gpu_preflight`, `gpu_git_clone`, `gpu_patch_*`, `gpu_build_*`,
`gpu_stage_guest`, `gpu_inject_guest`, `gpu_transition`, `gpu_policy`, `gpu_setup`, `gpu_firstboot`.

## Common changes and where they go

- **A new config setting:** default it right after the config file is sourced (next to `shared_folder`, `guest_hz`, `tpm`), document it in `resources/bvm-config`, mention it in `help` if users need to know.
- **A change to the guest setup (Windows side):** `resources/guest-gpu-setup.cmd` (add a `call :step` line plus `:test_X`/`:run_X`, bump the step version). It is plain cmd on purpose (PowerShell is too slow to start at logon);
  the file is staged with Windows line endings by `gpu_stage_guest`. It can be tested piecewise with Wine's `cmd` and shim tools; it has not been run in a real Windows install from scratch yet.
- **A change to one of our patches:** follow `resources/gpu-patches/README.md`. A changed pin or patch means that component is downloaded again and rebuilt (and what depends on it); there is no revert logic on purpose.
- **A new dependency:** add it to both `required_commands` and `apt_packages` in `install_dependencies`. The user installs packages; scripts and agents do not run apt/pip.

## Working rules

- Do not run a whole VM, a multi-hour build or anything that boots Windows to "test" a change; test pieces (functions sourced with `source bvm source`, synthetic files, Wine for batch files) and say what was not run.
- Do not commit or publish; the maintainer does that by hand.
- Keep personal details (home paths, passwords, MAC addresses, e-mail) out of every file in the repository.
- Throwaway files go in `scratchpad/` (create it; it is never committed), not in `/tmp`.
- Read `dev/GPU-MODE-CHECKLIST.md` for the current state of GPU mode before changing it, and keep it current when you finish something or decide something.
