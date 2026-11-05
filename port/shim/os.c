// libultra for a single stepped thread. Message queues are plain rings.
// A blocking receive on an empty queue can never be satisfied here, so it is fatal.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "port.h"

typedef void *OSMesg;
typedef struct OSMesgQueue { void *mt, *full; int validCount, first, msgCount; OSMesg *msg; } OSMesgQueue;
#define NOBLOCK 0

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
EM_JS(void, port_js_fatal, (const char *why), { throw new Error("port fatal: " + UTF8ToString(why)); });
#else
static void port_js_fatal(const char *why) { abort(); }
#endif

extern void port_dump_state(void);
void port_fatal(const char *why) { static int once; fprintf(stderr, "PORT FATAL: %s\n", why); if (!once) { once = 1; port_dump_state(); } port_js_fatal(why); abort(); }
void port_log(const char *fmt, ...) { va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); fputc('\n', stderr); }

void port_vlog(const char *fmt, va_list ap) { vfprintf(stderr, fmt, ap); fflush(stderr); }

void osCreateMesgQueue(OSMesgQueue *q, OSMesg *m, int n) { q->mt = q->full = 0; q->validCount = q->first = 0; q->msgCount = n; q->msg = m; }
int osSendMesg(OSMesgQueue *q, OSMesg m, int flag) {
    if (q->validCount >= q->msgCount) { if (flag == NOBLOCK) return -1; port_fatal("osSendMesg: would block on a full queue"); }
    q->msg[(q->first + q->validCount) % q->msgCount] = m; q->validCount++; return 0;
}
int osJamMesg(OSMesgQueue *q, OSMesg m, int flag) {
    if (q->validCount >= q->msgCount) { if (flag == NOBLOCK) return -1; port_fatal("osJamMesg: would block"); }
    q->first = (q->first + q->msgCount - 1) % q->msgCount; q->msg[q->first] = m; q->validCount++; return 0;
}
int osRecvMesg(OSMesgQueue *q, OSMesg *m, int flag) {
    if (q->validCount == 0) {
        if (flag == NOBLOCK) return -1;
        port_sched_drain();   // a task reply: the "scheduler" finishes queued tasks here
        if (q->validCount == 0) port_fatal("osRecvMesg: would block on an empty queue");
    }
    if (m) *m = q->msg[q->first];
    q->first = (q->first + 1) % q->msgCount; q->validCount--; return 0;
}
void osSetEventMesg(int e, OSMesgQueue *q, OSMesg m) {}

// No threads exist. Starting one would mean a code path this port does not support.
void osCreateThread(void *t, int id, void (*entry)(void*), void *arg, void *sp, int pri) { port_fatal("osCreateThread"); }
void osStartThread(void *t) { port_fatal("osStartThread"); }
void osStopThread(void *t) { port_fatal("osStopThread"); }
void osDestroyThread(void *t) { }
void osSetThreadPri(void *t, int p) { }
int osGetThreadPri(void *t) { return 0; }
void *__osGetActiveQueue(void) { return NULL; }

void osInitialize(void) {}
unsigned int osSetIntMask(unsigned int m) { return 0; }
void __osSetWatchLo(unsigned int v) {}
int osAfterPreNMI(void) { return 0; }
unsigned int osVirtualToPhysical(void *p) { return (unsigned int)(uintptr_t)p; }
void osWritebackDCache(void *p, int n) {} void osWritebackDCacheAll(void) {} void osInvalDCache(void *p, int n) {} void osInvalICache(void *p, int n) {}

// Time feeds profiling only. Fixed values keep it out of anything hashed.
unsigned int osGetCount(void) { return 0; }
unsigned long long osGetTime(void) { return 0; }
void osSetTime(unsigned long long t) {}

// VI
void osCreateViManager(int p) {} void osViSetEvent(OSMesgQueue *q, OSMesg m, unsigned int c) {}
void osViSetMode(void *m) {} void osViBlack(unsigned char b) {} void osViSwapBuffer(void *f) {}
void osViSetYScale(float v) {}
void *osViGetCurrentFramebuffer(void) { return 0; } void *osViGetNextFramebuffer(void) { return 0; }
int osDpSetNextBuffer(void *b, unsigned long long s) { return 0; }
void osSpTaskLoad(void *t) {} void osSpTaskStartGo(void *t) {} void osSpTaskYield(void) {} int osSpTaskYielded(void *t) { return 0; }

// PI: the "ROM" is the data image.
static const uint8_t *sRom; static uint32_t sRomSize;
void port_rom_set(const uint8_t *image, uint32_t size) { sRom = image; sRomSize = size; }

typedef struct { struct { unsigned short t; unsigned char pri, st; OSMesgQueue *ret; } hdr; void *dram; uint32_t dev; uint32_t size; void *piHandle; } IoMesg;
static uint8_t sCartHandle[0x80];
void *osCartRomInit(void) { return sCartHandle; }
void osCreatePiManager(int pri, OSMesgQueue *q, OSMesg *m, int n) {}
int osEPiLinkHandle(void *h) { return 0; }
int osEPiStartDma(void *handle, IoMesg *mb, int dir) {
    if (handle == sCartHandle) {
        if (dir == 0) {
            if (mb->dev + mb->size > sRomSize) { port_log("rom read %x+%x past %x", mb->dev, mb->size, sRomSize); port_fatal("ROM read out of range"); }
            memcpy(mb->dram, sRom + mb->dev, mb->size);
        }
    } else if (dir == 0) {
        memset(mb->dram, 0, mb->size); // SRAM reads as empty: the game restores its default save
    }
    if (mb->hdr.ret) osSendMesg(mb->hdr.ret, (OSMesg)mb, NOBLOCK);
    return 0;
}

// SI: pads never come through here (see controller.c).
int osContInit(OSMesgQueue *q, unsigned char *b, void *d) { *b = 0xF; return 0; }
int osContStartQuery(OSMesgQueue *q) { return 0; } void osContGetQuery(void *d) {}
int osContStartReadData(OSMesgQueue *q) { return 0; } void osContGetReadData(void *d) {}
int osMotorInit(OSMesgQueue *q, void *p, int c) { return 1; } int __osMotorAccess(void *p, unsigned int f) { return 0; }

// Debug printing is not supported.
int _Printf(void *f, void *a, const char *s, va_list ap) { return 0; }

const uint8_t *port_rom_ptr(uint32_t off) { return sRom + off; }
