// The sim's memory: game .data and .bss between sentinel objects, plus the used part of the arena.
// Link order in Makefile.port puts audio, debug and platform objects outside the sentinels.
#include "port.h"
#include <ssb_types.h>
#include <sys/malloc.h>
extern SYMallocRegion gSYTaskmanGeneralHeap;
void *memcpy(void *, const void *, unsigned long);
void *memset(void *, int, unsigned long);
void *malloc(unsigned long);

extern char gGameDataBegin[], gGameDataEnd[], gGameBssBegin[], gGameBssEnd[];
char gPortArena[PORT_ARENA_SIZE] __attribute__((aligned(16)));
unsigned long gPortArenaSize = PORT_ARENA_SIZE;

static char *sBoot;
static unsigned long sBootData, sBootBss;

unsigned long port_snapshot_static_size(void) { return (unsigned long)(gGameDataEnd - gGameDataBegin) + (unsigned long)(gGameBssEnd - gGameBssBegin); }
unsigned long port_snapshot_arena_used(void) { return (unsigned long)((char*)gSYTaskmanGeneralHeap.ptr - gPortArena); }

void port_snapshot_boot_take(void)
{
    sBootData = gGameDataEnd - gGameDataBegin;
    sBootBss = gGameBssEnd - gGameBssBegin;
    sBoot = malloc(sBootData + sBootBss);
    memcpy(sBoot, gGameDataBegin, sBootData);
    memcpy(sBoot + sBootData, gGameBssBegin, sBootBss);
}

void port_snapshot_boot_restore(void)
{
    memcpy(gGameDataBegin, sBoot, sBootData);
    memcpy(gGameBssBegin, sBoot + sBootData, sBootBss);
}

// One frame's copy of the sim. Sized on first use.
static unsigned long sStaticSize;

unsigned long port_snapshot_size(void) { return port_snapshot_static_size() + 16 + port_snapshot_arena_used(); }

void port_snapshot_save(char *dst)
{
    unsigned long d = gGameDataEnd - gGameDataBegin, b = gGameBssEnd - gGameBssBegin, a = port_snapshot_arena_used();
    void *ptr = gSYTaskmanGeneralHeap.ptr;

    memcpy(dst, &ptr, sizeof(ptr)); dst += 16;
    memcpy(dst, gGameDataBegin, d); dst += d;
    memcpy(dst, gGameBssBegin, b); dst += b;
    memcpy(dst, gPortArena, a);
}

void port_snapshot_restore(const char *src)
{
    unsigned long d = gGameDataEnd - gGameDataBegin, b = gGameBssEnd - gGameBssBegin, a;
    void *ptr;

    memcpy(&ptr, src, sizeof(ptr)); src += 16;
    a = (char*)ptr - gPortArena;
    // Memory above the restored pointer must read as it did the first time: zero, as StartMatch left it.
    if ((char*)gSYTaskmanGeneralHeap.ptr > (char*)ptr) memset(ptr, 0, (char*)gSYTaskmanGeneralHeap.ptr - (char*)ptr);
    memcpy(gGameDataBegin, src, d); src += d;
    memcpy(gGameBssBegin, src, b); src += b;
    memcpy(gPortArena, src, a);
    gSYTaskmanGeneralHeap.ptr = ptr;
}

extern void port_audio_fields_stash(void), port_audio_fields_unstash(void);

// FNV-1a over every byte of the sim, except the sound handles the audio thread's serials write through.
unsigned int port_snapshot_digest(void)
{
    unsigned int h = 2166136261u;
    const unsigned char *p;
    const char *ranges[3][2];
    int r;

    port_audio_fields_stash();
    ranges[0][0] = gGameDataBegin; ranges[0][1] = gGameDataEnd;
    ranges[1][0] = gGameBssBegin;  ranges[1][1] = gGameBssEnd;
    ranges[2][0] = gPortArena;     ranges[2][1] = (char*)gSYTaskmanGeneralHeap.ptr;
    for (r = 0; r < 3; r++)
        for (p = (const unsigned char*)ranges[r][0]; p < (const unsigned char*)ranges[r][1]; p++) { h ^= *p; h *= 16777619u; }
    port_audio_fields_unstash();
    return h;
}
