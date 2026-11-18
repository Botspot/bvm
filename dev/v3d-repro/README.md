# v3d-repro: standalone Vulkan reproducer for the Pi 5 V3D "Resetting GPU for hang"

Goal: show (or rule out) that the GPU hang seen with the Triton/DXVK/V3DV VM stack can be triggered
by plain Vulkan on stock Mesa, without Windows, DXVK, virglrenderer or QEMU, so it can be reported
upstream (raspberrypi/linux, Mesa) with a small test case. Background: `~/bvm/DEV-NOTES.md`,
items 104-110.

## What it does

`hang-repro` replays the *kind* of work the guest DWM sends when the hang happens:
chains of render passes that LOAD and STORE a small BGRA8 attachment and draw nothing, with barriers
between them, on images suballocated from one shared 32 MB memory block (like DXVK), e.g.

* `yt` scenario (the pair that hung ~every time during the YouTube test):
  `1920x970, 2048x128, 1024x64, 512x32, 256x16, 128x16, 120x8 (on a 128x16 image) x2, 1897x112`
* `shadow` scenario (hung once per Windows boot at the first tooltip shadow):
  `80x80 clear, 128x128 clear x2, 512x512 x3, 80x80, 64x64 clear x3`
* `small`: only the 128x16 / 120x8 / 120x8 trio.

Each iteration is one command buffer (or one submit per pass), repeated every ~4 ms like a compositor
frame. A submit that takes more than 300 ms is printed as `SLOW`; the kernel resets the GPU after about
1 s, so a hang shows up as a ~1 s submit plus new `Resetting GPU for hang` lines in `dmesg`.
`--bg N` starts N extra processes that keep drawing full-screen triangles (a stand-in for labwc, the VM
window blit, Chromium: real bin jobs from other processes were always in flight at the stalls).

## Build (already done once)

    make            # hang-repro + tri.vert.spv / tri.frag.spv (needs glslangValidator, libvulkan, vulkan.h)

## Run (you run it; I did not)

Close the VM first (the point is to run without it), then:

    cd ~/v3d-repro
    sudo dmesg | grep -c 'Resetting GPU'                  # note the count
    ./hang-repro --scenario small --iters 3000            # simplest case
    ./hang-repro --scenario yt --bg 1 --iters 4000
    ./hang-repro --scenario shadow --bg 1 --iters 4000
    ./hang-repro --scenario both --bg 2 --iters 6000
    sudo dmesg | grep -c 'Resetting GPU'                  # compare

Variants if nothing hangs:

    ./hang-repro --scenario yt --bg 2 --bg-sleep-us 500   # heavier concurrent load
    ./hang-repro --scenario yt --one-submit-per-pass      # every pass its own submit/job
    ./hang-repro --scenario yt --no-barriers
    ./hang-repro --shapes '128x16#5,120x8@128x16#6,120x8@128x16#5' --bg 1
    ./hang-repro --shapes '120x8@128x16!y,64x64!c'        # !y = copy into the image before the pass, !c = clear pass

Round 5: the `-s` jobs now match the guest's flags (draws=0 ser=7 bclsync=1 bclB=24) but still did not hang
(nbo=9 vs 13, rclB 99 vs 98). The guest's draws sample textures: DWM's halving sizes (2048x128 -> 1024x64 -> ...) are a
blur/downsample chain where every pass samples the previous pass's image through a descriptor set, with a vertex
buffer and a uniform buffer. `!t` / `-t` adds exactly that to the secondary command buffer (TMU reads of an image
written by the previous job, plus extra BOs):

    ./hang-repro --scenario small-t --iters 3000
    ./hang-repro --scenario yt-t --bg 1 --iters 4000
    ./hang-repro --scenario both-t --bg 2 --cpu 3 --procs 3 --iters 6000
    VK_DRIVER_FILES=<vmdir>/gpu/broadcom_icd.json BVM_V3DV_JOBLOG=1 ./hang-repro --scenario small-t --iters 20 2>&1 | grep BVMJOB | tail -8

Round 4 (the real path): DXVK on a tiling GPU (V3DV) records the contents of a render pass in a SECONDARY
command buffer and executes it inside the pass (`dxvk_context.cpp`: `useSecondaryCmdBuffer`,
`VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT`, `m_cmd->cmdExecuteCommands`). V3DV then makes a job whose own
BCL only has `BRANCH_TO_SUB_LIST` (so `draws=0` in our job log although there are draws; bclB=24, bclsync=1 from the
pending barrier, nbo=13 because the secondary's BOs are merged in) and the binner jumps into the secondary's BCL.
The `-b` variant below does not produce that (in-pass barrier jobs showed `draws=1 bclsync=0`). Use `-s` / `-e`:

    ./hang-repro --scenario small-s --iters 3000
    ./hang-repro --scenario yt-s --bg 1 --iters 4000
    ./hang-repro --scenario both-s --bg 2 --cpu 3 --procs 3 --iters 6000
    ./hang-repro --scenario both-e --bg 2 --cpu 3 --procs 3 --iters 6000       # empty secondaries
    VK_DRIVER_FILES=<vmdir>/gpu/broadcom_icd.json BVM_V3DV_JOBLOG=1 ./hang-repro --scenario small-s --iters 20 2>&1 | grep BVMJOB | tail -8
    # target: 120x8 lines with draws=0 ser=7 bclsync=1 bclB=24 nbo~13

Round 3: the jobs that hang in the guest are NOT plain draw-less passes. Their `ser=7 bclsync=1` flags mean
V3DV split a job at a pipeline barrier recorded INSIDE a render pass (`cmd_buffer_subpass_split_for_barrier`);
the second half is an empty LOAD/STORE job that carries the barrier. The `-b` scenarios (and the `!b` shape flag)
record exactly that: one tiny draw, a barrier inside the pass, end of pass.

    ./hang-repro --scenario small-b --iters 3000
    ./hang-repro --scenario yt-b --bg 1 --iters 4000
    ./hang-repro --scenario both-b --bg 2 --cpu 3 --procs 3 --iters 6000
    VK_DRIVER_FILES=<vmdir>/gpu/broadcom_icd.json BVM_V3DV_JOBLOG=1 ./hang-repro --scenario small-b --iters 20 2>&1 | grep BVMJOB | tail -8
    # the 120x8 lines should now read ser=7 bclsync=1 like the guest (rclB~98, bclB~24)

Closer to the VM's conditions (round 2, after round 1 with `--bg 1/2` found nothing):

    ./hang-repro --scenario both --bg 2 --cpu 3 --procs 3 --iters 6000   # 3 busy CPUs + 3 concurrent contexts + 2 drawing bg
    VK_DRIVER_FILES=<vmdir>/gpu/broadcom_icd.json ./hang-repro --scenario both --bg 2 --cpu 3 --procs 3 --iters 6000
    # compare the jobs with the guest's (ser/bclsync/nbo/flags in ~/bvm/scratchpad/run-*.log):
    VK_DRIVER_FILES=<vmdir>/gpu/broadcom_icd.json BVM_V3DV_JOBLOG=1 ./hang-repro --scenario small --iters 20 2>&1 | grep BVMJOB | tail -6

Shape syntax: `FWxFH[@IWxIH][#slot][!c][!y]` (frame size, optional image size, image identity, clear
instead of load, copy before). The two 120x8 passes of the real chain use different images (#6 then #5).

## Which Mesa

`hang-repro` uses whatever Vulkan ICD the loader finds, i.e. the system Mesa (stock 24.2.8 from
Raspberry Pi OS, the right one to report against). To try the patched 25.0.7 from the VM stack:

    VK_DRIVER_FILES=<vmdir>/gpu/broadcom_icd.json ./hang-repro ...

(`vulkaninfo --summary` with the same variable shows which driver is used).

## Reading the result

* Resets in `dmesg` plus `SLOW` lines: reproduced without the VM -> report with this folder, the command
  line and `hangwatch`/`gpu-trace-watch` logs (`~/bvm/dev/host/v3d-hangwatch.py`, `~/bvm/dev/host/gpu-trace-watch.sh`).
* No hang with any combination: the trigger needs something our guest path adds (DXVK/virgl specific
  memory layout, host mapping/dmabuf export, QEMU's blit) -> next step is shrinking that difference
  (e.g. dmabuf-exported images, `VK_EXT_external_memory_dma_buf`).
* Run `~/bvm/dev/host/v3d-hangwatch.py` in another terminal to see which engine/address is stuck.
