# Production review of the GPU patches (2026-11-17)

State: everything below was measured on the working trees and `gpu-patch verify` (each patch applied to a pristine copy of its base equals the
working tree: mesa, dxvk, dxbc-spirv, virgl, virgl-protocol, qemu all clean). `bvm-virgl.patch` was re-exported after the NULL-guard fix (README 155).

| patch | added lines | files | env switches | verdict |
|---|---|---|---|---|
| `bvm-mesa.patch` | 263 | 6 | none | **ready** (3 `fprintf(stderr)` messages could become `mesa_log*`, cosmetic) |
| `bvm-dxbc-spirv.patch` | 12 | 2 | none | **ready** (`noBaseVertex` option) |
| `bvm-virgl-protocol.patch` | 20 | 1 generated header | `NPT_TRACE` x3 | diagnostics only: drop from production |
| `bvm-virgl.patch` | 290 | 21 | `NPT_TRACE`, `NPT_CAPS_SHIM`, `BVM_GFXR_DIR/LAYER_PATH/COMPRESSION/MEM/FLUSH` | **needs work**: split diagnostics off |
| `bvm-qemu.patch` | 670 | 18 | `BVM_GUEST_HZ`, `BVM_SDL_POLL_MS`, `BVM_GL_NOBLOCK`, `BVM_HDA_FLUSH`, `BVM_AUDIO_{STATS,RT,NICE,THREAD,PREFILL_MS}` | **needs work**: audio experiments out, SDL part cleaned |
| `bvm-dxvk.patch` | 2199 | 15 | `BVM_DXVK_{TESSEMU,NOMERGE,NOCOVERAGE,COPYFB,COPYSYNC,SAMPLER0,SAMPLER_ALL,SAMPLERLOG,PASSLOG}`, `BVM_TESSEMU_ICD` | **needs the most work**: features mixed with ~50 log sites |
| `bvm-mesa.diagnostics-2026-11-05.patch` | 828 | 11 | many | diagnostics, already separate, never merge |

Housekeeping: patch files are mode 600 (should be 644); `*.patch.prev` backups should be deleted; the three tree-level `gpu-patch` states are `applied`.

## QEMU (`checkpatch.pl` run on the diff: 57 errors, 29 warnings, ALL of them "line over 90 characters" / 80, nothing else)
Keep (production, upstream candidates): SDL2 dmabuf scanout (`ui/sdl2-gl.c`, `include/ui/sdl2.h`, `ui/sdl2.c`), SDL input poll timer (make 4 ms a
constant, drop `BVM_SDL_POLL_MS`), SDL text clipboard peer (`ui/sdl2-clipboard.c`, GPL-2 header already present), virtio-gpu changes in
`virtio-gpu-virgl.c`/`virtio-gpu-udmabuf.c`/`virtio-gpu.h` (review each hunk), guest refresh rate (`BVM_GUEST_HZ`: turn into a device property or keep
as the documented launcher default of 30).
Remove from production (experiments, audio crackle is still unresolved): the audio IOThread pump + statistics + jitter buffer (`audio/audio.c`,
`audio.h`, `audio_int.h`, `pwaudio.c`), `hw/audio/intel-hda.c`, `hw/usb/dev-audio.c`, `hw/usb/hcd-xhci.c`, `system/cpus.c`, `include/qemu/main-loop.h`
(the xHCI/main-loop/cpus hunks exist for the audio statistics), `BVM_GL_NOBLOCK` in `ui/gtk-gl-area.c` (GTK is no longer the display).
`sdl2-gl.c` calls `bvm_audio_stat()` in the blit path: that coupling must go when the audio code is dropped.
Fix: long lines (checkpatch), "BVMGPU" markers in comments.

## virglrenderer
Production: Neptune ring/queue/shared-surface/render-server changes, `vrend_renderer_reset` NULL guard, async-fence plumbing.
Diagnostics (split into their own patch): `NPT_TRACE` (npt_context.c + generated protocol headers), `BVM_GFXR_*` capture/replay injection in
`server/render_context.c` (this is our capture workflow: `bvm-gpu-capture`; keep it in the diagnostics patch, not in production).
To review: `NPT_CAPS_SHIM` in `npt_overrides_shared_handle.c` (121 lines): production behaviour or a switch?
Not ours: `NPT_BACKEND`, `NPT_*_LIBRARY_PATH`, `NPT_WA_FLAGS` already exist in the UTM code.

## DXVK
Features (keep, make unconditional, drop the env switch): tessellation emulation (`BVM_DXVK_TESSEMU`, default on), render-pass merge (`NOMERGE`,
default on), forced-coverage loop (`NOCOVERAGE`, default on), color-discard ignoring, dynamic sampler heap handling, feature demotion in
`dxvk_device_info.cpp`.
Diagnostics (marked `BVMDIAG` / `bvm*` helpers, ~50 log sites): `PASSLOG`, `COPYFB`, `COPYSYNC`, `SAMPLER0`, `SAMPLER_ALL`, `SAMPLERLOG`, memory/image
tracing in `dxvk_memory.cpp`/`dxvk_image.cpp`/`dxvk_graphics.cpp`. `BVM_TESSEMU_ICD` + `VK_ICD_FILENAMES`/`VK_DRIVER_FILES` handling: a second Vulkan
instance for the emulation (review: hard dependency on the environment).

## Plan (each stage: edit, `gpu-patch export`, `verify`, rebuild with `~/bvm/scratchpad/rebuild.sh <tree>`, one VM test)
1. Housekeeping (chmod 644, delete `.prev`): minutes.
2. Split each tree into a production patch and a `*.diagnostics.patch` (nothing is lost; the diagnostics patch is applied only for debugging builds):
   QEMU audio, virgl GFXR/trace, DXVK diag blocks. Tooling: `gpu-patch` gets a second set of component patches.
3. QEMU SDL part: constants instead of env, checkpatch clean, remove the audio coupling.
4. DXVK: unconditional features, remove the diagnostic blocks; the largest piece (`dxvk_context.cpp` +859 lines).
5. Upstream candidates (Mesa TMU ordering + supertile + SPIR-V guard; QEMU SDL dmabuf/input/clipboard): rebase on current upstream, cover letters.

## Decisions needed
a. Diagnostics: separate `*.diagnostics.patch` per tree (recommended, keeps the capture/replay workflow) or delete outright?
b. Audio experiments: drop from the production QEMU patch (recommended) or keep the jitter buffer?
c. Make tessellation emulation / pass merge / coverage loop unconditional (recommended) or keep the env switches as kill switches?
