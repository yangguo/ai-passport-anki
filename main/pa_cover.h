#ifndef PA_COVER_H
#define PA_COVER_H

#include <stdbool.h>

/* Deck list is the existing start screen. UP long is the Cover Arts return.
 * These numbers match UI_DECKS, BSP_BTN_UP, and BSP_BTN_LONG. Firmware
 * static asserts keep them tied to those enums. The hold time is
 * BSP_BTN_LONG_PRESS_MS (500), not a separate 1.5 s gesture. */
#define PA_COVER_UI_DECKS 0
#define PA_COVER_BTN_UP 0
#define PA_COVER_BTN_LONG 3

bool pa_cover_requests_launcher(int state, int button, int event);

#endif
