// Rollback netcode core. The relay's frame bundles are the only truth; everything newer is a prediction.
//   now  last frame simulated
//   C    last frame simulated with every real pad; nothing at or before it can change
//   top  highest frame ever simulated; a frame above top is a first-time frame
#include "port.h"
#include <PR/ultratypes.h>
#include <ft/fighter.h>
#include <sc/scmanager.h>
#include <sys/objdef.h>
#include <sys/objman.h>
#include <sys/malloc.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EXPORT
#endif
void *memcpy(void *, const void *, unsigned long);
void *memset(void *, int, unsigned long);
void *malloc(unsigned long);

extern unsigned long port_snapshot_size(void), port_snapshot_static_size(void);
extern void port_snapshot_save(char *), port_snapshot_restore(const char *);
extern unsigned int port_snapshot_digest(void);
extern uint32_t port_hash_sim(void);
extern void port_set_sim_frame(uint32_t f);
extern void port_audio_tick(void);
extern void port_bgm_reconcile(void);
extern void ftComputerSetupAll(GObj *);
extern void port_draw_only(void);

#define NET_SLOTS 8      // the wire and rings carry 8 slots; the game reads as many as it has fighters
#define R 512            // frames of pad history
#define MAX_W 30
#define MASK (R - 1)
#define SNAPS (MAX_W + 2)
#define SNAP_BYTES (5 * 1024 * 1024) // well above the largest 8-fighter match; overflow is a fatal check, not corruption

typedef struct { uint16_t b; int8_t x, y; } Pad;

static Pad sTruth[NET_SLOTS][R], sApplied[NET_SLOTS][R], sLocal[NET_SLOTS][R];
static Pad sLastTruth[NET_SLOTS];
static Pad sCur[NET_SLOTS];
static uint8_t sReplaced[R];
static int32_t sNow, sC, sTop, sTruthTop, sBase;
static int sW = 12, sOn, sLocalMask, sLockstep, sEndFrame = -1, sEnded;
static char *sSnaps[SNAPS];
static int32_t sCpuAt[NET_SLOTS];
static uint32_t sHash[R];
static int32_t sHashOutNext;
static struct { uint32_t rollbacks, resim_frames, max_depth, replaced, stalls, ms_max; } sStats;
static uint32_t sDepthHist[MAX_W + 2];
int gPortResim;

static inline char *snap_for(int32_t f) { return sSnaps[(f - sBase + SNAPS * 4) % SNAPS]; }

EXPORT void NetInit(int window, int localMask, int lockstep)
{
    int i;
    if (window > MAX_W) window = MAX_W;
    sW = window; sLocalMask = localMask; sLockstep = lockstep; sOn = 1;
    if (!sSnaps[0]) for (i = 0; i < SNAPS; i++) { sSnaps[i] = malloc(SNAP_BYTES); if (!sSnaps[i]) port_fatal("snapshot ring allocation"); }
    memset(sTruth, 0, sizeof sTruth); memset(sApplied, 0, sizeof sApplied); memset(sLocal, 0, sizeof sLocal);
    memset(sLastTruth, 0, sizeof sLastTruth); memset(&sStats, 0, sizeof sStats); memset(sDepthHist, 0, sizeof(sDepthHist)); memset(sCur, 0, sizeof sCur);
    sNow = sC = sTop = sTruthTop = -1; sEndFrame = -1; sEnded = 0; sHashOutNext = 0;
    for (i = 0; i < NET_SLOTS; i++) sCpuAt[i] = -1;
}

EXPORT void NetOff(void) { sOn = 0; }
int port_net_on(void) { return sOn; }

// Called when a scene has been set up: this state is what frame `first` starts from.
void port_net_scene_ready(int32_t first)
{
    if (!sOn) return;
    sBase = first - 1;
    sNow = sC = first - 1;
    if (sTop < sNow) sTop = sNow;
    if (port_snapshot_size() > SNAP_BYTES) port_fatal("snapshot larger than its ring slot");
    port_snapshot_save(snap_for(sNow));
    sEndFrame = -1;
}

// A pad the local player holds now, for the next first-time frame.
EXPORT void NetSetLocal(int slot, int b, int x, int y)
{
    sCur[slot].b = (uint16_t)b; sCur[slot].x = (int8_t)x; sCur[slot].y = (int8_t)y;
}

// The last `n` local pads ending at frame `f`, packed for the wire. Returns how many.
EXPORT int NetLocalPacked(int slot, int32_t f, int n, uint8_t *out)
{
    int i, c = 0;
    for (i = n - 1; i >= 0; i--) {
        int32_t g = f - i;
        if (g < 0) continue;
        { const Pad *p = &sLocal[slot][g & MASK]; out[c * 4] = p->b >> 8; out[c * 4 + 1] = p->b & 0xFF; out[c * 4 + 2] = (uint8_t)p->x; out[c * 4 + 3] = (uint8_t)p->y; }
        c++;
    }
    return c;
}

// One frame bundle from the relay: pads is n x {u16 big-endian, i8, i8}.
EXPORT void NetBundle(int32_t frame, int n, int replaced, const uint8_t *pads)
{
    int i;
    if (frame != sTruthTop + 1) return; // one socket, in order; anything else is a stale duplicate
    for (i = 0; i < n && i < NET_SLOTS; i++) {
        Pad *t = &sTruth[i][frame & MASK];
        t->b = (pads[i * 4] << 8) | pads[i * 4 + 1]; t->x = (int8_t)pads[i * 4 + 2]; t->y = (int8_t)pads[i * 4 + 3];
        sLastTruth[i] = *t;
    }
    sReplaced[frame & MASK] = (uint8_t)replaced;
    if (replaced) sStats.replaced++;
    sTruthTop = frame;
}

// A racer dropped: its fighter turns CPU from frame + 1 on every page.
EXPORT void NetCpu(int slot, int32_t frame) { sCpuAt[slot] = frame + 1; }

// The pad for a frame: truth if it has arrived, the player's own pad if local, otherwise the last real pad.
static Pad pad_for(int slot, int32_t f)
{
    if (f <= sTruthTop) return sTruth[slot][f & MASK];
    if (sLocalMask & (1 << slot)) return sLocal[slot][f & MASK];
    return sLastTruth[slot];
}

void port_pad_net(int slot, uint32_t f, u16 *b, s8 *x, s8 *y)
{
    const Pad *p = &sApplied[slot][f & MASK];
    *b = p->b; *x = p->x; *y = p->y;
}

static void apply_cpu(int32_t f)
{
    int s;
    GObj *g;
    for (s = 0; s < GMCOMMON_PLAYERS_MAX; s++) {
        if (sCpuAt[s] != f) continue;
        for (g = gGCCommonLinks[nGCCommonLinkIDFighter]; g != NULL; g = g->link_next) {
            FTStruct *fp = ftGetStruct(g);
            if (fp->player == s && fp->pkind == nFTPlayerKindMan) { fp->pkind = nFTPlayerKindCom; ftComputerSetupAll(g); }
        }
    }
}

// Simulate frame f. Returns 1 while the scene runs.
static int step(int32_t f, int resim, int draw)
{
    int s, r;
    extern void port_sfx_frame_begin(int32_t), port_sfx_frame_end(int32_t, int);
    port_sfx_frame_begin(f);
    if (f > sTop) for (s = 0; s < NET_SLOTS; s++) if (sLocalMask & (1 << s)) sLocal[s][f & MASK] = sCur[s];
    for (s = 0; s < NET_SLOTS; s++) sApplied[s][f & MASK] = pad_for(s, f);
    apply_cpu(f);
    gPortResim = resim;
    port_set_sim_frame(f);
    r = port_step(draw);
    port_sfx_frame_end(f, resim);
    gPortResim = 0;
    if (r) {
        if (port_snapshot_size() > SNAP_BYTES) port_fatal("snapshot larger than its ring slot");
        port_snapshot_save(snap_for(f));
        if (f % 15 == 0) sHash[f & MASK] = port_hash_sim();
        sNow = f;
        if (f > sTop) sTop = f;
    }
    return r;
}

// The match may end only on a final frame: a finish cannot be taken back.
int port_net_can_end(void)
{
    if (!sOn) return 1;
    {
        int32_t f = (int32_t)port_sim_frame();
        if (f <= sC + 1 && f <= sTruthTop) return 1;
        sEndFrame = f;
        return 0;
    }
}

extern int port_force_scene_end(uint32_t end_frame);

static void rollback_to(int32_t r, int anyFirstTime)
{
    int32_t target = sNow, g;
    uint32_t depth = (uint32_t)(target - r + 1);
    sStats.rollbacks++; sStats.resim_frames += depth;
    sDepthHist[depth < MAX_W + 2 ? depth : MAX_W + 1]++;
    if (depth > sStats.max_depth) sStats.max_depth = depth;
    port_snapshot_restore(snap_for(r - 1));
    sNow = r - 1;
    if (sEndFrame >= r) sEndFrame = -1;
    for (g = r; g <= target; g++) if (!step(g, 1, 0)) break;
    port_bgm_reconcile();
}

// One browser tick. `steps` is how many new frames pacing asks for. Returns frames run; draws the newest.
EXPORT int NetTick(int steps, int draw)
{
    int ran = 0, did_roll = 0, i;

    if (!sOn || sEnded) return 0;

    // compare what arrived against what we simulated
    while (sC < sTruthTop && sC < sNow) {
        int32_t f = sC + 1;
        int s, bad = 0;
        for (s = 0; s < NET_SLOTS; s++) {
            const Pad *a = &sApplied[s][f & MASK], *t = &sTruth[s][f & MASK];
            if (a->b != t->b || a->x != t->x || a->y != t->y) { bad = 1; break; }
        }
        for (s = 0; s < GMCOMMON_PLAYERS_MAX; s++) if (sCpuAt[s] >= 0 && sCpuAt[s] <= sNow && sCpuAt[s] == f) bad = 1;
        if (bad) { rollback_to(f, 0); did_roll = 1; if (sEnded) return 0; }
        sC = f;
    }
    // confirmed hashes go out
    // a held finish is final once its frame is
    if (sEndFrame >= 0 && sC >= sEndFrame) {
        int32_t e = sEndFrame;
        sEndFrame = -1;
        if (!port_force_scene_end(e)) { sEnded = 1; return 0; }
    }
    for (i = 0; i < steps; i++) {
        int32_t f = sNow + 1;
        if (sEndFrame >= 0) break;
        if (sLockstep ? (f > sTruthTop) : (f > sC + sW)) { sStats.stalls++; break; }
        if (!step(f, f <= sTop, (draw && i == steps - 1) ? 1 : 0)) { sEnded = 1; break; }
        ran++;
    }
    if (did_roll && ran == 0 && draw) port_draw_only();
    port_audio_tick();
    return ran;
}

EXPORT int NetPollHash(int32_t *frame, uint32_t *hash)
{
    while (sHashOutNext <= sC) {
        int32_t f = sHashOutNext;
        sHashOutNext += 15;
        if (f < sBase + 1) continue;
        *frame = f; *hash = sHash[f & MASK];
        return 1;
    }
    return 0;
}

EXPORT int32_t NetNow(void) { return sNow; }
EXPORT int32_t NetConfirmed(void) { return sC; }
EXPORT int32_t NetTop(void) { return sTop; }
EXPORT int32_t NetTruthTop(void) { return sTruthTop; }
EXPORT int NetEnded(void) { return sEnded; }
EXPORT uint32_t NetStat(int i) { return ((uint32_t *)&sStats)[i]; }
EXPORT uint32_t NetDepthHist(int depth) { return (depth >= 0 && depth < MAX_W + 2) ? sDepthHist[depth] : 0; }
EXPORT unsigned long NetSnapshotSize(void) { return port_snapshot_size(); }

// ---- offline proof: restore k frames back every K frames, re-simulate, compare every byte ----
static uint32_t sDigest[R];
static uint32_t sRbMismatch, sRbChecks;
EXPORT uint32_t RbMismatches(void) { return sRbMismatch; }
EXPORT uint32_t RbChecks(void) { return sRbChecks; }

static char *sRbFirst[MAX_W + 2];
extern char gGameDataBegin[], gGameDataEnd[], gGameBssBegin[], gGameBssEnd[], gPortArena[];
static void rb_diff(const char *a, const char *b, int g, int f)
{

    unsigned long d = gGameDataEnd - gGameDataBegin, bs = gGameBssEnd - gGameBssBegin, n = port_snapshot_size(), i;
    int shown = 0;
    port_log("digest mismatch frame %d (rollback at %d) ptr first %p again %p", g, f, *(void**)a, *(void**)b);
    for (i = 16; i < n && shown < 14; i += 4) {
        if (*(const unsigned *)(a + i) != *(const unsigned *)(b + i)) {
            unsigned long o = i - 16; char *addr;
            if (o < d) addr = gGameDataBegin + o; else if (o < d + bs) addr = gGameBssBegin + (o - d); else addr = gPortArena + (o - d - bs);
            port_log(" %p first %08x again %08x", addr, *(const unsigned *)(a + i), *(const unsigned *)(b + i));
            shown++;
        }
    }
}

EXPORT void RbInit(int window) { int i; for (i = 0; i < MAX_W + 2; i++) if (!sRbFirst[i]) sRbFirst[i] = malloc(SNAP_BYTES); NetInit(window, 0xFF, 0); port_net_scene_ready((int32_t)port_sim_frame()); }

// Frame f with pads from the script: save, digest; every K frames go back k and verify.
EXPORT int RbStep(int K, int k, int corrupt)
{
    int32_t f = sNow + 1, g;
    int s;
    for (s = 0; s < NET_SLOTS; s++) { sLastTruth[s] = sTruth[s][0]; }
    if (!step(f, 0, 0)) return 0;
    sDigest[f & MASK] = port_snapshot_digest();
    if (K && f > k && f % K == 0 && f - k > sBase) {
        int32_t r = f - k + 1, target = f;
        port_snapshot_restore(snap_for(r - 1));
        if (corrupt && f == corrupt) { extern s32 syUtilsRandSeed(void); extern void syUtilsSetRandomSeed(s32); syUtilsSetRandomSeed(syUtilsRandSeed() ^ 1); port_log("control: seed flipped at %d", f); }
        for (g = r; g <= target; g++) memcpy(sRbFirst[g - r], snap_for(g), SNAP_BYTES);
        sNow = r - 1;
        for (g = r; g <= target; g++) {
            int alive = step(g, 1, 0);
            sRbChecks++;
            if (port_snapshot_digest() != sDigest[g & MASK]) { sRbMismatch++; if (sRbMismatch < 3) rb_diff(sRbFirst[g - r], snap_for(g), g, f); }
            if (!alive) return 0;
        }
    }
    return 1;
}

// ---- the budget of plan section 3.8, in ms: save, restore, one re-simulated frame ----
extern double emscripten_get_now(void);
EXPORT void RbBudget(double *out)
{
    static char *buf;
    double t0, t1;
    int i, n = 200;
    int32_t f = sNow;
    if (!buf) buf = malloc(SNAP_BYTES);
    t0 = emscripten_get_now(); for (i = 0; i < n; i++) port_snapshot_save(buf); t1 = emscripten_get_now(); out[0] = (t1 - t0) / n;
    t0 = emscripten_get_now(); for (i = 0; i < n; i++) port_snapshot_restore(buf); t1 = emscripten_get_now(); out[1] = (t1 - t0) / n;
    // worst case rollback from here: restore, then 12 frames with no draw and no audio, then put the frame back
    port_snapshot_save(buf);
    t0 = emscripten_get_now();
    for (i = 0; i < 50; i++) {
        int j;
        port_snapshot_restore(buf);
        for (j = 1; j <= 12; j++) { int s; for (s = 0; s < NET_SLOTS; s++) sApplied[s][(f + j) & MASK] = sApplied[s][f & MASK]; gPortResim = 1; port_set_sim_frame(f + j); port_step(0); gPortResim = 0; }
    }
    t1 = emscripten_get_now();
    out[2] = (t1 - t0) / 50; // one 12-frame rollback including its restore
    port_snapshot_restore(buf);
    port_set_sim_frame(f + 1);
}
