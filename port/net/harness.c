// The hook layer between the game and the page. No network knowledge lives here.
#include "port.h"
#include <PR/ultratypes.h>
#include <ft/fighter.h>
#include <it/item.h>
#include <wp/weapon.h>
#include <sc/scmanager.h>
#include <sc/sctypes.h>
#include <if/ifdef.h>
#include <gm/gmcamera.h>
#include <sys/objdef.h>
#include <sys/objman.h>
#include <sys/utils.h>
#include <ef/effect.h>
#include <mp/map.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EXPORT
#endif
void *memset(void *, int, unsigned long);

extern void scVSBattleStartScene(void);
extern ub8 dIFCommonPlayerTeamColorIDs[];
extern void syUtilsSetRandomSeed(s32 seed);
extern s32 syUtilsRandSeed(void);

// ---- pad ring: ring[slot][frame & 63] = { buttons, x<<8 | y } ----
#define RING 64
static int16_t sRing[PORT_SLOTS][RING][2];
static int32_t sConfirmed[PORT_SLOTS];
static int16_t sLast[PORT_SLOTS][2];

EXPORT int16_t *PadRing(void) { return &sRing[0][0][0]; }
EXPORT int32_t *Confirmed(void) { return sConfirmed; }
EXPORT int32_t PadRingLen(void) { return RING; }

// ---- scripted pads: a pure function of frame and slot ----
enum { SCRIPT_OFF, SCRIPT_A, SCRIPT_HUMAN };
static int sScript[PORT_SLOTS];
EXPORT void SetScriptedSlot(int slot, int mode) { sScript[slot] = mode; }

static uint32_t mix(uint32_t a, uint32_t b) { uint32_t h = a * 2654435761u ^ (b + 0x9E3779B9u); h ^= h >> 15; h *= 2246822519u; h ^= h >> 13; return h; }

// N64 button bits
#define BA 0x8000
#define BB 0x4000
#define BZ 0x2000
#define BR 0x0010
#define BCU 0x0008

static int sFlipFrame;
static void script_pad(int slot, uint32_t f, u16 *b, s8 *x, s8 *y)
{
    if (sScript[slot] == SCRIPT_A)
    {
        uint32_t h = mix(f / 7, slot);
        static const s8 ext[5] = { -80, -40, 0, 40, 80 };
        *x = ext[h % 5];
        *y = ext[(h >> 8) % 5];
        *b = 0;
        if ((h >> 16) & 1) *b |= BA;
        if ((h >> 17) & 1) *b |= BB;
        if (((h >> 18) & 7) == 0) *b |= BZ;
        if (((h >> 21) & 7) == 0) *b |= BR;
        if (((h >> 24) & 3) == 0) *b |= BCU;
    }
    else
    {
        uint32_t h = mix(f / 60, slot + 100);
        uint32_t g = mix(f / 11, slot + 200);
        *x = (s8)((int)(h % 161) - 80);
        *y = (h >> 9) % 4 == 0 ? (s8)((int)((h >> 12) % 161) - 80) : 0;
        *b = 0;
        if ((g & 7) == 0) *b |= BA;
        if (((g >> 3) & 15) == 0) *b |= BB;
        if (((g >> 7) & 31) == 0) *b |= BZ;
        if (((g >> 12) & 15) == 0) *b |= BR;
    }
}

extern int port_net_on(void);
extern void port_pad_net(int slot, uint32_t f, u16 *b, s8 *x, s8 *y);

// A proof client's pad: the same scripted patterns, for a slot driven through the net path.
EXPORT uint32_t ScriptPadPacked(int mode, int slot, uint32_t f)
{
    u16 b; s8 x, y;
    int keep = sScript[slot];
    sScript[slot] = mode;
    script_pad(slot, f, &b, &x, &y);
    sScript[slot] = keep;
    return ((uint32_t)b << 16) | ((uint8_t)x << 8) | (uint8_t)y;
}

void port_pad_for_frame(int slot, u16 *buttons, s8 *x, s8 *y)
{
    uint32_t f = port_sim_frame();

    if (port_net_on() && sScript[slot] == SCRIPT_OFF) { port_pad_net(slot, f, buttons, x, y); return; }

    if (sScript[slot] != SCRIPT_OFF) { script_pad(slot, f, buttons, x, y); if (slot == 0 && (int)f == sFlipFrame - 1) *buttons ^= BA; return; }
    {
        int16_t *p = sRing[slot][f & (RING - 1)];
        if ((int32_t)f <= sConfirmed[slot]) { sLast[slot][0] = p[0]; sLast[slot][1] = p[1]; }
        *buttons = (u16)sLast[slot][0];
        *x = (s8)(sLast[slot][1] >> 8);
        *y = (s8)(sLast[slot][1] & 0xFF);
    }
}

// ---- events ----
#define EVENTS 256
static int32_t sEvents[EVENTS * 3 + 1];
void port_event(int kind, int arg)
{
    int n = sEvents[0]++ % EVENTS;
    sEvents[1 + n * 3] = (int32_t)port_sim_frame();
    sEvents[2 + n * 3] = kind;
    sEvents[3 + n * 3] = arg;
}
EXPORT int32_t *EventRing(void) { return sEvents; }

// ---- RNG call-site trace ----
#define TRACE 4096
static struct { const char *file; int32_t line; int32_t seed; int32_t frame; } sTrace[TRACE];
static uint32_t sTraceN;
void port_rng_site(const char *file, int line)
{
    uint32_t i = sTraceN++ % TRACE;
    sTrace[i].file = file; sTrace[i].line = line; sTrace[i].seed = syUtilsRandSeed(); sTrace[i].frame = (int32_t)port_sim_frame();
}
EXPORT uint32_t RngTraceCount(void) { return sTraceN; }
EXPORT void *RngTrace(void) { return sTrace; }

// ---- hash ----
static uint32_t sH;
static void h32(uint32_t v) { sH ^= v; sH *= 16777619u; }
static void hf(f32 v) { union { f32 f; uint32_t u; } c; c.f = v; h32(c.u); }
static void hv(const Vec3f *v) { hf(v->x); hf(v->y); hf(v->z); }

uint32_t port_hash_sim(void)
{
    GObj *g;
    int i;

    sH = 2166136261u;
    h32((uint32_t)syUtilsRandSeed());
    h32(port_sim_frame());
    if (gSCManagerBattleState != NULL)
    {
        h32(gSCManagerBattleState->game_status);
        h32(gSCManagerBattleState->time_remain);
        h32(gSCManagerBattleState->time_passed);
        for (i = 0; i < GMCOMMON_PLAYERS_MAX; i++)
        {
            SCPlayerData *p = &gSCManagerBattleState->players[i];
            h32((uint32_t)p->stock_count); h32((uint32_t)p->falls); h32((uint32_t)p->score);
            h32((uint32_t)p->total_damage_all); h32(p->stale_id);
        }
    }
    for (g = gGCCommonLinks[nGCCommonLinkIDFighter]; g != NULL; g = g->link_next)
    {
        FTStruct *fp = ftGetStruct(g);
        h32(fp->player); h32(fp->fkind); h32(fp->status_id); h32(fp->motion_id); h32(fp->status_total_tics);
        h32(fp->percent_damage); h32(fp->shield_health); h32(fp->hitlag_tics); h32(fp->lr); h32(fp->ga);
        h32(fp->jumps_used); h32((uint32_t)fp->stock_count);
        hv(&fp->physics.vel_air); hv(&fp->physics.vel_damage_air); hv(&fp->physics.vel_ground); hf(fp->physics.vel_damage_ground);
        hv(&fp->coll_data.pos_prev);
        hv(&DObjGetStruct(g)->translate.vec.f);
        hv(&DObjGetStruct(g)->rotate.vec.f);
        h32(fp->input.pl.button_hold); h32(fp->input.pl.stick_range.x); h32(fp->input.pl.stick_range.y);
    }
    for (g = gGCCommonLinks[nGCCommonLinkIDItem]; g != NULL; g = g->link_next)
    {
        ITStruct *ip = itGetStruct(g);
        h32(ip->kind); h32(ip->player); h32(ip->lr); h32(ip->ga); h32(ip->percent_damage);
        hf(ip->physics.vel_air.x); hf(ip->physics.vel_air.y); hf(ip->physics.vel_air.z);
        hv(&DObjGetStruct(g)->translate.vec.f);
    }
    for (g = gGCCommonLinks[nGCCommonLinkIDWeapon]; g != NULL; g = g->link_next)
    {
        WPStruct *wp = wpGetStruct(g);
        h32(wp->kind); h32(wp->player); h32(wp->lr); h32(wp->ga);
        hv(&wp->physics.vel_air);
        hv(&DObjGetStruct(g)->translate.vec.f);
    }
    {
        int n = 0;
        for (g = gGCCommonLinks[nGCCommonLinkIDEffect]; g != NULL; g = g->link_next) n++;
        h32(n);
    }
    h32(gMPCollisionUpdateTic);
    return sH;
}

// ---- state dump for a fatal error ----
void port_dump_state(void)
{
    GObj *g;
    port_log("frame %u game_status %d time_remain %d", port_sim_frame(), gSCManagerBattleState ? gSCManagerBattleState->game_status : -1, gSCManagerBattleState ? gSCManagerBattleState->time_remain : -1);
    for (g = gGCCommonLinks[nGCCommonLinkIDFighter]; g != NULL; g = g->link_next)
    {
        FTStruct *fp = ftGetStruct(g);
        port_log(" p%d fkind %d status %d motion %d tics %d pct %d stocks %d pos %f %f scale %f", fp->player, fp->fkind, fp->status_id, fp->motion_id, fp->status_total_tics, fp->percent_damage, fp->stock_count, DObjGetStruct(g)->translate.vec.f.x, DObjGetStruct(g)->translate.vec.f.y, DObjGetStruct(g)->scale.vec.f.x);
    }
}

EXPORT int Seed(void) { return syUtilsRandSeed(); }
void port_dump_state_out(void)
{
    GObj *g;
    for (g = gGCCommonLinks[nGCCommonLinkIDFighter]; g != NULL; g = g->link_next)
    {
        FTStruct *fp = ftGetStruct(g);
        port_log(" p%d st %d mot %d t %d pct %d shield %d hit %d ga %d pos %08x %08x vel %08x %08x", fp->player, fp->status_id, fp->motion_id, fp->status_total_tics, fp->percent_damage, fp->shield_health, fp->hitlag_tics, fp->ga,
          *(unsigned*)&DObjGetStruct(g)->translate.vec.f.x, *(unsigned*)&DObjGetStruct(g)->translate.vec.f.y, *(unsigned*)&fp->physics.vel_air.x, *(unsigned*)&fp->physics.vel_air.y);
    }
    { int n = 0; for (g = gGCCommonLinks[nGCCommonLinkIDItem]; g != NULL; g = g->link_next) { ITStruct *ip = itGetStruct(g); port_log(" item kind %d pos %08x", ip->kind, *(unsigned*)&DObjGetStruct(g)->translate.vec.f.x); n++; } }
    for (g = gGCCommonLinks[nGCCommonLinkIDWeapon]; g != NULL; g = g->link_next) { WPStruct *wp = wpGetStruct(g); port_log(" wp kind %d pos %08x", wp->kind, *(unsigned*)&DObjGetStruct(g)->translate.vec.f.x); }
}

// ---- sound handles live in sim structs but their serials follow the audio thread ----
extern FTStruct *sFTManagerStructsAllocBuf;
extern WPStruct *sWPManagerStructsAllocBuf;
static struct { void *a, *b, *c; u16 x, y, z; } sStashFt[GMCOMMON_PLAYERS_MAX];
static struct { void *a; u16 x; } sStashWp[WEAPON_ALLOC_MAX];
extern void *sFTPublicCommonALSound, *sFTPublicCallALSound, *sFTPublicDefeatedALSound;
extern u16 sFTPublicCommonOrder, sFTPublicCallOrder, sFTPublicDefeatedCurrentOrder;
static void *sStashPublic[3]; static u16 sStashPublicOrder[3];
void port_audio_fields_stash(void)
{
    int i;
    sStashPublic[0] = sFTPublicCommonALSound; sStashPublic[1] = sFTPublicCallALSound; sStashPublic[2] = sFTPublicDefeatedALSound;
    sStashPublicOrder[0] = sFTPublicCommonOrder; sStashPublicOrder[1] = sFTPublicCallOrder; sStashPublicOrder[2] = sFTPublicDefeatedCurrentOrder;
    sFTPublicCommonALSound = sFTPublicCallALSound = sFTPublicDefeatedALSound = NULL; sFTPublicCommonOrder = sFTPublicCallOrder = sFTPublicDefeatedCurrentOrder = 0;
    for (i = 0; i < GMCOMMON_PLAYERS_MAX; i++) {
        FTStruct *f = &sFTManagerStructsAllocBuf[i];
        sStashFt[i].a = f->p_sfx; sStashFt[i].b = f->p_voice; sStashFt[i].c = f->p_loop_sfx;
        sStashFt[i].x = f->sfx_id; sStashFt[i].y = f->voice_id; sStashFt[i].z = f->loop_sfx_id;
        f->p_sfx = f->p_voice = f->p_loop_sfx = NULL; f->sfx_id = f->voice_id = f->loop_sfx_id = 0;
    }
    for (i = 0; i < WEAPON_ALLOC_MAX; i++) {
        WPStruct *w = &sWPManagerStructsAllocBuf[i];
        sStashWp[i].a = w->p_sfx; sStashWp[i].x = w->sfx_id; w->p_sfx = NULL; w->sfx_id = 0;
    }
}
void port_audio_fields_unstash(void)
{
    int i;
    sFTPublicCommonALSound = sStashPublic[0]; sFTPublicCallALSound = sStashPublic[1]; sFTPublicDefeatedALSound = sStashPublic[2];
    sFTPublicCommonOrder = sStashPublicOrder[0]; sFTPublicCallOrder = sStashPublicOrder[1]; sFTPublicDefeatedCurrentOrder = sStashPublicOrder[2];
    for (i = 0; i < GMCOMMON_PLAYERS_MAX; i++) {
        FTStruct *f = &sFTManagerStructsAllocBuf[i];
        f->p_sfx = sStashFt[i].a; f->p_voice = sStashFt[i].b; f->p_loop_sfx = sStashFt[i].c;
        f->sfx_id = sStashFt[i].x; f->voice_id = sStashFt[i].y; f->loop_sfx_id = sStashFt[i].z;
    }
    for (i = 0; i < WEAPON_ALLOC_MAX; i++) { WPStruct *w = &sWPManagerStructsAllocBuf[i]; w->p_sfx = sStashWp[i].a; w->sfx_id = sStashWp[i].x; }
}

// ---- match setup ----
static int sMatchOn;
// For proofs: which item kinds have been on the stage this match (bit = 1 << ITKind, kinds 0 to 31).
// Outside the sim: read only, never part of a snapshot or a hash.
static uint32_t sItemsSeen;
static int sPoison = 0;
EXPORT void SetPoison(int v) { sPoison = v; }
EXPORT void SetFlipFrame(int f) { sFlipFrame = f; }

// Slot arrays are 4 entries. kinds: 0 human, 1 CPU, 2 none.
EXPORT void StartMatch(const int32_t *kinds, const int32_t *fkinds, const int32_t *costumes, const int32_t *levels,
                       const int32_t *teams, const int32_t *handicaps, int stage, int rules, int time_limit, int stocks,
                       int damage_ratio, int team_battle, int team_attack, int item_rate, uint32_t item_toggles, int seed)
{
    SCBattleState *st;
    int i;

    port_snapshot_boot_restore();
    { extern char gPortArena[]; extern unsigned long gPortArenaSize; memset(gPortArena, sPoison, gPortArenaSize); }
    gSCManagerBackupData = dSCManagerDefaultBackupData;
    gSCManagerBackupData.error_flags = 0;
    gSCManagerBackupData.boot = 1;
    gSCManagerBackupData.unlock_mask = ~0;
    gSCManagerBackupData.fighter_mask = ~0;

    memset(sRing, 0, sizeof(sRing));
    memset(sLast, 0, sizeof(sLast));
    for (i = 0; i < PORT_SLOTS; i++) sConfirmed[i] = -1;

    st = &gSCManagerTransferBattleState;
    st->time_limit = time_limit;
    st->stocks = stocks;
    st->is_team_battle = team_battle;
    st->game_rules = rules;
    st->damage_ratio = damage_ratio;
    st->is_team_attack = team_attack;
    st->handicap = 0;
    st->item_toggles = item_toggles;
    st->item_appearance_rate = item_rate;
    st->is_show_score = TRUE;
    st->is_not_teamshadows = FALSE;
    st->pl_count = st->cp_count = 0;

    for (i = 0; i < GMCOMMON_PLAYERS_MAX; i++)
    {
        SCPlayerData *p = &st->players[i];
        int k = kinds[i];
        int team = team_battle ? (teams[i] % 3) : teams[i]; // 3 teams; default per-slot numbers run to 7

        p->pkind = (k == 0) ? nFTPlayerKindMan : (k == 1) ? nFTPlayerKindCom : nFTPlayerKindNot;
        p->fkind = (k == 2) ? nFTKindNull : fkinds[i];
        p->team = team;
        p->player = team_battle ? team : i;
        p->shade = 0;
        p->is_single_stockicon = (rules & SCBATTLE_GAMERULE_TIME) ? TRUE : FALSE;
        p->level = levels[i] ? levels[i] : 1;
        p->handicap = handicaps[i] ? handicaps[i] : 9;
        p->is_spgame_enemy = FALSE;
        if (k != 2) p->costume = team_battle ? ftParamGetCostumeTeamID(fkinds[i], team) : ftParamGetCostumeCommonID(fkinds[i], costumes[i]);
        if (k == 0) { p->color = team_battle ? dIFCommonPlayerTeamColorIDs[team] : i; st->pl_count++; }
        else if (!team_battle) p->color = GMCOMMON_PLAYERS_MAX;
        else p->color = dIFCommonPlayerTeamColorIDs[team];
        if (k == 1) st->cp_count++;
        p->tag = (k == 0) ? i : nIFPlayerTagKindCP;
    }
    gSCManagerSceneData.gkind = stage;
    gSCManagerSceneData.is_reset = FALSE;
    gSCManagerSceneData.scene_prev = gSCManagerSceneData.scene_curr;
    gSCManagerSceneData.scene_curr = nSCKindVSBattle;

    syUtilsSetRandomSeed(seed);
    sMatchOn = 1;
    sItemsSeen = 0;
    port_event(1, seed);
    port_match_begin();
}

EXPORT void Park(void) { port_match_park(); sMatchOn = 0; port_event(12, 0); }

EXPORT uint32_t PortItemsSeen(void) { return sItemsSeen; }
// For proofs: what StartMatch handed the sim. 0 stage, 1 item mask, 2 a slot's FTKind, 3 a slot's player kind,
// 4 frames to the next random item, 5 items on the stage now, 6 the stage music playing.
EXPORT int PortMatchInfo(int what, int slot)
{
    if (gSCManagerBattleState == NULL) return -1;
    switch (what) {
    case 0: return gSCManagerSceneData.gkind;
    case 1: return (int)gSCManagerBattleState->item_toggles;
    case 2: return gSCManagerBattleState->players[slot].fkind;
    case 3: return gSCManagerBattleState->players[slot].pkind;
    case 4: return (int)gITManagerAppearActor.spawn_wait;
    case 6: return (int)gMPCollisionBGMCurrent;
    case 5: { int n = 0; GObj *g; for (g = gGCCommonLinks[nGCCommonLinkIDItem]; g != NULL; g = g->link_next) n++; return n; }
    default: return -1;
    }
}

void port_net_frame_begin(void) {}
void port_net_frame_end(void)
{
    GObj *g;
    static int reported;
    for (g = gGCCommonLinks[nGCCommonLinkIDItem]; g != NULL; g = g->link_next)
    {
        s32 k = itGetStruct(g)->kind;
        if (k >= 0 && k < 32) sItemsSeen |= 1u << k;
    }
    if (reported) return;
    for (g = gGCCommonLinks[nGCCommonLinkIDFighter]; g != NULL; g = g->link_next)
    {
        f32 x = DObjGetStruct(g)->translate.vec.f.x;
        if (x != x) { reported = 1; port_log("first NaN at frame %u", port_sim_frame()); port_dump_state(); return; }
    }
}

extern void port_audio_tick(void), port_set_audio_off(int);
EXPORT int StepFrame(int draw) { int r = port_step(draw); port_audio_tick(); return r; }
EXPORT void SetAudioOff(int v) { port_set_audio_off(v); }
EXPORT uint32_t SimFrame(void) { return port_sim_frame(); }
EXPORT uint32_t Hash(void) { return port_hash_sim(); }
EXPORT int GameStatus(void) { return gSCManagerBattleState ? gSCManagerBattleState->game_status : -1; }
EXPORT void PortBoot(const uint8_t *image, uint32_t size) { port_rom_set(image, size); port_boot(); }

// Where everyone stands, for the spawn proof.
EXPORT void port_dump_fighters(void)
{
    extern int printf(const char *, ...);
    int i;
    for (i = 0; i < GMCOMMON_PLAYERS_MAX; i++) {
        GObj *g = gSCManagerBattleState->players[i].fighter_gobj;
        if (gSCManagerBattleState->players[i].pkind == nFTPlayerKindNot || g == NULL) continue;
        printf("slot %d ground %d x %d y %d stocks %d\n", i, ftGetStruct(g)->ga == nMPKineticsGround,
               (int)DObjGetStruct(g)->translate.vec.f.x, (int)DObjGetStruct(g)->translate.vec.f.y, ftGetStruct(g)->stock_count);
    }
}

EXPORT void port_dump_camera(void)
{
    extern int printf(const char *, ...);
    printf("camera dist %d status %d default %d at %d %d\n", (int)gGMCameraStruct.target_dist, (int)gGMCameraStruct.status_curr, (int)gGMCameraStruct.status_default, 0, 0);
}

// For proofs: where a fighter is, and how many sounds the sim has started.
EXPORT int port_fighter_x(int slot)
{
    GObj *g = gSCManagerBattleState->players[slot].fighter_gobj;
    return (g == NULL) ? 0 : (int)DObjGetStruct(g)->translate.vec.f.x;
}
