#ifndef SVGA_VLKN_WINSYS_H
#define SVGA_VLKN_WINSYS_H

/* Factory for the in-process winsys that connects Mesa's svga (vmwgfx)
 * Gallium driver directly to svga3_vlkn, with no QEMU and no kernel
 * vmwgfx device. Enabled at runtime via the SVGA_VLKN_WINSYS env var.
 *
 * Implemented in svga_vlkn_winsys.cpp. The returned screen implements
 * struct svga_winsys_screen from Mesa's svga_winsys.h.
 */
struct svga_winsys_screen;

#ifdef __cplusplus
extern "C" {
#endif

struct svga_winsys_screen *svga_vlkn_winsys_screen_create(void);

#ifdef __cplusplus
}
#endif

#endif /* SVGA_VLKN_WINSYS_H */
