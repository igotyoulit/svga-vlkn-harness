# svga-vlkn-harness

Run Mesa's SVGA Gallium driver against `vlkn4svga3d` **without QEMU** — no VM, no kernel `vmwgfx` device.

## Architecture

```
glmark2 / GLES2 app
    ↓ (EGL surfaceless, GLES2)
Mesa SVGA Gallium driver
    ↓ (svga_winsys_screen / svga_winsys_context)
svga_vlkn_winsys (this repo, winsys/)
    ↓ (SVGA3D FIFO commands, GMR buffers)
vlkn4svga3d (libqemu_svga3d)
    ↓ (Vulkan)
llvmpipe / lavapipe (software Vulkan)
```

The winsys implements a classic VGPU9 device in-process:
- `have_gb_objects=false`, `have_vgpu10=false`
- GMR-backed allocations
- Synchronous fences
- Hardware version `SVGA3D_HWVERSION_WS8_B1`

## Prerequisites

- Linux x86_64
- LLVM 20, Flex, Bison, libdrm-dev, m4
- `glslangValidator` (for Mesa build)
- A Mesa source checkout (tested at `52b557af72`)
- `vlkn4svga3d` built as a shared library

## Build

```bash
# 1. Apply Mesa patches
cd ~/workspace/mesa-src
for p in ~/workspace/svga-vlkn-harness/patches/*.patch; do
    patch -p1 < "$p"
done

# 2. Build Mesa with the winsys
~/workspace/svga-vlkn-harness/scripts/build-mesa.sh

# 3. Build the smoke test
PREFIX=~/workspace/svga-vlkn-harness/prefix
gcc -o $PREFIX/bin/gles2_smoke \
    ~/workspace/svga-vlkn-harness/tests/gles2_smoke.c \
    -I$PREFIX/include -L$PREFIX/lib/x86_64-linux-gnu \
    -lEGL -lGLESv2 -Wl,-rpath,$PREFIX/lib/x86_64-linux-gnu
```

## Run

```bash
PREFIX=~/workspace/svga-vlkn-harness/prefix
SVGA_VLKN_WINSYS=1 \
VK_ICD_FILENAMES=$PREFIX/share/vulkan/icd.d/lvp_icd.x86_64.json \
EGL_PLATFORM=surfaceless \
LD_LIBRARY_PATH=$PREFIX/lib/x86_64-linux-gnu \
$PREFIX/bin/gles2_smoke
```

Expected output:
```
GL version: OpenGL ES 2.0 Mesa 26.3.0-devel (git-...)
pixel(32,32) = 255 0 0 255
SMOKE TEST PASSED
```

## Environment variables

| Variable | Description |
|----------|-------------|
| `SVGA_VLKN_WINSYS=1` | Enable the vlkn winsys (required) |
| `SVGA_VLKN_DEBUG=1` | Verbose winsys logging |
| `SVGA_VLKN_DUMP_DIR` | Directory for PPM frame dumps |
| `SVGA_VLKN_DUMP_STRIDE` | Dump every Nth frame |

## Patches

| Patch | Description |
|-------|-------------|
| `01a-gallium-meson.patch` | Build system: include vlkn winsys |
| `01b-svga-meson.patch` | Build system: svga driver wiring |
| `02-drm-helper-hook.patch` | DRI helper hook for winsys |
| `03-surfaceless-fallback.patch` | Surfaceless platform fallback |
| `04-egl-dri2-null-checks.patch` | NULL guards in `dri2_setup_screen`, bounds check in `dri2_add_config` |
| `05-dri-context-null-dev.patch` | Handle NULL `screen->dev` in `dri_create_context` (no DRM device) |
| `06-dri2-vlkn-winsys.patch` | Direct `svga_vlkn_winsys_screen_create()` when `SVGA_VLKN_WINSYS=1` |
| `07-surfaceless-vlkn.patch` | Surfaceless platform: FD -1, `vmwgfx` driver name |
| `08-dri-screen-null-dev.patch` | NULL device handling in `dri_init_options` |
| `09-dri-util-null-dev.patch` | Default modes in `dri_fill_in_modes` when device is NULL |

## Status

- [x] Winsys: screen/context lifecycle, FIFO, GMR, surfaces, fences
- [x] Mesa builds (1260/1260 targets)
- [x] EGL surfaceless initializes
- [x] GLES2 context creation works
- [x] Red-pixel smoke test passes (full pipeline: SVGA → winsys → Vulkan → llvmpipe)
- [ ] glmark2: runs but canvas init needs a depth-capable config
- [ ] Frame dump validation

## License

MIT (winsys code). Mesa patches are derived from Mesa (MIT).
