// Force-included before every game source file in the port build.
#ifndef PORT_PREFIX_H
#define PORT_PREFIX_H
extern char gPortArena[];
extern unsigned long gPortArenaSize;
#include "port_syms.h"
// Called with no declaration in scope by IDO-era code, defined void. wasm traps on a return type mismatch.
extern void __n_setInstChanState_Alt();
extern void efGroundMakeAppearActor();
extern void ftCommonOttottoSetStatus();
extern void ftCommonStopCeilSetStatus();
extern void ftManagerInitFighter();
extern void ftParamTryUpdateItemMusic();
extern void func_80026204_26E04();
extern void func_800266A0_272A0();
extern void func_80026738_27338();
extern void func_800267F4_273F4();
extern void gcParseMObjMatAnimJoint();
extern void gmCameraMakePlayerArrowsCamera();
extern void grCommonSetupInitAll();
extern void grInishiePakkunSetWaitFighter();
extern void grWallpaperMakeDecideKind();
extern void ifCommonBattleInitPlacement();
extern void ifCommonItemArrowSetAttr();
extern void ifCommonPlayerDamageStopBreakAnim();
extern void ifScreenFlashSetColAnimID();
extern void itMainDestroyItem();
extern void itManagerInitItems();
extern void mnCongraStartScene();
extern void mnMessageStartScene();
extern void mnPlayers1PGameContinueStartScene();
extern void mvEndingStartScene();
extern void sc1PBonusStageSetupFiles();
extern void sc1PGameBossDefeatInitInterface();
extern void sc1PGameBossInitWallpaper();
extern void scManagerRunPrintGObjStatus();
extern void syAudioSetBGMVolume();
extern void syAudioSetBGMVolumeFade();
extern void syAudioSetQuality();
extern void syAudioStopBGMAll();
extern void syControllerInitRumble();
extern void syControllerStartRumble();
extern void syControllerStopRumble();
extern void syDmaReadRom();
extern void syDmaReadSram();
extern void syDmaWriteSram();
extern void sySchedulerSetTicCount();
extern void syTaskmanInitSegmentF();
extern void wpManagerAllocWeapons();
extern void syDebugPrintf(const char *fmt, ...);
extern void ftCommonEscapeSetStatus();
#endif
