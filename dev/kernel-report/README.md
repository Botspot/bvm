# V3D GPU reset can deadlock in `drm_sched_stop()` (Raspberry Pi 5, rpi-6.12.y)

Notes collected 2026-11-05 for a possible upstream report (raspberrypi/linux issue and/or dri-devel, drm/v3d + drm/scheduler).
Nothing here has been reported yet. Evidence files are in this folder.

## Summary

After a V3D job timeout ("Resetting GPU for hang") the reset handler `v3d_gpu_reset_for_timeout()` can block forever in
`drm_sched_stop()` -> `dma_fence_wait_timeout()` (uninterruptible, 120 s hung-task warnings repeat every 120 s). The display
atomic commit worker waits on the same fence chain, so the whole desktop stops and the machine becomes unusable (SSH still
works for a while; the machine ignored SysRq reboot on one occasion and needed a power cycle, on others the magic keys worked).
It does not happen on every reset: ordinary hangs normally recover (hundreds of "Resetting GPU for hang" lines in a boot are fine);
the deadlock shows up in roughly 1 of 10-20 bursts of repeated hangs on this machine.

## Environment

* Raspberry Pi 5, Raspberry Pi OS Bookworm 64-bit, kernel `6.12.96+rpt-rpi-v8` (`#1 Debian 1:6.12.96-1+rpt1`), V3D 7.1.7, Wayland/labwc.
* Mesa 24.2.8 (system) and Mesa 25.0.7 V3DV (patched, see `../gpu-patches/`); hangs came from a Mesa bug of our own (see below),
  the kernel problem is independent of which userspace triggers the hang.
* The kernel already contains the V3D_SMS reset fix of the March 2025 series by Maira Canal ("drm/v3d: Use V3D_SMS registers for
  power on/off and reset on V3D 7.x"): `v3d_reset_sms()` is in `v3d_gem.c`.

## Symptom and traces

Typical kernel log (full example: `trace-example.txt`; complete kernel logs of three affected boots: `boot-5-kernel.log`,
`boot-4-kernel.log`, `boot-1-kernel.log`, i.e. `journalctl -b N -k`):

```
v3d 1002000000.v3d: [drm:v3d_reset [v3d]] *ERROR* Resetting GPU for hang.
v3d 1002000000.v3d: [drm:v3d_reset [v3d]] *ERROR* V3D_ERR_STAT: 0x00001000
   ... (a burst of these, one per second) ...
INFO: task kworker/2:1:181329 blocked for more than 120 seconds.
Workqueue: events drm_sched_job_timedout [gpu_sched]
Call trace:
 schedule+0x3c/0x108
 schedule_timeout+0x184/0x1a0
 dma_fence_default_wait+0x1cc/0x200
 dma_fence_wait_timeout+0x19c/0x1f0
 drm_sched_stop+0x144/0x220 [gpu_sched]
 v3d_gpu_reset_for_timeout+0x58/0xd0 [v3d]
 v3d_cl_job_timedout+0xdc/0xf8 [v3d]
 v3d_bin_job_timedout+0x24/0x38 [v3d]
 drm_sched_job_timedout+0x7c/0x120 [gpu_sched]
INFO: task kworker/u19:0:181935 blocked for more than 120 seconds.
Workqueue: events_unbound commit_work [drm_kms_helper]
Call trace:
 dma_fence_wait_timeout+0x19c/0x1f0
 drm_atomic_helper_wait_for_fences+0x180/0x248 [drm_kms_helper]
 commit_tail+0x40/0x1a0 [drm_kms_helper]
 commit_work+0x1c/0x30 [drm_kms_helper]
```

Seen in three boots (06:10, 15:06 and 16:41 on 2026-11-05; the boots before had no persistent journal evidence because the
machine was power-cycled before anything was flushed). The timeout handler that blocks was `v3d_bin_job_timedout` each time
(a bin job timed out while a render job was also stuck).

## Where it blocks (source, rpi-6.12.y)

`drivers/gpu/drm/v3d/v3d_sched.c`:

```c
static enum drm_gpu_sched_stat
v3d_gpu_reset_for_timeout(struct v3d_dev *v3d, struct drm_sched_job *sched_job)
{
	mutex_lock(&v3d->reset_lock);
	/* block scheduler */
	for (q = 0; q < V3D_MAX_QUEUES; q++)
		drm_sched_stop(&v3d->queue[q].sched, sched_job);
	...
```

`drivers/gpu/drm/scheduler/sched_main.c`, `drm_sched_stop()`: after `drm_sched_wqueue_stop(sched)` it walks `pending_list`;
for each job whose hardware fence callback cannot be removed (`s_job->s_fence->parent == NULL` or the callback already ran) it does

```c
	spin_lock(&sched->job_list_lock);
	list_del_init(&s_job->list);
	spin_unlock(&sched->job_list_lock);
	/* Wait for job's HW fence callback to finish using s_job before releasing it. */
	dma_fence_wait(&s_job->s_fence->finished, false);      <-- blocks here (uninterruptible, no timeout)
```

Hypothesis (not proven): the awaited `finished` fence belongs to a job that was already moved to `pending_list` but never
ran (no hw fence, `parent == NULL`) or whose completion work is queued on the very scheduler workqueue that
`drm_sched_wqueue_stop()` just stopped, so nothing can ever signal it; `reset_lock` is held, so the other V3D queues and the
display commit (waiting for the render job's out fence) block behind it. The same function is called for all five V3D queues in a
row, so a job pending on a later queue while an earlier one is already stopped is a plausible cycle. The bin and render queues are
separate schedulers that depend on each other (bin -> render fences), which makes this more likely than on single-queue drivers.

Possible directions (untested): wait with a timeout in the reset path and force-signal the fence with an error; do not wait for
`finished` when `parent == NULL` (signal it with `-ECANCELED` instead); stop all queues first and only then walk them; or check
whether `v3d_sched_skip_reset()` (which re-adds a job to `pending_list` by hand) can leave a job in a state that this loop then
waits on.

## How to reproduce (needs any V3D job hang)

The kernel bug needs a GPU hang to start. Our hang came from a Mesa bug in our own dynamic-sampler patch (TMU config words
written out of order); that bug is FIXED in the patch. To get the old behaviour back for kernel testing:

* Standalone Vulkan reproducers: `~/bvm/dev/v3d-repro/blur-repro` and `blur2-repro` (shaders in `~/bvm/dev/v3d-repro/shaders/`).
* Build Mesa from the diagnostics patch `~/bvm/dev/patches/bvm-mesa.diagnostics-2026-11-05.patch` (the production patch has
  the knobs stripped) and run `BVM_V3D_TMUCORDER=0 ./blur2-repro --iters 30` (hangs within a second, 1 reset per second until killed).
* Run it repeatedly (10 s each, `timeout 10`); each run that hangs has a small chance of ending in the deadlock above. Do it with
  nothing important open: the machine may need a power cycle.
* Not yet tried: a trigger that does not depend on our Mesa patch (for example a fragment shader with an infinite loop, or a CL
  with a bad branch) to show the deadlock on stock Mesa; this would make the report much stronger.

Also useful for a report: `echo 1 > /proc/sys/kernel/hung_task_panic` is NOT needed, the trace appears anyway after 120 s;
`echo l > /proc/sysrq-trigger` or `echo w > /proc/sysrq-trigger` while it is stuck would show all blocked tasks and the holder of
`reset_lock` (not captured so far, worth doing next time it happens if SSH still works).

## Related facts

* Every hang also logs, once per boot, `v3d 1002000000.v3d: MMU error from client PTB (1) at 0xf0000000, pte invalid` (the client
  table in `v3d_irq.c` is the V3D 4.1 one, the label may be wrong on 7.1). That address was caused by our Mesa bug and is not
  expected on a correct stream.
* Upstream series that touched the same code: "drm/v3d: Fix GPU reset issues on the Raspberry Pi 5" (Maira Canal, Feb-Mar 2025,
  v1-v6, adds `v3d_reset_sms`).
