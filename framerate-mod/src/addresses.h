#pragma once
#include <cstdint>

// So far these are addresses for non-SecuROM builds for version 1.0.1.0

namespace Addr {

constexpr uintptr_t GDeltaTime       = 0x1F723E0; // double
constexpr uintptr_t GEngine          = 0x1FF8B20; // UGameEngine*
constexpr uintptr_t GWorld           = 0x1FFBCA4; // UWorld*

// ULocalPlayer view override statics.
// CalcSceneView copies them into the view before it builds the view matrix.
// The shipped game never writes the flag.
constexpr uintptr_t LP_bOverrideView    = 0x1FFB22C; // UBOOL, 4 bytes.
constexpr uintptr_t LP_OverrideLocation = 0x1FFB294; // FVector, 12 bytes.
constexpr uintptr_t LP_OverrideRotation = 0x1FFB2A0; // FRotator, 12 bytes.

// Secondary world engine tick.
// The live tick is GEngine vtable slot 69, read at init.
constexpr uintptr_t UTdGameEngine_Tick = 0x007C76D0;

constexpr uintptr_t UWorld_Tick        = 0x00B5B830; // World simulation. The camera is updated at the end.
constexpr uintptr_t UGameViewportClient_Draw = 0x00B421C0; // FViewportClient vtable +0x10. Builds the scene for one frame.
constexpr uintptr_t ULocalPlayer_CalcSceneView = 0x00B400E0; // Reads the view override globals.
constexpr uintptr_t FD3D9DynamicRHI_EndDrawingViewport = 0x018B9C80; // Present boundary. Counted only.

// Copies the matrix and bounds by value into the render command.
// FScene vtable 0x01C358B0, slot +0x08.
constexpr uintptr_t FScene_UpdatePrimitiveTransform = 0x00BC3C70;

// RemoveLight plus AddLight, building a new FLightSceneInfo from LightToWorld and WorldToLight.
// FScene vtable 0x01C358B0, slot +0x14.
// AddLight is slot +0x0C at 0xBCBA00.
// RemoveLight is slot +0x10 at 0xBCBB60.
constexpr uintptr_t FScene_UpdateLightTransform = 0x00BBED20;

// CPU skin Update returns immediately when this+0x64 equals GFrameNumber.
constexpr uintptr_t FSkeletalMeshObjectCPUSkin_vtable = 0x01CBDE00;

constexpr uintptr_t UWorld_GetWorldInfo = 0x00B822C0; // __thiscall(UWorld*) -> AWorldInfo*.
constexpr uintptr_t UWorld_IsPaused     = 0x00B53A30; // __thiscall(UWorld*) -> UBOOL.

// While paused, PlayerInput still consumes counts and UpdateRotation does not run.
// The tail also runs joystick input unless AllowJoystickInput is 0.
// The mod calls it again before the draw
constexpr uintptr_t UWindowsClient_ReadMouseDirectInput = 0x00417B90; // __thiscall(UWindowsClient*, float deltaTime).
constexpr uintptr_t UWindowsClient_vtable = 0x01A99328;                // Checked against GEngine->Client.

constexpr uintptr_t USkeletalMeshComponent_UpdateTransform = 0x00D1D770; // Component vtable +0x120. Fills the animated component registry.
constexpr uintptr_t UGameViewportClient_Tick = 0x00B389A0; // Ticks the UI scene chain with the dt it is given.

// Two call sites, and both are redirected.
// 0xECD833 is inside UGameEngine::Tick at 0xECD580, which the live UTdGameEngine::Tick calls.
// 0x7C7A7C is inside the secondary tick at UTdGameEngine_Tick.
constexpr uintptr_t UISceneTick_CallSite  = 0x007C7A7C;
constexpr uintptr_t UISceneTick_CallSite2 = 0x00ECD833;

constexpr uintptr_t ATdHUD_TickSpecial = 0x012679D0; // Actor vtable +0x1B8. Reads HUD+0x4C4 on sim frames.
constexpr uintptr_t UObject_StaticTick = 0x01101A30; // Calls ProcessAsyncLoading with the stock 0.005s budget. The dt argument is unused.

// Called with the stock budget on the PaceRate schedule.
constexpr uintptr_t UObject_ProcessAsyncLoading = 0x0112D940; // __cdecl(int bUseTimeLimit, float timeLimit).

// Nonzero makes the engine pass bUseTimeLimit false and drain the loader.
constexpr uintptr_t G_PriorityAsyncLoading = 0x01FFB250;
constexpr uintptr_t GObjAsyncPackages_Num  = 0x0204A378; // Pending async packages. Zero means the loader is idle.
constexpr uintptr_t Const_StockSliceBudget = 0x01A95B8C; // The engine's 0.005f per slice budget, in rdata.

// FlushAsyncLoading at 0x0112E210 calls ProcessAsyncLoading directly, so the StaticTick hook cannot stall a flush.
// The high priority slices inside UWorld::Tick (0.02s at 0xB5BA9C) and seamless travel
// (0.003s at 0xB8C8E2 and 0xB8C92F) already run on the sim tick.
// UTdHUD::SetPostProcessVars at 0x01267330 integrates fades from RealTimeRenderDelta or DeltaTime, clamped to 0.066s.
// The EffectManager update at 0x01265490 ticks the saturation effect from RealTimeRenderDelta.

// Three per tick call sites.
// Gating them to sim presents keeps the 10-call cooldown at 0x1FFBD54
// and the 5 ms AddToWorld budget on the stock schedule.
// LoadMap, FlushLevelStreaming, and CommitMapChange use other call sites.
constexpr uintptr_t UWorld_UpdateLevelStreaming = 0x00B8BB10; // __thiscall(UWorld*, FSceneViewFamily* or NULL), ret 4.
constexpr uintptr_t LS_CallSite_Draw        = 0x00B43066; // UGameViewportClient::Draw calls 0xB8BB10 with &ViewFamily.
constexpr uintptr_t LS_CallSite_HelperInner = 0x00B74A64; // Helper 0xB748B0 calls 0xB8BB10 with &ViewFamily.
constexpr uintptr_t LS_CallSite_TickTail    = 0x00ECE068; // UGameEngine::Tick, when GIsServer is set, calls 0xB8BB10 with NULL.

// Do not gate the outer call to helper 0xB748B0 at 0xECDE8B. It also arms the draw.
// The movie stop flag at 0x1F6443C rests at 1 during play, so it does not mark the post load window.
// Helper 0xB748B0 decrements this countdown once per engine tick at 0xB74C7D and stops the movie at 0.
constexpr uintptr_t G_MovieStopCountdown = 0x01FFBC2C;

// Trace probes. Installed or read only while logging is on, except SkipCutscene and SetPosition,
// which also schedule the post skip sim tick.

// Returns 0 even when it jumps a matinee.
// A performed skip is visible only as the SetPosition call below.
constexpr uintptr_t ATdPlayerController_SkipCutscene = 0x011C3910;

// The skip calls this once per matinee, with NewPosition = Length - 0.01 and bJump true.
constexpr uintptr_t USeqAct_Interp_SetPosition = 0x00E4F960;

// Returns 0 when the press is already in PressedKeys at +0x84, or the release has no matching press.
constexpr uintptr_t UInput_InputKey       = 0x00C1BED0;
constexpr uintptr_t FName_SpaceBar        = 0x01FF9EF8; // FName {Index, Number}.

// +0x50 HBINK is nonzero while the loading movie swallows every key.
constexpr uintptr_t G_FullScreenMovieBink = 0x01FF8B10;
constexpr uintptr_t G_IsLoadingLevelCached = 0x01FFA2E8; // Cached result of ATdPlayerController::IsLoadingLevel.

// FName::Names, a TArray of FNameEntry*.
// FNameEntry is Index, then a QWORD of flags, then HashNext, then a UTF-16 name at +0x10.
constexpr uintptr_t FName_Names_Data = 0x0204E7D8;
constexpr uintptr_t FName_Names_Num  = 0x0204E7DC;

// UWorld::ProcessLevelStreamingVolumes at 0x00B5B110 draws nothing.
// The per frame draw is the FViewport::Draw dispatch at 0x007C7E85.
// The stock material system keeps a texture pointer in two caches.
// Covering one of them still leaves the render thread reading the other after a load time GC.

// Prologue of UWorld::Tick.
constexpr const char* Pat_UWorld_Tick =
    "\x83\xEC\x48\x53\x55\x56\x57\x8B\xF9\xE8";
constexpr const char* Mask_UWorld_Tick =
    "xxxxxxxxxx";

}
