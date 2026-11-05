// The N64 scheduler thread reduced to what is left with no RCP: finish every queued task at once.
#include "port.h"
#include "port_syms.h"
#include <ssb_types.h>
#include <PR/os.h>
#include <PR/sptask.h>
#include <sys/scheduler.h>

extern OSMesgQueue gSYSchedulerTaskMesgQueue;
extern void port_gfx_run(void *dl);

static uint8_t sGfxArena[3 * 1024 * 1024];
static uint32_t sGfxUsed;
static OSMesg sTaskMesgs[64];
static int sInit;

uint8_t *port_parts_save_buffer(size_t bytes)
{
    static uint8_t buf[1024 * 1024];
    if (bytes > sizeof(buf)) port_fatal("parts save buffer too small");
    return buf;
}

void port_sched_init(void) { osCreateMesgQueue(&gSYSchedulerTaskMesgQueue, sTaskMesgs, 64); sInit = 1; }
void port_gfx_reset(void) { sGfxUsed = 0; }
uint8_t *port_gfx_arena_end(void) { return sGfxArena + sizeof(sGfxArena); }

void *port_gfx_malloc(size_t size, uint32_t align)
{
    uint32_t o = (sGfxUsed + (align ? align : 1) - 1) & ~((align ? align : 1) - 1);
    if (o + size > sizeof(sGfxArena)) port_fatal("gfx arena exhausted");
    sGfxUsed = o + size;
    return sGfxArena + o;
}

extern void port_audio_run_list(const uint32_t *list, int n);
extern uint32_t port_resample_lut_value;

// The audio thread's SP task: run its command list at once.
void port_audio_run_task(SYTaskAudio *t) { port_audio_run_list((const uint32_t *)t->task.t.data_ptr, t->task.t.data_size / 8); }

uint32_t port_resample_lut_rom(void) { return PORT_RESAMPLE_LUT_ROM; }

void port_sched_drain(void)
{
    OSMesg m;

    while (osRecvMesg(&gSYSchedulerTaskMesgQueue, &m, OS_MESG_NOBLOCK) != -1)
    {
        SYTaskInfo *t = (SYTaskInfo*)m;

        if (t->type == nSYTaskTypeGfx) port_gfx_run(((SYTaskGfx*)t)->task.t.data_ptr);
        if (t->mq != NULL) osSendMesg(t->mq, (OSMesg)(intptr_t)t->retVal, OS_MESG_NOBLOCK);
    }
}
