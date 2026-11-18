# GPU mode: checklist

Goal: a **GPU** view mode (and **GPU fullscreen**) in BVM: virtio-gpu with Neptune/virglrenderer and DXVK on the host's Vulkan driver (Mesa V3DV on the Pi's V3D GPU; developed on
the Pi 5, to be tested on the Pi 4 and other boards), with Windows using the UTM Triton WDDM driver. Everything is built on the user's machine (patches in `resources/gpu-patches`).
The experiment this comes from lived in `~/bvm-gpu` (retired 2026-11-18); its dev log, the old notes and small tools are in `DEV-NOTES.md` and the other files of this `dev/` folder (developer-only, personal details removed). Style notes for the code: `CODE-STYLE.md` (same folder). The layout of the repository is explained in `../AGENTS.md`.
Last full review: 2026-11-18. **`[x]` = written; "tested" says what was actually run. Nothing has been run in a real Windows install from scratch yet.**

---

## 1. Decisions (do not revisit without a reason)
- **No undo logic for our patches and no auto-repair of Windows.** A changed pin or patch means the sources are downloaded again (never reverted, pulled or re-applied over old ones); a clean exit within 240 s only gets a hint to try `boot-ramfb`.
- Python 3 + Pillow, `samba`, `unzip`, `7z` are normal BVM dependencies (`install_dependencies`); the GPU build packages are installed by `gpu-setup` (`gpu_install_build_dependencies`).
- Everything GPU lives in `$vmdir/gpu/`; nothing is written into `bvm/` at run time. `$vmdir/gpu` is always safe to delete (`gpu-setup` rebuilds it). No `gpu-status`/`gpu-clean`/`gpu-patch` commands.
- Sources stay stock; our changes are the patch files (`resources/gpu-patches`). The original firmware is untouched; GPU mode uses a patched copy (logo).
- GPU mode is **not Pi 5 only**: preflight warns about untested boards but does not refuse; capability (render node, KVM, RAM, disk) is what counts.
- Audio is the default usb-audio only (no `AUDIO_DEVICE` switch). Pointer: `mouse=off` on the vdagent chardev. Windows restarts inside a running GPU QEMU work (the display was only lost in an
  early boot loop, README 155); no `-no-reboot`/relaunch logic.
- One build per VM folder (no shared `gpu_build_dir`, no lock: too few users have two VMs). No build progress display beyond the step lines. `mount`/`expand` never touch `gpu/`.
- **Three kinds of guest changes:** (1) one-time personal settings (theme, wallpaper, debloat, RDP, activation): `firstlogin.ps1`, once at the install, never again; (2) what every non-RDP display
  mode wants (host folder as Z:, clipboard, automatic login repair, power) and (3) the GPU driver (cheap, so merged into 2): `guest-gpu-setup.cmd`, repeatable.
- **Three user commands:** `gpu-setup` (host side only), `gpu-firstboot` (guest side only; ALWAYS boots Windows and runs the setup again, however often it was run), `boot-gpu` (refuses unless
  `gpu/state` is `transitioned`). The GUI click runs `gpu-setup`, then `gpu-firstboot` only if the state is not `transitioned`, then `boot-gpu`.

## 2. Host side: built (`bvm`, section "GPU mode (experimental)")
- [x] Foundation: pinned `gpu_*_repo`/`gpu_*_commit` variables, `gpu_hash`, `gpu_run_step` (hash markers in `gpu/done/`, logs in `gpu/logs/`, a failed step reruns), `gpu_preflight`
      (board warning, render node, RAM+swap, disk space, filesystem probe, moved folder via `gpu/location`, network), `gpu_git_clone` (one commit, shallow), `nproc` (2 threads under 4 GB).
- [x] Patch functions: `gpu_patch_apply` (fresh tree only; a marker in the tree's `.git` skips a patch that an interrupted run already applied), and the authoring helpers `gpu_patch_dir/base/run/generate` (the last one writes a patch from an edited tree; needed to
      maintain the patch files). **No revert/state/compare code:** a changed pin or patch downloads the sources again. Verified on fresh clones earlier: all patches apply; the marker logic tested on a synthetic repo.
- [x] Steps of `gpu_setup`: `build-dependencies`, `driver` (Triton v0.6, SHA256-checked), `spice-agent` (UTM guest tools, SHA256-checked), `mesa`, `dxvk`, `virglrenderer`, `qemu`, `firmware`. A moved VM folder is
      warned about, deleted and rebuilt. A 1-2 hour warning is printed before compiling. **After a successful build `gpu/src` (2 GB+) is always deleted** after a successful build; a changed pin or patch downloads it again.
      Tested: download, patch, verify, driver and agent unpack, firmware. **The compile steps were run by the user once (the build worked), not re-run since the refactoring.**
- [x] `boot-gpu` is a display mode of the `boot*` branch: env with library paths, render server, Vulkan ICD, NPT variables, `guest_hz` (config, default 30, checked 1-240), patched firmware, SDL display.
      The 240 s boot-failure guess is skipped in GPU mode. Wayland is not required (warning only).
- [x] **All modes except `boot-nodisplay` now get the clipboard channel (`clipboard_flags`) and the QEMU Samba share** (`shared_folder`, default `$HOME`, empty = off; Remmina uses the same folder).
- [x] `detect-zeroes=unmap` in every boot mode (confirmed working).
- [x] Config: `tpm=true` (software TPM 2.0 through `swtpm`, a dependency now; state in `$vmdir/tpm`; off by default; **never booted with it**). `bvm_debug=true` is the debug mode: more output and a core dump if QEMU crashes (`ulimit -c unlimited`, written to the folder
      BVM was started from). The diagnostic environment switches of the patches (`NPT_TRACE`, `BVM_DXVK_*`, `BVM_GFXR_*`, ...) stay developer-only: set them in the shell, they reach QEMU through `env`; see `DEV-NOTES.md`.
- [x] The firmware logo tool is inline in `bvm` (`gpu_firmware_logo`, python through a here-document; output byte-identical to the old script's; the old script with all its options is `dev/old-design/bvm-firmware-logo.py`). The `firmware` step hash covers the function text.
- [x] Config (`resources/bvm-config`): `shared_folder`, `guest_hz` documented. `help` text documents the three commands, the new-VM shortcut, deleting `gpu/`, and the revert one-liner.
- [x] GUI: `GPU (experimental)` and `GPU fullscreen` in the combo, click chain as above. GPU mode needs an installed VM: the Run button (and with it the combo) unlocks at `gui-steps-complete` 5, which the terminal
      modes write too (`new-vm` 1, `download` 2, `prepare` 3, `firstboot` 4 then 5); `gpu-firstboot` refuses a lower number or a missing file.
- [x] FreeRDP support removed (only the deprecation error of `connect-freerdp` stays); Remmina stays.

## 3. Guest side: built
**Files:** `resources/guest-gpu-setup.cmd` (plain cmd on purpose: PowerShell took ~2 min to start at logon), `guest-gpu-autologon.cmd`, `gpu-policy.py`, `firstlogin.ps1` (new block at the end), `guest-gpu-revert.ps1` (debugging only).
- [x] **`guest-gpu-setup.cmd`**: steps `autologon`, `smb-client`, `smb-map` (Z:), `serial-driver`, `spice-agent`, `gpu-driver` (restart, max 3), `power`; each has a version and a test, so finished steps are skipped. Window = progress display (header, debug info,
      every line also in `C:\bvm\setup.log`, colour = state, 10 s countdown then shutdown when `shutdown-when-done.flag` exists). Gives up after 6 runs. Removes itself when done (launcher, files, payload, credentials; log and
      registry state stay). On failure it waits for a key, except at a first boot (`firstboot.flag`: 60 s, so the install goes on).
      Limits: a password with a double quote cannot be used for the login repair; log lines must not contain `!` or `^`. Tested with Wine's cmd and shim tools (fresh, skipped, restart, finish, failure, give-up, first boot failure).
- [x] **One trigger only:** the launcher `bvm-guest-setup.cmd` in the all-users Startup folder (copies the setup to `C:\Windows\Temp\bvm-setup.cmd` and runs it in a visible window). Windows runs it ~3-4 min after QEMU starts.
- [x] **Autologon repair without a logon:** `guest-autologon.cmd` is the only local Group Policy startup script (`gpu_policy add|remove`, `gpu-policy.py`); it runs as SYSTEM at boot, fixes Winlogon (also "Windows Hello only") and restarts
      (max 2 in a row, log `C:\bvm\autologon.log`). Removed again when the setup finished. Hands-off test was done by the user on a real VM (worked).
- [x] **Existing VMs: `gpu-firstboot`** = `gpu_inject_guest` (mounts `disk.qcow2`, copies the staged files, adds the policy entry and launcher) + `gpu_transition` (plain ramfb boot, status ticker, a hint after 4 min if the login is broken,
      reads the log after the shutdown: needs a new run and `all steps passed`; sets `transitioned`). Tested on a real VM: whole transition ~5 min, the setup itself 10 s.
- [x] **New VMs:** the install step downloads the driver and agent (a failure only warns), `gpu_stage_guest` puts everything in `unattended/bvm`, `firstlogin.ps1` copies it to `C:\bvm` and runs the setup before its shutdown; after the
      install `firstboot` reads `setup.log` and writes `transitioned`. Tested: staging layout, `firstlogin.ps1` parses, the failing first-boot path under Wine. **Not tested: a real install.**
- [x] `guest-gpu-revert.ps1` + one-liner (in the script and `bvm help`): undoes the guest effects of `gpu-firstboot` for debugging; parse-checked only.

## 4. Still to do (not testing)
Small:
Decided not to do: versioned guest state (a `-2` can be appended when a guest-side change needs it), an `update_check` change (`git pull` brings everything and `gpu-setup` always runs), marking the mode experimental in messages
(the README does that), a tarball fallback for pinned commits, the update-hygiene guest step (the image is up to date), a `pvpanic` step.
- [ ] Mesa is always compiled from the pinned source (never the system's), so Trixie needs no Mesa decision. `gpu_build_packages` is the experiment's list, proven only on this Pi: check that every package exists and nothing is missing on a clean Bookworm and Trixie.
- [ ] Delete `~/bvm-gpu` and `~/v3d-repro` (everything worth keeping is in `DEV-NOTES.md` and `dev/`, see the "Not carried over" list there; the repro sources are in `dev/v3d-repro/`).
Bigger:
- [ ] **Upstream the patches** and report the V3D kernel reset-deadlock and Triton UMD/KMD issues (section 7).
- [ ] **Docs and changelog** (section 8).

## 5. Failure detection and auto-repair: removed (2026-11-18)
The old loop (QEMU exits with code 0 within 240 s -> mount the disk, delete `UCPD.sys`, run QEMU again) is gone, with `remove_UCPD_from_mountpoint`. Checked first: on the development VM (Windows 26100.9711)
`C:\Windows\System32\drivers\UCPD.sys` is back (dated 2026-11-06: Windows restored it with an update) and the VM boots and runs fine, so the repair is not needed there. What replaced it: when QEMU
ends cleanly within 240 s (and the mode is not `boot-ramfb`), one warning suggests `boot-ramfb` (simpler display, F8 for the Windows boot menu, 5 = safe mode with networking). Nothing is changed in the disk any more.
`remove_microsoft_defender` (debloat, used by `firstboot`) stays. Exit code 137 still gives the ZRAM hint.
- [ ] Not verified: the original UCPD bluescreen (issue 72) on other builds, e.g. the Pi 4's older Windows 22631 image. If a user reports it again, the answer is the ramfb safe-mode workaround in the warning.
- [ ] Optional: `qemu` stderr to `$vmdir/qemu.log`, and the last lines printed on a non-zero exit.

## 6. Known limitations to document
- [ ] The home-folder share gives Windows full access to the user's files; the guest allows password-less SMB logons on the QEMU user network.
- [ ] Audio quality depends on the `pipewire` group (nice -11) and the 120 ms ring; `AUDIO_DEVICE=hda/ich9` unsupported.
- [ ] 30 fps is the default (`guest_hz`); a Pi's GPU cannot do more.
- [ ] Only the Pi 5 (V3D 7.1, Mesa 25.0.x) was tested; say which boards are known to work after the Pi 4 test.
- [ ] GPU mode keeps ~0.6 GB per VM folder (~15 GB free needed while building).
- [ ] The Triton driver is a pre-release from the UTM project; Neptune is Windows-only (D3D11, no D3D12).
- [ ] The guest setup starts at a logon (3-4 min after QEMU starts); a broken login is repaired by the startup script, then needs one restart.
- [ ] Shrinking the image: free space only shrinks the qcow2 if it is zero-filled (`detect-zeroes=unmap`); protip for the docs.

## 7. Source patches (see `PRODUCTION-REVIEW.md` in the experiment)
- [ ] Mesa/V3DV: TMU configuration ordering (fixes "Resetting GPU for hang" bursts), supertile alignment, SPIR-V failure guard, dual-source blend: candidates for upstream (rebase onto current Mesa first).
- [ ] QEMU: SDL2 dmabuf scanout, SDL input polling, SDL text clipboard peer (checkpatch clean except line length).
- [ ] Optional cleanups: split diagnostics from the DXVK/virglrenderer patches, make the remaining env switches constants.
- [ ] Report the V3D kernel reset-deadlock (`dev/kernel-report/`) and the Triton issues to their projects.

## 8. Documentation and housekeeping
- [ ] User docs: what GPU mode is, requirements, build time and disk use, new VMs vs `gpu-firstboot`, the share, going back, known limitations.
- [ ] Changelog: GPU mode, clipboard + shared folder in all non-RDP modes, FreeRDP removed, `detect-zeroes=unmap` everywhere, new failure detection (when it exists).
- [ ] Licences: QEMU/virglrenderer/Mesa/DXVK are built from their own sources with our patches (publish the patches); the SPICE agent and the Triton driver are downloaded, not redistributed; check the Triton terms before advertising.
- [ ] Decide whether `dev/` (developer-only; ~1 MB, of which `DEV-NOTES.md` ~280 KB) is committed or kept out of the repository.

## 9. Testing (all still to do)
- [ ] **Fresh VM from scratch:** `new-vm` -> `download` -> `prepare` -> `firstboot` (guest setup at the end, GPU driver restart, `transitioned`) -> `gpu-setup` -> `boot-gpu`; also with `debloat=false`, other username/password, a password with special characters.
- [ ] Existing VM from an older BVM (no `gui-steps-complete`, old username/password): `gpu-firstboot` then `boot-gpu`.
- [ ] Clipboard and Z: in `boot-gtk`/`boot-ramfb`/`boot` with the stock QEMU (the chardev is accepted; the behaviour is not seen), and with the stock `viogpudo` display driver active.
- [ ] After a transition, inspect the disk (`bvm mount`): `C:\bvm` has only the logs, the policy entry, the launcher and `guest-autologon.cmd` are gone, Z: Run entry and step state present.
- [ ] Resume: Ctrl+C in each build step, power loss, full disk, network loss during clones, delete `gpu/` by hand, move/rename the VM folder after the build (must warn, delete and rebuild).
- [ ] `gpu-setup` after a pin/patch change: only dependent steps rerun, sources are downloaded again after the automatic deletion, including an interrupted run that had applied a patch (marker) and the regenerated Neptune headers.
- [ ] `tpm=true` (swtpm starts, Windows sees a TPM 2.0, swtpm is gone after QEMU exits) and `bvm_debug=true` (a killed QEMU, `kill -SEGV`, leaves a core in the start folder); `gpu_firmware_logo` after a `qemu-efi-aarch64` package update.
- [ ] Second GPU boot starts in seconds without sudo prompts; switch freely between ramfb/Direct/Remmina/GPU on one VM; stock firmware outside GPU mode.
- [ ] `guest-gpu-revert.ps1` then `gpu-firstboot`: compare the new setup log with the first.
- [ ] Pi 4 (V3D 4.2), Pi OS Bookworm and Trixie, a board without a Wayland session (X11 path untested).
- [ ] The "ran less than 240 s" hint (close a window early in `boot`, `boot-gtk` and `boot-gpu`; none in `boot-ramfb`); Remmina flow after the FreeRDP removal (boot-nodisplay + connect, reconnect, fullscreen); multiple VMs and a non-default `rdp_port`.
- [ ] `shellcheck` on the new functions (`IFS=$'\n'` review), `bash -n`, `pwsh` parse of the `.ps1` files.
