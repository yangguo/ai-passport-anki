#include "pa_tail.h"

#include <assert.h>
#include <stdint.h>

/* Padded image from the firmware layout check: length % 65536 == 4096. */
static const uint32_t PADDED_IMAGE = 2232320u;

static uint32_t align_up(uint32_t value, uint32_t boundary)
{
    return (value + boundary - 1u) & ~(boundary - 1u);
}

int main(void)
{
    uint32_t start = 1;
    uint32_t end = 1;
    uint32_t bank_a = 0;
    uint32_t bank_b = 0;
    uint32_t slot;

    assert(PADDED_IMAGE % PA_TAIL_SLOT_ALIGN == 4096u);
    slot = align_up(PADDED_IMAGE, PA_TAIL_SLOT_ALIGN) + PA_TAIL_SIDECAR;

    assert(pa_tail_data_end(slot, PADDED_IMAGE, false, false, &start, &end));
    assert(start == PADDED_IMAGE);
    assert(end == slot - PA_TAIL_SIDECAR);
    assert(end - start == 61440u);

    assert(pa_tail_data_end(0x7F0000u, PADDED_IMAGE, true, false, &start, &end));
    assert(start == PADDED_IMAGE);
    assert(end == 0x7F0000u);

    /* Same large partition, neither a slot nor factory nor a sidecar. */
    start = 1;
    end = 1;
    assert(!pa_tail_data_end(0x7F0000u, PADDED_IMAGE, false, false, &start, &end));
    assert(start == 1 && end == 1);
    assert(!pa_tail_data_end(0x200000u, 0x100000u, false, false, &start, &end));

    assert(pa_tail_sidecar_banks(0x180000u, 0x100000u, &bank_a, &bank_b));
    assert(bank_a == 0x170000u + PA_TAIL_DPS1_A);
    assert(bank_b == 0x170000u + PA_TAIL_DPS1_B);
    assert(pa_tail_data_end(0x180000u, 0x100000u, false, true, &start, &end));
    assert(end == 0x170000u);
    assert(!pa_tail_data_end(0x180000u, 0x100000u, false, false, &start, &end));

    assert(!pa_tail_sidecar_banks(0x180000u, 0x170001u, &bank_a, &bank_b));
    assert(!pa_tail_data_end(0x180000u, 0x170001u, false, true, &start, &end));

    /* A slot still reserves the sidecar when that partition is factory. */
    assert(pa_tail_data_end(slot, PADDED_IMAGE, true, true, &start, &end));
    assert(end == slot - PA_TAIL_SIDECAR);

    assert(!pa_tail_data_end(slot, 0, true, true, &start, &end));
    assert(!pa_tail_data_end(PADDED_IMAGE, PADDED_IMAGE, true, false, &start, &end));
    return 0;
}
