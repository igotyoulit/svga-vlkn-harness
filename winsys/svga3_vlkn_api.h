/*
 * svga3_vlkn_api.h
 *
 * Minimal forward declarations for the svga3_vlkn public C API used by the
 * in-process winsys.
 *
 * Rationale: the full "svga3_vlkn.h" header pulls in the library's own copy
 * of the SVGA register headers (vmsvga/svga_reg.h, vmsvga/svga3d_reg.h),
 * whose type definitions collide with Mesa's copies included via
 * "svga_winsys.h".  The register-level types are ABI-compatible (same
 * VMware spec), so this shim declares only the functions the winsys calls,
 * using plain C types plus Mesa's register types where a struct is passed
 * by pointer.  The real definitions live in libsvga3_vlkn.a.
 */

#ifndef SVGA3_VLKN_API_H
#define SVGA3_VLKN_API_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* The register types below (SVGA3dSurfaceFormat, SVGA3dSize, SVGA3dBox)
 * must already be declared before this header is included -- in practice
 * via Mesa's "svga_winsys.h".  They are layout-compatible with the
 * library's own copies, so the shim passes them through as void* or int
 * where possible and as const void* for the structs. */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Svga3VlknDevice Svga3VlknDevice;

typedef enum Svga3VlknStatus {
   SVGA3_VLKN_SUCCESS = 0,
   SVGA3_VLKN_ERROR_INVALID_PARAM = -1,
   SVGA3_VLKN_ERROR_OUT_OF_MEMORY = -2,
   SVGA3_VLKN_ERROR_NOT_FOUND = -3,
   SVGA3_VLKN_ERROR_ALREADY_EXISTS = -4,
   SVGA3_VLKN_ERROR_VULKAN_INIT_FAILED = -5,
   SVGA3_VLKN_ERROR_DEVICE_LOST = -6,
   SVGA3_VLKN_ERROR_UNSUPPORTED_FORMAT = -7,
   SVGA3_VLKN_ERROR_UNSUPPORTED_COMMAND = -8,
   SVGA3_VLKN_ERROR_PIPELINE_CREATION = -9,
   SVGA3_VLKN_ERROR_INVALID_COMMAND_BUFFER = -10,
   SVGA3_VLKN_ERROR_UNSUPPORTED_SHADER = -11,
   SVGA3_VLKN_ERROR_SHADER_TRANSLATION = -12,
} Svga3VlknStatus;

typedef struct Svga3VlknConfig {
   const char *appName;
   uint32_t    apiVersion;
   bool        enableValidationLayers;
   bool        preferIntegratedGpu;
   bool        forceMockBackend;
   uint32_t    maxSurfaces;
   uint32_t    maxContexts;
   size_t      stagingBufferSize;
} Svga3VlknConfig;

/* Device lifetime */
Svga3VlknDevice *svga3_vlkn_device_create(const Svga3VlknConfig *config);
void             svga3_vlkn_device_destroy(Svga3VlknDevice *dev);
Svga3VlknStatus  svga3_vlkn_device_wait_idle(Svga3VlknDevice *dev);

/* Capability query: returns 1 on success (writes *outCapValue), 0 if
 * the capability index is unknown or unsupported. */
uint32_t         svga3_vlkn_query_cap(Svga3VlknDevice *dev,
                                      uint32_t capIndex,
                                      uint32_t *outCapValue);

/* FIFO execution: consumes Mesa's command stream (u32 id, u32 size, payload)
 * and reports how many bytes were consumed. */
Svga3VlknStatus  svga3_vlkn_fifo_execute(Svga3VlknDevice *dev,
                                         const void *commandBuffer,
                                         size_t bufferSizeBytes,
                                         size_t *bytesConsumed);

/* Context management */
Svga3VlknStatus  svga3_vlkn_context_create(Svga3VlknDevice *dev, uint32_t cid);
Svga3VlknStatus  svga3_vlkn_context_destroy(Svga3VlknDevice *dev, uint32_t cid);

/* Surface management.  The driver never emits DEFINE_SURFACE on the classic
 * path; device-side surfaces are created here in the winsys (like the DRM
 * winsys does via ioctl).  Format/size structs use Mesa's definitions,
 * which are layout-compatible with the library's. */
Svga3VlknStatus  svga3_vlkn_surface_define(Svga3VlknDevice *dev,
                                           uint32_t sid,
                                           uint32_t surfaceFlags,
                                           int format, /* SVGA3dSurfaceFormat */
                                           const void *sizes, /* SVGA3dSize[] */
                                           uint32_t numSizes);
Svga3VlknStatus  svga3_vlkn_surface_destroy(Svga3VlknDevice *dev, uint32_t sid);
Svga3VlknStatus  svga3_vlkn_surface_dma_download(Svga3VlknDevice *dev,
                                                 uint32_t sid,
                                                 uint32_t mipLevel,
                                                 const void *box, /* SVGA3dBox* */
                                                 void *outGuestData,
                                                 size_t guestStride);

/* Guest memory: register page-aligned host allocations as synthetic guest
 * RAM, then back each winsys buffer with a GMR2 region so SVGAGuestPtr
 * relocations (gpa field = GMR id) resolve to the host pointer. */
Svga3VlknStatus  svga3_vlkn_device_map_guest_ram(Svga3VlknDevice *dev,
                                                 uint64_t gpaBase,
                                                 void *hvaBase,
                                                 size_t size);
Svga3VlknStatus  svga3_vlkn_device_unmap_guest_ram(Svga3VlknDevice *dev,
                                                   uint64_t gpaBase);
Svga3VlknStatus  svga3_vlkn_gmr_define(Svga3VlknDevice *dev,
                                       uint32_t gmrId,
                                       uint32_t numPages);
Svga3VlknStatus  svga3_vlkn_gmr_remap(Svga3VlknDevice *dev,
                                      uint32_t gmrId,
                                      uint32_t flags,
                                      uint32_t offsetPages,
                                      uint32_t numPages,
                                      const void *descriptors,
                                      size_t descriptorBytes);
Svga3VlknStatus  svga3_vlkn_gmr_destroy(Svga3VlknDevice *dev,
                                        uint32_t gmrId);

#ifdef __cplusplus
}
#endif

#endif /* SVGA3_VLKN_API_H */
