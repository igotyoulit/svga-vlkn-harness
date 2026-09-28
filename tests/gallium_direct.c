// Direct Gallium test for svga-vlkn winsys, bypassing EGL.
// Creates winsys screen -> svga pipe screen -> renders a clear.
#include <stdio.h>
#include <stdlib.h>

#include "svga/svga_public.h"
#include "svga_vlkn_winsys.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "util/format/u_format.h"

int main(void) {
    struct svga_winsys_screen *sws = svga_vlkn_winsys_screen_create();
    if (!sws) {
        fprintf(stderr, "FAIL: svga_vlkn_winsys_screen_create\n");
        return 1;
    }
    printf("winsys screen created\n");

    struct pipe_screen *screen = svga_screen_create(sws);
    if (!screen) {
        fprintf(stderr, "FAIL: svga_screen_create\n");
        return 1;
    }
    printf("svga pipe screen created: %s\n", screen->get_name(screen));

    struct pipe_context *ctx = screen->context_create(screen, NULL, 0);
    if (!ctx) {
        fprintf(stderr, "FAIL: context_create\n");
        return 1;
    }
    printf("pipe context created\n");

    // Create a 64x64 RGBA8 texture as render target
    struct pipe_resource tmpl;
    memset(&tmpl, 0, sizeof(tmpl));
    tmpl.target = PIPE_TEXTURE_2D;
    tmpl.format = PIPE_FORMAT_R8G8B8A8_UNORM;
    tmpl.width0 = 64;
    tmpl.height0 = 64;
    tmpl.depth0 = 1;
    tmpl.array_size = 1;
    tmpl.bind = PIPE_BIND_RENDER_TARGET;
    struct pipe_resource *tex = screen->resource_create(screen, &tmpl);
    if (!tex) {
        fprintf(stderr, "FAIL: resource_create\n");
        return 1;
    }
    printf("render target created\n");

    struct pipe_surface surf_tmpl;
    memset(&surf_tmpl, 0, sizeof(surf_tmpl));
    surf_tmpl.format = PIPE_FORMAT_R8G8B8A8_UNORM;
    surf_tmpl.level = 0;
    surf_tmpl.first_layer = 0;
    surf_tmpl.last_layer = 0;
    struct pipe_surface *surf = screen->create_surface(ctx, tex, &surf_tmpl);
    if (!surf) {
        fprintf(stderr, "FAIL: create_surface\n");
        return 1;
    }

    struct pipe_framebuffer_state fb;
    memset(&fb, 0, sizeof(fb));
    fb.width = 64;
    fb.height = 64;
    fb.nr_cbufs = 1;
    fb.cbufs[0] = surf;
    ctx->set_framebuffer_state(ctx, &fb);

    union pipe_color_union clear_color;
    clear_color.f[0] = 1.0f;
    clear_color.f[1] = 0.0f;
    clear_color.f[2] = 0.0f;
    clear_color.f[3] = 1.0f;
    ctx->clear(ctx, PIPE_CLEAR_COLOR, &clear_color, 0, 0, 64, 64, false);
    ctx->flush(ctx, NULL, 0);
    printf("clear submitted\n");

    // Read back
    struct pipe_transfer *transfer;
    uint8_t *map = ctx->texture_map(ctx, tex, 0, PIPE_MAP_READ,
        &(struct pipe_box){ .x=32, .y=32, .width=1, .height=1, .depth=1 },
        &transfer);
    if (!map) {
        fprintf(stderr, "FAIL: texture_map\n");
        return 1;
    }
    printf("pixel(32,32) = %u %u %u %u\n", map[0], map[1], map[2], map[3]);
    int ok = (map[0] > 200 && map[1] < 50 && map[2] < 50);
    ctx->texture_unmap(ctx, transfer);

    printf(ok ? "DIRECT GALLIUM TEST PASSED\n" : "DIRECT GALLIUM TEST FAILED\n");

    ctx->destroy(ctx);
    screen->destroy(screen);
    return ok ? 0 : 1;
}
