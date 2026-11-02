#include <assert.h>
#include "pax_inline_player.h"
int main(void)
{
    /* A six-second clip becomes replayable as soon as its clock reaches 6s. */
    assert(!pax_inline_end_due(true,true,5999999,0,6000000));
    assert(pax_inline_end_due(true,true,6000000,0,6000000));
    assert(!pax_inline_end_due(false,true,6000000,0,6000000));
    assert(!pax_inline_end_due(true,false,6000000,0,6000000));
    assert(!pax_inline_end_due(true,true,6000000,0,-1));
    assert(pax_inline_end_due(true,true,5000000,-1000000,6000000));
    return 0;
}
