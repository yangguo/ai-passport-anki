#include "pa_cover.h"

bool pa_cover_requests_launcher(int state, int button, int event)
{
    return state == PA_COVER_UI_DECKS && button == PA_COVER_BTN_UP && event == PA_COVER_BTN_LONG;
}
