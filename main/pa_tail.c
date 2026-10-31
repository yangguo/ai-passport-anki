#include "pa_tail.h"

#include <stddef.h>

static uint32_t align_up(uint32_t value, uint32_t boundary)
{
    uint32_t mask = boundary - 1u;
    if (value > UINT32_MAX - mask) {
        return 0;
    }
    return (value + mask) & ~mask;
}

bool pa_tail_sidecar_banks(uint32_t part_size, uint32_t image_len,
                           uint32_t *bank_a, uint32_t *bank_b)
{
    uint32_t data_start;
    uint32_t side;
    if (bank_a == NULL || bank_b == NULL || image_len == 0 || image_len > part_size ||
        part_size < PA_TAIL_SIDECAR) {
        return false;
    }
    data_start = align_up(image_len, PA_TAIL_SECTOR);
    if (data_start == 0) {
        return false;
    }
    side = part_size - PA_TAIL_SIDECAR;
    if (side < data_start || side > UINT32_MAX - PA_TAIL_DPS1_B) {
        return false;
    }
    if (4u > part_size - (side + PA_TAIL_DPS1_B)) {
        return false;
    }
    *bank_a = side + PA_TAIL_DPS1_A;
    *bank_b = side + PA_TAIL_DPS1_B;
    return true;
}

bool pa_tail_data_end(uint32_t part_size, uint32_t image_len, bool is_factory,
                      bool sidecar_present, uint32_t *data_start, uint32_t *data_end)
{
    uint32_t start;
    uint32_t slot;
    uint32_t side;
    if (data_start == NULL || data_end == NULL || image_len == 0 || image_len > part_size) {
        return false;
    }
    start = align_up(image_len, PA_TAIL_SECTOR);
    if (start == 0 || start >= part_size) {
        return false;
    }
    slot = align_up(image_len, PA_TAIL_SLOT_ALIGN);
    if (slot != 0 && slot <= UINT32_MAX - PA_TAIL_SIDECAR &&
        part_size == slot + PA_TAIL_SIDECAR) {
        *data_start = start;
        *data_end = part_size - PA_TAIL_SIDECAR;
        return *data_end >= start;
    }
    if (sidecar_present && part_size >= PA_TAIL_SIDECAR) {
        side = part_size - PA_TAIL_SIDECAR;
        if (side >= start) {
            *data_start = start;
            *data_end = side;
            return true;
        }
    }
    if (is_factory) {
        *data_start = start;
        *data_end = part_size;
        return true;
    }
    return false;
}

bool pa_tail_span_allowed(const pa_tail_bounds *bounds, uint32_t offset, uint32_t length)
{
    uint32_t image_end;
    if (bounds == NULL || length == 0 || bounds->image_len == 0 || bounds->part_size == 0) {
        return false;
    }
    image_end = align_up(bounds->image_len, PA_TAIL_SECTOR);
    if (image_end == 0 || bounds->data_start < image_end || bounds->data_end < bounds->data_start ||
        bounds->data_end > bounds->part_size) {
        return false;
    }
    if (offset < bounds->data_start || offset > bounds->data_end || length > bounds->data_end - offset) {
        return false;
    }
    return true;
}

uint32_t pa_tail_programmed_length(uint32_t length)
{
    if (length == 0 || length > UINT32_MAX - 15u) {
        return 0;
    }
    return (length + 15u) & ~15u;
}

bool pa_tail_protected_io(const pa_tail_bounds *bounds, pa_tail_set_protect_fn set_protect,
                          void *protect_ctx, uint32_t offset, uint32_t length, const void *src,
                          bool pad_to_16, pa_tail_io_fn io, void *io_ctx)
{
    uint32_t span = length;
    bool ok;
    bool restored;
    if (bounds == NULL || set_protect == NULL || io == NULL || length == 0) {
        return false;
    }
    if (pad_to_16) {
        span = pa_tail_programmed_length(length);
        if (span == 0) {
            return false;
        }
    }
    if (!pa_tail_span_allowed(bounds, offset, span)) {
        return false;
    }
    if (!set_protect(protect_ctx, false)) {
        (void)set_protect(protect_ctx, true);
        return false;
    }
    ok = io(io_ctx, offset, length, src);
    restored = set_protect(protect_ctx, true);
    return ok && restored;
}
