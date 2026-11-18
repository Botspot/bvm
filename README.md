<a href="https://github.com/Botspot/pi-apps"><img src="https://github.com/Botspot/pi-apps/blob/master/icons/badge.png?raw=true" align="right"/></a>
![BVM](https://raw.githubusercontent.com/Botspot/bvm/refs/heads/main/resources/graphics/icon-128.png)  
## Botspot Virtual Machine - Windows 11 QEMU KVM on ARM Linux  
![20250304_01h53m41s_grim](https://github.com/user-attachments/assets/7b368dad-eb80-4579-be2e-12a737819f24)  
The Pi 5 just became a much more capable desktop replacement.  
Please [report](https://github.com/Botspot/bvm/issues) issues. Do not assume I am already aware of an issue, unless you can find an existing issue report, in which case please comment with something like "I'm having this problem too."  

### What to expect:
- A full Windows 11 ARM virtual machine on ARM Linux. Thanks to [KVM](https://en.wikipedia.org/wiki/Kernel-based_Virtual_Machine), this uses virtualization instead of emulation, resulting in no significant speed difference compared to installing Windows directly.
- A first-class setup experience. None of the usual sticking points are present here, thanks to how automated it is.
- It uses network passthrough to the Linux network stack, so Ethernet and WiFi all work out of the box.
- It uses audio passthrough to pipewire/pulseaudio/ALSA, so audio playback works out of the box.
- After installation and startup, the VM uses less than 1GB of RAM and minimal CPU when not in use, leaving plenty of resources free for Linux applications.
- The `connect` mode gives the VM access to files stored on Linux, and any changes are immediately synchronized. (Go to This PC and look for the `home` network share) The window modes share the same folder as drive `Z:`.
- It is capable of USB passthrough - any USB device can be made to directly communicate with Windows.
- Thanks to Microsoft's built-in [Prism emulator](https://learn.microsoft.com/en-us/windows/arm/apps-on-arm-x86-emulation), all Windows applications should work, including x86 and x64. Compare that to [Wine](https://pi-apps.io/install-app/install-wine-x64-on-raspberry-pi/), which fails on everything but old, simple programs.
- The graphics are snappy and quicker than you would expect, at least with the `connect` mode on Wayland on a Pi 5. Youtube and lightweight web games are actually somewhat playable without any overclocking or extra tweaks.
- **New, experimental: GPU mode.** Windows can draw with the Pi's GPU (DirectX 11 through [Mesa V3DV](https://docs.mesa3d.org/drivers/v3d.html)). See [GPU mode](#gpu-mode-experimental) below.

### What not to expect:
- A gaming rig. The regular modes have no graphics acceleration, so 3D features and WebGL won't work there. GPU mode is a first step: it accelerates DirectX 11 only (no DirectX 12), it is experimental, it has only been tested on a Raspberry Pi 5, and it runs the screen at 30 frames per second.

### Get started:
```
git clone https://github.com/Botspot/bvm
bvm/bvm help
```
Or, [install BVM from Pi-Apps.](https://pi-apps.io/install-app/install-botspot-virtual-machine-on-raspberry-pi/) It does the same thing, but makes it easier to uninstall. BVM will stay updated no matter which way it is installed.

<details>
<summary>Click to see what BVM does on first run.</summary>

BVM will install some dependencies. At the time of writing these are:
```
git jq wget genisoimage qemu-utils qemu-system-arm qemu-system-gui qemu-efi-aarch64 remmina remmina-plugin-rdp nmap wget yad uuid-runtime seabios ipxe-qemu wimtools socat ntfs-3g netcat-traditional bc python3 python3-pil samba unzip p7zip-full swtpm
```
(`samba` is for the folder that the window modes share with Windows: QEMU runs its own copy of `smbd`. The package also starts a Samba service on your computer, which BVM does not use; `sudo systemctl disable --now smbd nmbd` stops it. `swtpm` is only used if you enable the optional software TPM.)  
GPU mode installs more packages (compilers and development libraries) when you run `gpu-setup`, and only then.  
If you are on Arch or some other non-standard OS, you will need to install these manually. Hey Arch users: If you want to help out, consider changing the `install_dependencies` function in the `bvm` script to install dependencies, then send it to me in a Pull Request or something. I would love to support Arch but I don't use Arch.  
If Debian Bookworm is detected, BVM will set up the `bookworm-backports` APT repository in order to upgrade QEMU from 7.2 to 9.2. Version 7.2 does work, but it is missing pipewire audio output support, and besides, everyone knows newer is always better! If you have a good reason to not want bookworm-backports, please open an issue, explain why, and I may be willing to change this behavior.  

BVM also makes some icon symlinks in `~/.local/share/icons/hicolor/scalable/apps` to set the GUI's taskbar icon, and to override the QEMU taskbar icon. If this does not work on your distro, or if you do not like this feature, please get in contact with me.  
BVM now adds a menu launcher to `~/.local/share/applications/bvm.desktop` in the Office category to run the GUI. Contact me if you can make an argument that it belongs in a different category.  
BVM symlinks itself to `~/.local/bin/bvm` so it can be run in a terminal with a simple invokation of `bvm`.

</details>

### Usage instructions
Read the help message and follow the instructions. BVM has simplified the VM-creation process to a tidy sequence of completely automated steps. Between each step you have the opportunity to change what is happening, modify the config file, retry a step, or do whatever else you want. This split-step approach helps enable better learning about how it works, adjusting how it works, and creating new features on top.

To get a fresh VM up and running, use a sequence like this:  
- `bvm/bvm new-vm ~/win11`  
    This makes a config file: `~/win11/bvm-config` <--- Please read the config file!  
- `bvm/bvm download ~/win11`  
    This downloads Windows and necessary drivers. It also downloads the GPU driver and the clipboard agent (a few MB), so the install can set them up.  
- `bvm/bvm prepare ~/win11`  
    This bundles everything up to get ready for first boot.  
- `bvm/bvm firstboot ~/win11`  
    This boots the Windows installer, installs Windows and some drivers, sets up a local user account, debloats the OS, and runs the guest setup (the shared folder as drive `Z:`, clipboard sync, an automatic-login repair, power settings and the GPU driver). Please allow it to complete all steps automatically. When Windows finishes installing, the VM will shut down. Once it's done, you can delete all .iso files and the `unattended` folder from `~/win11` to reclaim some storage space.  
- `bvm/bvm boot ~/win11`  
    Main command to use the VM in a window. While this does work, it's a little laggy and you can't resize the screen. Copy and paste between Linux and Windows and the shared folder (drive `Z:`) work through the guest setup. It is recommended to boot the VM headless and then connect to it via RDP. (keep reading)  
- Boot Windows headless, without a screen:  
    `bvm/bvm boot-nodisplay ~/win11`  
- Then in a second terminal, connect to the RDP service:  
    `bvm/bvm connect ~/win11`  
    The connect mode has:
  - Better audio.
  - Clipboard synchronization.
  - File sharing. (Your home folder is accessible from This PC)
  - Dynamic screen resizing.
  - Higher performance graphics.
    Note: the connect mode used FreeRDP in the past; [now](https://github.com/Botspot/bvm/commit/4650cd52fafc499c0a21f636da11b1d5ed5504c0) it uses Remmina, and FreeRDP support has been removed.
- Mount the Windows main hard drive with:  
    `bvm/bvm mount ~/win11`  
    Direct file access can be useful for troubleshooting and further debloating.
- Expand the Windows main hard drive with:  
    `bvm/bvm expand ~/win11`
    It will ask how many gigabytes of space to add.

Full list of modes: `new-vm`, `download`, `prepare`, `firstboot`, `boot`, `connect`, `mount`, `help`, `list-languages`, `boot-nodisplay`, `boot-ramfb`, `boot-gtk`, `connect-remmina`, `expand`, `gui`, and for GPU mode `gpu-setup`, `gpu-firstboot`, `boot-gpu`  
That last one there deserves a mention. BVM has a graphical user interface.  
![20250304_01h55m15s_grim](https://github.com/user-attachments/assets/cc84632d-466d-4332-b6e2-382dd9277a7b)  
Run the GUI:
```
bvm/bvm gui
```
Or open the applications menu, go to Office, click Botspot Virtual Machine.  
Right now it is quite simplistic, but functional. It might stay that way, it might not. Much of BVM's future depends on how much of an impact it makes in the community. If nobody uses BVM, then I will stop working on it and find a new project.  

### GPU mode (experimental)
GPU mode lets Windows draw with the GPU of your board. The pieces: the [Triton](https://github.com/utmapp/viogpu-wddm) Windows display driver and [Neptune](https://github.com/utmapp/virglrenderer) (virglrenderer) from the UTM project send Windows' DirectX 11 commands to the host, where [DXVK](https://github.com/doitsujin/dxvk) turns them into Vulkan for [Mesa's V3DV driver](https://docs.mesa3d.org/drivers/v3d.html), on a patched QEMU. The result: a smooth desktop, DirectX 11 programs and browsers that use the GPU instead of the CPU. All of this is compiled on your computer.

**Be aware:**
- It is experimental. Expect rough edges, and please report what you find.
- It has only been tested on a Raspberry Pi 5 on a Wayland desktop. Other boards with a Vulkan-capable GPU and kernel 6.12 or newer might work: please tell me how it went, a Pi 4 is next on my list. BVM only warns on boards it has not seen.
- DirectX 11 only (no DirectX 12), and the screen runs at 30 frames per second, which is what a Pi's GPU can keep up with. Windows uses the unfinished pre-release Triton driver.
- The first build takes **1 to 2 hours** and needs about **15 GB of free space** in the VM folder while it works (about 0.6 GB stay afterwards).

**How to use it:**
- *New VM:* nothing special. `firstboot` already installed the GPU driver and the rest of the guest setup in Windows. Run `bvm/bvm gpu-setup ~/win11` to build the host side, then `bvm/bvm boot-gpu ~/win11`.
- *Existing VM:* run `bvm/bvm gpu-setup ~/win11` (downloads, patches and compiles everything; run it again any time, it skips what is done and rebuilds only what changed), then `bvm/bvm gpu-firstboot ~/win11`. That boots Windows once with a plain display. A window inside Windows shows the setup (about 5 minutes in all, with most of it Windows booting), then Windows shuts down by itself. Then `bvm/bvm boot-gpu ~/win11`. If the Windows login was broken (for example "Windows Hello only"), the setup repairs it by itself and restarts once.
- *GUI:* pick **GPU (experimental)** or **GPU fullscreen** as the viewing mode and press Run: it does all of the above in order, and skips what is finished.
- `gpu-firstboot` always boots Windows and runs the guest setup again, so it is also the way to retry after a failure.

**Settings** in `~/win11/bvm-config`:
- `shared_folder`: the folder shared with Windows, as drive `Z:` in the window modes and in the `connect` mode. It defaults to your whole home folder, so Windows can read, change and delete everything you can, and the VM may log in to it without a password. Point it at another folder, or empty (`shared_folder=''`) to share nothing.
- `guest_hz`: the refresh rate Windows thinks the screen has. The default is 30, which is also the recommendation on a Pi. Faster boards may like more.
- `tpm=true`: an optional software TPM 2.0. Windows works without one.
- `bvm_debug=true`: debug mode, with more output and a core dump if QEMU crashes.

**Troubleshooting:**
- GPU mode keeps its files in `~/win11/gpu/`: `logs/` has the log of every build step, `state` says whether Windows is prepared. Inside Windows the setup writes `C:\bvm\setup.log`. The whole `gpu` folder is safe to delete; `gpu-setup` builds it again. It can be left out of backups.
- If Windows seems stuck while it boots, start it with `bvm/bvm boot-ramfb ~/win11`: it uses a simpler display, and F8 there opens the Windows boot menu (5 = safe mode with networking).
- `bvm/bvm help` has a one-line command for debugging: run inside Windows, it undoes what `gpu-firstboot` did, so you can try again from a clean state.
- Audio gets priority only if your user is in the `pipewire` group (`sudo usermod -aG pipewire $USER`, then log in again). Without it, audio may crackle.

**How it fits together, for the curious:** everything GPU mode does is in the `gpu_*` functions of the `bvm` script and in `resources/`. The patches we apply to Mesa, DXVK, virglrenderer and QEMU are in `resources/gpu-patches/`. Windows-side files are the plain batch files `resources/guest-gpu-*.cmd`. Nothing here is downloaded as a binary except the Triton driver and the SPICE guest agent (from the UTM project's releases, checked against a SHA256), which are not redistributed in this repository. The licenses of QEMU, virglrenderer, DXVK and Mesa apply to what BVM builds from their sources.

### Tips:
- Use an ARM 64-bit Linux OS with the `kvm` kernel module enabled. This is a hard requirement.
- Use Wayland. This is not a hard requirement, but it makes a big difference. (GPU mode was only tested on Wayland.)
- Use ZRAM to avoid running out of RAM as easily. This is basically a hard requirement if you have a 4GB Pi model, but strongly recommended everywhere. [Instructions here.](https://pi-apps.io/install-app/install-more-ram-on-raspberry-pi/) It also helps a lot when compiling GPU mode.
- Use Debian Bookworm, Trixie, or an Ubuntu image from 2024 or newer.
- Some questions keep being asked. "How to use my own Windows ISO?" "How to change the drive size or the language or the screen resolution?" [Find the answers here.](https://github.com/Botspot/bvm/discussions/35)
- Encounter an issue? [Open an issue.](https://github.com/Botspot/bvm/issues) Deal? Deal. :)
- Please post how you use BVM in [the Show-And-Tell](https://github.com/Botspot/bvm/discussions/categories/show-and-tell)!

### Other announcements:
- **GPU mode (experimental):** Windows can now use the GPU. It took a lot of work: the host side (patched Mesa V3DV, DXVK, virglrenderer with Neptune, QEMU) is compiled on your machine by `bvm gpu-setup`, and new VMs get the Windows side during their install. See [GPU mode](#gpu-mode-experimental). Thanks to the UTM project for the Triton driver and Neptune, and to the Mesa, DXVK and QEMU developers.
- **Clipboard and shared folder in the window modes:** `boot`, `boot-gtk`, `boot-ramfb` and the GPU modes now share the clipboard and your folder (as drive `Z:`) with Windows, through the guest setup that `firstboot` installs. The folder is the new `shared_folder` setting; `connect` shares the same folder.
- **Gratitude**: Thanks to [Jeff Geerling](https://www.youtube.com/watch?v=mkfILjKJ8nc) and [Leepspvideo](https://www.youtube.com/watch?v=b7puJhWLQkU) for featuring BVM in their videos just 2 days after [release day](https://github.com/Botspot/bvm/commit/038a5b452f3b9691cc64355c8272967c17ac2e9a)!
- **Pi 4 working:** After [this commit](https://github.com/Botspot/bvm/commit/d0b0a1ff228bbebfc2250b97255048032f3df3c7), for hardware that lacks A76 CPU cores, such as the Raspberry Pi 4 and other SBCs, Windows 11 version 22631.2861 will be downloaded instead of the most recent public release. This is because the latest Windows 11 requires some extra CPU instructions.
- **RockChip RK3588 working:** BVM is now [configured](https://github.com/Botspot/bvm/commit/79fa60a1be0a9b9c2c6599f77b1a1d799e82eba9) to use only the performance cores, making it functional on Rock Pi 5, Orange Pi 5, and other single board computers that use this CPU. All ARM RockChip CPUs ought to work.
- **Other devices:** BVM has the potential to work on all modern ARM Linux distros and CPUs. It should be easy to support more SBCs, but I only own Raspberry Pi boards. Contact me if you have an ARM system not mentioned here - getting it working should be fairly straightforward if the kernel has `kvm`.
- **RAM allocation improvements:** If you encountered this error on or before February 27th: `bvm/bvm: line 662: 38680 Killed qemu-system-aarch64`, it should be fixed now. I [restructured](https://github.com/Botspot/bvm/commit/a4c85011e769269ef3655c8cb167cba979f93b82) how RAM allocation works to try to prevent the Out-Of-Memory killer from activating. The VM now dynamically adjusts its RAM usage once per second to always leave a bit free for Linux. On linux your RAM will always look nearly full, but there is actually plenty to spare, the moment you need it.
- **USB passthrough:** Now available and [recently](https://github.com/Botspot/bvm/commit/6355505d31b3fb15baa4c5d48bedf17457276fa4) [improved](https://github.com/Botspot/bvm/commit/ea306ee86c49480db61689417817467781a952e4) to correctly passthrough multiple devices at different USB speeds. Edit the config file to use the `usb_passthrough` variable, or click the USB passthrough button in the GUI.
- **QEMU stopping during startup (UCPD.sys):** Some sort of memory misalignment bug was being caused by the UCPD.sys driver, causing a Blue Screen of Death during startup. Numerous people [reported](https://github.com/Botspot/bvm/issues/72) the same general issue, and BVM used to delete the file automatically when QEMU stopped early. That repair is gone: Windows has put the file back and boots fine with it. If a window is closed within 4 minutes, BVM now only suggests trying `boot-ramfb`. If you still see the bluescreen, [tell me](https://github.com/Botspot/bvm/issues).
- **Expertise wanted:** If you have past experience with QEMU, I would like to ask for your ongoing help to help build out more features. Surely somebody will want to do serial passthrough, bluetooth passthrough, or something really specific that I will have no idea how to implement.
- **Windows 10 support:** It's possible, but I see little value in adding an option for it. If you have some solid reasons, I would be willing to change my mind, so please get in touch.
- **RemoteApp**: One exciting possibility is the chance to break out individual windows programs and integrate them directly into the linux desktop, using a RDP mode called RemoteApp. See a screenshot of this [here.](https://forums.raspberrypi.com/viewtopic.php?t=384433) Unfortunately it is [very inconsistent](https://github.com/FreeRDP/FreeRDP/issues/11218) right now, and I deemed it too unstable to include as a feature at the moment.

### Ask me anything!
- Who made this?  
    I'm [Botspot](https://github.com/Botspot), a college student, bash scripter, Raspberry Pi user, and founder of [Pi-Apps](https://github.com/Botspot/pi-apps) and [WoR-Flasher](https://github.com/Botspot/wor-flasher).
- Is this safe to use?  
    Some people are skeptical of free software. What if it has a virus or backdoor? In the case of BVM, all source code is easy to inspect. Read the code and see for yourself.
- Is this legal?  
    Yes. This project does not distribute copyrighted material, and the license key used is a free one provided by Microsoft for virtual machines. Read the files in `resources/` for more details.
- Is this unique?  
    Yes. Before BVM there existed no decent tool or tutorial, to do much of this. Other projects share some similar ideas though. The [QuickEMU project](https://github.com/quickemu-project/quickemu) probably comes closest, but it is not ARM-compatible. There is also the [UTM project](https://getutm.app/), which is more MacOS ARM focused. Windows ARM64 can be run in docker with the [Dockur project](https://github.com/dockur/windows-arm/). Of all the unique features here, the most unique is likely its flexibility or automation. You could change a wide variety of parameters, run all the steps in sequence with a script, walk away, and come back in a few hours to a fully-installed VM ready to use.  
    Before this, you would need to watch over it and press a key in a 5-second time window to boot the installer, install drivers, deploy some registry workarounds, then navigate through the installation steps, and debloat the OS later. BVM bypasses the keypress by patching the Windows installer ISO in a very unconventional way, (see the `patch_iso_noprompt` function), bypasses the installation steps and registry workarounds with an autounattend.xml file, and removes the bloatware using a couple PowerShell scripts. To my knowledge, nothing else does that by default with zero human intervention.  
- Does it have graphics acceleration?  
    Experimentally, yes: [GPU mode](#gpu-mode-experimental) accelerates DirectX 11 on a Raspberry Pi 5. It builds on the UTM project's Triton driver and Neptune, which were made for Apple and desktop GPUs. Making them work on the Pi's much smaller GPU took patches in Mesa, DXVK, virglrenderer and QEMU; they are all in `resources/gpu-patches/`.
- Why did you make this?  
    First, it will benefit me personally quite a bit over the long term. As a die-hard Raspberry Pi user without convenient access to another computer, with BVM I now have an alternative to Wine when I need to run a Windows-only program, such as Lego Mindstorms, Microsoft Office, or [this specific coding IDE needed for class](https://github.com/Botspot/bvm/discussions/39).  
    But also, I genuinely want to help you all out wherever possible. The ARM desktop community is special, but small. The more daily users we can keep around, the better the community can become for everyone. Some will oppose the promotion of a Microsoft product, saying it defeats the point of FOSS. But for most folks, it's either Linux with compromise, or no Linux at all. If we want a growing userbase, we should support any project that brings a great Linux desktop experience to all beginner users.
- What was the timeline for making this?  
    I have wanted to do a KVM Windows VM on a Pi ever since I was a high-schooler with a Pi 3, so 5+ years. The task is not trivial, and no good tutorials exist. Occasionally someone would get it working, but leave behind very incomplete instructions and not respond to questions. While I gave it a try every year or so, with partial success, I never "cracked" the code completely until February 2025 when I made up my mind to sit down and figure it out. This has been a multi-week effort of near total dedication, with a number of all-night coding sessions. It became an obsession.  
    Sidenote: I probably would have not succeeded without ChatGPT, especially for the Windows automation stuff. Anyone who says LLMs are useless is wrong.

### Contributing
Pull requests are welcome. [`AGENTS.md`](AGENTS.md) explains the layout of this repository, and [`dev/CODE-STYLE.md`](dev/CODE-STYLE.md) describes the code style: please read it before editing `bvm`. [`dev/GPU-MODE-CHECKLIST.md`](dev/GPU-MODE-CHECKLIST.md) lists what is done and what is left in GPU mode.

If communication on github is not your thing, [join my Discord server!](https://discord.gg/RXSTvaUvuu)
