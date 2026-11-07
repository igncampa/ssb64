// Replaces scManagerRunLoop. Boot, parked, VS match with sudden death, parked.
#include "port.h"
#include "port_prof.h"
#include <PR/ultratypes.h>
#include <sc/scmanager.h>
#include <sys/audio.h>
#include <sys/scheduler.h>

extern void syControllerPortInit(void);
extern void syAudioPortInit(void), syAudioPortFrame(void);
extern void scManagerPortBoot(void), port_sched_init(void), port_gfx_reset(void);
extern void sySchedulerPortTic(void);
extern sb32 syTaskmanPortStep(sb32 draw);
extern void syTaskmanPortEnd(void);
extern void scVSBattleStartScene(void);
extern sb32 scVSBattlePortCheckSuddenDeath(void);
extern void scVSBattlePortStopAudio(void), scVSBattlePortAudioStopped(void);
extern void func_80006B80(void);
extern void syDmaCreateMesgQueue(void);
extern void *syDmaSramPiInit(void);
extern OSPiHandle *gSYDmaRomPiHandle;
extern sb8 gSYMainImemOK, gSYMainDmemOK;
extern void port_net_scene_ready(int32_t first);
extern void syTaskmanPortForceEnd(void), gcEjectAll(void);
extern void port_net_frame_begin(void), port_net_frame_end(void), port_event(int kind, int arg);

// PARKED: no scene. MATCH: the VS scene steps (the match, then sudden death). MATCH_END_WAIT: offline only,
// the scene is over and the tail waits for the music to stop; netplay runs the tail at once.
enum { PARKED, MATCH, MATCH_END_WAIT };
static int sState = PARKED;
static int sWait;
static uint32_t sFrame;

uint32_t port_sim_frame(void) { return sFrame; }
void port_set_sim_frame(uint32_t f) { sFrame = f; }
extern int port_net_on(void);

void port_boot(void)
{
    { extern char gPortArena[]; extern unsigned long gPortArenaSize; extern uint8_t *port_gfx_arena_end(void);
      if ((uintptr_t)gPortArena + gPortArenaSize > 0x01000000u || (uintptr_t)port_gfx_arena_end() > 0x01000000u) port_fatal("display list memory crosses 16 MB"); }
    port_sched_init();
    gSYDmaRomPiHandle = osCartRomInit();
    syDmaSramPiInit();
    syDmaCreateMesgQueue();
    gSYMainImemOK = gSYMainDmemOK = TRUE; // the SP probes read hardware; the save-poison checks must pass
    syControllerPortInit();
    func_80006B80();   // taskman's startup half
    port_log("boot: audio init");
    syAudioPortInit();
    port_log("boot: scene manager");
    scManagerPortBoot();
    port_log("boot: done");
    port_snapshot_boot_take();
}

// StartMatch has restored the boot image and written the settings.
void port_match_begin(void)
{
    port_gfx_reset();
    sFrame = 0;
    sState = MATCH;
    scVSBattleStartScene();
    port_net_scene_ready(0);
}

// The tail of scVSBattleStartScene after its frame loop, for both paths: sudden death or parked.
// Returns 1 if another scene follows.
static int match_end_tail(void)
{
    scVSBattlePortAudioStopped();
    port_gfx_reset();
    if (scVSBattlePortCheckSuddenDeath()) { sState = MATCH; port_event(10, 0); port_net_scene_ready((int32_t)sFrame); return 1; }
    sState = PARKED;
    port_event(11, 0);
    return 0;
}

// A finish held by rollback is now final: end the scene as it ended at end_frame, then the tail.
int port_force_scene_end(uint32_t end_frame)
{
    syTaskmanPortForceEnd();
    gcEjectAll();
    sFrame = end_frame + 1;
    syTaskmanPortEnd();
    scVSBattlePortStopAudio();
    return match_end_tail();
}

// Stop whatever runs (a desync, a page that left its match). A scene still running is ended the way a
// forced end ends it, so the parked state is the same whichever way it was reached.
void port_match_park(void)
{
    if (sState == MATCH) { syTaskmanPortForceEnd(); gcEjectAll(); }
    if (sState != PARKED) syTaskmanPortEnd();
    sState = PARKED;
    scVSBattlePortStopAudio();
}

static int sAudioOff;
void port_set_audio_off(int off) { sAudioOff = off; }
// Runs once per browser tick after the last sim step. Audio never feeds the sim.
void port_audio_tick(void) { double t0 = port_now(); if (!sAudioOff) syAudioPortFrame(); gPortProf[PROF_AUDIO] += port_now() - t0; }

static int port_step_inner(int draw);
int port_step(int draw) { double t0 = port_now(); int r = port_step_inner(draw); gPortProf[PROF_STEP] += port_now() - t0; return r; }
static int port_step_inner(int draw)
{
    switch (sState)
    {
    case MATCH:
        sySchedulerPortTic();
        port_net_frame_begin();
        if (syTaskmanPortStep(draw))
        {
            sFrame++;
            syTaskmanPortEnd();
            scVSBattlePortStopAudio();
            sState = MATCH_END_WAIT;
            sWait = 0;
            if (port_net_on()) goto end_wait;
        }
        else sFrame++;
        if (sState == MATCH) port_net_frame_end();
        break;

    case MATCH_END_WAIT:
    end_wait:
        // The tail of scVSBattleStartScene, which busy-waited on the audio thread.
        if (port_net_on() || syAudioCheckBGMPlaying(0) == FALSE || ++sWait > 120) match_end_tail();
        break;

    default:
        break;
    }
    return sState != PARKED;
}
