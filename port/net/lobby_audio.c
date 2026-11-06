// The lobby's sounds are the game's own: its menu cues, the announcer, the select-screen music.
// Only the audio thread runs here; no scene, no sim. Before a match the page stops it all (LobbyQuiet),
// and the match starts its own music as it always does.
#include "port.h"
#include <PR/ultratypes.h>
#include <sys/audio.h>
#include <gm/gmsound.h>
#include <ft/ftdef.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EXPORT
#endif

extern void *func_800269C0_275C0(u16 id);
extern void func_800266A0_272A0(void); // every sound effect off
extern void func_80026738_27338(void *h);
extern void port_audio_tick(void);

enum { CUE_SCROLL, CUE_SELECT, CUE_DENIED, CUE_FIGHTER, CUE_CHOOSE, CUE_FREE_FOR_ALL, CUE_STAGE, CUE_TEAMS, CUE_COUNT };

// The lobby passes a fighter's FTKind (src/ft/ftdef.h). 0 to 11 as on the game's own select screen;
// 13 Metal Mario, 14 to 25 the Fighting Polygons, 26 Giant DK use the 1P game's own calls for them.
// 12 is Master Hand: never offered, so no call.
static const u16 sAnnounce[12] = {
    nSYAudioVoiceAnnounceMario, nSYAudioVoiceAnnounceFox, nSYAudioVoiceAnnounceDonkey, nSYAudioVoiceAnnounceSamus,
    nSYAudioVoiceAnnounceLuigi, nSYAudioVoiceAnnounceLink, nSYAudioVoiceAnnounceYoshi, nSYAudioVoiceAnnounceCaptain,
    nSYAudioVoiceAnnounceKirby, nSYAudioVoiceAnnouncePikachu, nSYAudioVoiceAnnouncePurin, nSYAudioVoiceAnnounceNess,
};
static void *sVoice;

EXPORT void LobbyCue(int cue, int arg)
{
    switch (cue) {
    case CUE_SCROLL: func_800269C0_275C0(nSYAudioFGMMenuScroll2); break;
    case CUE_SELECT: func_800269C0_275C0(nSYAudioFGMMenuSelect); break;
    case CUE_DENIED: func_800269C0_275C0(nSYAudioFGMMenuDenied); break;
    case CUE_STAGE: func_800269C0_275C0(nSYAudioFGMStageSelect); break;
    case CUE_FIGHTER:
        {
            u16 v;
            if (arg >= 0 && arg < 12) v = sAnnounce[arg];
            else if (arg == nFTKindMMario) v = nSYAudioVoiceAnnounceMMario;
            else if (arg >= nFTKindNStart && arg <= nFTKindNEnd) v = nSYAudioVoiceAnnounceZako;
            else if (arg == nFTKindGDonkey) v = nSYAudioVoiceAnnounceGDonkey;
            else break;
            // One name at a time, as on the select screen: a new pick cuts the last call short.
            if (sVoice != NULL) func_80026738_27338(sVoice);
            func_800269C0_275C0(nSYAudioFGMMarioDash);
            sVoice = func_800269C0_275C0(v);
        }
        break;
    case CUE_CHOOSE: func_800269C0_275C0(nSYAudioVoiceAnnounceSelectPlayer); break;
    case CUE_FREE_FOR_ALL: func_800269C0_275C0(nSYAudioVoiceAnnounceFreeForAll); break;
    case CUE_TEAMS: func_800269C0_275C0(nSYAudioVoiceAnnounceTeamBattle); break;
    default: break;
    }
}

// on: the select-screen theme on song player 0. vol: 0 to 0x7F00, for the mute button and the mics.
EXPORT void LobbyMusic(int on, int vol)
{
    static int playing;
    if (on && !playing) { syAudioPlayBGM(0, nSYAudioBGMBattleSelect); playing = 1; }
    if (!on && playing) { syAudioStopBGMAll(); playing = 0; }
    if (on) syAudioSetBGMVolume(0, vol < 0 ? 0 : vol > 0x7F00 ? 0x7F00 : vol);
}

// One audio frame with no game frame: the lobby's clock.
EXPORT void LobbyAudioTick(void) { port_audio_tick(); }

// Everything off and the volume back where the match expects it.
EXPORT void LobbyQuiet(void)
{
    LobbyMusic(0, 0);
    func_800266A0_272A0();
    sVoice = NULL;
    syAudioSetBGMVolume(0, 0x7800);
}
