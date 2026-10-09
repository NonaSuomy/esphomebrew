// Exercise the patched upstream playback loop, not a copy of its logic.
#include <cassert>
#include <cstdio>
#include "common/vqatask.cpp"

static unsigned pending, draws, loads, flips;
static bool skip;
void VQA_ConfigureDrawer(VQAHandle*) {}
int VQA_StartAudio(VQAHandle*) { return 0; }
void VQA_StopAudio(VQAHandle*) {}
void VQA_SetTimer(VQAHandle*, int, int) {}
unsigned VQA_GetTime(VQAHandle*) { return 0; }
int VQA_LoadFrame(VQAHandle*) { ++loads; return VQAERR_ERROR; }
static int draw(VQAHandle*) {
    if (!pending) return VQAERR_NOBUFFER;
    --pending;
    ++draws;
    return VQAERR_NONE;
}
static int flip(VQAHandle*) { ++flips; return skip ? 1 : 0; }

int main() {
    VQAHandle handle = {};
    VQAData data = {};
    handle.VQABuf = &data;
    data.Draw_Frame = draw;
    data.Page_Flip = flip;
    data.Flags = VQA_DATA_FLAG_32; // Already configured, as at loader EOF.
    pending = 3;
    VQA_Play(&handle, VQAMODE_RUN);
    assert(loads == 1 && draws == 3 && pending == 0 && flips == 3);
    assert(data.Flags & VQA_DATA_FLAG_8);

    // Manual skip still exits immediately, without draining buffered frames.
    data.Flags = VQA_DATA_FLAG_32;
    loads = draws = flips = 0;
    pending = 3;
    skip = true;
    VQA_Play(&handle, VQAMODE_RUN);
    assert(loads == 1 && draws == 1 && pending == 2 && flips == 1);
    puts("Red Alert VQA end-of-file / manual skip tests passed");
}
