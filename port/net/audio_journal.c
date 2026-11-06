// Sound handles by frame, so a re-simulated frame gets back what its first run started.
// A re-run is the truth: a sound it asks for that the first run did not start starts now, a few frames late,
// and a sound the first run started that the re-run did not ask for is stopped. Without that, a punch whose
// first frame was predicted wrong loses its opening voice ("Falcon"), and a wrong guess leaves a ghost sound.
// Audio only: the handles and their ids are stashed out of every hash (harness.c).
#include "port.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif
#define FRAMES 64
#define CALLS 256
uint32_t gPortSfxStarted;
extern int gPortResim;
extern void port_sfx_stop_if(void *h, uint16_t serial); // n_env.c: stops h if it still plays that sound
typedef struct { uint16_t id, serial; uint8_t claimed; int32_t log; void *h; } Rec;
// What was heard, for proofs: frame, id, and 1 if a re-run took it back. Two clients must agree on it.
#define LOG 16384
static int32_t sLog[LOG][3]; static int32_t sLogN;
EMSCRIPTEN_KEEPALIVE int32_t *SfxLog(void) { return &sLog[0][0]; }
EMSCRIPTEN_KEEPALIVE int32_t SfxLogCount(void) { return sLogN; }
// For the proof's negative control only: 1 is the old journal (re-runs never start or stop a sound).
static int sOld;
EMSCRIPTEN_KEEPALIVE void SfxJournalOld(int on) { sOld = on; }
int port_sfx_old(void) { return sOld; }
static Rec sJ[FRAMES][CALLS];
static uint16_t sN[FRAMES];
static int32_t sF[FRAMES];
static int32_t sCur = -1;
static uint32_t sLate, sCut;

static int slot_for(int32_t f) { return f & (FRAMES - 1); }

// Called before every run of frame f, first or not.
void port_sfx_frame_begin(int32_t f)
{
    int i = slot_for(f), k;
    sCur = f;
    if (sF[i] != f) { sF[i] = f; sN[i] = 0; return; }
    for (k = 0; k < sN[i]; k++) sJ[i][k].claimed = 0;
}

// Called after every run of frame f. After a re-run, what it did not ask for again was a wrong guess.
void port_sfx_frame_end(int32_t f, int resim)
{
    int i = slot_for(f), k, n = 0;
    if (!resim || sF[i] != f || sOld) return;
    for (k = 0; k < sN[i]; k++) {
        if (sJ[i][k].claimed) { sJ[i][n++] = sJ[i][k]; continue; }
        if (sJ[i][k].log >= 0 && sJ[i][k].log < LOG) sLog[sJ[i][k].log][2] = 1;
        if (sJ[i][k].h) { port_sfx_stop_if(sJ[i][k].h, sJ[i][k].serial); sCut++; if (gPortSfxStarted) gPortSfxStarted--; }
    }
    sN[i] = n;
}

static int cursor(void)
{
    int32_t f = (int32_t)port_sim_frame();
    int i = slot_for(f);
    // A sound outside a stepped frame (the scene's own calls): keep it in the frame it happened in.
    if (f != sCur) port_sfx_frame_begin(f);
    return i;
}

// A sound just started, on a first run or late on a re-run.
void port_sfx_record(uint16_t id, void *h, uint16_t serial)
{
    int i = cursor();
    gPortSfxStarted++;
    if (gPortResim) sLate++;
    if (sN[i] < CALLS) { Rec *r = &sJ[i][sN[i]++]; r->id = id; r->h = h; r->serial = serial; r->claimed = 1; r->log = sLogN; }
    if (sLogN < LOG) { sLog[sLogN][0] = sCur; sLog[sLogN][1] = id; sLog[sLogN][2] = 0; sLogN++; }
}

// On a re-run: the first-run sound with this id that this run has not taken yet. 0 if there is none.
int port_sfx_claim(uint16_t id, void **h)
{
    int i = cursor(), k;
    for (k = 0; k < sN[i]; k++) {
        Rec *r = &sJ[i][k];
        if (!r->claimed && r->id == id) { r->claimed = 1; *h = r->h; return 1; }
    }
    if (sOld) { *h = 0; return 1; }
    return 0;
}

// Sounds heard: first runs, plus late starts, less the ones a re-run took back.
uint32_t port_sfx_started(void) { return gPortSfxStarted; }
uint32_t port_sfx_late(void) { return sLate; }
uint32_t port_sfx_cut(void) { return sCut; }
