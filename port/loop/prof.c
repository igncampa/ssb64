// Frame profile: where a browser tick's CPU time goes and what the renderer submitted. Render-side
// bookkeeping only: nothing here is read by the simulation. Cumulative; a reader takes differences.
#include <stdint.h>
#include <stddef.h>
#include <emscripten.h>
#include "port_prof.h"

double gPortProf[PROF_N];
uint32_t gPortCount[COUNT_N];

double port_now(void) { return emscripten_get_now(); }

// i < PROF_N: milliseconds accumulated in that phase; else gPortCount[i - PROF_N].
EMSCRIPTEN_KEEPALIVE double PortProf(int i)
{
    if (i < 0) return 0;
    if (i < PROF_N) return gPortProf[i];
    i -= PROF_N;
    return i < COUNT_N ? (double)gPortCount[i] : 0;
}
