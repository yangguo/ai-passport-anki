#include "pa_cover.h"

#include <assert.h>

_Static_assert(PA_COVER_UI_DECKS == 0, "deck screen is state 0");
_Static_assert(PA_COVER_BTN_UP == 0, "up is button 0");
_Static_assert(PA_COVER_BTN_LONG == 3, "long press is event 3");

int main(void)
{
    assert(pa_cover_requests_launcher(0, 0, 3));
    assert(!pa_cover_requests_launcher(1, 0, 3));
    assert(!pa_cover_requests_launcher(0, 1, 3));
    assert(!pa_cover_requests_launcher(0, 0, 1));
    assert(!pa_cover_requests_launcher(0, 0, 0));
    return 0;
}
