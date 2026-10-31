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

#endif
