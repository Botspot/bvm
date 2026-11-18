# GPU mode patches

Our changes to third-party source trees (Mesa V3DV, DXVK, dxbc-spirv, virglrenderer, QEMU) live here as patch files, one per tree, made
against a pinned base commit (the pins are the `gpu_*_commit` variables at the top of the GPU section in `bvm`). The sources are
downloaded stock into `$vmdir/gpu/src/` and patched by `bvm gpu-setup`.

| patch | tree (`$vmdir/gpu/src/...`) | base | contents |
|---|---|---|---|
| `bvm-mesa.patch` | `mesa` | `mesa-25.0.7` (`742a20f`) | V3DV: push constants 256 (DXVK needs it), UIF pitch check for imported tiled images, supertile-alignment fix (login-screen black band); dynamic sampler-heap lowering (DXVK) with TMU config-word ordering in the scheduler (fixes the V3D "Resetting GPU for hang" bursts); SPIR-V-to-NIR failure guard; dual-source blend: exact in-shader emulation of Skia's "coverage" blend (fixes the black box behind transparent canvas shadows in Edge), the old alpha fold stays for the other dual-source cases. |
| `bvm-dxvk.patch` | `dxvk` | `e3386161` | feature demotion (V3D lacks features DXVK marks as required), feature level 11_1 on V3D, tessellation emulation, pass merge, forced-coverage loop, color-discard ignoring, diagnostics |
| `bvm-dxbc-spirv.patch` | `dxvk/subprojects/dxbc-spirv` | `0e79a70` | `noBaseVertex` option |
| `bvm-virgl.patch` | `virglrenderer` | `cf5af71` | Neptune ring/queue/shared-surface/render-server changes, caps shim (`NPT_CAPS_SHIM`), host transport `context_id`, NULL guard in `vrend_renderer_reset` |
| `bvm-virgl-protocol.patch` | `virglrenderer/src/neptune/neptune-protocol` | the *generated* headers | `NPT_TRACE` reply logging. The headers are regenerated from the neptune-protocol repository at build time, this patch applies on top of them |
| `bvm-qemu.patch` | `qemu` | `ae92cdc` | virtio-gpu dmabuf/modifier handling, SDL GL scanout, vdagent and audio changes |

## Applying (everyone)

`bvm gpu-setup VMDIR` does everything: it downloads the pinned sources, applies these patches and compiles. Nothing here needs to be run by hand.

## Working on the patches (developers)

There is no mode for this. `gpu-setup` deletes the sources (`$vmdir/gpu/src`) when everything is built, and there is no way back from a patched tree to the base: sources are
always downloaded again when a pin or a patch changed. To get a tree to work on, load the functions of `bvm` into a shell and download and patch one component by hand:

```
IFS=$'\n'; source ~/bvm/bvm source; DIRECTORY=~/bvm; vmdir=~/win11
gpu_git_clone "$gpu_mesa_repo" "$gpu_mesa_commit" "$(gpu_patch_dir mesa)" dev
gpu_patch_apply mesa
```

Components: `mesa dxvk dxbc-spirv virgl virgl-protocol qemu` (`${gpu_components[@]}`; the repository and commit variables are `gpu_<name>_repo` and `gpu_<name>_commit`).

- `gpu_patch_apply mesa` applies the patch to a fresh tree (`gpu-setup` calls it). An interrupted run that already applied it is recognised by a marker in the tree's `.git` folder.
- `gpu_patch_generate mesa > ~/bvm/resources/gpu-patches/bvm-mesa.patch` writes the patch from the tree as it is now (everything that differs from the base commit). The previous version is in git.
- To verify a patch, apply it to a clean copy of the base and compare: `git -C "$(gpu_patch_dir mesa)" archive "$(gpu_patch_base mesa)" | tar -x -C DIR`,
  then `cd DIR && git apply ~/bvm/resources/gpu-patches/bvm-mesa.patch` and `cmp` the files it names with the ones in the source folder.

**New files** (not just changed ones) are only exported if they are listed in the `new_files` of `gpu_patch_generate` in `bvm`
(now `src/dxvk/dxvk_tessemu.*` for DXVK and `ui/sdl2-clipboard.c` for QEMU). A new file that is not listed is silently missing from the patch, and
the build of a fresh download fails (this happened with `sdl2-clipboard.c`). After exporting, look for untracked files with `git -C <tree> status --short`.

To change something: edit the files in the tree you set up above (it stays patched), build and test it by hand, then write the patch with `gpu_patch_generate`.
A normal `gpu-setup` run downloads fresh sources when a patch file or a pin changed, so exported changes reach everybody.

To move a pin to a newer commit: change the `gpu_*_commit` variable in `bvm`, download that tree as above (`gpu_git_clone` starts over because the commit changed), apply the old
patch with `git apply --3way` or by hand, fix what does not apply, then write the patch with `gpu_patch_generate`.
