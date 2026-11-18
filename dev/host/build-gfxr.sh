#!/bin/bash
# Build and install a current GFXReconstruct (capture layer + replay tools) into ~/gfxr/gfxr-install.
# Why: the Debian package is 0.9.18 (Vulkan headers 1.3.239); with DXVK 2.7 / Mesa 25 it printed
# "pNext ... unrecognized VkStructureType ... omitted" and the render-server workers died with
# "malloc(): mismatching next->prev_size" (heap corruption) -> black guest screen (README item 117).
# The source is in ~/gfxr/gfxreconstruct-src (cloned 2026-11-04, submodules Vulkan-Headers + SPIRV-Headers).
# Takes a while on the Pi (-j3 keeps the desktop usable).  Run:  ~/gfxr/build-gfxr.sh
set -e
SRC=$HOME/gfxr/gfxreconstruct-src
PREFIX=$HOME/gfxr/gfxr-install
cd "$SRC"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DLZ4_OPTIONAL=ON \
  -DGFXRECON_ENABLE_OPENXR=OFF -DGFXRECON_TOCPP_SUPPORT=OFF -DBUILD_WERROR=OFF \
  -DBUILD_WSI_XCB_SUPPORT=OFF -DBUILD_WSI_XLIB_SUPPORT=OFF \
  -DCMAKE_INSTALL_PREFIX="$PREFIX"
ninja -C build -j${JOBS:-3} install
echo
echo "installed to $PREFIX"
find "$PREFIX" -name 'VkLayer_gfxreconstruct.json' -o -name 'gfxrecon-replay' -o -name 'libVkLayer_gfxreconstruct.so' | sort
