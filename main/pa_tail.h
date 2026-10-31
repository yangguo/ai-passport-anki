#ifndef PA_TAIL_H
#define PA_TAIL_H

#include <stdbool.h>
#include <stdint.h>

/* Card bytes follow the verified image inside the running app partition.
 * A Cover Arts slot is align64k(image) + 64 KiB. The last 64 KiB is the
 * sidecar (DPS1 banks), not card data. Only a factory app may use the
 * rest of its partition. Anything else is refused. */

#define PA_TAIL_SECTOR 4096u
#define PA_TAIL_SLOT_ALIGN 65536u
#define PA_TAIL_SIDECAR 0x10000u
#define PA_TAIL_DPS1_A 0xA000u
#define PA_TAIL_DPS1_B 0xB000u

/* Exclusive end of card bytes. *data_start is image_len aligned to 4 KiB.
 * sidecar_present is true only after a DPS1 read at the banks below.
 * Returns false for an unknown partition instead of using part_size. */
bool pa_tail_data_end(uint32_t part_size, uint32_t image_len, bool is_factory,
                      bool sidecar_present, uint32_t *data_start, uint32_t *data_end);

/* Offsets of the two DPS1 banks. False when the last 64 KiB overlaps the
 * image or does not fit in the partition. */
bool pa_tail_sidecar_banks(uint32_t part_size, uint32_t image_len,
                           uint32_t *bank_a, uint32_t *bank_b);

/* Partition-relative window. data_start/data_end come from pa_tail_data_end.
 * The running app partition includes this window, so IDF's default
 * SPI_FLASH_DANGEROUS_WRITE_ABORTS calls abort() on an erase or write here
 * unless protection is dropped for that call only. */
typedef struct {
    uint32_t part_size;
    uint32_t image_len;
    uint32_t data_start;
    uint32_t data_end;
} pa_tail_bounds;

/* True when [offset, offset+length) sits in [data_start, data_end) and the
 * window itself starts on or after the first sector past the verified image.
 * A span that shares a sector with the image is refused. */
bool pa_tail_span_allowed(const pa_tail_bounds *bounds, uint32_t offset, uint32_t length);

/* Length programmed by a 16-byte padded flash write. 0 if length is 0 or the
 * padded end would wrap. */
uint32_t pa_tail_programmed_length(uint32_t length);

typedef bool (*pa_tail_set_protect_fn)(void *ctx, bool protect);
typedef bool (*pa_tail_io_fn)(void *ctx, uint32_t offset, uint32_t length, const void *src);

/* Drop dangerous-write protection, run io, then turn protection back on.
 * io is not called unless the span is allowed, and it is not called while
 * protection is still enabled. pad_to_16 checks the programmed length but
 * passes the caller's length to io. */
bool pa_tail_protected_io(const pa_tail_bounds *bounds, pa_tail_set_protect_fn set_protect,
                          void *protect_ctx, uint32_t offset, uint32_t length, const void *src,
                          bool pad_to_16, pa_tail_io_fn io, void *io_ctx);

#endif
