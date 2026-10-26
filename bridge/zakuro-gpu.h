/* Zakuro's PICA200 renderer (software) behind a C interface: the runtime
 * draws with it until its native Vulkan renderer exists. Built by
 * bridge/zakuro-gpu-c (Rust). Addresses are physical. */

#ifndef ZAKURO_GPU_H
#define ZAKURO_GPU_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ZGpu ZGpu;

ZGpu *zgpu_new(uint8_t *fcram, size_t fcram_size, uint8_t *vram, size_t vram_size, uint8_t *axi, size_t axi_size);
void zgpu_free(ZGpu *gpu);
void zgpu_write_register(ZGpu *gpu, uint32_t offset, uint32_t value);
uint32_t zgpu_read_register(ZGpu *gpu, uint32_t offset);
void zgpu_command_list(ZGpu *gpu, uint32_t paddr, uint32_t size);
void zgpu_memory_fill(ZGpu *gpu, uint32_t start, uint32_t end, uint32_t value, uint32_t width);
void zgpu_display_transfer(ZGpu *gpu, uint32_t input, uint32_t output, uint32_t input_size, uint32_t output_size,
                           uint32_t flags);
void zgpu_texture_copy(ZGpu *gpu, uint32_t input, uint32_t output, uint32_t size, uint32_t input_gap,
                       uint32_t output_gap);
void zgpu_set_framebuffer(ZGpu *gpu, uint32_t screen, uint32_t active, uint32_t left, uint32_t right, uint32_t stride,
                          uint32_t format);
/* draws on the host GPU through Vulkan at scale times the 3DS resolution; 0 when it cannot.
   title, when not 0, keeps the compiled pipelines on disk for the next run */
int zgpu_enable_hardware(ZGpu *gpu, uint32_t scale, uint64_t title);
/* memory over a range made to hold what the host GPU drew there */
void zgpu_sync(ZGpu *gpu, uint32_t addr, uint32_t len);
/* the same for the depth it drew, for the CPU reading there */
void zgpu_sync_depth(ZGpu *gpu, uint32_t addr, uint32_t len);
/* guard is told of each range the host GPU drew: physical address, size, and 1 for
   the CPU's writes there, 0 for its reads */
void zgpu_set_guard(ZGpu *gpu, void (*guard)(void *user, uint32_t addr, uint32_t len, int32_t write), void *user);
/* what a screen shows as the host GPU drew it, upright RGBA, no write-back; null when
   there is none. good until the next call for the same screen */
const uint8_t *zgpu_scaled_screen(ZGpu *gpu, uint32_t screen, uint32_t *width, uint32_t *height);
void zgpu_framebuffer(ZGpu *gpu, uint32_t screen, uint32_t *address, uint32_t *stride, uint32_t *format);

#ifdef __cplusplus
}
#endif

#endif
