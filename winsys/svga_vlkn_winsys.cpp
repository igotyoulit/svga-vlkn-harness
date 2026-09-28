/*
 * svga_vlkn_winsys.cpp
 *
 * In-process winsys connecting Mesa's svga (vmwgfx) Gallium driver directly
 * to the svga3_vlkn device library, with no QEMU and no kernel vmwgfx
 * device.
 *
 * How it works:
 *  - Mesa emits the classic VGPU9 SVGA3D command stream into a command
 *    buffer owned by this winsys.
 *  - At flush time relocations are patched (surface ids, guest pointers)
 *    and the raw byte stream is handed to svga3_vlkn_fifo_execute(), which
 *    consumes the exact framing Mesa produces: SVGA3dCmdHeader { u32 id,
 *    u32 size-in-bytes } followed by the payload.
 *  - Guest memory (winsys buffers used for DMA upload/download, queries,
 *    vertex/index data) is backed by page-aligned host allocations. Each
 *    buffer is registered with the device as synthetic guest RAM under a
 *    fake guest-physical address and exposed to the command stream through
 *    a GMR2 region, so SVGAGuestPtr relocations (gpa field = GMR id)
 *    resolve to the host pointer without any hypervisor.
 *  - Device-side surfaces are created here in surface_create (the classic
 *    driver never emits DEFINE_SURFACE on the FIFO; the DRM winsys creates
 *    them via ioctl, we create them via the library API).
 *  - The device executes synchronously (vkQueueWaitIdle per submit), so
 *    fences are trivially signaled and no async completion is needed.
 *
 * Environment:
 *  SVGA_VLKN_WINSYS=1   select this winsys in pipe_vmwgfx_create_screen
 *  SVGA_VLKN_DEBUG=1    verbose logging to stderr
 *  SVGA_VLKN_DUMP_DIR   write a PPM screenshot of the current render
 *                       target every SVGA_VLKN_DUMP_STRIDE flushes
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <vector>
#include <unordered_map>

/* Mesa's svga winsys interface.  Included normally (not in extern "C"):
 * the headers carry their own guards and pull in C++ templates. */
#include "svga_winsys.h"
#include "pipe/p_defines.h"

/* Minimal declarations for the svga3_vlkn public API.  The full
 * "svga3_vlkn.h" cannot be included here: it pulls in the library's own
 * copy of the SVGA register headers, which collide with Mesa's.  The
 * register types are ABI-compatible, so the shim below is sufficient. */
#include "svga3_vlkn_api.h"

/* ------------------------------------------------------------------ */
/* Internal objects                                                    */
/* ------------------------------------------------------------------ */

#define VLKN_CMD_BUF_SIZE (1024 * 1024)

struct vlkn_screen;
struct vlkn_surface;
struct vlkn_buffer;

struct vlkn_fence {
   uint64_t seq;
   int refcount;
};

struct vlkn_surface {
   uint32_t sid;
   SVGA3dSurfaceFormat format;
   uint32_t width, height, depth;
   uint32_t num_mip_levels;
   uint32_t num_faces;
   int refcount;
};

struct vlkn_buffer {
   void *ptr;            /* page-aligned host memory */
   size_t size;          /* page-rounded size */
   uint32_t gmr_id;      /* GMR2 id registered with the device */
   uint64_t fake_gpa;    /* synthetic guest-physical base address */
};

struct vlkn_surf_reloc {
   uint32_t *sid_ptr;
   vlkn_surface *surf;   /* NULL means patch SVGA3D_INVALID_ID */
};

struct vlkn_region_reloc {
   SVGAGuestPtr *where;
   vlkn_buffer *buf;
   uint32_t offset;
};

struct vlkn_context {
   struct svga_winsys_context base; /* must be first */
   vlkn_screen *vs;

   uint8_t *cmd;
   size_t cmd_used;
   size_t cmd_reserved;

   std::vector<vlkn_surf_reloc> surf_relocs;
   std::vector<vlkn_region_reloc> region_relocs;
};

struct vlkn_screen {
   struct svga_winsys_screen base; /* must be first */
   Svga3VlknDevice *dev;

   uint32_t next_sid;
   uint32_t next_cid;
   uint32_t next_gmr;
   uint64_t next_fake_gpa;
   uint64_t fence_seq;

   std::unordered_map<uint32_t, vlkn_surface *> live_surfaces;

   /* Frame-dump state */
   const char *dump_dir;
   unsigned dump_stride;
   uint64_t flush_no;
   uint32_t cur_rt_sid;
};

static inline vlkn_screen *to_screen(struct svga_winsys_screen *s)
{ return (vlkn_screen *)s; }
static inline vlkn_context *to_context(struct svga_winsys_context *c)
{ return (vlkn_context *)c; }
static inline vlkn_surface *to_surface(struct svga_winsys_surface *s)
{ return (vlkn_surface *)s; }
static inline vlkn_buffer *to_buffer(struct svga_winsys_buffer *b)
{ return (vlkn_buffer *)b; }
static inline vlkn_fence *to_fence(struct pipe_fence_handle *f)
{ return (vlkn_fence *)f; }

static void vlkn_log(const char *fmt, ...)
{
   if (!getenv("SVGA_VLKN_DEBUG"))
      return;
   va_list ap;
   va_start(ap, fmt);
   fprintf(stderr, "[svga-vlkn] ");
   vfprintf(stderr, fmt, ap);
   va_end(ap);
}

/* ------------------------------------------------------------------ */
/* Frame dumps                                                         */
/* ------------------------------------------------------------------ */

/* Track the most recent SETRENDERTARGET type 0 so flushes can dump the
 * current framebuffer.  Framing is SVGA3dCmdHeader { u32 id, u32 size },
 * where size is the payload size in bytes (as Mesa's emit helpers set it
 * and as svga3_vlkn_fifo_execute parses it). */
static void vlkn_track_render_target(vlkn_screen *vs, vlkn_context *vc)
{
   const uint8_t *p = vc->cmd;
   const uint8_t *end = vc->cmd + vc->cmd_used;
   while (p + 8 <= end) {
      uint32_t id, size_bytes;
      memcpy(&id, p, 4);
      memcpy(&size_bytes, p + 4, 4);
      const uint8_t *payload = p + 8;
      if (payload + size_bytes > end)
         break;
      if (id == (uint32_t)SVGA_3D_CMD_SETRENDERTARGET &&
          size_bytes >= sizeof(SVGA3dCmdSetRenderTarget)) {
         const SVGA3dCmdSetRenderTarget *rt =
            (const SVGA3dCmdSetRenderTarget *)payload;
         if (rt->type == SVGA3D_RT_COLOR0)
            vs->cur_rt_sid = rt->target.sid;
      }
      p = payload + size_bytes;
   }
}

static void vlkn_write_ppm(const char *path, const uint8_t *bgra,
                           uint32_t w, uint32_t h, bool swap_rb)
{
   FILE *f = fopen(path, "wb");
   if (!f)
      return;
   fprintf(f, "P6\n%u %u\n255\n", w, h);
   for (uint32_t y = 0; y < h; y++) {
      for (uint32_t x = 0; x < w; x++) {
         const uint8_t *px = bgra + (y * w + x) * 4;
         uint8_t rgb[3] = { px[swap_rb ? 2 : 0], px[1], px[swap_rb ? 0 : 2] };
         fwrite(rgb, 1, 3, f);
      }
   }
   fclose(f);
}

static void vlkn_maybe_dump_frame(vlkn_screen *vs)
{
   if (!vs->dump_dir)
      return;
   vs->flush_no++;
   if (vs->dump_stride == 0 || (vs->flush_no % vs->dump_stride) != 0)
      return;
   if (vs->cur_rt_sid == 0 || vs->cur_rt_sid == SVGA3D_INVALID_ID)
      return;

   auto it = vs->live_surfaces.find(vs->cur_rt_sid);
   if (it == vs->live_surfaces.end())
      return;
   vlkn_surface *surf = it->second;
   if (surf->width == 0 || surf->height == 0)
      return;

   /* Only 32-bit packed formats are convertible; anything else is skipped. */
   bool swap_rb;
   switch (surf->format) {
   case SVGA3D_X8R8G8B8:
   case SVGA3D_A8R8G8B8:
      swap_rb = true;
      break;
   case SVGA3D_R8G8B8A8_UNORM:
      swap_rb = false;
      break;
   default:
      return;
   }

   size_t stride = (size_t)surf->width * 4;
   std::vector<uint8_t> pixels(stride * surf->height);
   SVGA3dBox box;
   box.x = 0; box.y = 0; box.z = 0;
   box.w = surf->width; box.h = surf->height; box.d = 1;
   if (svga3_vlkn_surface_dma_download(vs->dev, surf->sid, 0, &box,
                                       pixels.data(), stride) !=
       SVGA3_VLKN_SUCCESS)
      return;

   char path[1024];
   snprintf(path, sizeof(path), "%s/frame-%06llu.ppm",
            vs->dump_dir, (unsigned long long)vs->flush_no);
   vlkn_write_ppm(path, pixels.data(), surf->width, surf->height, swap_rb);
   vlkn_log("dumped frame %llu to %s\n", (unsigned long long)vs->flush_no, path);
}

/* ------------------------------------------------------------------ */
/* Context                                                             */
/* ------------------------------------------------------------------ */

static void vlkn_context_destroy(struct svga_winsys_context *swc)
{
   vlkn_context *vc = to_context(swc);
   svga3_vlkn_context_destroy(vc->vs->dev, vc->base.cid);
   free(vc->cmd);
   delete vc;
}

static void *vlkn_reserve(struct svga_winsys_context *swc,
                          uint32_t nr_bytes, uint32_t nr_relocs)
{
   vlkn_context *vc = to_context(swc);
   (void)nr_relocs; /* relocations are stored in growable vectors */

   if (nr_bytes > VLKN_CMD_BUF_SIZE)
      return NULL;
   if (vc->cmd_used + nr_bytes > VLKN_CMD_BUF_SIZE)
      return NULL; /* driver flushes and retries */

   vc->cmd_reserved = nr_bytes;
   return vc->cmd + vc->cmd_used;
}

static unsigned vlkn_get_command_buffer_size(struct svga_winsys_context *swc)
{
   return (unsigned)to_context(swc)->cmd_used;
}

static void vlkn_surface_relocation(struct svga_winsys_context *swc,
                                    uint32 *sid, uint32 *mobid,
                                    struct svga_winsys_surface *surface,
                                    unsigned flags)
{
   vlkn_context *vc = to_context(swc);
   (void)flags;
   /* Non-GB path never carries a mobid. */
   (void)mobid;
   vlkn_surf_reloc r;
   r.sid_ptr = sid;
   r.surf = surface ? to_surface(surface) : NULL;
   vc->surf_relocs.push_back(r);
}

static void vlkn_region_relocation(struct svga_winsys_context *swc,
                                   struct SVGAGuestPtr *ptr,
                                   struct svga_winsys_buffer *buffer,
                                   uint32 offset, unsigned flags)
{
   vlkn_context *vc = to_context(swc);
   (void)flags;
   vlkn_region_reloc r;
   r.where = ptr;
   r.buf = to_buffer(buffer);
   r.offset = offset;
   vc->region_relocs.push_back(r);
}

static void vlkn_mob_relocation(struct svga_winsys_context *swc,
                                SVGAMobId *id, uint32 *offset_into_mob,
                                struct svga_winsys_buffer *buffer,
                                uint32 offset, unsigned flags)
{
   /* Classic path treats the buffer's GMR as the MOB. */
   vlkn_context *vc = to_context(swc);
   (void)flags;
   vlkn_buffer *buf = to_buffer(buffer);
   if (id)
      *id = buf->gmr_id;
   if (offset_into_mob)
      *offset_into_mob = offset;
   (void)vc;
}

static void vlkn_shader_relocation(struct svga_winsys_context *swc,
                                   uint32 *shid, uint32 *mobid, uint32 *offset,
                                   struct svga_winsys_gb_shader *shader,
                                   unsigned flags)
{
   (void)swc; (void)shid; (void)mobid; (void)offset; (void)shader;
   (void)flags;
   /* GB-only; unreachable on this path. */
}

static void vlkn_context_relocation(struct svga_winsys_context *swc,
                                    uint32 *cid)
{
   *cid = to_context(swc)->base.cid;
}

static void vlkn_query_relocation(struct svga_winsys_context *swc,
                                  SVGAMobId *id,
                                  struct svga_winsys_gb_query *query)
{
   (void)swc; (void)id; (void)query;
   /* GB-only; unreachable on this path. */
}

static enum pipe_error vlkn_query_bind(struct svga_winsys_context *swc,
                                       struct svga_winsys_gb_query *query,
                                       unsigned flags)
{
   (void)swc; (void)query; (void)flags;
   return PIPE_OK;
}

static void vlkn_commit(struct svga_winsys_context *swc)
{
   vlkn_context *vc = to_context(swc);
   vc->cmd_used += vc->cmd_reserved;
   vc->cmd_reserved = 0;
}

static enum pipe_error vlkn_flush(struct svga_winsys_context *swc,
                                  struct pipe_fence_handle **pfence)
{
   vlkn_context *vc = to_context(swc);
   vlkn_screen *vs = vc->vs;

   /* Apply relocations.  Both Mesa and the library name the first u32 of
    * SVGAGuestPtr "gmrId"; the winsys puts the buffer's GMR id there. */
   for (size_t i = 0; i < vc->surf_relocs.size(); i++) {
      vlkn_surf_reloc &r = vc->surf_relocs[i];
      *r.sid_ptr = r.surf ? r.surf->sid : SVGA3D_INVALID_ID;
   }
   for (size_t i = 0; i < vc->region_relocs.size(); i++) {
      vlkn_region_reloc &r = vc->region_relocs[i];
      r.where->gmrId = r.buf->gmr_id;
      r.where->offset = r.offset;
   }

   if (vs->dump_dir)
      vlkn_track_render_target(vs, vc);

   Svga3VlknStatus st = SVGA3_VLKN_SUCCESS;
   if (vc->cmd_used) {
      size_t consumed = 0;
      st = svga3_vlkn_fifo_execute(vs->dev, vc->cmd, vc->cmd_used, &consumed);
      if (st != SVGA3_VLKN_SUCCESS)
         vlkn_log("fifo_execute failed: %d (used=%zu consumed=%zu)\n",
                  (int)st, vc->cmd_used, consumed);
   }

   vc->cmd_used = 0;
   vc->cmd_reserved = 0;
   vc->surf_relocs.clear();
   vc->region_relocs.clear();

   vlkn_maybe_dump_frame(vs);

   /* Execution is synchronous, so the fence is already signaled. */
   vlkn_fence *fence = new vlkn_fence;
   fence->seq = ++vs->fence_seq;
   fence->refcount = 1;
   if (pfence) {
      if (*pfence) {
         vlkn_fence *old = to_fence(*pfence);
         if (--old->refcount == 0)
            delete old;
      }
      *pfence = (struct pipe_fence_handle *)fence;
   } else {
      delete fence;
   }

   return st == SVGA3_VLKN_SUCCESS ? PIPE_OK : PIPE_ERROR;
}

static void *vlkn_surface_map(struct svga_winsys_context *swc,
                              struct svga_winsys_surface *surface,
                              unsigned flags, bool *retry, bool *rebind)
{
   (void)swc; (void)surface; (void)flags;
   if (retry)
      *retry = false;
   if (rebind)
      *rebind = false;
   return NULL; /* GB-only; unreachable on this path. */
}

static void vlkn_surface_unmap(struct svga_winsys_context *swc,
                               struct svga_winsys_surface *surface,
                               bool *rebind)
{
   (void)swc; (void)surface;
   if (rebind)
      *rebind = false;
}

static struct svga_winsys_gb_shader *
vlkn_context_shader_create(struct svga_winsys_context *swc, uint32 shaderId,
                           SVGA3dShaderType shaderType, const uint32 *bytecode,
                           uint32 bytecodeLen,
                           const SVGA3dDXShaderSignatureHeader *sgnInfo,
                           uint32 sgnLen)
{
   (void)swc; (void)shaderId; (void)shaderType; (void)bytecode;
   (void)bytecodeLen; (void)sgnInfo; (void)sgnLen;
   return NULL; /* GB-only; unreachable on this path. */
}

static void
vlkn_context_shader_destroy(struct svga_winsys_context *swc,
                            struct svga_winsys_gb_shader *shader)
{
   (void)swc; (void)shader;
}

static enum pipe_error
vlkn_resource_rebind(struct svga_winsys_context *swc,
                     struct svga_winsys_surface *surface,
                     struct svga_winsys_gb_shader *shader, unsigned flags)
{
   (void)swc; (void)surface; (void)shader; (void)flags;
   return PIPE_OK;
}

/* ------------------------------------------------------------------ */
/* Screen                                                              */
/* ------------------------------------------------------------------ */

static void vlkn_screen_destroy(struct svga_winsys_screen *sws)
{
   vlkn_screen *vs = to_screen(sws);
   svga3_vlkn_device_destroy(vs->dev);
   delete vs;
}

static SVGA3dHardwareVersion vlkn_get_hw_version(struct svga_winsys_screen *sws)
{
   (void)sws;
   return SVGA3D_HWVERSION_WS8_B1;
}

static int vlkn_get_fd(struct svga_winsys_screen *sws)
{
   (void)sws;
   return -1;
}

static bool vlkn_get_cap(struct svga_winsys_screen *sws,
                         SVGA3dDevCapIndex index, SVGA3dDevCapResult *result)
{
   vlkn_screen *vs = to_screen(sws);
   uint32_t value = 0;
   if (!svga3_vlkn_query_cap(vs->dev, (uint32_t)index, &value)) {
      return false;
   }
   result->u = value;
   return true;
}

static struct svga_winsys_context *
vlkn_context_create(struct svga_winsys_screen *sws)
{
   vlkn_screen *vs = to_screen(sws);
   vlkn_context *vc = new vlkn_context();

   vc->vs = vs;
   vc->cmd = (uint8_t *)malloc(VLKN_CMD_BUF_SIZE);
   if (!vc->cmd) {
      delete vc;
      return NULL;
   }

   vc->base.destroy = vlkn_context_destroy;
   vc->base.reserve = vlkn_reserve;
   vc->base.get_command_buffer_size = vlkn_get_command_buffer_size;
   vc->base.surface_relocation = vlkn_surface_relocation;
   vc->base.region_relocation = vlkn_region_relocation;
   vc->base.shader_relocation = vlkn_shader_relocation;
   vc->base.context_relocation = vlkn_context_relocation;
   vc->base.mob_relocation = vlkn_mob_relocation;
   vc->base.query_relocation = vlkn_query_relocation;
   vc->base.query_bind = vlkn_query_bind;
   vc->base.commit = vlkn_commit;
   vc->base.flush = vlkn_flush;
   vc->base.cid = vs->next_cid++;
   vc->base.hints = 0;
   vc->base.imported_fence_fd = -1;
   vc->base.have_gb_objects = false;
   vc->base.force_coherent = false;
   vc->base.surface_map = vlkn_surface_map;
   vc->base.surface_unmap = vlkn_surface_unmap;
   vc->base.shader_create = vlkn_context_shader_create;
   vc->base.shader_destroy = vlkn_context_shader_destroy;
   vc->base.resource_rebind = vlkn_resource_rebind;
   vc->base.debug_callback = NULL;
   vc->base.in_retry = 0;

   if (svga3_vlkn_context_create(vs->dev, vc->base.cid) != SVGA3_VLKN_SUCCESS) {
      free(vc->cmd);
      delete vc;
      return NULL;
   }
   return &vc->base;
}

/* Surface sizes for every face and mip level, matching the layout the
 * surface_define API expects (faces * mipLevels entries). */
static std::vector<SVGA3dSize>
vlkn_mip_sizes(SVGA3dSize base, uint32_t num_faces, uint32_t num_mips)
{
   std::vector<SVGA3dSize> sizes;
   sizes.reserve((size_t)num_faces * num_mips);
   for (uint32_t f = 0; f < num_faces; f++) {
      for (uint32_t l = 0; l < num_mips; l++) {
         SVGA3dSize s;
         s.width = (base.width >> l) ? (base.width >> l) : 1;
         s.height = (base.height >> l) ? (base.height >> l) : 1;
         s.depth = (base.depth >> l) ? (base.depth >> l) : 1;
         sizes.push_back(s);
      }
   }
   return sizes;
}

static struct svga_winsys_surface *
vlkn_surface_create(struct svga_winsys_screen *sws,
                    SVGA3dSurfaceAllFlags flags, SVGA3dSurfaceFormat format,
                    unsigned usage, SVGA3dSize size, uint32 numLayers,
                    uint32 numMipLevels, unsigned sampleCount)
{
   vlkn_screen *vs = to_screen(sws);
   (void)usage;

   if (numMipLevels == 0)
      numMipLevels = 1;
   if (numLayers == 0)
      numLayers = 1;
   if (sampleCount > 1) {
      vlkn_log("multisample surfaces not supported (count=%u)\n", sampleCount);
      return NULL;
   }
   if (numLayers > 1) {
      /* Texture arrays are not representable as faces on VGPU9; fail
       * closed rather than mistranslating the layout. */
      vlkn_log("array surfaces not supported (layers=%u)\n", numLayers);
      return NULL;
   }

   uint32_t num_faces = (flags & SVGA3D_SURFACE_CUBEMAP) ? 6 : 1;

   vlkn_surface *surf = new vlkn_surface();
   surf->sid = vs->next_sid++;
   surf->format = format;
   surf->width = size.width;
   surf->height = size.height;
   surf->depth = size.depth ? size.depth : 1;
   surf->num_mip_levels = numMipLevels;
   surf->num_faces = num_faces;
   surf->refcount = 1;

   /* The classic driver never emits DEFINE_SURFACE on the FIFO; the
    * device-side surface is created here, like the DRM winsys does
    * via ioctl. */
   std::vector<SVGA3dSize> sizes =
      vlkn_mip_sizes(size, num_faces, numMipLevels);
   Svga3VlknStatus st = svga3_vlkn_surface_define(
      vs->dev, surf->sid, (uint32_t)flags, (int)format,
      sizes.data(), (uint32_t)sizes.size());
   if (st != SVGA3_VLKN_SUCCESS) {
      vlkn_log("surface_define failed for sid=%u fmt=%u (%ux%ux%u): %d\n",
               surf->sid, (unsigned)format,
               size.width, size.height, size.depth, (int)st);
      delete surf;
      return NULL;
   }

   vs->live_surfaces[surf->sid] = surf;
   return (struct svga_winsys_surface *)surf;
}

static struct svga_winsys_surface *
vlkn_surface_from_handle(struct svga_winsys_screen *sws,
                         struct winsys_handle *whandle,
                         SVGA3dSurfaceFormat *format)
{
   (void)sws; (void)whandle; (void)format;
   return NULL; /* cross-process sharing is not supported */
}

static bool vlkn_surface_get_handle(struct svga_winsys_screen *sws,
                                    struct svga_winsys_surface *surface,
                                    unsigned stride,
                                    struct winsys_handle *whandle)
{
   (void)sws; (void)surface; (void)stride; (void)whandle;
   return false;
}

static bool vlkn_surface_is_flushed(struct svga_winsys_screen *sws,
                                    struct svga_winsys_surface *surface)
{
   (void)sws; (void)surface;
   return true; /* execution is synchronous; nothing is ever pending */
}

static void vlkn_surface_reference(struct svga_winsys_screen *sws,
                                    struct svga_winsys_surface **pdst,
                                    struct svga_winsys_surface *src)
{
   vlkn_screen *vs = to_screen(sws);
   vlkn_surface *dst = src ? to_surface(src) : NULL;

   if (*pdst) {
      vlkn_surface *old = to_surface(*pdst);
      if (--old->refcount == 0) {
         vs->live_surfaces.erase(old->sid);
         svga3_vlkn_surface_destroy(vs->dev, old->sid);
         delete old;
      }
      *pdst = NULL;
   }
   if (dst) {
      dst->refcount++;
      *pdst = src;
   }
}

static bool vlkn_surface_can_create(struct svga_winsys_screen *sws,
                                    SVGA3dSurfaceFormat format,
                                    SVGA3dSize size, uint32 numLayers,
                                    uint32 numMipLevels, uint32 numSamples)
{
   (void)sws; (void)format; (void)numLayers; (void)numMipLevels;
   if (numSamples > 1)
      return false;
   return size.width <= 16384 && size.height <= 16384 && size.depth <= 2048;
}

static void vlkn_surface_init(struct svga_winsys_screen *sws,
                              struct svga_winsys_surface *surface,
                              unsigned surf_size, SVGA3dSurfaceAllFlags flags)
{
   (void)sws; (void)surf_size; (void)flags;
   to_surface(surface)->refcount = 1;
}

static struct svga_winsys_buffer *
vlkn_buffer_create(struct svga_winsys_screen *sws, unsigned alignment,
                   unsigned usage, unsigned size)
{
   vlkn_screen *vs = to_screen(sws);
   (void)usage;

   if (alignment < 4096)
      alignment = 4096;
   size_t rounded = (size + 4095) & ~(size_t)4095;
   if (rounded == 0)
      rounded = 4096;

   void *ptr = NULL;
   if (posix_memalign(&ptr, alignment, rounded) != 0)
      return NULL;
   memset(ptr, 0, rounded);

   /* Synthetic guest-physical address, clear of low memory.  The range is
    * contiguous so a single PPN remap covers the whole GMR. */
   uint64_t gpa = vs->next_fake_gpa;
   vs->next_fake_gpa += rounded;
   uint32_t gmr_id = vs->next_gmr++;
   uint32_t num_pages = (uint32_t)(rounded / 4096);

   Svga3VlknDevice *dev = vs->dev;
   if (svga3_vlkn_device_map_guest_ram(dev, gpa, ptr, rounded) !=
       SVGA3_VLKN_SUCCESS) {
      free(ptr);
      return NULL;
   }
   if (svga3_vlkn_gmr_define(dev, gmr_id, num_pages) != SVGA3_VLKN_SUCCESS) {
      svga3_vlkn_device_unmap_guest_ram(dev, gpa);
      free(ptr);
      return NULL;
   }
   uint32_t ppn = (uint32_t)(gpa >> 12);
   if (svga3_vlkn_gmr_remap(dev, gmr_id, SVGA_REMAP_GMR2_SINGLE_PPN,
                            0, num_pages, &ppn, sizeof(ppn)) !=
       SVGA3_VLKN_SUCCESS) {
      svga3_vlkn_gmr_destroy(dev, gmr_id);
      svga3_vlkn_device_unmap_guest_ram(dev, gpa);
      free(ptr);
      return NULL;
   }

   vlkn_buffer *buf = new vlkn_buffer();
   buf->ptr = ptr;
   buf->size = rounded;
   buf->gmr_id = gmr_id;
   buf->fake_gpa = gpa;
   return (struct svga_winsys_buffer *)buf;
}

static void *vlkn_buffer_map(struct svga_winsys_screen *sws,
                             struct svga_winsys_buffer *buf, unsigned usage)
{
   (void)sws; (void)usage;
   return to_buffer(buf)->ptr;
}

static void vlkn_buffer_unmap(struct svga_winsys_screen *sws,
                              struct svga_winsys_buffer *buf)
{
   (void)sws; (void)buf;
}

static void vlkn_buffer_destroy(struct svga_winsys_screen *sws,
                                struct svga_winsys_buffer *buf)
{
   vlkn_screen *vs = to_screen(sws);
   vlkn_buffer *b = to_buffer(buf);
   Svga3VlknDevice *dev = vs->dev;
   svga3_vlkn_gmr_destroy(dev, b->gmr_id);
   svga3_vlkn_device_unmap_guest_ram(dev, b->fake_gpa);
   free(b->ptr);
   delete b;
}

static void vlkn_fence_reference(struct svga_winsys_screen *sws,
                                 struct pipe_fence_handle **pdst,
                                 struct pipe_fence_handle *src)
{
   (void)sws;
   vlkn_fence *dst = src ? to_fence(src) : NULL;
   if (*pdst) {
      vlkn_fence *old = to_fence(*pdst);
      if (--old->refcount == 0)
         delete old;
      *pdst = NULL;
   }
   if (dst) {
      dst->refcount++;
      *pdst = src;
   }
}

static int vlkn_fence_signalled(struct svga_winsys_screen *sws,
                                struct pipe_fence_handle *fence,
                                unsigned flag)
{
   (void)sws; (void)flag;
   return fence ? 0 : -1; /* synchronous execution: always signaled */
}

static int vlkn_fence_finish(struct svga_winsys_screen *sws,
                             struct pipe_fence_handle *fence,
                             uint64_t timeout, unsigned flag)
{
   (void)timeout; (void)flag;
   if (fence)
      svga3_vlkn_device_wait_idle(to_screen(sws)->dev);
   return 0;
}

static int vlkn_fence_get_fd(struct svga_winsys_screen *sws,
                             struct pipe_fence_handle *fence, bool duplicate)
{
   (void)sws; (void)fence; (void)duplicate;
   return -1;
}

static void vlkn_fence_create_fd(struct svga_winsys_screen *sws,
                                 struct pipe_fence_handle **fence, int32_t fd)
{
   (void)sws; (void)fd;
   *fence = NULL;
}

static int vlkn_fence_server_sync(struct svga_winsys_screen *sws,
                                  int32_t *context_fd,
                                  struct pipe_fence_handle *fence)
{
   (void)sws; (void)context_fd; (void)fence;
   return 0;
}

static struct svga_winsys_gb_shader *
vlkn_screen_shader_create(struct svga_winsys_screen *sws,
                          SVGA3dShaderType shaderType, const uint32 *bytecode,
                          uint32 bytecodeLen)
{
   (void)sws; (void)shaderType; (void)bytecode; (void)bytecodeLen;
   return NULL; /* GB-only */
}

static void vlkn_screen_shader_destroy(struct svga_winsys_screen *sws,
                                       struct svga_winsys_gb_shader *shader)
{
   (void)sws; (void)shader;
}

static struct svga_winsys_gb_query *
vlkn_query_create(struct svga_winsys_screen *sws, uint32 len)
{
   (void)sws; (void)len;
   return NULL; /* GB-only */
}

static void vlkn_query_destroy(struct svga_winsys_screen *sws,
                               struct svga_winsys_gb_query *query)
{
   (void)sws; (void)query;
}

static int vlkn_query_init(struct svga_winsys_screen *sws,
                           struct svga_winsys_gb_query *query, unsigned offset,
                           SVGA3dQueryState queryState)
{
   (void)sws; (void)query; (void)offset; (void)queryState;
   return 0;
}

static void vlkn_query_get_result(struct svga_winsys_screen *sws,
                                  struct svga_winsys_gb_query *query,
                                  unsigned offset,
                                  SVGA3dQueryState *queryState,
                                  void *result, uint32 resultLen)
{
   (void)sws; (void)query; (void)offset; (void)result; (void)resultLen;
   if (queryState)
      *queryState = SVGA3D_QUERYSTATE_NEW;
}

static void vlkn_stats_inc(struct svga_winsys_screen *sws,
                           enum svga_stats_count count)
{
   (void)sws; (void)count;
}

static void vlkn_stats_time_push(struct svga_winsys_screen *sws,
                                 enum svga_stats_time time,
                                 struct svga_winsys_stats_timeframe *tf)
{
   (void)sws; (void)time; (void)tf;
}

static void vlkn_stats_time_pop(struct svga_winsys_screen *sws)
{
   (void)sws;
}

static void vlkn_host_log(struct svga_winsys_screen *sws, const char *message)
{
   (void)sws;
   fprintf(stderr, "[svga-vlkn] host_log: %s\n", message);
}

/* ------------------------------------------------------------------ */
/* Factory                                                             */
/* ------------------------------------------------------------------ */

extern "C" struct svga_winsys_screen *svga_vlkn_winsys_screen_create(void)
{
   Svga3VlknConfig config;
   memset(&config, 0, sizeof(config));
   config.appName = "svga-vlkn-harness";
   config.apiVersion = 0; /* let the library pick its default */
   config.forceMockBackend = false;

   Svga3VlknDevice *dev = svga3_vlkn_device_create(&config);
   if (!dev) {
      fprintf(stderr, "[svga-vlkn] svga3_vlkn_device_create failed\n");
      return NULL;
   }

   vlkn_screen *vs = new vlkn_screen();
   vs->dev = dev;
   vs->next_sid = 1;
   vs->next_cid = 0;
   vs->next_gmr = 1;
   vs->next_fake_gpa = 0x100000000ULL; /* 4 GiB, clear of low memory */
   vs->fence_seq = 0;

   const char *dump_dir = getenv("SVGA_VLKN_DUMP_DIR");
   if (dump_dir && dump_dir[0]) {
      vs->dump_dir = dump_dir;
      const char *stride = getenv("SVGA_VLKN_DUMP_STRIDE");
      vs->dump_stride = stride ? (unsigned)atoi(stride) : 60;
      if (vs->dump_stride == 0)
         vs->dump_stride = 60;
   }

   vs->base.destroy = vlkn_screen_destroy;
   vs->base.get_hw_version = vlkn_get_hw_version;
   vs->base.get_fd = vlkn_get_fd;
   vs->base.get_cap = vlkn_get_cap;
   vs->base.context_create = vlkn_context_create;
   vs->base.surface_create = vlkn_surface_create;
   vs->base.surface_from_handle = vlkn_surface_from_handle;
   vs->base.surface_get_handle = vlkn_surface_get_handle;
   vs->base.surface_is_flushed = vlkn_surface_is_flushed;
   vs->base.surface_reference = vlkn_surface_reference;
   vs->base.surface_can_create = vlkn_surface_can_create;
   vs->base.surface_init = vlkn_surface_init;
   vs->base.buffer_create = vlkn_buffer_create;
   vs->base.buffer_map = vlkn_buffer_map;
   vs->base.buffer_unmap = vlkn_buffer_unmap;
   vs->base.buffer_destroy = vlkn_buffer_destroy;
   vs->base.fence_reference = vlkn_fence_reference;
   vs->base.fence_signalled = vlkn_fence_signalled;
   vs->base.fence_finish = vlkn_fence_finish;
   vs->base.fence_get_fd = vlkn_fence_get_fd;
   vs->base.fence_create_fd = vlkn_fence_create_fd;
   vs->base.fence_server_sync = vlkn_fence_server_sync;
   vs->base.shader_create = vlkn_screen_shader_create;
   vs->base.shader_destroy = vlkn_screen_shader_destroy;
   vs->base.query_create = vlkn_query_create;
   vs->base.query_init = vlkn_query_init;
   vs->base.query_destroy = vlkn_query_destroy;
   vs->base.query_get_result = vlkn_query_get_result;
   vs->base.stats_inc = vlkn_stats_inc;
   vs->base.stats_time_push = vlkn_stats_time_push;
   vs->base.stats_time_pop = vlkn_stats_time_pop;
   vs->base.host_log = vlkn_host_log;

   /* Classic VGPU9 path: no guest-backed objects, no VGPU10. */
   vs->base.have_gb_objects = false;
   vs->base.have_gb_dma = false;
   vs->base.have_coherent = false;
   vs->base.max_mob_memory_mib = 256;
   vs->base.have_vgpu10 = false;
   vs->base.have_sm4_1 = false;
   vs->base.have_sm5 = false;
   vs->base.have_gl43 = false;
   vs->base.need_to_rebind_resources = false;
   vs->base.have_generate_mipmap_cmd = true;
   vs->base.have_set_predication_cmd = false;
   vs->base.have_transfer_from_buffer_cmd = false;
   vs->base.have_fence_fd = false;
   vs->base.have_intra_surface_copy = false;
   vs->base.have_constant_buffer_offset_cmd = false;
   vs->base.have_index_vertex_buffer_offset_cmd = false;
   vs->base.have_rasterizer_state_v2_cmd = false;
   vs->base.device_id = 0x0405; /* SVGA_ID_2, same class as the DRM winsys */

   fprintf(stderr, "[svga-vlkn] winsys created (dump_dir=%s)\n",
           vs->dump_dir ? vs->dump_dir : "(none)");
   return &vs->base;
}
