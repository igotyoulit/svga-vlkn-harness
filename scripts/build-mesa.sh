#!/bin/bash
# Build Mesa with the in-process svga3_vlkn winsys for the svga Gallium driver,
# plus lavapipe (software Vulkan ICD) that svga3_vlkn needs underneath.
#
# Usage: ./build-mesa.sh [--reconfigure]
set -euo pipefail

HARNESS="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MESA_SRC="${MESA_SRC:-$HOME/workspace/mesa-src}"
VLKN_DIR="${VLKN_DIR:-$HOME/workspace/vlkn4svga3d}"
PREFIX="$HARNESS/prefix"
BUILD_DIR="$HARNESS/build/mesa"

echo "== harness : $HARNESS"
echo "== mesa    : $MESA_SRC"
echo "== vlkn    : $VLKN_DIR"
echo "== prefix  : $PREFIX"

[ -d "$MESA_SRC" ] || { echo "missing mesa source: $MESA_SRC"; exit 1; }
[ -f "$VLKN_DIR/lib/libsvga3_vlkn.a" ] || { echo "missing $VLKN_DIR/lib/libsvga3_vlkn.a"; exit 1; }

# 1. Install the winsys sources into the Mesa tree.
WINSYS_DST="$MESA_SRC/src/gallium/winsys/svga/vlkn"
mkdir -p "$WINSYS_DST"
cp "$HARNESS/winsys/svga_vlkn_winsys.cpp" "$HARNESS/winsys/svga_vlkn_winsys.h" \
   "$HARNESS/winsys/svga3_vlkn_api.h" "$HARNESS/winsys/meson.build" "$WINSYS_DST/"
echo "winsys sources installed"

# 2. Add the svga-vlkn-dir meson option (once).
if ! grep -q "svga-vlkn-dir" "$MESA_SRC/meson.options"; then
  cat "$HARNESS/patches/meson-option.snippet" >> "$MESA_SRC/meson.options"
  echo "meson option added"
else
  echo "meson option already present"
fi

# 3. Apply patches (idempotent).
cd "$MESA_SRC"
for p in 01a-gallium-meson 01b-svga-meson 02-drm-helper-hook 03-surfaceless-fallback; do
  f="$HARNESS/patches/$p.patch"
  if patch --dry-run -R -p1 < "$f" >/dev/null 2>&1; then
    echo "patch $p already applied"
  elif patch --dry-run -p1 < "$f" >/dev/null 2>&1; then
    patch -p1 < "$f" && echo "patch $p applied"
  else
    echo "patch $p does not apply cleanly"; exit 1
  fi
done
cd - >/dev/null

# 4. Configure.
RECONF=0
[ "${1:-}" = "--reconfigure" ] && RECONF=1
if [ ! -d "$BUILD_DIR" ]; then
  meson setup "$BUILD_DIR" "$MESA_SRC" \
    --prefix="$PREFIX" \
    --buildtype=release \
    -Dsvga-vlkn-dir="$VLKN_DIR" \
    -Dgallium-drivers=svga \
    -Dvulkan-drivers=swrast \
    -Dplatforms= \
    -Degl-native-platform=surfaceless \
    -Degl=enabled \
    -Dglx=disabled \
    -Dopengl=false \
    -Dgles1=disabled \
    -Dgles2=enabled \
    -Dgbm=disabled \
    -Dvulkan-layers=device-select,overlay \
    -Dllvm=enabled \
    -Dshared-llvm=enabled
elif [ "$RECONF" = 1 ]; then
  meson setup --reconfigure "$BUILD_DIR" \
    -Dsvga-vlkn-dir="$VLKN_DIR"
fi

# 5. Build and install.
ninja -C "$BUILD_DIR"
meson install -C "$BUILD_DIR"

echo
echo "== installed to $PREFIX"
ls "$PREFIX/lib/x86_64-linux-gnu/dri" 2>/dev/null | head
ls "$PREFIX/share/vulkan/icd.d" 2>/dev/null
