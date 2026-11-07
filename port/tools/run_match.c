// Headless match runner under node. Prints "frame hash" every 15 frames.
//   node ssb64_run.js <data> stage seed framesMax mode[,mode..] [fighters] [matches]
// mode per slot: a scripted A, h scripted human-like, c CPU, n none.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
extern void PortBoot(const uint8_t *, uint32_t);
extern void StartMatch(const int32_t*, const int32_t*, const int32_t*, const int32_t*, const int32_t*, const int32_t*,
                       int,int,int,int,int,int,int,int,uint32_t,int);
extern void SetScriptedSlot(int, int);
extern short *AudioRing(void); extern uint32_t AudioHead(void), AudioRingFrames(void);
extern void SetPoison(int), SetFlipFrame(int), SetAudioOff(int);
extern int Seed(void);
extern void port_dump_state_out(void);
extern int StepFrame(int);
extern void RbInit(int); extern int RbStep(int,int,int); extern unsigned RbMismatches(void), RbChecks(void); extern unsigned long NetSnapshotSize(void); extern void NetOff(void);
extern uint32_t SimFrame(void), Hash(void);
extern int GameStatus(void);
extern unsigned long port_snapshot_static_size(void), port_snapshot_arena_used(void);

int main(int argc, char **argv)
{
    const char *dp = argv[1];
    int stage = atoi(argv[2]), seed = atoi(argv[3]), fmax = atoi(argv[4]);
    const char *modes = argc > 5 ? argv[5] : "aaaa";
    const char *fk = argc > 6 ? argv[6] : "0,1,2,3";
    int matches = argc > 7 ? atoi(argv[7]) : 1;
    int draw = argc > 8 ? atoi(argv[8]) : 0;
    int dfrom = getenv("DFROM") ? atoi(getenv("DFROM")) : 0, dto = getenv("DTO") ? atoi(getenv("DTO")) : 0;
    FILE *f = fopen(dp, "rb");
    if (!f) { perror(dp); return 2; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *img = malloc(n); fread(img, 1, n, f); fclose(f);
    PortBoot(img, n);
    if (getenv("NOAUDIO")) SetAudioOff(1);
    if (getenv("POISON")) SetPoison(atoi(getenv("POISON")));
    if (getenv("FLIP")) SetFlipFrame(atoi(getenv("FLIP")));
    int32_t kinds[8], fks[8] = {0,1,2,3,4,5,6,7}, zero[8] = {0}, lev[8] = {9,9,9,9,9,9,9,9};
    sscanf(fk, "%d,%d,%d,%d,%d,%d,%d,%d", &fks[0], &fks[1], &fks[2], &fks[3], &fks[4], &fks[5], &fks[6], &fks[7]);
    for (int m = 0; m < matches; m++) {
        int modes_n = (int)strlen(modes);
        for (int i = 0; i < 8; i++) {
            char c = i < modes_n ? modes[i] : 'n';
            kinds[i] = c == 'c' ? 1 : c == 'n' ? 2 : 0;
            SetScriptedSlot(i, c == 'a' ? 1 : c == 'h' ? 2 : 0);
        }
        { int32_t teams[8]; int tb = getenv("TEAM") != NULL; for (int i = 0; i < 8; i++) teams[i] = tb ? i % 3 : i;
          StartMatch(kinds, fks, zero, lev, teams, zero, stage, 1 /*time*/, 3, 3, 100, tb, tb, 3, 0xFFFFFFFFu, seed + 0); }
        printf("match %d\n", m); fflush(stdout);
        if (getenv("RB")) {
            /* offline rollback proof: every K frames restore k back, re-simulate, compare every byte */
            int K = atoi(getenv("RB")), k = getenv("RBK") ? atoi(getenv("RBK")) : 12, corrupt = getenv("RBBREAK") ? atoi(getenv("RBBREAK")) : 0;
            unsigned long maxs = 0;
            RbInit(k + 2);
            while (RbStep(K, k, corrupt)) { unsigned long z = NetSnapshotSize(); if (z > maxs) maxs = z; if ((int)SimFrame() >= fmax) break; }
            if (getenv("RBTIME")) { double o[3]; extern void RbBudget(double*); RbBudget(o); printf("budget save %.3f ms restore %.3f ms rollback12 %.3f ms\n", o[0], o[1], o[2]); }
            printf("rb frames %u checks %u mismatches %u snapshot_max %lu\n", SimFrame(), RbChecks(), RbMismatches(), maxs);
            { extern uint32_t port_sfx_started(void); printf("sounds %u\n", port_sfx_started()); }
            NetOff();
            continue;
        }
        while (StepFrame(draw)) {
            uint32_t fr = SimFrame();
            if (dfrom && (int)fr >= dfrom && (int)fr <= dto) { printf("F %u %08x seed %08x\n", fr, Hash(), Seed()); port_dump_state_out(); }
            if (fr % 15 == 0) { printf("%u %08x\n", fr, Hash()); fflush(stdout); }
            if (getenv("SPAWNS") && (int)fr == atoi(getenv("SPAWNS"))) { extern void port_dump_fighters(void); port_dump_fighters(); }
            if (getenv("RNGDUMP") && (int)fr == atoi(getenv("RNGDUMP"))) { extern uint32_t RngTraceCount(void); extern void *RngTrace(void); struct T { const char *f; int32_t l, s, fr; } *t = RngTrace(); uint32_t n = RngTraceCount(); for (uint32_t i = n > 4096 ? n - 4096 : 0; i < n; i++) { struct T *e = &t[i % 4096]; if (e->fr == (int32_t)fr - 1) printf("rng frame %d %s:%d seed %08x\n", e->fr, e->f, e->l, e->s); } }
            if (getenv("GFXTRACE") && (int)fr == atoi(getenv("GFXTRACE"))) { extern void GfxTrace(int); GfxTrace(1); }
            if (getenv("CAMLOG") && fr % 60 == 0) { extern void port_dump_camera(void); printf("f %u ", fr); port_dump_camera(); }
            if ((int)fr >= fmax) break;
        }
        if (getenv("PROF")) { extern double PortProf(int); double n = PortProf(8 + 13), d = PortProf(8 + 0);
            printf("prof step %.3f ms/frame audio %.3f ms/frame frames %.0f drawn %.0f update %.3f ms/frame dlbuild %.3f ms/drawn gfxrun %.3f ms/drawn texdecode %.3f ms total resim %.0f frames %.3f ms/frame | per drawn frame: ops %.0f tris %.0f rects %.0f draws %.0f vtx %.0f texlookups %.0f texdecodes %.0f\n",
                PortProf(6) / n, PortProf(7) / n, n, d, PortProf(0) / n, d ? PortProf(1) / d : 0, d ? PortProf(2) / d : 0, PortProf(4), PortProf(8 + 12), PortProf(8 + 12) ? PortProf(5) / PortProf(8 + 12) : 0,
                d ? PortProf(9) / d : 0, d ? PortProf(10) / d : 0, d ? PortProf(11) / d : 0, d ? PortProf(12) / d : 0, d ? PortProf(13) / d : 0, d ? PortProf(15) / d : 0, PortProf(14));
            printf("prof caches: decoded textures live %.0f evicted %.0f; near-clipped triangles %.0f rejected %.0f far-cut %.0f; state splits combine %.0f blend %.0f colour %.0f texture %.0f depth %.0f view %.0f other %.0f\n", PortProf(8 + 17), PortProf(8 + 20), PortProf(8 + 22), PortProf(8 + 23), PortProf(8 + 24), PortProf(8 + 25), PortProf(8 + 26), PortProf(8 + 27), PortProf(8 + 28), PortProf(8 + 29), PortProf(8 + 30), PortProf(8 + 31)); }
        { extern uint32_t port_gfx_tris(void), port_gfx_rects(void); extern uint32_t dSYTaskmanFrameCount; if (getenv("DRAWSTATS")) printf("drawn frames %u tris %u rects %u\n", dSYTaskmanFrameCount, port_gfx_tris(), port_gfx_rects()); } { extern uint32_t port_sfx_started(void); if (getenv("SOUNDS")) printf("sounds %u\n", port_sfx_started()); } printf("end frame %u status %d static %lu arena %lu\n", SimFrame(), GameStatus(), port_snapshot_static_size(), port_snapshot_arena_used());
    }
    if (getenv("PCMOUT")) {
        FILE *o = fopen(getenv("PCMOUT"), "wb");
        uint32_t h = AudioHead(), n = AudioRingFrames();
        short *r = AudioRing();
        for (uint32_t i = h > n ? h - n : 0; i < h; i++) fwrite(r + (i & (n - 1)) * 2, 2, 2, o);
        fclose(o);
        printf("pcm frames total %u\n", h);
    }
    return 0;
}

// Report for the array-bounds check build (-fsanitize=array-bounds): every distinct place, once.
struct UbLoc { const char *file; unsigned line, col; };
__attribute__((weak)) void __ubsan_handle_out_of_bounds(struct UbLoc *loc, unsigned long idx)
{
    static struct UbLoc *seen[256]; static int n;
    for (int i = 0; i < n; i++) if (seen[i] == loc) return;
    if (n < 256) seen[n++] = loc;
    printf("BOUNDS %s:%u index %ld\n", loc->file, loc->line, (long)idx);
}
