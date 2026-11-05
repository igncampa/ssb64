// Sound handles by frame and call order, so a re-simulated frame gets back what its first run got.
#include "port.h"
#define FRAMES 64
#define CALLS 256
uint32_t gPortSfxStarted;
static struct { uint16_t id; void *h; } sJ[FRAMES][CALLS];
static uint8_t sN[FRAMES];
static int32_t sF[FRAMES];
static int32_t sCur = -1; static int sCall;

static int cursor(void)
{
    int32_t f = (int32_t)port_sim_frame();
    int i = f & (FRAMES - 1);
    if (f != sCur) { sCur = f; sCall = 0; if (sF[i] != f) { sF[i] = f; sN[i] = 0; } }
    return i;
}
void port_sfx_record(uint16_t id, void *h)
{
    gPortSfxStarted++;
    int i = cursor();
    if (sN[i] < CALLS) { sJ[i][sN[i]].id = id; sJ[i][sN[i]].h = h; sN[i]++; }
    sCall++;
}
void *port_sfx_replay(uint16_t id)
{
    int i = cursor(), c = sCall++;
    return (sF[i] == sCur && c < sN[i] && sJ[i][c].id == id) ? sJ[i][c].h : 0;
}

// Every simulated frame starts its call count over, including one that re-runs the frame just finished.
void port_sfx_frame_begin(void) { sCur = -1; }

// Sounds started by first runs of frames. A proof compares it with a run that had no rollbacks.
uint32_t port_sfx_started(void) { return gPortSfxStarted; }
