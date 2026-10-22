#include "framework.h"
#include "hooks.h"
#include "pattern.h"
#include "addresses.h"
#include "config.h"
#include <cstdlib>
#include <timeapi.h>

#define MOD_VERSION "v0.9.0"

// A longer frame runs as one stock variable delta tick.
static constexpr int MAX_SIM_STEPS_PER_FRAME = 4;

static double g_simDt    = 1.0 / 62.0;
static float  g_simDtF   = 1.0f / 62.0f;
static double g_maxAccum = 4.0 / 62.0;

// Each loader call overshoots its budget, so the call rate is what matches stock.
// Slices prefer render only presents, so they do not stack on a sim frame
// zero disables pacing.
static double g_loadSliceDt = 1.0 / 62.0;
static float  g_stockSliceBudget = 0.005f; // Read from rdata at init. The engine's per slice budget.

// A sim step past any of these snaps the interpolation buffer.
static constexpr float MAX_INTERP_STEP_UU  = 500.0f;
static constexpr int   MAX_INTERP_STEP_ROT = 10923;  // About 60 degrees, in UE3 rotator units.
static constexpr float MAX_INTERP_STEP_FOV = 15.0f;

static constexpr bool ENABLE_BONE_INTERPOLATION = true;
static constexpr int  MAX_VIEWMODEL_COMPONENTS = 64;
static constexpr int  MAX_VIEWMODEL_BONES = 192;
static constexpr int  MAX_PAWN_WALK = 64;
static constexpr int  MAX_SKEL_REGISTRY = 96;
static constexpr int  MAX_MOVER_REGISTRY = 96;
static constexpr int  MAX_LIGHT_REGISTRY = 64;
static constexpr int  MAX_LIGHTS = 16;

static constexpr float WORLD_SKEL_RENDERED_WINDOW = 1.0f;
static constexpr float WORLD_SKEL_TELEPORT_UU = 300.0f;

// Squared cutoff in uu. Zero leaves the cull off. One uu is 1 cm
// as that's how DICE authored their meshes (note stock UE3 convention is 2 cm)
static float g_worldMeshRangeSqUU = 0.0f;

static HMODULE g_hSelf = nullptr;  // Set in DllMain. LoadConfig uses it to find the ini.

static double g_accumulator = 0.0;
static bool   g_seeded      = false;

static FTPOV  g_simA = {};
static FTPOV  g_simB = {};
static int    g_simCount = 0;

// The residual is added to the newest controller rotation, with camera effects lerped apart.
// Adding it to the interpolated POV doubles the rate and snaps back at every sim tick.
// Look-at points replace the mouse delta, so both axes interpolate.
// Melee soft lock, precise rotation, and root rotation own yaw only.
// A parity miss disables the residual until two ticks match.
static FRotator g_ctrlRotA = {};
static FRotator g_ctrlRotB = {};
static FRotator g_camFxA = {};
static FRotator g_camFxB = {};
static bool     g_ctrlRotValid = false;
static bool     g_llParityOk = true;
static int      g_llCleanTicks = 0;
static bool     g_llPredValid = false;
static FRotator g_llPred = {};
static int      g_llFollowYaw = 0;  // Precise rotation follow from the last tick, then interpolated.

static volatile bool g_skipWorldTick    = false;
static volatile bool g_inSimulationTick = false;
static volatile bool g_capturedThisSim  = false;
static volatile bool g_meshPushArmed    = false;
static volatile bool g_rtsArmed         = false;

// Render only frames add their dt here, and the sum is removed before the next sim tick.
// Script PostRender still overwrites the HUD fade delta every present
static float g_rtsRenderOnlyAccum = 0.0f;

static float g_wallSecondsPerSimTick = 1.0f / 62.0f;
static LONGLONG g_lastSimTickQpc = 0;
static double g_qpcSecondsPerCount = 0.0;

static LONGLONG g_lastLoadSliceQpc = 0;
static double   g_loadSliceCredit = 0.0;

// Index 0 is render only and index 1 is sim.
// the EMA weight is the same on the way up and the way down.
// A faster fall tracks the minimum of an oscillating sim cost and collapses the mix towards sim frames.
static double   g_paceEma[2]  = {};
static double   g_pacePeak[2] = {};
static double   g_paceSpikeAt[2] = {};
static double   g_paceP = 0.0;
static bool     g_paceEngaged = false;
static double   g_paceEdgeSince = 0.0;
static LONGLONG g_lastPresentQpc = 0;
static double   g_dbgPaceWaitMs = 0.0;
static double   g_dbgPaceP = 0.0;
static int      g_dbgPaceLayer = 0;  // 0 off, 1 cap, 2 evening, 3 warmup.

// bSmoothFrameRate limits each frame to a 300-frame running average, which ratchets towards the sim frame rate.
// Init clears the flag. MaxSmoothedFrameRate stays live for the console command.
static bool     g_capOwned = false;
static LONGLONG g_passWaitQpc = 0;

// Debt is clamped so a hitch cannot bank a burst.
static double   g_capSchedule = 0.0;
static constexpr double CAP_DEBT_FACTOR = 2.0;

static constexpr double PACE_EMA_ALPHA = 0.08;
static constexpr double PACE_PEAK_RELAX   = 0.04;
static constexpr double PACE_SLEW_UP      = 0.08;
static constexpr double PACE_SLEW_DOWN    = 0.02;
static constexpr double PACE_GAP_MIN      = 0.0015; // Seconds of sim versus render peak gap required to engage.
static constexpr double PACE_OVER_CAP     = 1.10;
static constexpr double PACE_ENGAGE_SEC   = 0.25;
static constexpr double PACE_DISENGAGE_SEC = 0.50;

// Uncapped, natural is the work feasible rate.
// Capped, natural is at least the cap interval, so 2.0 allows pacing down to half the cap.
static constexpr double PACE_MAX_TANK_ENGAGE = 2.00;
static constexpr double PACE_MAX_TANK_STAY   = 2.40;

// Ground acceleration is accel times dt, so these long deltas are the load boost.
// Warmup lasts while the movie stop countdown is nonzero, then until two ticks settle.
// The present clamp runs even when EvenPacing is off.
static constexpr double WARMUP_SETTLE_FACTOR = 1.5;
static constexpr int    WARMUP_SETTLED_NEEDED = 2;
static constexpr int    WARMUP_MAX_TICKS = 8;
static bool g_warmupActive = false;
static int  g_warmupSettled = 0;
static int  g_warmupTicksRun = 0;

// A stall after a render only frame ticks the world before the next input pump.
static bool g_lastFrameRenderOnly = false;

// Cinematic mode can last about a second after a skip, and per tick Kismet still runs then.
// One sim tick runs after a pump that actually skipped. Later ticks stay on the sim cadence
static bool g_skipReqThisFrame = false;

static void PaceResetStats() {
    g_paceEma[0] = g_paceEma[1] = 0.0;
    g_pacePeak[0] = g_pacePeak[1] = 0.0;
    g_paceSpikeAt[0] = g_paceSpikeAt[1] = 0.0;
    g_paceP = 0.0;
    g_paceEngaged = false;
    g_paceEdgeSince = 0.0;
    g_capSchedule = 0.0;
    g_lastPresentQpc = 0;
    g_dbgPaceP = 0.0;
}

struct ViewModelPose {
    FMatrix localToWorld;
    FBoxSphereBounds bounds;
    int boneCount;  // Zero means the bones were not snapshotted.
    FMatrix bones[MAX_VIEWMODEL_BONES];
};
struct ViewModelEntry {
    uintptr_t component;
    uintptr_t vtable;       // A change means the object was freed.
    uintptr_t scene;        // The push requires the same scene pointer.
    uintptr_t parentAnim;   // The renderer reads this component's SpaceBases.
    bool cameraRelative;
    bool skeletal;          // False for a mover, which has no bones.
    bool hasPrev;
    bool lead;              // This present is one interval ahead, at t = 1+alpha.
    uintptr_t ownerRoot;
    ViewModelPose prev;
    ViewModelPose curr;
};
static ViewModelEntry g_vm[MAX_VIEWMODEL_COMPONENTS] = {};
static int   g_vmCount = 0;

// The engine rebuilds a moving light once per sim tick, so an uninterpolated light
// slides against the interpolated mesh by one tick of travel.
// A light uses the phase of the mover that shares its Base chain root.
struct LightEntry {
    uintptr_t component;
    uintptr_t vtable;
    uintptr_t scene;
    uintptr_t ownerRoot;
    bool hasPrev;
    bool lead;
    FMatrix prev;
    FMatrix curr;
};
static LightEntry g_lights[MAX_LIGHTS] = {};
static int g_lightCount = 0;
static LightEntry g_lightsLastTick[MAX_LIGHTS] = {};
static int g_lightsLastTickCount = 0;

// Constant velocity motion is presented at t = 1+alpha, so the mesh meets the encroach test.
// the platform the player rides stays behind, matched to the camera.
// That platform is the root of the Base chain, kept while the pawn is airborne.
// A mover whose step matches the ridden root is in that frame even with no Base link
static constexpr float MOVER_LEAD_MIN_STEP_UU = 0.5f;
static constexpr float MOVER_LEAD_ABS_TOL_UU  = 2.0f;
static constexpr float MOVER_LEAD_REL_TOL     = 0.10f;  // Used when 10 percent of the step is larger than the absolute tolerance.
static constexpr float MOVER_LEAD_MIN_ROT_DOT = 0.9999813f; // Cosine of 0.35 degrees per tick.
static constexpr int   MAX_BASE_CHAIN = 8;
static uintptr_t g_pawnRideRoot = 0;
static FVector   g_rideStep = {};
static bool      g_rideStepValid = false;
static uintptr_t g_rideStepActor = 0;
static FVector   g_rideStepLastLoc = {};
static FTPOV g_vmCamPrev = {};
static FTPOV g_vmCamCurr = {};
static FTPOV g_lastInterp = {};
static bool  g_lastInterpValid = false;
static float g_lastAlpha = 0.0f;

// The engine composes these with aligned SSE loads in FMatrix::Multiply at 0x4465B0.
// An unaligned buffer faults.
__declspec(align(16))
static FMatrix g_boneScratch[MAX_VIEWMODEL_COMPONENTS][MAX_VIEWMODEL_BONES];

// Every skeletal component whose UpdateTransform ran on a recent sim tick.
struct SkelRegistryEntry {
    uintptr_t component;
    DWORD lastSeenTick;
};
static SkelRegistryEntry g_skelRegistry[MAX_SKEL_REGISTRY] = {};
static volatile DWORD g_simTickCounter = 0;

// An entry interpolates only after two consecutive ticks, so a streaming attach is not swept.
struct MoverRegistryEntry {
    uintptr_t component;
    uintptr_t vtable;
    DWORD lastSeenTick;
    DWORD prevSeenTick;
};
static MoverRegistryEntry g_moverRegistry[MAX_MOVER_REGISTRY] = {};
static MoverRegistryEntry g_lightRegistry[MAX_LIGHT_REGISTRY] = {};
static volatile bool g_inOurPush = false;

static uintptr_t g_pGEngine    = 0;
static double*   g_pGDeltaTime = nullptr;
static bool      g_overrideGlobalsOk = false;
static bool      g_overrideActive    = false;

static int   g_total = 0;
static int   g_dbgSim = 0, g_dbgRdr = 0, g_dbgPass = 0;
static DWORD g_dbgLast = 0;
static LONG  g_dbgCatchUp = 0;
static LONG  g_dbgOverrideWrite = 0;
static LONG  g_dbgDraw = 0;
static LONG  g_dbgCalcView = 0;
static LONG  g_dbgCalcViewOv = 0;
static LONG  g_dbgRhiEnd = 0;
static LONG  g_dbgVmPush = 0;
static LONG  g_dbgVmSkip = 0;
static LONG  g_dbgVmSkel = 0;
static volatile LONG g_dbgVmWorld = 0;
static volatile LONG g_dbgVmMover = 0;
static LONG g_dbgVmLead = 0;
static volatile LONG g_dbgVmLight = 0;
static LONG g_dbgLightPush = 0;
static LONG g_dbgUiTick = 0;
static LONG g_dbgUiDedup = 0;
static LONG g_dbgLoadSlice = 0;
static LONG g_dbgLoadSkip = 0;
static LONG g_dbgVmStale = 0;
static LONG g_dbgVmFar = 0;
static LONG g_dbgLsRun = 0;     // Three calls per sim tick.
static LONG g_dbgLsSkip = 0;
static LONG g_dbgCutSnap = 0;   // A storm of these means the view is stepping.
static LONG g_dbgStall = 0;
static LONG g_dbgLlApply = 0;
static LONG g_dbgLlMoves = 0;
static LONG g_dbgLlGate = 0;
static LONG g_dbgLlCheck = 0;
static LONG g_dbgLlMismatch = 0; // The post tick state was still latchable.
static LONG g_dbgLlTrans = 0;    // A move or state class began inside the tick.
static LONG g_dbgLlErrMax = 0;
static float g_dbgAlphaMin = 2.0f;
static float g_dbgAlphaMax = -1.0f; // A stuck pair means the view is stepping.

// A catch up present calls the engine tick more than once, and the later UI calls are dropped.
static volatile unsigned char g_uiTickedThisPresent = 0;
static volatile float g_uiRealFrameDelta = 1.0f / 62.0f;
static volatile LONG g_dbgPawnWalk = 0; // Negative one means WorldInfo did not resolve.
static LONG  g_dbgVmCapHit = 0;

typedef void (__thiscall *fn_EngineTick)(void* thisptr, float dt);
typedef void (__thiscall *fn_WorldTick)(void* thisptr, int tickType, float dt);

// __fastcall stand-in for a __thiscall.
// ecx is this, and edx is unused.
typedef void (__fastcall *fn_UpdatePrimitiveTransform)(void* scene, void* edx,
                                                       void* component);
typedef void (__fastcall *fn_MeshObjectUpdate)(void* meshObject, void* edx,
                                               int lodIndex, void* component,
                                               void* activeMorphs);
typedef void* (__fastcall *fn_GetWorldInfo)(void* world, void* edx);
typedef int   (__fastcall *fn_WorldIsPaused)(void* world, void* edx);
typedef void  (__fastcall *fn_ReadMouseDirectInput)(void* client, void* edx, float deltaTime);
typedef void (__thiscall *fn_CompUpdateTransform)(void* comp);
typedef int  (__fastcall *fn_ShouldRecreateProxy)(void* comp, void* edx);
typedef void (__fastcall *fn_SceneUpdateTransform)(void* scene, void* edx,
                                                   void* component);

typedef void (__thiscall *fn_HudTickSpecial)(void* thisptr, float dt);
typedef void (__thiscall *fn_ViewportTick)(void* thisptr, float dt);
typedef void (__cdecl *fn_StaticTick)(float dt);
typedef void (__cdecl *fn_ProcessAsyncLoading)(int bUseTimeLimit, float timeLimit);

static fn_EngineTick g_origEngineTick = nullptr;
static fn_WorldTick  g_origWorldTick  = nullptr;
static fn_CompUpdateTransform g_origSkelUpdateTransform = nullptr;
static fn_SceneUpdateTransform g_origSceneUpdateTransform = nullptr;
static fn_SceneUpdateTransform g_origSceneUpdateLightTransform = nullptr;
static fn_HudTickSpecial g_origHudTickSpecial = nullptr;
static fn_StaticTick g_origStaticTick = nullptr;
static void*         g_origViewportClientDraw = nullptr;
static void*         g_origCalcSceneView = nullptr;
static void*         g_origEndDrawingViewport = nullptr;

static void Log(const char* msg) {
    if (!g_cfg.logging) return;
    OutputDebugStringA(msg);
    static FILE* f = nullptr;
    if (!f)
        f = fopen("framerate-mod.log", "a");
    if (f) { fputs(msg, f); fflush(f); }
}

// Safe memory access.
static bool CanAccess(const void* ptr, size_t size, DWORD accessMask) {
    if (!ptr || size == 0) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(ptr, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return false;
    if ((mbi.Protect & accessMask) == 0) return false;

    uintptr_t begin = (uintptr_t)ptr;
    uintptr_t end = begin + size;
    uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    return end >= begin && end <= regionEnd;
}

static bool CanRead(const void* ptr, size_t size) {
    constexpr DWORD readable =
        PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
        PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return CanAccess(ptr, size, readable);
}

static bool CanWrite(const void* ptr, size_t size) {
    constexpr DWORD writable =
        PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE |
        PAGE_EXECUTE_WRITECOPY;
    return CanAccess(ptr, size, writable);
}

template <typename T>
static bool ReadAt(uintptr_t addr, T* out) {
    if (!out || !CanRead((void*)addr, sizeof(T))) return false;
    *out = *(T*)addr;
    return true;
}

static bool ReadPtrAt(uintptr_t addr, uintptr_t* out) {
    return ReadAt<uintptr_t>(addr, out);
}

// Section bounds from the in-memory PE header.
// The constants are the GOG 1.0.1.0 layout, used until that header is read.
// A live object's vtable is in .rdata and its virtual functions are in .text.
// Freed heap almost never rereads as a vtable inside .rdata
static uintptr_t g_exeTextLo  = 0x00401000, g_exeTextHi  = 0x01A8B000;
static uintptr_t g_exeRdataLo = 0x01A8B000, g_exeRdataHi = 0x01F53000;

static void ResolveExeSections() {
    uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
    if (!base) return;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        uintptr_t lo = base + sec[i].VirtualAddress;
        uintptr_t hi = lo + sec[i].Misc.VirtualSize;
        if (memcmp(sec[i].Name, ".text\0\0\0", 8) == 0) {
            g_exeTextLo = lo; g_exeTextHi = hi;
        } else if (memcmp(sec[i].Name, ".rdata\0\0", 8) == 0) {
            g_exeRdataLo = lo; g_exeRdataHi = hi;
        }
    }
}

static bool IsEngineCodePtr(uintptr_t p) {
    return p >= g_exeTextLo && p < g_exeTextHi;
}

static bool IsEngineVtablePtr(uintptr_t p) {
    return p >= g_exeRdataLo && p < g_exeRdataHi && (p & 3) == 0;
}

static bool IsReasonableObject(uintptr_t ptr) {
    uintptr_t vtable = 0;
    return ReadPtrAt(ptr, &vtable) && IsEngineVtablePtr(vtable);
}

// Camera access runs GEngine, LocalPlayer, Controller, then PlayerCamera.
static uintptr_t GetLocalPlayerController() {
    uintptr_t engine = 0;
    if (!ReadPtrAt(g_pGEngine, &engine) || !IsReasonableObject(engine))
        return 0;

    int num = 0;
    if (!ReadAt<int>(engine + UGameEngineArrayOffsets::GamePlayers_Num, &num) ||
        num <= 0)
        return 0;

    uintptr_t data = 0;
    if (!ReadPtrAt(engine + UGameEngineArrayOffsets::GamePlayers_Data, &data))
        return 0;

    uintptr_t lp = 0;
    if (!ReadPtrAt(data, &lp) || !IsReasonableObject(lp))
        return 0;

    uintptr_t ctrl = 0;
    if (!ReadPtrAt(lp + UPlayerOffsets::Actor, &ctrl) ||
        !IsReasonableObject(ctrl))
        return 0;

    return ctrl;
}

static FTPOV* GetCameraPOVPtr() {
    uintptr_t ctrl = GetLocalPlayerController();
    if (!ctrl) return nullptr;

    uintptr_t cam = 0;
    if (!ReadPtrAt(ctrl + APlayerControllerOffsets::PlayerCamera, &cam) ||
        !IsReasonableObject(cam))
        return nullptr;

    uintptr_t pov = cam + ACameraOffsets::CameraCache + offsetof(FTCameraCache, POV);
    return CanRead((void*)pov, sizeof(FTPOV)) &&
           CanWrite((void*)pov, sizeof(FTPOV)) ? (FTPOV*)pov : nullptr;
}

static uintptr_t GetPlayerPawnPtr() {
    uintptr_t ctrl = GetLocalPlayerController();
    if (!ctrl) return 0;

    uintptr_t pawn = 0;
    if (ReadPtrAt(ctrl + AControllerOffsets::Pawn, &pawn) &&
        IsReasonableObject(pawn))
        return pawn;

    if (ReadPtrAt(ctrl + ATdPlayerControllerOffsets::myPawn, &pawn) &&
        IsReasonableObject(pawn))
        return pawn;

    return 0;
}

// True while the pawn's active mesh is Mesh1p.
static bool IsFirstPersonGameplayCamera() {
    uintptr_t pawn = GetPlayerPawnPtr();
    if (!pawn) return false;

    uintptr_t activeMesh = 0;
    uintptr_t mesh1p = 0;
    if (!ReadPtrAt(pawn + APawnOffsets::Mesh, &activeMesh) ||
        !ReadPtrAt(pawn + ATdPawnOffsets::Mesh1p, &mesh1p))
        return false;

    return activeMesh && mesh1p && activeMesh == mesh1p;
}

// Every view target presents through CameraCache.POV, including death cameras.
// CaptureSimulationCamera snaps the buffer across a hard cut.
static bool ShouldApplyViewOverride() {
    return GetCameraPOVPtr() != nullptr;
}

static int ShortestAngleDiff(int from, int to) {
    int diff = to - from;
    while (diff > 32768)  diff -= 65536;
    while (diff < -32768) diff += 65536;
    return diff;
}

// One rotator component, wrapped the way UnrealScript Normalize does.
static int NormalizeRotUnits(int v) {
    v &= 0xFFFF;
    return v > 32767 ? v - 65536 : v;
}

static bool MousePendingCounts(float* dx, float* dy);
static void LatProbeLatched(LONGLONG nowQpc);
static uintptr_t GetWorldInfoPtr();

// One rotator unit is 0.0055 degrees.
// The model follows the script's float association and int truncations.
// this slack covers single precision evaluation order only.
static constexpr int LL_PARITY_TOL_UNITS = 4;

// Factors of the count to rotation transform.
// Read once per present for the residual, and once per tick for the parity prediction.
// An axis the engine owns this tick keeps interpolating.
// Both axes off means the present is gated.
enum LLStateClass : unsigned char {
    LLC_Constrained = 1 << 0,  // A modelled ConstrainAxis window is active.
    LLC_SoftLock    = 1 << 1,  // Melee soft lock owns yaw. Pitch stays latched.
    LLC_Precise     = 1 << 2,  // Precise rotation owns the yaw follow. Pitch stays latched.
    LLC_RootRot     = 1 << 3,  // Animation root rotation owns yaw.
};

struct LLInputState {
    float mouseSensitivity;
    float fovAngle;          // The value GetFOVAngle returns to PlayerInput.
    float handicap;          // GetMobilityMultiplier, times the walk button multiplier when that button is down.
    bool  invertMouse;
    float viewPitchMin;
    float viewPitchMax;
    int   pawnPitch;         // Pawn pitch. RotSpeedMod and the constraint window both use it.
    int   pawnYaw;
    unsigned char moveState;
    unsigned char cls;       // LLStateClass bits.
    bool  latchYaw;
    bool  latchPitch;

    // Look window relative to the pawn.
    // An absolute yaw window is already rebased onto the pawn yaw.
    // Valid when constrain is true.
    bool  constrain;
    bool  climbSpill;        // Climb's PostConstrainCamera spills the removed pitch into yaw.
    int   minYaw, maxYaw, minPitch, maxPitch;

    // UTdMove::Tick runs after UpdateRotation.
    // On a step tick the controller yaw follows the pawn.
    // On the snap tick the predicted yaw is preciseYaw, and no yaw residual is shown,
    // because that tick discards the mouse yaw.
    // The snap itself is interpolated as the follow.
    bool  follow;
    bool  followSnap;
    bool  faceRotActive;     // FaceRotation already set pawn yaw from the controller yaw.
    int   preciseYaw;
    float preciseT;
    float tickDt;            // Dilated actor dt the follow step uses.
};

// Clamp(1 - TaserDamageLevel * 0.01, 0, 1), matching GetMobilityMultiplier at 0x12B3000.
// The pawn's override is the same function.
// Only RegenerateHealth changes the field, and that runs on the sim tick,
// so a read between ticks is the value the next PlayerInput uses.
static bool LLMobilityMultiplier(uintptr_t pawn, float* out) {
    float taser = 0.0f;
    if (!ReadAt<float>(pawn + ATdPawnOffsets::TaserDamageLevel, &taser))
        return false;
    float r = 1.0f - taser * 0.01f;
    if (r < 0.0f) r = 0.0f;
    if (r > 1.0f) r = 1.0f;
    *out = r;
    return true;
}

// A paused world still runs PlayerInput, which consumes the counts, and skips UpdateRotation.
// pending counts therefore add no rotation while paused.
static bool WorldIsPaused() {
    uintptr_t world = 0;
    if (!ReadPtrAt(Addr::GWorld, &world) || !IsReasonableObject(world))
        return true;
    fn_WorldIsPaused isPaused = (fn_WorldIsPaused)Addr::UWorld_IsPaused;
    return isPaused((void*)world, nullptr) != 0;
}

// Grab zeroes look input while folded or in the hang free vertigo state.
// GrabJump drops a large yaw input during its first 50 ms.
// Returns false when the mouse does not take the modelled path this tick
static bool LLMoveInputIntact(uintptr_t move, int moveState) {
    if (moveState == EMove::Grab) {
        DWORD gbits = 0;
        unsigned char folded = 0;
        if (!ReadAt<DWORD>(move + UTdMoveGrabOffsets::Bits1B4, &gbits) ||
            !ReadAt<unsigned char>(move + UTdMoveGrabOffsets::CurrentFoldedType, &folded))
            return false;
        return !(gbits & UTdMoveGrabOffsets::Bit_HangFreeVertigoEffect) && folded == 0;
    }
    if (moveState == EMove::GrabJump) {
        float active = 0.0f;
        return ReadAt<float>(move + UTdMoveOffsets::MoveActiveTime, &active) &&
               active >= 0.05f;
    }
    return true;
}

// The move's look window after the per class getters, intersected with the pawn window.
// Grab widens the window while shimmying.
// Climb uses its animation window.
// Wallruns store a world space window, which is rebased onto the pawn yaw.
// Returns false when the window depends on state this model cannot read.
// Balance does that while a heavy weapon is held, via the native GetWeaponType.
static bool LLResolveConstraintWindow(uintptr_t pawn, uintptr_t move,
                                      int moveState, DWORD moveBits,
                                      const FRotator& ctrlRot, const FRotator& pawnRot,
                                      LLInputState* out) {
    FRotator mn{}, mx{};
    if (!ReadAt<FRotator>(move + UTdMoveOffsets::MinLookConstraint, &mn) ||
        !ReadAt<FRotator>(move + UTdMoveOffsets::MaxLookConstraint, &mx))
        return false;
    int minYaw = mn.Yaw, maxYaw = mx.Yaw, minPitch = mn.Pitch, maxPitch = mx.Pitch;

    switch (moveState) {
    case EMove::Grab: {
        unsigned char shimmy = 0;
        if (!ReadAt<unsigned char>(move + UTdMoveGrabOffsets::CurrentShimmyMove, &shimmy))
            return false;
        if (shimmy != UTdMoveGrabOffsets::ShimmyType_NoShimmy) {
            minYaw = -5000; maxYaw = 5000; minPitch = 0;
        }
        break;
    }
    case EMove::Climb: {
        DWORD cbits = 0;
        int startTurning = 0;
        if (!ReadAt<DWORD>(move + UTdMoveClimbOffsets::Bits194, &cbits) ||
            !ReadAt<int>(move + UTdMoveClimbOffsets::StartTurningAngle, &startTurning))
            return false;
        if (cbits & UTdMoveClimbOffsets::Bit_IsPlayingAnimation) {
            minYaw = -startTurning; maxYaw = startTurning;
        }
        int delta = abs(NormalizeRotUnits(ctrlRot.Yaw - pawnRot.Yaw));
        if (delta > startTurning) minPitch = -11000;
        out->climbSpill = true;
        break;
    }
    case EMove::WallRunLeft:
    case EMove::WallRunRight:
    case EMove::WallRunVertical:
        if (!ReadAt<int>(move + UTdMoveWallRunOffsets::MinContraintWorld, &minYaw) ||
            !ReadAt<int>(move + UTdMoveWallRunOffsets::MaxContraintWorld, &maxYaw))
            return false;
        break;
    case EMove::WallrunJump:
        if (!ReadAt<int>(move + UTdMoveWallrunJumpOffsets::MinContraintWorld, &minYaw) ||
            !ReadAt<int>(move + UTdMoveWallrunJumpOffsets::MaxContraintWorld, &maxYaw))
            return false;
        break;
    case EMove::Balance: {
        uintptr_t weapon = 0;
        if (ReadPtrAt(pawn + ATdPawnOffsets::MyWeapon, &weapon) && weapon)
            return false;  // heavy weapons narrow the window (native GetWeaponType)
        break;
    }
    default:
        break;
    }

    FRotator pmn{}, pmx{};
    if (!ReadAt<FRotator>(pawn + ATdPawnOffsets::MinLookConstraint, &pmn) ||
        !ReadAt<FRotator>(pawn + ATdPawnOffsets::MaxLookConstraint, &pmx))
        return false;
    if (moveBits & UTdMoveOffsets::Bit_UseAbsoluteYawConstraint) {
        minYaw = NormalizeRotUnits(minYaw - pawnRot.Yaw);
        maxYaw = NormalizeRotUnits(maxYaw - pawnRot.Yaw);
    } else {
        if (pmn.Yaw > minYaw) minYaw = pmn.Yaw;
        if (pmx.Yaw < maxYaw) maxYaw = pmx.Yaw;
    }
    if (pmn.Pitch > minPitch) minPitch = pmn.Pitch;
    if (pmx.Pitch < maxPitch) maxPitch = pmx.Pitch;

    out->constrain = true;
    out->minYaw = minYaw;   out->maxYaw = maxYaw;
    out->minPitch = minPitch; out->maxPitch = maxPitch;
    out->cls |= LLC_Constrained;

    // Outside the window, ConstrainAxis pulls the view back at dt/0.2 per tick.
    // that pull is engine motion, so the axis keeps interpolating.
    int cdYaw   = NormalizeRotUnits(NormalizeRotUnits(ctrlRot.Yaw)   - NormalizeRotUnits(pawnRot.Yaw));
    int cdPitch = NormalizeRotUnits(NormalizeRotUnits(ctrlRot.Pitch) - NormalizeRotUnits(pawnRot.Pitch));
    if (cdYaw < minYaw || cdYaw > maxYaw)       out->latchYaw = false;
    if (cdPitch < minPitch || cdPitch > maxPitch) out->latchPitch = false;
    return true;
}

// Whether UTdMove::Tick will move the controller yaw this tick, and by which rule.
// When FaceRotation runs, the step measures from the post input controller yaw.
// TdPawn.FaceRotation sets pawn yaw from the controller yaw for every physics mode.
// Otherwise the step measures from the pawn's own yaw.
// worldDt is the world tick delta the step is computed with.
static bool LLReadPreciseFollow(uintptr_t pawn, uintptr_t move, DWORD moveBits,
                                DWORD pawnBits, float worldDt, LLInputState* out) {
    out->cls |= LLC_Precise;
    if ((moveBits & UTdMoveOffsets::Bit_ReachedPreciseRotation) ||
        (moveBits & UTdMoveOffsets::Bit_DisableControllerFacingPawnYawRotation))
        return true;  // The controller is untouched, so the plain input path applies.
    uintptr_t worldInfo = GetWorldInfoPtr();
    float timeDilation = 1.0f, customDilation = 1.0f, faceLeft = 0.0f;
    if (!worldInfo ||
        !ReadAt<float>(worldInfo + AWorldInfoOffsets::TimeDilation, &timeDilation) ||
        !ReadAt<float>(pawn + AActorOffsets::CustomTimeDilation, &customDilation) ||
        !ReadAt<float>(pawn + ATdPawnOffsets::FaceRotationTimeLeft, &faceLeft) ||
        !ReadAt<float>(move + UTdMoveOffsets::PreciseRotationInterpolationTime, &out->preciseT) ||
        !ReadAt<int>(move + UTdMoveOffsets::PreciseRotationYaw, &out->preciseYaw))
        return false;
    out->tickDt = (worldDt * timeDilation) * customDilation;
    if (faceLeft > 0.0f) {
        out->latchYaw = false;  // FaceRotation is blending pawn yaw over time, which this model does not reproduce.
        return true;
    }
    out->faceRotActive = !(pawnBits & ATdPawnOffsets::Bit_IsUsingRootRotation) &&
                         !(moveBits & UTdMoveOffsets::Bit_DisableFaceRotation);
    if (out->preciseT > out->tickDt)
        out->follow = true;
    else
        out->followSnap = true;
    return true;
}

// Returns false when no axis can be latched this tick.
// The order matches TdPlayerInput.PlayerInput, TdPlayerController.UpdateRotation,
// TdMove.UpdateViewRotation, and the pawn's UTdMove::Tick after them.
static bool LLReadLookState(uintptr_t ctrl, float worldDt, LLInputState* out) {
    out->cls = 0;
    out->latchYaw = out->latchPitch = true;
    out->constrain = false;
    out->climbSpill = false;
    out->follow = out->followSnap = out->faceRotActive = false;
    out->preciseYaw = 0;
    out->preciseT = out->tickDt = 0.0f;
    if (WorldIsPaused())
        return false;
    DWORD pcBits = 0;
    if (!ReadAt<DWORD>(ctrl + APlayerControllerOffsets::Flags, &pcBits) ||
        (pcBits & APlayerControllerOffsets::bCinematicMode_Mask))
        return false;  // UpdateRotation: SetRotation(Pawn.Rotation)
    unsigned char ignoreLook = 0;
    if (!ReadAt<unsigned char>(ctrl + APlayerControllerOffsets::bIgnoreLookInput,
                               &ignoreLook) || ignoreLook)
        return false;  // PlayerInput: aTurn = aLookUp = 0
    DWORD tdBits = 0;
    if (!ReadAt<DWORD>(ctrl + APlayerControllerOffsets::TdFlags, &tdBits) ||
        !(tdBits & ATdPlayerControllerOffsets::bRightThumbStickPassedDeadZone_Mask))
        return false;  // UpdateRotation drops DeltaRot until the stick centres

    uintptr_t p = 0;
    if ((ReadPtrAt(ctrl + ATdPlayerControllerOffsets::CurrentLookAtPoint, &p) && p) ||
        (ReadPtrAt(ctrl + ATdPlayerControllerOffsets::CurrentForcedLookAtPoint, &p) && p))
        return false;  // TdLookAtPoint.UpdateViewRotation replaces DeltaRot outright
    if (ReadPtrAt(ctrl + ATdPlayerControllerOffsets::TargetingPawn, &p) && p) {
        // With a mouse, RawJoyLookRight stays 0.
        // The yaw delta is then the pull onto the target, or about 0.1 of the mouse delta within 100 units of it.
        // Pitch passes through.
        out->latchYaw = false;
        out->cls |= LLC_SoftLock;
    }

    uintptr_t cam = 0;
    int modifiers = 0;
    DWORD camBits = 0;
    if (!ReadPtrAt(ctrl + APlayerControllerOffsets::PlayerCamera, &cam) ||
        !IsReasonableObject(cam) ||
        !ReadAt<int>(cam + ACameraOffsets::ModifierList_Num, &modifiers) ||
        modifiers != 0 ||
        !ReadAt<DWORD>(cam + ACameraOffsets::Flags1D0, &camBits))
        return false;  // Camera.ProcessViewRotation hands the delta to modifiers

    uintptr_t input = 0;
    if (!ReadPtrAt(ctrl + APlayerControllerOffsets::PlayerInput, &input) ||
        !IsReasonableObject(input))
        return false;
    DWORD inputBits = 0;
    if (!ReadAt<DWORD>(input + UPlayerInputOffsets::Bits, &inputBits))
        return false;
    if (inputBits & UPlayerInputOffsets::Bit_UsingGamepad)
        return false;  // stick path is scaled by dt and shaped by the deadzone
    if (inputBits & UPlayerInputOffsets::Bit_MouseSmoothing)
        return false;  // SmoothMouse redistributes counts across ticks
    out->invertMouse = (inputBits & UPlayerInputOffsets::Bit_InvertMouse) != 0;
    if (!ReadAt<float>(input + UPlayerInputOffsets::MouseSensitivity,
                       &out->mouseSensitivity))
        return false;

    uintptr_t pawn = 0;
    if (!ReadPtrAt(ctrl + ATdPlayerControllerOffsets::myPawn, &pawn) ||
        !IsReasonableObject(pawn))
        return false;
    DWORD pawnBits = 0;
    if (!ReadAt<DWORD>(pawn + ATdPawnOffsets::Bits41C, &pawnBits))
        return false;
    if (pawnBits & ATdPawnOffsets::Bit_IsUsingRootRotation) {
        out->latchYaw = false;  // The animation drives pawn yaw, and FaceRotation returns immediately.
        out->cls |= LLC_RootRot;
    }
    int movesNum = 0;
    uintptr_t movesData = 0, move = 0;
    DWORD moveBits = 0;
    if (!ReadAt<unsigned char>(pawn + ATdPawnOffsets::MovementState, &out->moveState) ||
        !ReadAt<int>(pawn + ATdPawnOffsets::Moves_Num, &movesNum) ||
        !ReadPtrAt(pawn + ATdPawnOffsets::Moves_Data, &movesData) ||
        (int)out->moveState >= movesNum || !movesData ||
        !ReadPtrAt(movesData + (uintptr_t)out->moveState * 4, &move) ||
        !IsReasonableObject(move) ||
        !ReadAt<DWORD>(move + UTdMoveOffsets::Bits, &moveBits))
        return false;
    if (moveBits & UTdMoveOffsets::SteerFlagsMask)
        return false;  // look-at target pulls / camera reset move the view on both axes
    if (!LLMoveInputIntact(move, out->moveState))
        return false;
    if ((moveBits & UTdMoveOffsets::Bit_UsePreciseRotation) &&
        !LLReadPreciseFollow(pawn, move, moveBits, pawnBits, worldDt, out))
        return false;

    if (!LLMobilityMultiplier(pawn, &out->handicap))
        return false;
    DWORD tdInputBits = 0;
    if (ReadAt<DWORD>(input + UTdPlayerInputOffsets::Bits, &tdInputBits) &&
        (tdInputBits & UTdPlayerInputOffsets::Bit_WalkButton)) {
        float walkMult = 0.0f;
        if (!ReadAt<float>(input + UTdPlayerInputOffsets::WalkButtonMultiplier,
                           &walkMult))
            return false;
        out->handicap *= walkMult;
    }

    if (!ReadAt<float>(pawn + APawnOffsets::ViewPitchMin, &out->viewPitchMin) ||
        !ReadAt<float>(pawn + APawnOffsets::ViewPitchMax, &out->viewPitchMax))
        return false;
    FRotator pawnRot{}, ctrlRot{};
    if (!ReadAt<FRotator>(pawn + AActorOffsets::Rotation, &pawnRot) ||
        !ReadAt<FRotator>(ctrl + AActorOffsets::Rotation, &ctrlRot))
        return false;
    out->pawnPitch = pawnRot.Pitch;
    out->pawnYaw = pawnRot.Yaw;

    if ((moveBits & UTdMoveOffsets::Bit_ConstrainLook) ||
        (pawnBits & ATdPawnOffsets::Bit_ConstrainLook)) {
        if (!LLResolveConstraintWindow(pawn, move, out->moveState, moveBits,
                                       ctrlRot, pawnRot, out))
            return false;
    }

    // PlayerInput reads GetFOVAngle(): the camera's LockedFOV when locked,
    // otherwise CameraCache.POV.FOV, which the tick sees as the true sim FOV
    // (RestoreSimFOV puts it back before every world tick).
    if (g_simCount < 2)
        return false;
    if (camBits & ACameraOffsets::bLockedFOV_Mask) {
        if (!ReadAt<float>(cam + ACameraOffsets::LockedFOV, &out->fovAngle))
            return false;
    } else {
        out->fovAngle = g_simB.FOV;
    }
    return out->latchYaw || out->latchPitch;
}

// TdMove.ConstrainAxis, inside the window branches (the outside branches are
// the based on dt pull, gated at read time). Int/float order as in the script.
static int LLConstrainAxis(int angle, int minA, int maxA, int delta) {
    int target = angle + delta;
    if (target < minA)      delta -= (target - minA);
    else if (target > maxA) delta -= (target - maxA);
    target = angle + delta;
    if ((float)target < (float)minA * 0.6f) {
        float denom = (float)minA * 0.4f;
        float damp = denom != 0.0f ? fabsf((float)(minA - angle) / denom) : 0.0f;
        int d = (int)((float)delta * damp);
        if (d > delta) delta = d;
    } else if ((float)target > (float)maxA * 0.6f) {
        float denom = (float)maxA * 0.4f;
        float damp = denom != 0.0f ? fabsf((float)(maxA - angle) / denom) : 0.0f;
        int d = (int)((float)delta * damp);
        if (d < delta) delta = d;
    }
    return delta;
}

// The float association matches the script, so the int truncations land on the same values.
// FOVScale is FOV times 0.01111.
static FRotator LLApplyCounts(const LLInputState& s, const FRotator& base,
                              float countsX, float countsY) {
    const float fovScale = s.fovAngle * 0.01111f;
    float aTurn   = countsX * s.mouseSensitivity;
    float aLookUp = countsY * s.mouseSensitivity;
    if (s.invertMouse)
        aLookUp *= -1.0f;
    aLookUp *= (fovScale * 1.78f);
    aTurn   *= fovScale;
    aTurn   *= s.handicap;
    aLookUp *= s.handicap;

    int pitchDiff = NormalizeRotUnits(base.Pitch - s.pawnPitch);
    int steep = (int)(fabsf((float)pitchDiff) / 16384.0f + 0.3f);
    if (steep > 1) steep = 1;
    float rotSpeedMod = 1.0f - (float)steep;
    if (rotSpeedMod < 0.4f) rotSpeedMod = 0.4f;

    int dYaw   = (int)(aTurn * rotSpeedMod);
    int dPitch = (int)aLookUp;

    if (s.constrain) {
        int cdYaw   = NormalizeRotUnits(NormalizeRotUnits(base.Yaw)   - NormalizeRotUnits(s.pawnYaw));
        int cdPitch = NormalizeRotUnits(NormalizeRotUnits(base.Pitch) - NormalizeRotUnits(s.pawnPitch));
        int inPitch = dPitch;
        dYaw   = LLConstrainAxis(cdYaw,   s.minYaw,   s.maxYaw,   dYaw);
        dPitch = LLConstrainAxis(cdPitch, s.minPitch, s.maxPitch, dPitch);
        int removedPitch = inPitch - dPitch;  // PostConstrainCamera's ConstrainAmount.Pitch
        if (s.climbSpill && abs(cdYaw) <= 12000 && removedPitch < 0) {
            int spill = (int)((float)removedPitch * 0.5f);
            if ((float)(base.Yaw - s.pawnYaw) > 0.0f) {
                if (dYaw >= 0) dYaw -= spill;
            } else {
                if (dYaw <= 0) dYaw += spill;
            }
        }
        dYaw   = NormalizeRotUnits(dYaw);
        dPitch = NormalizeRotUnits(dPitch);
    }

    FRotator r = base;
    r.Yaw   += dYaw;
    r.Pitch += dPitch;

    int p = r.Pitch & 65535;
    if ((float)p > s.viewPitchMax && (float)p < 65535.0f + s.viewPitchMin)
        p = (p < 32768) ? (int)s.viewPitchMax : (int)(65535.0f + s.viewPitchMin);
    r.Pitch = p;
    r.Roll = 0;
    return r;
}

// The whole tick: the input transform, then UTdMove::Tick's precise rotation
// yaw follow. *followOut receives the yaw the follow added beyond the input
// (the engine owned part the presentation interpolates across the interval).
static FRotator LLPredictTick(const LLInputState& s, const FRotator& pre,
                              float countsX, float countsY, int* followOut) {
    FRotator r = LLApplyCounts(s, pre, countsX, countsY);
    int follow = 0;
    if (s.follow) {
        int pawnYaw = s.faceRotActive ? NormalizeRotUnits(r.Yaw) : s.pawnYaw;
        int delta = NormalizeRotUnits(s.preciseYaw - pawnYaw);
        int step = (int)((float)delta * (s.tickDt / s.preciseT));
        int newYaw = NormalizeRotUnits(r.Yaw + step);
        follow = ShortestAngleDiff(r.Yaw, newYaw);
        r.Yaw = newYaw;
    } else if (s.followSnap) {
        // The input of this tick is discarded with the snap: the whole yaw
        // change from the pre tick rotation is engine motion.
        follow = ShortestAngleDiff(pre.Yaw, s.preciseYaw);
        r.Yaw = s.preciseYaw;
    }
    *followOut = follow;
    return r;
}

// Decompose the freshly captured sim POV into controller rotation + camera
// effects (called from CaptureSimulationCamera, right after the world tick
// that produced it).
static void LLCaptureDecomposition(const FTPOV& pov, bool firstCapture) {
    uintptr_t ctrl = GetLocalPlayerController();
    FRotator ctrlRot{};
    if (!ctrl || !ReadAt<FRotator>(ctrl + AActorOffsets::Rotation, &ctrlRot)) {
        g_ctrlRotValid = false;
        return;
    }
    FRotator fx = { ShortestAngleDiff(ctrlRot.Pitch, pov.Rotation.Pitch),
                    ShortestAngleDiff(ctrlRot.Yaw,   pov.Rotation.Yaw), 0 };
    if (firstCapture || !g_ctrlRotValid) {
        g_ctrlRotA = ctrlRot;
        g_camFxA = fx;
    } else {
        g_ctrlRotA = g_ctrlRotB;
        g_camFxA = g_camFxB;
    }
    g_ctrlRotB = ctrlRot;
    g_camFxB = fx;
    g_ctrlRotValid = true;
}

static void LLSnapDecomposition() {
    g_ctrlRotA = g_ctrlRotB;
    g_camFxA = g_camFxB;
}

static void LLReset() {
    g_ctrlRotValid = false;
    g_llParityOk = true;
    g_llCleanTicks = 0;
    g_llPredValid = false;
    g_llFollowYaw = 0;
}

static bool LLModelActive() {
    return g_cfg.lateLatchMouse || g_cfg.logging;
}

// Parity prediction: called in Hook_WorldTick right before the primary sim
// tick, after the frame's input pump, so the accumulators hold exactly the
// batch the tick is about to consume.
static LLInputState g_llPredState = {};
static FRotator     g_llPredPre = {};
static float        g_llPredBatchX = 0.0f, g_llPredBatchY = 0.0f;
static int          g_llPredFollow = 0;  // yaw the precise rotation follow adds this tick

static void LLPreTick(float worldDt) {
    g_llPredValid = false;
    if (!LLModelActive())
        return;
    uintptr_t ctrl = GetLocalPlayerController();
    if (!ctrl)
        return;
    LLInputState s;
    if (!LLReadLookState(ctrl, worldDt, &s))
        return;  // gated state: nothing to predict this tick
    uintptr_t input = 0;
    float bx = 0.0f, by = 0.0f;
    FRotator pre{};
    if (!ReadPtrAt(ctrl + APlayerControllerOffsets::PlayerInput, &input) ||
        !ReadAt<float>(input + UPlayerInputOffsets::aMouseX, &bx) ||
        !ReadAt<float>(input + UPlayerInputOffsets::aMouseY, &by) ||
        !ReadAt<FRotator>(ctrl + AActorOffsets::Rotation, &pre))
        return;
    g_llPred = LLPredictTick(s, pre, bx, by, &g_llPredFollow);
    g_llPredState = s;
    g_llPredPre = pre;
    g_llPredBatchX = bx;
    g_llPredBatchY = by;
    g_llPredValid = true;
}

// Compare the tick's actual controller rotation against the prediction.
static void LLPostTick() {
    g_llFollowYaw = 0;
    if (!g_llPredValid)
        return;
    g_llPredValid = false;
    uintptr_t ctrl = GetLocalPlayerController();
    FRotator now{};
    if (!ctrl || !ReadAt<FRotator>(ctrl + AActorOffsets::Rotation, &now))
        return;

    // Only the latched axes are checked.
    // An engine owned axis keeps the plain interpolation
    int errYaw   = g_llPredState.latchYaw   ? ShortestAngleDiff(g_llPred.Yaw,   now.Yaw)   : 0;
    int errPitch = g_llPredState.latchPitch ? ShortestAngleDiff(g_llPred.Pitch, now.Pitch) : 0;
    int err = abs(errYaw) > abs(errPitch) ? abs(errYaw) : abs(errPitch);
    InterlockedIncrement(&g_dbgLlCheck);
    if (err > g_dbgLlErrMax)
        g_dbgLlErrMax = err;

    if (err <= LL_PARITY_TOL_UNITS) {
        if (g_llPredState.latchYaw)
            g_llFollowYaw = g_llPredFollow;
        if (!g_llParityOk && ++g_llCleanTicks >= 2) {
            g_llParityOk = true;
            g_llCleanTicks = 0;
        }
        return;
    }
    g_llParityOk = false;
    g_llCleanTicks = 0;

    // A transition is a move or state class that began inside the tick,
    // or an erring axis that the post tick state no longer latches.
    // Those are counted apart from model errors.
    // A mismatch means the pre tick state still describes the post tick state
    // and the predicted rotation disagreed with the engine.
    LLInputState post;
    bool transition = !LLReadLookState(ctrl, g_simDtF, &post) ||
                      post.moveState != g_llPredState.moveState ||
                      post.cls != g_llPredState.cls ||
                      (errYaw != 0 && !post.latchYaw) ||
                      (errPitch != 0 && !post.latchPitch);
    InterlockedIncrement(transition ? &g_dbgLlTrans : &g_dbgLlMismatch);

    if (g_cfg.logging) {
        static DWORD s_lastLogMs = 0;
        static int   s_burst = 0;
        DWORD nowMs = GetTickCount();
        if (nowMs - s_lastLogMs >= 1000) { s_lastLogMs = nowMs; s_burst = 0; }
        if (++s_burst <= 8) {
            int movePost = -1;
            uintptr_t pawn = 0;
            unsigned char ms = 0;
            if (ReadPtrAt(ctrl + ATdPlayerControllerOffsets::myPawn, &pawn) &&
                IsReasonableObject(pawn) &&
                ReadAt<unsigned char>(pawn + ATdPawnOffsets::MovementState, &ms))
                movePost = ms;
            const LLInputState& ps = g_llPredState;
            char buf[420];
            snprintf(buf, sizeof(buf),
                "ME-LL: %s err=(%d,%d) pred=(%d,%d) act=(%d,%d) pre=(%d,%d) "
                "batch=(%.1f,%.1f) fov=%.2f sens=%.3f handicap=%.3f inv=%d "
                "pawnPitch=%d vp=%.0f..%.0f move=%d->%d cls=%d latch=%d%d "
                "win=(%d..%d,%d..%d) cd=(%d,%d) follow=%d%s\n",
                transition ? "transition" : "mismatch",
                errYaw, errPitch, g_llPred.Yaw, g_llPred.Pitch, now.Yaw, now.Pitch,
                g_llPredPre.Yaw, g_llPredPre.Pitch, g_llPredBatchX, g_llPredBatchY,
                ps.fovAngle, ps.mouseSensitivity, ps.handicap, ps.invertMouse ? 1 : 0,
                ps.pawnPitch, ps.viewPitchMin, ps.viewPitchMax,
                (int)ps.moveState, movePost, (int)ps.cls,
                ps.latchYaw ? 1 : 0, ps.latchPitch ? 1 : 0,
                ps.constrain ? ps.minYaw : 0, ps.constrain ? ps.maxYaw : 0,
                ps.constrain ? ps.minPitch : 0, ps.constrain ? ps.maxPitch : 0,
                NormalizeRotUnits(g_llPredPre.Yaw - ps.pawnYaw),
                NormalizeRotUnits(g_llPredPre.Pitch - ps.pawnPitch),
                g_llPredFollow, ps.followSnap ? "(snap)" : "");
            Log(buf);
        }
    }
}

// Rebuilds the presented rotation on each latched axis.
// The axis is the newest controller rotation, plus pending counts, plus the lerped camera effect.
// An unlatched axis keeps the caller's interpolation.
static void LLApplyResidual(FTPOV* interp, float alpha) {
    if (!g_cfg.lateLatchMouse)
        return;
    if (!g_llParityOk || !g_ctrlRotValid || !IsFirstPersonGameplayCamera()) {
        InterlockedIncrement(&g_dbgLlGate);
        return;
    }
    uintptr_t ctrl = GetLocalPlayerController();
    LLInputState s;
    float cx = 0.0f, cy = 0.0f;
    if (!ctrl || !LLReadLookState(ctrl, g_simDtF, &s) || !MousePendingCounts(&cx, &cy) ||
        (s.cls != 0 && !g_cfg.lateLatchMoves)) {
        InterlockedIncrement(&g_dbgLlGate);
        return;
    }
    FRotator ctrlNow = LLApplyCounts(s, g_ctrlRotB, cx, cy);
    int fxYaw   = g_camFxA.Yaw   + (int)((g_camFxB.Yaw   - g_camFxA.Yaw)   * alpha);
    int fxPitch = g_camFxA.Pitch + (int)((g_camFxB.Pitch - g_camFxA.Pitch) * alpha);
    if (s.latchYaw) {
        // The newest tick's yaw already includes the precise rotation follow.
        // That follow is shown at the interpolation phase.
        // only the input part is latched.
        // A snap tick discards pending yaw, so none of it is shown.
        int followBehind = (int)((float)g_llFollowYaw * (1.0f - alpha));
        int yawNow = s.followSnap ? g_ctrlRotB.Yaw : ctrlNow.Yaw;
        interp->Rotation.Yaw   = yawNow - followBehind + fxYaw;
    }
    if (s.latchPitch)
        interp->Rotation.Pitch = ctrlNow.Pitch + fxPitch;
    // Controller roll is forced to 0, so the interpolated POV roll is entirely camera effect.
    InterlockedIncrement(&g_dbgLlApply);
    if (s.cls != 0)
        InterlockedIncrement(&g_dbgLlMoves);

    if (g_cfg.logging && (cx > 0.5f || cx < -0.5f || cy > 0.5f || cy < -0.5f)) {
        LARGE_INTEGER q;
        if (QueryPerformanceCounter(&q))
            LatProbeLatched(q.QuadPart);
    }
}

// Mean absolute change between consecutive mouse batches, divided by the mean batch.
// When the cap is not an integer multiple of the tick rate, the sim consumes two or three presents of counts per tick.
// The display then spreads each tick across a uniform interval, so a steady hand ripples
// An interger ratio, or the resampler, keeps the value near the noise floor.
static double g_dbgBatchDiffSum = 0.0;
static double g_dbgBatchMeanSum = 0.0;
static float  g_dbgBatchPrev = -1.0f;

static void BatchRippleSample() {
    uintptr_t ctrl = GetLocalPlayerController();
    uintptr_t input = 0;
    float bx = 0.0f, by = 0.0f;
    if (!ctrl ||
        !ReadPtrAt(ctrl + APlayerControllerOffsets::PlayerInput, &input) ||
        !IsReasonableObject(input) ||
        !ReadAt<float>(input + UPlayerInputOffsets::aMouseX, &bx) ||
        !ReadAt<float>(input + UPlayerInputOffsets::aMouseY, &by))
        return;
    float m = fabsf(bx) + fabsf(by);
    if (m > 2.0f && g_dbgBatchPrev > 2.0f) {
        g_dbgBatchDiffSum += fabs((double)m - (double)g_dbgBatchPrev);
        g_dbgBatchMeanSum += ((double)m + (double)g_dbgBatchPrev) * 0.5;
    }
    g_dbgBatchPrev = m;
}

// The pacer parks the frame start before the present, so a residual built from frame start counts ripples.
// This drain runs again just before the draw
// AllowJoystickInput is cleared around the call, so a deflected stick is not applied twice.
static LONGLONG g_engineTickEntryQpc = 0;   // this frame's engine tick pump
static LONGLONG g_inputPumpQpc = 0;         // most recent pump this frame
static bool     g_lateDrainedThisFrame = false;
static LONG     g_dbgLateDrain = 0;         // mid frame drains per log window
static double   g_dbgAgeEma[2] = { 0.0, 0.0 };   // [0] render only, [1] sim

static void StampEngineTickEntry() {
    LARGE_INTEGER q;
    g_engineTickEntryQpc = QueryPerformanceCounter(&q) ? q.QuadPart : 0;
    g_inputPumpQpc = g_engineTickEntryQpc;
}

static void LateDrainMouse() {
    if (!g_cfg.lateLatchMouse || g_lateDrainedThisFrame)
        return;
    g_lateDrainedThisFrame = true;

    uintptr_t engine = 0, client = 0, vtable = 0;
    if (!ReadPtrAt(Addr::GEngine, &engine) || !IsReasonableObject(engine) ||
        !ReadPtrAt(engine + UGameEngineOffsets::Client, &client) ||
        !IsReasonableObject(client) ||
        !ReadPtrAt(client, &vtable) || vtable != Addr::UWindowsClient_vtable ||
        !CanWrite((void*)(client + UWindowsClientOffsets::AllowJoystickInput),
                  sizeof(DWORD)))
        return;

    LARGE_INTEGER q;
    if (!QueryPerformanceCounter(&q))
        return;
    double dt = (g_engineTickEntryQpc && g_qpcSecondsPerCount > 0.0)
                    ? (double)(q.QuadPart - g_engineTickEntryQpc) * g_qpcSecondsPerCount
                    : 0.0;
    if (dt < 0.0 || dt > 0.05)
        dt = 0.0;

    DWORD* allowJoystick =
        (DWORD*)(client + UWindowsClientOffsets::AllowJoystickInput);
    DWORD saved = *allowJoystick;
    *allowJoystick = 0;
    ((fn_ReadMouseDirectInput)Addr::UWindowsClient_ReadMouseDirectInput)(
        (void*)client, nullptr, (float)dt);
    *allowJoystick = saved;

    g_inputPumpQpc = q.QuadPart;
    InterlockedIncrement(&g_dbgLateDrain);
}

static void RecordInputAge(bool simFrame, LONGLONG nowQpc) {
    if (!g_inputPumpQpc || g_qpcSecondsPerCount <= 0.0)
        return;
    double age = (double)(nowQpc - g_inputPumpQpc) * g_qpcSecondsPerCount;
    g_inputPumpQpc = 0;
    if (age < 0.0 || age > 0.05)
        return;
    double& ema = g_dbgAgeEma[simFrame ? 1 : 0];
    ema = ema > 0.0 ? ema + (age - ema) * 0.05 : age;
}

// PlayerInput reads GetFOVAngle, which returns CameraCache.POV.FOV.
// Presents overwrite that field with the interpolated FOV.
// This puts the sim value back before each world tick.
static void RestoreSimFOV() {
    if (g_simCount < 2)
        return;
    FTPOV* pov = GetCameraPOVPtr();
    if (pov)
        pov->FOV = g_simB.FOV;
}

// ULocalPlayer statics consumed by CalcSceneView.
static void ClearViewOverride() {
    if (g_overrideGlobalsOk)
        *(volatile int*)Addr::LP_bOverrideView = 0;
    g_overrideActive = false;
}

static float LengthSq(const FVector& v) {
    return v.X * v.X + v.Y * v.Y + v.Z * v.Z;
}

static void LatProbeVisible(float alpha, LONGLONG nowQpc);  // latency probe (below)

static void UpdateViewOverride() {
    g_lastInterpValid = false;
    if (!g_overrideGlobalsOk)
        return;

    if (!g_seeded || g_simCount < 2 || !ShouldApplyViewOverride()) {
        if (g_overrideActive) ClearViewOverride();
        return;
    }

    float alpha = (float)(g_accumulator / g_simDt);
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    if (alpha < g_dbgAlphaMin) g_dbgAlphaMin = alpha;
    if (alpha > g_dbgAlphaMax) g_dbgAlphaMax = alpha;

    FTPOV interp = LerpPOV(g_simA, g_simB, alpha);

    // Late latch residual before g_lastInterp is latched, so the first person
    // meshes are compensated against the rotation the camera presents with.
    LLApplyResidual(&interp, alpha);

    g_lastInterp = interp;
    g_lastAlpha = alpha;
    g_lastInterpValid = true;

    *(FVector*)Addr::LP_OverrideLocation  = interp.Location;
    *(FRotator*)Addr::LP_OverrideRotation = interp.Rotation;
    *(volatile int*)Addr::LP_bOverrideView = 1;
    g_overrideActive = true;

    // The override branch does not cover FOV.
    // CalcSceneView reads it from CameraCache.POV.FOV.
    // on a sim frame the capture has already stored the fresh value.
    // On a render only frame nothing recomputes it
    FTPOV* pov = GetCameraPOVPtr();
    if (pov)
        pov->FOV = interp.FOV;

    if (g_cfg.logging) {
        LARGE_INTEGER lq;
        if (QueryPerformanceCounter(&lq)) {
            LatProbeVisible(alpha, lq.QuadPart);
            RecordInputAge(g_inSimulationTick, lq.QuadPart);
        }
    }

    InterlockedIncrement(&g_dbgOverrideWrite);
}

static bool IsCameraCut(const FTPOV& a, const FTPOV& b) {
    FVector step = SubVector(b.Location, a.Location);
    if (LengthSq(step) > MAX_INTERP_STEP_UU * MAX_INTERP_STEP_UU)
        return true;

    if (abs(ShortestAngleDiff(a.Rotation.Pitch, b.Rotation.Pitch)) > MAX_INTERP_STEP_ROT ||
        abs(ShortestAngleDiff(a.Rotation.Yaw,   b.Rotation.Yaw))   > MAX_INTERP_STEP_ROT ||
        abs(ShortestAngleDiff(a.Rotation.Roll,  b.Rotation.Roll))  > MAX_INTERP_STEP_ROT)
        return true;

    return fabsf(b.FOV - a.FOV) > MAX_INTERP_STEP_FOV;
}

static void CaptureSimulationCamera() {
    FTPOV* pov = GetCameraPOVPtr();
    if (!pov) return;

    if (g_simCount == 0) {
        g_simA = *pov;
        g_simB = *pov;
        g_simCount = 2;
        LLCaptureDecomposition(*pov, true);
        return;
    }

    g_simA = g_simB;
    g_simB = *pov;
    if (g_simCount < 2) g_simCount++;
    LLCaptureDecomposition(*pov, false);

    if (IsCameraCut(g_simA, g_simB)) {
        g_simA = g_simB;
        LLSnapDecomposition();
        InterlockedIncrement(&g_dbgCutSnap);
    }
}

// First person meshes are posed on the sim camera, so an interpolated camera leaves them jittering.
// The push copies by value, and the component fields are restored before the call returns

static bool IsPushableComponent(uintptr_t comp) {
    if (!IsReasonableObject(comp))
        return false;
    if (!CanRead((void*)(comp + UPrimitiveComponentOffsets::Scene),
                 0x1B8))  // covers all fields we touch through +0x1B4
        return false;
    if (!CanWrite((void*)(comp + UPrimitiveComponentOffsets::LocalToWorld),
                  sizeof(FMatrix)))
        return false;

    uintptr_t scene = *(uintptr_t*)(comp + UPrimitiveComponentOffsets::Scene);
    uintptr_t sceneInfo = *(uintptr_t*)(comp + UPrimitiveComponentOffsets::SceneInfo);
    DWORD flags = *(DWORD*)(comp + UPrimitiveComponentOffsets::ComponentFlags);
    DWORD detachFence = *(DWORD*)(comp + UPrimitiveComponentOffsets::DetachFence);

    return IsReasonableObject(scene) && sceneInfo != 0 &&
           (flags & 1) != 0 && detachFence == 0;
}

// A live component keeps the vtable and scene pointer it had at capture.
// Freed memory can stay readable, so a readability check is not enough.
// An entry that fails is dropped with no engine call, because its proxy is gone too.
static bool IsEntryStillValid(const ViewModelEntry& e) {
    uintptr_t vt = 0;
    if (!ReadPtrAt(e.component, &vt) || vt != e.vtable)
        return false;
    uintptr_t scene = 0;
    if (!ReadPtrAt(e.component + UPrimitiveComponentOffsets::Scene, &scene) ||
        scene != e.scene)
        return false;
    return IsPushableComponent(e.component);
}

static void CaptureViewModelPoseInto(ViewModelPose* pose, uintptr_t comp,
                                     bool skeletal) {
    pose->localToWorld =
        *(FMatrix*)(comp + UPrimitiveComponentOffsets::LocalToWorld);
    pose->bounds =
        *(FBoxSphereBounds*)(comp + UPrimitiveComponentOffsets::Bounds);
    pose->boneCount = 0;

    // A mover is smaller than a skeletal component.
    // The bone offsets below would read past it.
    if (!ENABLE_BONE_INTERPOLATION || !skeletal)
        return;

    // SpaceBases are component space bone transforms.
    // When ParentAnimComponent is set, the renderer reads the parent's array.
    // Leave boneCount at 0 and let the parent's entry carry the bones.
    uintptr_t parentAnim = 0;
    ReadPtrAt(comp + USkeletalMeshComponentOffsets::ParentAnimComponent,
              &parentAnim);
    if (parentAnim)
        return;

    int num = 0;
    uintptr_t data = 0;
    if (!ReadAt<int>(comp + USkeletalMeshComponentOffsets::SpaceBases_Num, &num) ||
        !ReadPtrAt(comp + USkeletalMeshComponentOffsets::SpaceBases_Data, &data))
        return;
    if (num <= 0 || num > MAX_VIEWMODEL_BONES ||
        !CanRead((void*)data, (size_t)num * sizeof(FMatrix)))
        return;

    memcpy(pose->bones, (void*)data, (size_t)num * sizeof(FMatrix));
    pose->boneCount = num;
}

// Previous tick's entries, kept so a rebuilt entry can inherit its prev pose.
static ViewModelEntry g_vmLastTick[MAX_VIEWMODEL_COMPONENTS] = {};
static int g_vmLastTickCount = 0;

static int FindViewModelEntry(uintptr_t comp);

// Root of an actor's Base chain.
// An actor with no Base is its own root.
// The walk is bounded, and callers only compare the pointer.
static uintptr_t RideRoot(uintptr_t actor) {
    for (int hops = 0; actor && hops < MAX_BASE_CHAIN; ++hops) {
        uintptr_t base = 0;
        if (!ReadPtrAt(actor + AActorOffsets::Base, &base) ||
            !IsReasonableObject(base))
            break;
        actor = base;
    }
    return actor;
}

// True when the mover shares the ridden platform's Base chain root,
// or its per tick step matches that platform's step within the lead tolerance.
// A stationary ridden root matches only by root
static bool InRiddenFrame(uintptr_t ownerRoot, const FVector& step) {
    if (!g_pawnRideRoot)
        return false;
    if (ownerRoot && ownerRoot == g_pawnRideRoot)
        return true;
    if (!g_rideStepValid)
        return false;
    float rideLen = sqrtf(LengthSq(g_rideStep));
    if (rideLen <= MOVER_LEAD_MIN_STEP_UU)
        return false;
    float diff = sqrtf(LengthSq(SubVector(step, g_rideStep)));
    return diff <= fmaxf(MOVER_LEAD_ABS_TOL_UU, MOVER_LEAD_REL_TOL * rideLen);
}

static void AddViewModelComponent(uintptr_t comp, bool cameraRelative,
                                  bool skeletal = true) {
    if (!comp)
        return;
    if (g_vmCount >= MAX_VIEWMODEL_COMPONENTS) {
        InterlockedIncrement(&g_dbgVmCapHit);
        return;
    }
    for (int i = 0; i < g_vmCount; ++i)
        if (g_vm[i].component == comp) return;
    if (!IsPushableComponent(comp))
        return;

    // Beyond the configured radius the 62 Hz pose is not visible as a step.
    // First person meshes stay inside the radius.
    // Movers are few, and they stay visable at distance, so they are not culled.
    if (!cameraRelative && skeletal && g_worldMeshRangeSqUU > 0.0f) {
        FVector origin{};
        if (ReadAt<FVector>(comp + UPrimitiveComponentOffsets::Bounds,
                            &origin)) {
            FVector d = SubVector(origin, g_simB.Location);
            if (LengthSq(d) > g_worldMeshRangeSqUU) {
                InterlockedIncrement(&g_dbgVmFar);
                return;
            }
        }
    }

    // Continuity: this component's pose from the previous tick becomes the
    // "prev" endpoint of the new interpolation pair.
    ViewModelEntry& e = g_vm[g_vmCount++];
    e.component = comp;
    e.vtable = *(uintptr_t*)comp;  // readable per IsPushableComponent above
    e.scene = *(uintptr_t*)(comp + UPrimitiveComponentOffsets::Scene);
    e.cameraRelative = cameraRelative;
    e.skeletal = skeletal;
    e.parentAnim = 0;
    if (skeletal)
        ReadPtrAt(comp + USkeletalMeshComponentOffsets::ParentAnimComponent,
                  &e.parentAnim);
    e.hasPrev = false;
    e.lead = false;
    e.ownerRoot = 0;
    if (!cameraRelative && !skeletal) {
        uintptr_t owner = 0;
        if (ReadPtrAt(comp + UPrimitiveComponentOffsets::Owner, &owner) && owner)
            e.ownerRoot = RideRoot(owner);
    }
    const ViewModelEntry* lastTick = nullptr;
    for (int i = 0; i < g_vmLastTickCount; ++i) {
        if (g_vmLastTick[i].component == comp) {
            e.hasPrev = true;
            e.prev = g_vmLastTick[i].curr;
            lastTick = &g_vmLastTick[i];
            break;
        }
    }
    CaptureViewModelPoseInto(&e.curr, comp, skeletal);
    if (e.hasPrev && e.prev.boneCount != e.curr.boneCount)
        e.hasPrev = false;  // LOD/skeleton changed between ticks

    if (!cameraRelative && e.hasPrev) {
        // A step past the teleport distance snaps.
        FVector step = SubVector(
            (FVector&)e.curr.localToWorld.M[3][0],
            (FVector&)e.prev.localToWorld.M[3][0]);
        if (LengthSq(step) > WORLD_SKEL_TELEPORT_UU * WORLD_SKEL_TELEPORT_UU)
            e.hasPrev = false;

        // Root motion splits the step between the actor location and the bone root.
        // Those two captures do not share a phase, so interpolating them oscillates.
        // The engine pose is kept instead
        unsigned char rmm = RMM_Ignore;
        if (skeletal &&
            ReadAt<unsigned char>(
                comp + USkeletalMeshComponentOffsets::RootMotionMode, &rmm) &&
            rmm != RMM_Ignore)
            e.hasPrev = false;
    }

    // Mover lead: engage only when last tick's constant velocity prediction
    // proves out against this tick's actual pose. Needs three poses: lastTick->prev/curr + fresh.
    if (!cameraRelative && !skeletal && e.hasPrev && g_cfg.moverLead &&
        lastTick && lastTick->hasPrev) {
        FMatrix pred{};
        RigidLerpMatrix(lastTick->prev.localToWorld,
                        lastTick->curr.localToWorld, 2.0f, &pred);
        FVector predT = { pred.M[3][0], pred.M[3][1], pred.M[3][2] };
        const FVector& actT  = (FVector&)e.curr.localToWorld.M[3][0];
        const FVector& prevT = (FVector&)e.prev.localToWorld.M[3][0];
        FVector step = SubVector(actT, prevT);
        float stepLen = sqrtf(LengthSq(step));
        float predErr = sqrtf(LengthSq(SubVector(predT, actT)));
        if (stepLen > MOVER_LEAD_MIN_STEP_UU &&
            predErr <= fmaxf(MOVER_LEAD_ABS_TOL_UU,
                             MOVER_LEAD_REL_TOL * stepLen) &&
            MatrixBasisDeltaSmall(e.prev.localToWorld, e.curr.localToWorld,
                                  MOVER_LEAD_MIN_ROT_DOT))
            e.lead = !InRiddenFrame(e.ownerRoot, step);
    }
}

// Moving lights: pool membership and pose capture (see LightEntry).
static int FindLightEntry(uintptr_t comp) {
    for (int i = 0; i < g_lightCount; ++i)
        if (g_lights[i].component == comp) return i;
    return -1;
}

// A light can be interpolated while it is still the object that registered,
// it is attached, and it has a scene info.
// AddLight skips a light whose lighting is entirely precomputed, so SceneInfo stays null.
// A light owned by a light environment is per actor bookkeeping, not a world light.
static bool IsTrackableLight(uintptr_t comp, uintptr_t vtable) {
    uintptr_t vt = 0;
    if (!ReadPtrAt(comp, &vt) || vt != vtable || !IsEngineVtablePtr(vt))
        return false;
    if (!CanRead((void*)(comp + ULightComponentOffsets::Scene),
                 ULightComponentOffsets::LightEnvironment + 4 -
                     ULightComponentOffsets::Scene))
        return false;
    if (!CanWrite((void*)(comp + ULightComponentOffsets::WorldToLight),
                  2 * sizeof(FMatrix)))
        return false;
    DWORD flags = *(DWORD*)(comp + ULightComponentOffsets::ComponentFlags);
    uintptr_t sceneInfo = *(uintptr_t*)(comp + ULightComponentOffsets::SceneInfo);
    uintptr_t scene = *(uintptr_t*)(comp + ULightComponentOffsets::Scene);
    uintptr_t lightEnv = *(uintptr_t*)(comp + ULightComponentOffsets::LightEnvironment);
    return (flags & 1) && sceneInfo && scene && !lightEnv;
}

static bool IsLightEntryStillValid(const LightEntry& e) {
    return IsTrackableLight(e.component, e.vtable) &&
           *(uintptr_t*)(e.component + ULightComponentOffsets::Scene) == e.scene;
}

static void AddLightComponent(uintptr_t comp, uintptr_t vtable) {
    if (g_lightCount >= MAX_LIGHTS || FindLightEntry(comp) >= 0 ||
        !IsTrackableLight(comp, vtable))
        return;

    LightEntry& e = g_lights[g_lightCount++];
    e.component = comp;
    e.vtable = vtable;
    e.scene = *(uintptr_t*)(comp + ULightComponentOffsets::Scene);
    e.curr = *(FMatrix*)(comp + ULightComponentOffsets::LightToWorld);
    e.hasPrev = false;
    e.lead = false;
    e.ownerRoot = 0;
    uintptr_t owner = 0;
    if (ReadPtrAt(comp + ULightComponentOffsets::Owner, &owner) && owner)
        e.ownerRoot = RideRoot(owner);

    for (int i = 0; i < g_lightsLastTickCount; ++i) {
        if (g_lightsLastTick[i].component == comp) {
            e.prev = g_lightsLastTick[i].curr;
            e.hasPrev = true;
            break;
        }
    }
    if (e.hasPrev) {
        FVector step = SubVector((FVector&)e.curr.M[3][0], (FVector&)e.prev.M[3][0]);
        if (LengthSq(step) > WORLD_SKEL_TELEPORT_UU * WORLD_SKEL_TELEPORT_UU)
            e.hasPrev = false;  // teleport: snap, don't sweep
    }

    // Phase follows the mover that carries the light (shared Base chain
    // root): a light on a led platform leads with it, a light on the ridden
    // platform stays behind phase with it and the camera.
    for (int i = 0; e.ownerRoot && i < g_vmCount; ++i) {
        const ViewModelEntry& m = g_vm[i];
        if (!m.cameraRelative && !m.skeletal && m.ownerRoot == e.ownerRoot) {
            e.lead = m.lead;
            break;
        }
    }
}

// Records every skeletal component the engine animates, so capture needs no class list.
static void RegisterSkeletalComponent(uintptr_t comp) {
    DWORD tick = g_simTickCounter;
    int freeSlot = -1;
    int oldestSlot = 0;
    DWORD oldestSeen = 0xFFFFFFFF;
    for (int i = 0; i < MAX_SKEL_REGISTRY; ++i) {
        if (g_skelRegistry[i].component == comp) {
            g_skelRegistry[i].lastSeenTick = tick;
            return;
        }
        if (!g_skelRegistry[i].component && freeSlot < 0)
            freeSlot = i;
        if (g_skelRegistry[i].lastSeenTick < oldestSeen) {
            oldestSeen = g_skelRegistry[i].lastSeenTick;
            oldestSlot = i;
        }
    }
    int slot = freeSlot >= 0 ? freeSlot : oldestSlot;
    g_skelRegistry[slot].component = comp;
    g_skelRegistry[slot].lastSeenTick = tick;
}

static void __fastcall Hook_SkelUpdateTransform(void* comp, void* /*edx*/) {
    RegisterSkeletalComponent((uintptr_t)comp);
    g_origSkelUpdateTransform(comp);
}

// Registry of components the engine is moving: records the last two sim ticks
// each was seen in, so a capture can require consecutive tick motion. The
// vtable is captured while the engine is calling into the component (alive).
static void RegisterMovingComponent(MoverRegistryEntry* reg, int count,
                                    uintptr_t comp) {
    DWORD tick = g_simTickCounter;
    int freeSlot = -1;
    int oldestSlot = 0;
    DWORD oldestSeen = 0xFFFFFFFF;
    for (int i = 0; i < count; ++i) {
        if (reg[i].component == comp) {
            if (reg[i].lastSeenTick != tick) {
                reg[i].prevSeenTick = reg[i].lastSeenTick;
                reg[i].lastSeenTick = tick;
            }
            return;
        }
        if (!reg[i].component && freeSlot < 0)
            freeSlot = i;
        if (reg[i].lastSeenTick < oldestSeen) {
            oldestSeen = reg[i].lastSeenTick;
            oldestSlot = i;
        }
    }
    int slot = freeSlot >= 0 ? freeSlot : oldestSlot;
    reg[slot].component = comp;
    reg[slot].vtable = 0;
    ReadPtrAt(comp, &reg[slot].vtable);
    reg[slot].lastSeenTick = tick;
    reg[slot].prevSeenTick = 0;
}

// The engine calls this for every primitive whose render transform changed.
// Skeletal components use their own registry.
// Movers, physics props, and emitters land here.
static void __fastcall Hook_SceneUpdatePrimitiveTransform(void* scene,
                                                          void* /*edx*/,
                                                          void* comp) {
    if (!g_inOurPush)
        RegisterMovingComponent(g_moverRegistry, MAX_MOVER_REGISTRY,
                                (uintptr_t)comp);
    g_origSceneUpdateTransform(scene, nullptr, comp);
}

// Called from ULightComponent::UpdateTransform for every light that moved.
static void __fastcall Hook_SceneUpdateLightTransform(void* scene,
                                                      void* /*edx*/,
                                                      void* comp) {
    if (!g_inOurPush)
        RegisterMovingComponent(g_lightRegistry, MAX_LIGHT_REGISTRY,
                                (uintptr_t)comp);
    g_origSceneUpdateLightTransform(scene, nullptr, comp);
}

// Only a movable primitive keeps its proxy across a transform update.
// A static mesh rebuilds that proxy, so it is not pushed every present.
static bool TakesCheapTransformPath(uintptr_t comp) {
    uintptr_t vtable = 0;
    if (!ReadPtrAt(comp, &vtable) || !IsEngineVtablePtr(vtable))
        return false;
    uintptr_t fn = 0;
    if (!ReadPtrAt(vtable + 0x1D4, &fn) || !IsEngineCodePtr(fn))
        return false;
    fn_ShouldRecreateProxy shouldRecreate = (fn_ShouldRecreateProxy)fn;
    return shouldRecreate((void*)comp, nullptr) == 0;
}

static bool IsKnownSkeletalVtable(uintptr_t comp) {
    uintptr_t vtable = 0;
    if (!ReadPtrAt(comp, &vtable))
        return false;
    // Skeletal component vtables share the UpdateTransform impl at slot +0x120.
    uintptr_t updateFn = 0;
    return ReadPtrAt(vtable + 0x120, &updateFn) &&
           updateFn == Addr::USkeletalMeshComponent_UpdateTransform;
}

static uintptr_t GetWorldInfoPtr() {
    uintptr_t world = 0;
    if (!ReadPtrAt(Addr::GWorld, &world) || !IsReasonableObject(world))
        return 0;
    fn_GetWorldInfo getWorldInfo = (fn_GetWorldInfo)Addr::UWorld_GetWorldInfo;
    uintptr_t worldInfo = (uintptr_t)getWorldInfo((void*)world, nullptr);
    return IsReasonableObject(worldInfo) ? worldInfo : 0;
}

// World mesh capture filter: only track components rendered recently.
static bool WasRecentlyRendered(uintptr_t comp, float timeSeconds) {
    float lastRender = 0.0f;
    if (!ReadAt<float>(comp + UPrimitiveComponentOffsets::LastRenderTime,
                       &lastRender))
        return false;
    return lastRender > 0.0f &&
           (timeSeconds - lastRender) < WORLD_SKEL_RENDERED_WINDOW;
}

static void CaptureWorldSkeletalMeshes(uintptr_t playerPawn) {
    if (!g_cfg.smoothWorldMeshes && !g_cfg.smoothMovers && !g_cfg.smoothLights)
        return;

    uintptr_t worldInfo = GetWorldInfoPtr();
    if (!worldInfo) {
        g_dbgPawnWalk = -1;
        return;
    }

    float timeSeconds = 0.0f;
    ReadAt<float>(worldInfo + AWorldInfoOffsets::TimeSeconds, &timeSeconds);

    // The ridden platform is the root of the pawn's Base chain.
    // A null Base means the pawn is airborne, and the last root is kept.
    // Landing somewhere else clears it.
    if (!playerPawn) {
        g_pawnRideRoot = 0;
    } else {
        uintptr_t base = 0;
        if (ReadPtrAt(playerPawn + AActorOffsets::Base, &base) && base)
            g_pawnRideRoot = RideRoot(base);
    }

    // The ridden root's per tick step, for the lockstep test: a part driven by
    // its own matinee in sync with the platform has no Base relation to it.
    g_rideStepValid = false;
    FVector rideLoc{};
    if (g_pawnRideRoot && IsReasonableObject(g_pawnRideRoot) &&
        ReadAt<FVector>(g_pawnRideRoot + AActorOffsets::Location, &rideLoc)) {
        if (g_rideStepActor == g_pawnRideRoot) {
            g_rideStep = SubVector(rideLoc, g_rideStepLastLoc);
            g_rideStepValid = true;
        }
        g_rideStepActor = g_pawnRideRoot;
        g_rideStepLastLoc = rideLoc;
    } else {
        g_rideStepActor = 0;
    }

    // Mesh3p draws Faith's shadow in first person.
    // Shadow only rendering may not refresh LastRenderTime, so it is always included.
    if (playerPawn && g_cfg.smoothWorldMeshes) {
        uintptr_t mesh3p = 0;
        if (ReadPtrAt(playerPawn + ATdPawnOffsets::Mesh3p, &mesh3p))
            AddViewModelComponent(mesh3p, false);
    }

    // Last tick's world entries are re-added before new candidates.
    // An entry that drops out of the pool loses its previous pose and steps at the sim rate.
    // A component freed since last tick fails the identity check and is not re-added.
    for (int i = 0; i < g_vmLastTickCount &&
                    g_vmCount < MAX_VIEWMODEL_COMPONENTS; ++i) {
        if (g_vmLastTick[i].cameraRelative || !g_vmLastTick[i].component)
            continue;
        if (IsEntryStillValid(g_vmLastTick[i]) &&
            WasRecentlyRendered(g_vmLastTick[i].component, timeSeconds))
            AddViewModelComponent(g_vmLastTick[i].component, false,
                                  g_vmLastTick[i].skeletal);
    }

    int walked = 0;
    uintptr_t pawn = 0;
    if (g_cfg.smoothWorldMeshes)
        ReadPtrAt(worldInfo + AWorldInfoOffsets::PawnList, &pawn);
    for (int hops = 0;
         pawn && hops < MAX_PAWN_WALK && g_vmCount < MAX_VIEWMODEL_COMPONENTS;
         ++hops) {
        if (!IsReasonableObject(pawn))
            break;
        ++walked;

        if (pawn != playerPawn) {
            uintptr_t mesh = 0;
            if (ReadPtrAt(pawn + APawnOffsets::Mesh, &mesh) &&
                WasRecentlyRendered(mesh, timeSeconds))
                AddViewModelComponent(mesh, false);

            uintptr_t weapon = 0;
            uintptr_t wmesh = 0;
            if (ReadPtrAt(pawn + APawnOffsets::Weapon, &weapon) &&
                IsReasonableObject(weapon) &&
                ReadPtrAt(weapon + ATdWeaponOffsets::Mesh3p, &wmesh) &&
                WasRecentlyRendered(wmesh, timeSeconds))
                AddViewModelComponent(wmesh, false);
        }

        uintptr_t next = 0;
        if (!ReadPtrAt(pawn + APawnOffsets::NextPawn, &next))
            break;
        pawn = next;
    }

    // Anything else the engine animated in the last two sim ticks.
    // IsKnownSkeletalVtable also rejects a freed entry, whose vtable slot no longer holds UpdateTransform.
    DWORD tick = g_simTickCounter;
    for (int i = 0; g_cfg.smoothWorldMeshes && i < MAX_SKEL_REGISTRY &&
                    g_vmCount < MAX_VIEWMODEL_COMPONENTS; ++i) {
        uintptr_t comp = g_skelRegistry[i].component;
        if (!comp) continue;
        if (tick - g_skelRegistry[i].lastSeenTick > 2) {
            g_skelRegistry[i].component = 0;  // The animation has stopped.
            continue;
        }
        if (IsKnownSkeletalVtable(comp) &&
            WasRecentlyRendered(comp, timeSeconds))
            AddViewModelComponent(comp, false);
    }

    // Primitives whose scene transform updated on both of the last two ticks.
    // A single update is a streaming attach or a teleport.
    // Skeletal components are excluded.
    // Only a movable primitive whose proxy survives the update qualifies.
    if (g_cfg.smoothMovers) {
        for (int i = 0; i < MAX_MOVER_REGISTRY &&
                        g_vmCount < MAX_VIEWMODEL_COMPONENTS; ++i) {
            uintptr_t comp = g_moverRegistry[i].component;
            if (!comp) continue;
            if (tick - g_moverRegistry[i].lastSeenTick > 2) {
                g_moverRegistry[i].component = 0;  // stopped moving
                continue;
            }
            if (g_moverRegistry[i].lastSeenTick -
                g_moverRegistry[i].prevSeenTick > 1)
                continue;  // not yet seen in consecutive ticks
            // Identity: vtable must still match the one recorded while the
            // engine was calling into the component (freed since = reject
            // before the virtual call in TakesCheapTransformPath).
            uintptr_t vt = 0;
            if (!ReadPtrAt(comp, &vt) || !vt ||
                vt != g_moverRegistry[i].vtable) {
                g_moverRegistry[i].component = 0;
                continue;
            }
            if (IsKnownSkeletalVtable(comp))
                continue;
            if (WasRecentlyRendered(comp, timeSeconds) &&
                TakesCheapTransformPath(comp))
                AddViewModelComponent(comp, false, /*skeletal=*/false);
        }
    }

    // A component that leaves the pool would keep the last interpolated pose on its proxy.
    // Push the live pose once.
    // This also runs when streaming or GC has freed the component.
    // The identity check then skips it, because a freed component has no proxy left.
    for (int i = 0; i < g_vmLastTickCount; ++i) {
        uintptr_t comp = g_vmLastTick[i].component;
        if (!comp || g_vmLastTick[i].cameraRelative)
            continue;
        if (FindViewModelEntry(comp) >= 0)
            continue;  // still tracked
        if (!IsEntryStillValid(g_vmLastTick[i])) {
            InterlockedIncrement(&g_dbgVmStale);
            continue;
        }

        g_inOurPush = true;
        fn_UpdatePrimitiveTransform updateTransform = g_origSceneUpdateTransform
            ? (fn_UpdatePrimitiveTransform)g_origSceneUpdateTransform
            : (fn_UpdatePrimitiveTransform)Addr::FScene_UpdatePrimitiveTransform;
        updateTransform((void*)g_vmLastTick[i].scene, nullptr, (void*)comp);

        if (g_vmLastTick[i].skeletal) {
            // The live SpaceBases are already the engine pose, so they are not swapped.
            uintptr_t meshObject = 0;
            uintptr_t updateFn = 0;
            if (ReadPtrAt(comp + USkeletalMeshComponentOffsets::MeshObject,
                          &meshObject) && IsReasonableObject(meshObject)) {
                uintptr_t moVtable = *(uintptr_t*)meshObject;  // in .rdata
                ReadPtrAt(moVtable + FSkeletalMeshObjectOffsets::Update_VtblSlot,
                          &updateFn);
            }
            if (IsEngineCodePtr(updateFn)) {
                int lod = 0;
                ReadAt<int>(comp + USkeletalMeshComponentOffsets::PredictedLODLevel,
                            &lod);
                if (lod < 0) lod = 0;
                if (*(uintptr_t*)meshObject ==
                    Addr::FSkeletalMeshObjectCPUSkin_vtable)
                    *(DWORD*)(meshObject + 0x64) = 0;
                ((fn_MeshObjectUpdate)updateFn)(
                    (void*)meshObject, nullptr, lod, (void*)comp,
                    (void*)(comp + USkeletalMeshComponentOffsets::ActiveMorphs));
            }
        }
        g_inOurPush = false;
    }

    // Light sweep: same consecutive ticks rule as movers. Runs after the mover
    // sweep so a light can inherit the phase of the mover carrying it.
    if (g_cfg.smoothLights) {
        for (int i = 0; i < MAX_LIGHT_REGISTRY && g_lightCount < MAX_LIGHTS; ++i) {
            MoverRegistryEntry& r = g_lightRegistry[i];
            if (!r.component) continue;
            if (tick - r.lastSeenTick > 2) {
                r.component = 0;  // stopped moving
                continue;
            }
            if (r.lastSeenTick - r.prevSeenTick > 1)
                continue;  // not yet seen in consecutive ticks
            AddLightComponent(r.component, r.vtable);
        }
    }

    // Finalise dropped lights: the scene info holds our last interpolated
    // pose and the engine sends no update for a light that stopped moving, so
    // rebuild it once from the component's own (exact) matrices.
    fn_SceneUpdateTransform updateLight = g_origSceneUpdateLightTransform
        ? g_origSceneUpdateLightTransform
        : (fn_SceneUpdateTransform)Addr::FScene_UpdateLightTransform;
    for (int i = 0; i < g_lightsLastTickCount; ++i) {
        const LightEntry& e = g_lightsLastTick[i];
        if (!e.component || FindLightEntry(e.component) >= 0)
            continue;
        if (!IsLightEntryStillValid(e)) {
            InterlockedIncrement(&g_dbgVmStale);
            continue;
        }
        g_inOurPush = true;
        updateLight((void*)e.scene, nullptr, (void*)e.component);
        g_inOurPush = false;
    }

    int worldCount = 0;
    int moverCount = 0;
    for (int i = 0; i < g_vmCount; ++i) {
        if (!g_vm[i].cameraRelative) ++worldCount;
        if (!g_vm[i].skeletal) ++moverCount;
    }
    g_dbgVmWorld = worldCount;
    g_dbgVmMover = moverCount;
    g_dbgVmLight = g_lightCount;
    g_dbgPawnWalk = walked;
}

// Drops the previous tick pose pairs so interpolation reseeds from the next
// capture (stalls, reseeds: nothing sweeps from a pre stall pose).
static void DropPosePairs() {
    g_vmLastTickCount = 0;
    g_lightsLastTickCount = 0;
}

static void CaptureViewModelPose() {
    g_vmLastTickCount = g_vmCount;
    for (int i = 0; i < g_vmCount; ++i) g_vmLastTick[i] = g_vm[i];
    g_vmCount = 0;
    g_lightsLastTickCount = g_lightCount;
    for (int i = 0; i < g_lightCount; ++i) g_lightsLastTick[i] = g_lights[i];
    g_lightCount = 0;

    g_vmCamPrev = g_vmCamCurr;
    g_vmCamCurr = g_simB;

    uintptr_t pawn = GetPlayerPawnPtr();

    if (g_cfg.smoothFirstPerson && pawn && IsFirstPersonGameplayCamera()) {
        uintptr_t mesh = 0;
        if (ReadPtrAt(pawn + ATdPawnOffsets::Mesh1p, &mesh))
            AddViewModelComponent(mesh, true);
        if (ReadPtrAt(pawn + ATdPawnOffsets::Mesh1pLowerBody, &mesh))
            AddViewModelComponent(mesh, true);

        uintptr_t weapon = 0;
        if (!ReadPtrAt(pawn + ATdPawnOffsets::MyWeapon, &weapon) ||
            !IsReasonableObject(weapon)) {
            ReadPtrAt(pawn + APawnOffsets::Weapon, &weapon);
        }
        if (IsReasonableObject(weapon) &&
            ReadPtrAt(weapon + ATdWeaponOffsets::Mesh1p, &mesh))
            AddViewModelComponent(mesh, true);
    }

    CaptureWorldSkeletalMeshes(pawn);
}

// Bone counts for the current push. Zero means that slot was not built.
// Reset at the start of each PushViewModelCompensation pass.
static int g_scratchBoneCount[MAX_VIEWMODEL_COMPONENTS] = {};

static int FindViewModelEntry(uintptr_t comp) {
    for (int i = 0; i < g_vmCount; ++i)
        if (g_vm[i].component == comp) return i;
    return -1;
}

// Swaps SpaceBases to the scratch buffer for FSkeletalMeshObject::Update.
// The dynamic-data snapshot is built by value on the game thread, so the pointer is restored when the call returns.
// A component with ParentAnimComponent renders the parent's bones.
// Mesh1pLowerBody does this with Mesh1p.
// Entries are captured parent first, so the parent's scratch already exists.
static void PushInterpolatedBones(ViewModelEntry& e, int slot, float alpha) {
    if (!ENABLE_BONE_INTERPOLATION || !e.skeletal)
        return;

    uintptr_t comp = e.component;
    uintptr_t meshObject = 0;
    if (!ReadPtrAt(comp + USkeletalMeshComponentOffsets::MeshObject,
                   &meshObject) || !IsReasonableObject(meshObject))
        return;

    // Resolve and validate the virtual Update target before touching any
    // state: a stale mesh object (freed during streaming) yields a garbage
    // slot value, and calling it is an instant GP fault at a heap address.
    uintptr_t moVtable = *(uintptr_t*)meshObject;  // in .rdata per check above
    uintptr_t updateFn = 0;
    if (!ReadPtrAt(moVtable + FSkeletalMeshObjectOffsets::Update_VtblSlot,
                   &updateFn) || !IsEngineCodePtr(updateFn))
        return;

    uintptr_t boneOwner = 0;
    FMatrix* scratch = nullptr;
    int boneCount = 0;
    int lod = 0;

    if (e.curr.boneCount > 0) {
        boneOwner = comp;
        boneCount = e.curr.boneCount;
        scratch = g_boneScratch[slot];
        if (g_scratchBoneCount[slot] != boneCount) {
            if (e.hasPrev && e.prev.boneCount == boneCount) {
                for (int b = 0; b < boneCount; ++b)
                    RigidLerpMatrix(e.prev.bones[b], e.curr.bones[b], alpha,
                                    &scratch[b]);
            } else {
                memcpy(scratch, e.curr.bones,
                       (size_t)boneCount * sizeof(FMatrix));
            }
            g_scratchBoneCount[slot] = boneCount;
        }
        ReadAt<int>(comp + USkeletalMeshComponentOffsets::PredictedLODLevel,
                    &lod);
    } else if (e.parentAnim) {
        int p = FindViewModelEntry(e.parentAnim);
        if (p < 0 || g_scratchBoneCount[p] <= 0)
            return;  // parent not tracked/built: leave engine bones (62 Hz)
        boneOwner = e.parentAnim;
        boneCount = g_scratchBoneCount[p];
        scratch = g_boneScratch[p];
        // Engine clamps a parented component's LOD to the parent's.
        ReadAt<int>(e.parentAnim +
                    USkeletalMeshComponentOffsets::PredictedLODLevel, &lod);
    } else {
        return;
    }
    if (lod < 0) lod = 0;

    if (!CanWrite((void*)(boneOwner +
                          USkeletalMeshComponentOffsets::SpaceBases_Data),
                  sizeof(uintptr_t) + sizeof(int)))
        return;

    uintptr_t dataAddr = boneOwner + USkeletalMeshComponentOffsets::SpaceBases_Data;
    uintptr_t numAddr  = boneOwner + USkeletalMeshComponentOffsets::SpaceBases_Num;
    uintptr_t savedData = *(uintptr_t*)dataAddr;
    int savedNum = *(int*)numAddr;

    *(uintptr_t*)dataAddr = (uintptr_t)scratch;
    *(int*)numAddr = boneCount;

    // The CPU skin variant returns early when its frame stamp equals the current
    // GFrameNumber (set by the engine's own update on the tick path, on sim frames).
    // Clear the stamp so our per presented frame push always lands.
    if (moVtable == Addr::FSkeletalMeshObjectCPUSkin_vtable)
        *(DWORD*)(meshObject + 0x64) = 0;

    ((fn_MeshObjectUpdate)updateFn)(
        (void*)meshObject, nullptr, lod, (void*)comp,
        (void*)(comp + USkeletalMeshComponentOffsets::ActiveMorphs));
    *(DWORD*)(meshObject +
              FSkeletalMeshObjectOffsets::bHasBeenUpdatedAtLeastOnce) = 1;

    *(uintptr_t*)dataAddr = savedData;
    *(int*)numAddr = savedNum;

    InterlockedIncrement(&g_dbgVmSkel);
}

static void PushViewModelCompensation() {
    // The pool was filled under the smoothing toggles.
    // An empty pool means every subsystem is off.
    if (g_vmCount == 0 || !g_overrideActive || !g_lastInterpValid)
        return;

    FMatrix interpCam{}, camPrevM{}, camCurrM{}, invCamPrev{}, invCamCurr{};
    BuildCameraToWorld(g_lastInterp.Location, g_lastInterp.Rotation, &interpCam);
    BuildCameraToWorld(g_vmCamPrev.Location, g_vmCamPrev.Rotation, &camPrevM);
    BuildCameraToWorld(g_vmCamCurr.Location, g_vmCamCurr.Rotation, &camCurrM);
    MatrixInverseRigid(camPrevM, &invCamPrev);
    MatrixInverseRigid(camCurrM, &invCamCurr);

    // Delta used for bounds and for entries without a previous pose.
    FMatrix deltaCurr{};
    MatrixMultiply(invCamCurr, interpCam, &deltaCurr);

    // Call through the trampoline when the registration hook is installed so
    // our own pushes don't route through the detour.
    fn_UpdatePrimitiveTransform updateTransform = g_origSceneUpdateTransform
        ? (fn_UpdatePrimitiveTransform)g_origSceneUpdateTransform
        : (fn_UpdatePrimitiveTransform)Addr::FScene_UpdatePrimitiveTransform;

    g_inOurPush = true;

    for (int i = 0; i < MAX_VIEWMODEL_COMPONENTS; ++i)
        g_scratchBoneCount[i] = 0;

    for (int i = 0; i < g_vmCount; ++i) {
        ViewModelEntry& e = g_vm[i];
        if (!e.component) continue;

        // A world mesh needs both endpoints.
        // Otherwise the engine pose is already the one to show.
        if (!e.cameraRelative && !e.hasPrev)
            continue;

        if (!IsEntryStillValid(e)) {
            InterlockedIncrement(&g_dbgVmSkip);
            continue;
        }

        FMatrix compensated{};
        FVector boundsOrigin;
        if (e.cameraRelative) {
            // Camera relative interpolation: smooths bob/sway that steps per
            // sim tick and is immune to teleports/cuts (they cancel out of
            // both poses).
            if (e.hasPrev) {
                FMatrix relPrev{}, relCurr{}, rel{};
                MatrixMultiply(e.prev.localToWorld, invCamPrev, &relPrev);
                MatrixMultiply(e.curr.localToWorld, invCamCurr, &relCurr);
                RigidLerpMatrix(relPrev, relCurr, g_lastAlpha, &rel);
                MatrixMultiply(rel, interpCam, &compensated);
            } else {
                MatrixMultiply(e.curr.localToWorld, deltaCurr, &compensated);
            }
            boundsOrigin = TransformPoint(deltaCurr, e.curr.bounds.Origin);
        } else {
            // World space interpolation between the two sim poses.
            // A verified constant velocity mover is presented at t = 1+alpha.
            // Its collision already holds the newest sim pose,
            // so the mesh meets the encroach test as that test runs.
            float t = g_lastAlpha;
            if (e.lead) {
                t += 1.0f;
                InterlockedIncrement(&g_dbgVmLead);
            }
            RigidLerpMatrix(e.prev.localToWorld, e.curr.localToWorld,
                            t, &compensated);
            boundsOrigin = LerpVector(e.prev.bounds.Origin,
                                      e.curr.bounds.Origin, t);
        }

        FMatrix* fieldMatrix =
            (FMatrix*)(e.component + UPrimitiveComponentOffsets::LocalToWorld);
        FBoxSphereBounds* fieldBounds =
            (FBoxSphereBounds*)(e.component + UPrimitiveComponentOffsets::Bounds);

        FMatrix savedMatrix = *fieldMatrix;
        FBoxSphereBounds savedBounds = *fieldBounds;

        *fieldMatrix = compensated;
        fieldBounds->Origin = boundsOrigin;

        uintptr_t scene =
            *(uintptr_t*)(e.component + UPrimitiveComponentOffsets::Scene);
        updateTransform((void*)scene, nullptr, (void*)e.component);

        // Bone push happens while the compensated LocalToWorld is in place so
        // any component state reads in the snapshot ctor see a coherent pose.
        PushInterpolatedBones(e, i, g_lastAlpha);

        *fieldMatrix = savedMatrix;
        *fieldBounds = savedBounds;
        InterlockedIncrement(&g_dbgVmPush);
    }

    g_inOurPush = false;
}

// Writes an interpolated LightToWorld and WorldToLight, then calls UpdateLightTransform.
// That call is RemoveLight plus AddLight, which builds a new FLightSceneInfo from the component.
// The component's matrices are restored afterward, so later captures see the engine pose.
static void PushLightCompensation() {
    if (g_lightCount == 0 || !g_overrideActive || !g_lastInterpValid)
        return;
    fn_SceneUpdateTransform updateLight = g_origSceneUpdateLightTransform
        ? g_origSceneUpdateLightTransform
        : (fn_SceneUpdateTransform)Addr::FScene_UpdateLightTransform;

    g_inOurPush = true;
    for (int i = 0; i < g_lightCount; ++i) {
        const LightEntry& e = g_lights[i];
        if (!e.component || !e.hasPrev)
            continue;
        if (!IsLightEntryStillValid(e)) {
            InterlockedIncrement(&g_dbgVmSkip);
            continue;
        }

        float t = g_lastAlpha + (e.lead ? 1.0f : 0.0f);
        FMatrix l2w{}, w2l{};
        RigidLerpMatrix(e.prev, e.curr, t, &l2w);
        if (!MatrixInverseAffine(l2w, &w2l))
            continue;

        FMatrix* fieldL2W =
            (FMatrix*)(e.component + ULightComponentOffsets::LightToWorld);
        FMatrix* fieldW2L =
            (FMatrix*)(e.component + ULightComponentOffsets::WorldToLight);
        FMatrix savedL2W = *fieldL2W;
        FMatrix savedW2L = *fieldW2L;
        *fieldL2W = l2w;
        *fieldW2L = w2l;
        updateLight((void*)e.scene, nullptr, (void*)e.component);
        *fieldL2W = savedL2W;
        *fieldW2L = savedW2L;
        InterlockedIncrement(&g_dbgLightPush);
    }
    g_inOurPush = false;
}

static void MouseResampleReset();    // defined with the resampler below
static void MouseVerifyAfterTick();  // resampler post tick check (below)
static uintptr_t GetPlayerInputPtr();
static void LatProbeReset();         // latency probe (below)
static void LatProbePostTick(uintptr_t input, LONGLONG nowQpc);

static void ResetFixedStepState() {
    ClearViewOverride();
    g_accumulator = 0.0;
    g_seeded = false;
    g_warmupActive = true;
    g_warmupSettled = 0;
    g_warmupTicksRun = 0;
    MouseResampleReset();
    LatProbeReset();
    LLReset();
    g_simCount = 0;
    g_skipWorldTick = false;
    g_inSimulationTick = false;
    g_capturedThisSim = false;
    g_meshPushArmed = false;
    g_rtsArmed = false;
    g_rtsRenderOnlyAccum = 0.0f;
    g_vmCount = 0;
    g_lightCount = 0;
    DropPosePairs();
    g_lastInterpValid = false;
    g_pawnRideRoot = 0;
    g_rideStepValid = false;
    g_rideStepActor = 0;
    g_lastFrameRenderOnly = false;
    g_skipReqThisFrame = false;
    for (int i = 0; i < MAX_SKEL_REGISTRY; ++i)
        g_skelRegistry[i].component = 0;
    for (int i = 0; i < MAX_MOVER_REGISTRY; ++i)
        g_moverRegistry[i].component = 0;
    for (int i = 0; i < MAX_LIGHT_REGISTRY; ++i)
        g_lightRegistry[i].component = 0;
}

// After eventUpdateCamera and before the draw, so CalcSceneView consumes this write.
// Both worlds call this. Capture runs on the first invocation of a sim tick.

static void __fastcall Hook_WorldTick(void* thisptr, void* /*edx*/,
                                      int tickType, float deltaTime) {
    if (!g_skipWorldTick) {
        // Rebase RealTimeSeconds before the engine adds its own per tick
        // delta, removing what the render only frames added since the last
        // sim tick (first invocation per engine tick = primary world).
        if (g_inSimulationTick && !g_capturedThisSim &&
            g_rtsRenderOnlyAccum > 0.0f) {
            uintptr_t worldInfo = GetWorldInfoPtr();
            if (worldInfo &&
                CanWrite((void*)(worldInfo + AWorldInfoOffsets::RealTimeSeconds),
                         sizeof(float))) {
                *(float*)(worldInfo + AWorldInfoOffsets::RealTimeSeconds) -=
                    g_rtsRenderOnlyAccum;
            }
            g_rtsRenderOnlyAccum = 0.0f;
        }
        // Primary world, after this frame's input pump: the accumulators hold
        // the batch this tick consumes and the camera cache still holds the
        // last present's interpolated FOV.
        if (g_inSimulationTick && !g_capturedThisSim) {
            RestoreSimFOV();
            if (g_cfg.logging)
                BatchRippleSample();
            LLPreTick(deltaTime);
        }
        g_origWorldTick(thisptr, tickType, deltaTime);
    } else if (g_rtsArmed) {
        // Render only frame: keep real time flowing so per presented frame
        // real time deltas (HUD fades) are nonzero.
        g_rtsArmed = false;
        uintptr_t worldInfo = GetWorldInfoPtr();
        if (worldInfo &&
            CanWrite((void*)(worldInfo + AWorldInfoOffsets::RealTimeSeconds),
                     sizeof(float))) {
            *(float*)(worldInfo + AWorldInfoOffsets::RealTimeSeconds) +=
                deltaTime;
            g_rtsRenderOnlyAccum += deltaTime;
        }
    }

    if (g_inSimulationTick && !g_capturedThisSim) {
        g_capturedThisSim = true;
        // The resampler check and the latency probe must see the accumulators as the tick left them.
        // The late drain and this frame's present both run after that.
        MouseVerifyAfterTick();
        if (g_cfg.logging) {
            uintptr_t latInput = GetPlayerInputPtr();
            LARGE_INTEGER latQ;
            if (latInput && QueryPerformanceCounter(&latQ))
                LatProbePostTick(latInput, latQ.QuadPart);
        }
        LLPostTick();
        CaptureSimulationCamera();
        CaptureViewModelPose();
    }

    LateDrainMouse();
    UpdateViewOverride();

    // Once per engine tick (first world tick invocation = primary world),
    // before the FViewport::Draw dispatch in the engine tick tail.
    if (g_meshPushArmed) {
        g_meshPushArmed = false;
        PushViewModelCompensation();
        PushLightCompensation();
    }
}

// UGameViewportClient::Tick drives the UI chain with the dt it is given.
// Here that dt is the real present interval, and the call runs once per present.
// A catch up present invokes the engine tick more than once, and the later calls are dropped
// ecx is the viewport on entry.
static void __fastcall UISceneTickThunk(void* viewport) {
    if (g_uiTickedThisPresent) {
        InterlockedIncrement(&g_dbgUiDedup);
        return;  // This present already ticked the UI. Drop the catch up duplicate.
    }
    g_uiTickedThisPresent = 1;
    InterlockedIncrement(&g_dbgUiTick);
    ((fn_ViewportTick)Addr::UGameViewportClient_Tick)(viewport,
                                                      g_uiRealFrameDelta);
}

__declspec(naked) static void Hook_UISceneTickGate() {
    __asm {
        // Caller set ecx = GameViewport (this). Stack: [esp]=return addr,
        // [esp+4]=dt float. Preserve caller saved edx around the thunk call.
        push edx
        call UISceneTickThunk   // __fastcall: viewport in ecx (untouched)
        pop edx
        ret 4                   // clean the original callee's stack arg
    }
}

// One attach or removal per call, then a cooldown of ten calls, with a 5 ms attach budget.
// The three per present sites run only on sim presents.
// Do not gate the outer call to helper 0xB748B0 at 0xECDE8B. It also arms the draw.
// The gated call is the inner UpdateLevelStreaming at 0xB74A64.
static void* g_lsUpdateTarget = (void*)Addr::UWorld_UpdateLevelStreaming;

// Zero when PaceRate is 0, which leaves streaming scaled with the framerate.
static char  g_lsGateEnabled = 1;

// __thiscall(UWorld*, FSceneViewFamily*), ret 4.
// All three gated sites use this.
__declspec(naked) static void Gate_UpdateLevelStreaming() {
    __asm {
        cmp byte ptr [g_lsGateEnabled], 0
        je run
        cmp byte ptr [g_skipWorldTick], 0
        jne skip
    run:
        inc dword ptr [g_dbgLsRun]      // Game thread only, so a plain increment is enough.
        jmp dword ptr [g_lsUpdateTarget]
    skip:
        inc dword ptr [g_dbgLsSkip]
        ret 4
    }
}

// TickSpecial runs on sim frames and feeds HUD+0x4C4 into the fade and saturation updates.
// Script PostRender overwrites that field on every present.
// The value written here is the wall time since the previous sim tick.
static void __fastcall Hook_HudTickSpecial(void* thisptr, void* /*edx*/,
                                           float deltaTime) {
    float realDelta = g_wallSecondsPerSimTick;
    if (realDelta > 0.0f && realDelta < 0.25f &&
        CanWrite((void*)((uintptr_t)thisptr + ATdHUDOffsets::RealTimeRenderDelta),
                 sizeof(float))) {
        *(float*)((uintptr_t)thisptr + ATdHUDOffsets::RealTimeRenderDelta) =
            realDelta;
    }
    g_origHudTickSpecial(thisptr, deltaTime);
}

// Stock budget slices at PaceRate, preferably on render only presents.
// Menus, an unseeded driver, and a nonzero drain gate keep the stock per present slice.
// Blocking flushes never enter StaticTick.
static void __cdecl Hook_StaticTick(float deltaTime) {
    int gate = 0;
    ReadAt<int>(Addr::G_PriorityAsyncLoading, &gate);
    if (g_loadSliceDt <= 0.0 || !g_seeded || gate != 0) {
        g_origStaticTick(deltaTime);  // Pacing is off, this frame is passthrough, or the loader is draining.
        return;
    }

    LARGE_INTEGER qpc;
    if (!QueryPerformanceCounter(&qpc) || g_qpcSecondsPerCount <= 0.0) {
        g_origStaticTick(deltaTime);  // No timer, so loading keeps the stock schedule.
        return;
    }
    if (g_lastLoadSliceQpc) {
        double wallDt = (double)(qpc.QuadPart - g_lastLoadSliceQpc) *
                        g_qpcSecondsPerCount;
        if (wallDt > 0.0)
            g_loadSliceCredit += (wallDt > 0.25 ? 0.25 : wallDt);
    }
    g_lastLoadSliceQpc = qpc.QuadPart;
    if (g_loadSliceCredit > g_loadSliceDt * 2.0)
        g_loadSliceCredit = g_loadSliceDt * 2.0;  // Caps the credit after an idle gap or a hitch.

    int pending = 0;
    ReadAt<int>(Addr::GObjAsyncPackages_Num, &pending);
    if (pending <= 0) {
        InterlockedIncrement(&g_dbgLoadSkip);  // The loader is idle.
        return;
    }

    // One stock slice per accrued PaceRate interval.
    // render only presents are preferred.
    // At 1.5 intervals of credit the slice fires on a sim present,
    // which covers the case where no render only present arrived in half a slice.
    bool fire = g_loadSliceCredit >= g_loadSliceDt &&
                (g_skipWorldTick ||
                 g_loadSliceCredit >= g_loadSliceDt * 1.5);
    if (!fire) {
        InterlockedIncrement(&g_dbgLoadSkip);
        return;
    }
    g_loadSliceCredit -= g_loadSliceDt;
    ((fn_ProcessAsyncLoading)Addr::UObject_ProcessAsyncLoading)(
        1, g_stockSliceBudget);
    InterlockedIncrement(&g_dbgLoadSlice);
}

// Counters only. The hooks do not read arguments, so the calling convention does not matter.
__declspec(noinline) static void CountViewportClientDraw() {
    InterlockedIncrement(&g_dbgDraw);
}

__declspec(naked) static void Hook_ViewportClientDraw() {
    __asm {
        pushfd
        pushad
        call CountViewportClientDraw
        popad
        popfd
        jmp dword ptr [g_origViewportClientDraw]
    }
}

__declspec(noinline) static void CountCalcSceneView() {
    InterlockedIncrement(&g_dbgCalcView);
    if (g_overrideGlobalsOk && *(volatile int*)Addr::LP_bOverrideView)
        InterlockedIncrement(&g_dbgCalcViewOv);
}

__declspec(naked) static void Hook_CalcSceneView() {
    __asm {
        pushfd
        pushad
        call CountCalcSceneView
        popad
        popfd
        jmp dword ptr [g_origCalcSceneView]
    }
}

__declspec(noinline) static void CountRhiEnd() {
    InterlockedIncrement(&g_dbgRhiEnd);
}

__declspec(naked) static void Hook_EndDrawingViewport() {
    __asm {
        pushfd
        pushad
        call CountRhiEnd
        popad
        popfd
        jmp dword ptr [g_origEndDrawingViewport]
    }
}

// Each tick is fed one sim interval of counts.
// Writes start only after a nonzero batch is seen consumed to zero.
// A later mismatch restores the queued counts and returns to observe mode.

static constexpr int MOUSE_FIFO_CAP = 32;

// Bound on stale input while nothing consumes the FIFO.
// Scaled to the sim interval so a low tick rate still keeps two tick windows.
static double MouseMaxQueueSec() {
    double bound = g_simDt * 2.0;
    return bound > 0.10 ? bound : 0.10;
}

struct MouseSample { float dx, dy; double span; };
static MouseSample g_mouseFifo[MOUSE_FIFO_CAP];
static int      g_mouseFifoN = 0;
static float    g_mouseSeenX = 0.0f, g_mouseSeenY = 0.0f;  // Accumulator values at the last observation.
static LONGLONG g_mouseSampleQpc = 0;                      // Zero until the first sample.
static bool     g_mouseValidated = false;   // The consume and zero model has been seen this session.
static bool     g_mouseWroteThisTick = false;
static float    g_mousePreTickX = 0.0f, g_mousePreTickY = 0.0f;
static uintptr_t g_mouseTickInput = 0;
static uintptr_t g_mouseInputObj = 0;  // PlayerInput object the FIFO belongs to.
static LONG     g_dbgMouseRs = 0;       // Ticks that received a resampled batch.
static LONG     g_dbgMouseResync = 0;   // Consume model mismatches. The queued counts were restored.

static uintptr_t GetPlayerInputPtr() {
    uintptr_t ctrl = GetLocalPlayerController();
    if (!ctrl) return 0;
    uintptr_t input = 0;
    if (!ReadPtrAt(ctrl + APlayerControllerOffsets::PlayerInput, &input) ||
        !IsReasonableObject(input))
        return 0;
    if (!CanWrite((void*)(input + UPlayerInputOffsets::aMouseX),
                  2 * sizeof(float)))
        return 0;
    return input;
}

static void MouseResampleReset() {
    g_mouseFifoN = 0;
    g_mouseSeenX = g_mouseSeenY = 0.0f;
    g_mouseSampleQpc = 0;
    g_mouseValidated = false;
    g_mouseWroteThisTick = false;
    g_mouseInputObj = 0;
}

static double MouseFifoSpan() {
    double total = 0.0;
    for (int i = 0; i < g_mouseFifoN; ++i)
        total += g_mouseFifo[i].span;
    return total;
}

static void MouseFifoPopFront() {
    memmove(&g_mouseFifo[0], &g_mouseFifo[1],
            sizeof(MouseSample) * (size_t)(--g_mouseFifoN));
}

// Every present (hook entry, before any tick): record the counts that
// arrived since the last present together with the real time they span.
static void MouseSamplePresent() {
    if (!g_cfg.mouseResample || !g_seeded)
        return;
    uintptr_t input = GetPlayerInputPtr();
    if (!input) {
        MouseResampleReset();
        return;
    }
    if (input != g_mouseInputObj) {  // controller/input swap: re-prime
        MouseResampleReset();
        g_mouseInputObj = input;
    }
    float fx = *(float*)(input + UPlayerInputOffsets::aMouseX);
    float fy = *(float*)(input + UPlayerInputOffsets::aMouseY);
    LARGE_INTEGER q;
    if (!QueryPerformanceCounter(&q) || g_qpcSecondsPerCount <= 0.0)
        return;
    if (!g_mouseSampleQpc) {  // first observation after (re)seed
        g_mouseSeenX = fx;
        g_mouseSeenY = fy;
        g_mouseSampleQpc = q.QuadPart;
        return;
    }
    double span =
        (double)(q.QuadPart - g_mouseSampleQpc) * g_qpcSecondsPerCount;
    float dx = fx - g_mouseSeenX;
    float dy = fy - g_mouseSeenY;
    g_mouseSeenX = fx;
    g_mouseSeenY = fy;
    g_mouseSampleQpc = q.QuadPart;
    if (span <= 0.0 || span > 0.25) {
        // Blocking hitch: the motion's timing inside the gap is unknowable.
        // Drop it (the view isn't presenting during a hitch, and applying a
        // quarter of a second batch afterwards would yank the view).
        g_mouseFifoN = 0;
        return;
    }
    if (g_mouseFifoN == MOUSE_FIFO_CAP) {  // overflow: fold oldest pair
        g_mouseFifo[1].dx += g_mouseFifo[0].dx;
        g_mouseFifo[1].dy += g_mouseFifo[0].dy;
        g_mouseFifo[1].span += g_mouseFifo[0].span;
        MouseFifoPopFront();
    }
    g_mouseFifo[g_mouseFifoN++] = { dx, dy, span };
    // In observe mode nothing consumes the FIFO.
    // Keep only recent motion, so validation starts from a fresh queue.
    while (g_mouseFifoN > 1 && MouseFifoSpan() > MouseMaxQueueSec())
        MouseFifoPopFront();
}

// Immediately before a world tick. Validated: overwrite the accumulators
// with counts covering exactly simDt of real time from the FIFO head.
// Observe mode: leave the fields alone, just remember them for the
// post tick consume check.
static void MouseConsumeForTick() {
    g_mouseWroteThisTick = false;
    g_mouseTickInput = 0;
    if (!g_cfg.mouseResample || !g_mouseSampleQpc)
        return;
    uintptr_t input = GetPlayerInputPtr();
    if (!input)
        return;
    g_mouseTickInput = input;
    if (!g_mouseValidated) {
        g_mousePreTickX = *(float*)(input + UPlayerInputOffsets::aMouseX);
        g_mousePreTickY = *(float*)(input + UPlayerInputOffsets::aMouseY);
        return;
    }
    double need = g_simDt;
    float ax = 0.0f, ay = 0.0f;
    while (g_mouseFifoN > 0 && need > 1e-6) {
        MouseSample& s = g_mouseFifo[0];
        if (s.span <= need) {
            ax += s.dx;
            ay += s.dy;
            need -= s.span;
            MouseFifoPopFront();
        } else {
            float f = (float)(need / s.span);
            ax += s.dx * f;
            ay += s.dy * f;
            s.dx -= s.dx * f;
            s.dy -= s.dy * f;
            s.span -= need;
            need = 0.0;
        }
    }
    *(float*)(input + UPlayerInputOffsets::aMouseX) = ax;
    *(float*)(input + UPlayerInputOffsets::aMouseY) = ay;
    g_mouseSeenX = ax;
    g_mouseSeenY = ay;
    g_mouseWroteThisTick = true;
    InterlockedIncrement(&g_dbgMouseRs);
}

// After the world tick, checks that the engine consumed the batch to zero.
// Observe mode arms when a nonzero batch is consumed.
// Active mode restores the queued counts and returns to observe mode if any residue remains
static void MouseVerifyAfterTick() {
    uintptr_t input = g_mouseTickInput;
    bool wrote = g_mouseWroteThisTick;
    g_mouseWroteThisTick = false;
    g_mouseTickInput = 0;
    if (!input ||
        !CanRead((void*)(input + UPlayerInputOffsets::aMouseX),
                 2 * sizeof(float)) ||
        !CanWrite((void*)(input + UPlayerInputOffsets::aMouseX),
                  2 * sizeof(float)))
        return;
    float fx = *(float*)(input + UPlayerInputOffsets::aMouseX);
    float fy = *(float*)(input + UPlayerInputOffsets::aMouseY);
    bool consumed = fx > -0.5f && fx < 0.5f && fy > -0.5f && fy < 0.5f;

    if (!g_mouseValidated) {
        bool hadCounts = g_mousePreTickX < -0.5f || g_mousePreTickX > 0.5f ||
                         g_mousePreTickY < -0.5f || g_mousePreTickY > 0.5f;
        if (hadCounts && consumed) {
            g_mouseValidated = true;  // The engine consumed a batch, so resampling may write.
            g_mouseFifoN = 0;         // That batch is spent.
        }
        g_mouseSeenX = fx;
        g_mouseSeenY = fy;
        return;
    }

    if (!wrote)
        return;
    if (consumed) {
        g_mouseSeenX = fx;
        g_mouseSeenY = fy;
        return;
    }
    for (int i = 0; i < g_mouseFifoN; ++i) {
        fx += g_mouseFifo[i].dx;
        fy += g_mouseFifo[i].dy;
    }
    g_mouseFifoN = 0;
    *(float*)(input + UPlayerInputOffsets::aMouseX) = fx;
    *(float*)(input + UPlayerInputOffsets::aMouseY) = fy;
    g_mouseSeenX = fx;
    g_mouseSeenY = fy;
    g_mouseValidated = false;
    InterlockedIncrement(&g_dbgMouseResync);
}

// Counts that have arrived and no tick has consumed yet.
// With the resampler armed, that is the FIFO plus the gain since the last observation.
// Otherwise the accumulators hold everything since the last tick zeroed them.
static bool MousePendingCounts(float* dx, float* dy) {
    uintptr_t input = GetPlayerInputPtr();
    if (!input)
        return false;
    float fx = *(float*)(input + UPlayerInputOffsets::aMouseX);
    float fy = *(float*)(input + UPlayerInputOffsets::aMouseY);
    if (g_cfg.mouseResample && g_mouseValidated) {
        fx -= g_mouseSeenX;
        fy -= g_mouseSeenY;
        for (int i = 0; i < g_mouseFifoN; ++i) {
            fx += g_mouseFifo[i].dx;
            fy += g_mouseFifo[i].dy;
        }
    }
    *dx = fx;
    *dy = fy;
    return true;
}

// Logs one ME-LAT line per gesture.
// The latch stamp is taken in the present that pumped the counts, before this function's drain stamp,
// so a latch time at or below zero means that present showed them.

static LONGLONG g_latGestureQpc = 0;      // First drain of the gesture.
static LONGLONG g_latLastDrainQpc = 0;    // Most recent drain.
static LONGLONG g_latFirstConsumeQpc = 0;
static LONGLONG g_latLastConsumeQpc = 0;
static LONGLONG g_latFirstVisQpc = 0;
static float    g_latFirstVisAlpha = 0.0f;
static LONGLONG g_latEndFullQpc = 0;
static LONGLONG g_latFirstLatchQpc = 0;   // First residual present of the gesture.
static LONGLONG g_latLastLatchQpc = 0;    // Most recent residual present. Kept across a reset.
static float    g_latSeenX = 0.0f, g_latSeenY = 0.0f;
static bool     g_latAwaitVis = false;    // The consume happened. The next camera write is the visible stamp.
static bool     g_latAwaitFull = false;   // Waiting for the write after the post consume tick.
static int      g_latLastConsumeTick = 0; // g_simTickCounter at the last consume.

static void LatProbeReset() {
    g_latGestureQpc = g_latLastDrainQpc = 0;
    g_latFirstConsumeQpc = g_latLastConsumeQpc = 0;
    g_latFirstVisQpc = g_latEndFullQpc = 0;
    g_latFirstLatchQpc = 0;
    g_latAwaitVis = g_latAwaitFull = false;
    g_latLastConsumeTick = 0;
}

// From LLApplyResidual: a present showed nonzero pending counts.
static void LatProbeLatched(LONGLONG nowQpc) {
    g_latLastLatchQpc = nowQpc;
    if (g_latGestureQpc && !g_latFirstLatchQpc)
        g_latFirstLatchQpc = nowQpc;
}

static double LatMs(LONGLONG from, LONGLONG to) {
    return (double)(to - from) * g_qpcSecondsPerCount * 1000.0;
}

// Per present, before the engine tick: detect drain growth and report
// finished gestures.
static void LatProbePresent(uintptr_t input, LONGLONG nowQpc) {
    float fx = *(float*)(input + UPlayerInputOffsets::aMouseX);
    float fy = *(float*)(input + UPlayerInputOffsets::aMouseY);
    // Between sim ticks only the mouse drain writes these fields.
    // Any change is drain activity, in either direction.
    // A magnitude test misses the return half of a reversing gesture.
    float dx = fx - g_latSeenX, dy = fy - g_latSeenY;
    bool grew = (dx > 0.5f || dx < -0.5f || dy > 0.5f || dy < -0.5f);
    g_latSeenX = fx;
    g_latSeenY = fy;

    if (grew) {
        if (!g_latGestureQpc ||
            LatMs(g_latLastDrainQpc, nowQpc) > 250.0) {
            LatProbeReset();
            g_latGestureQpc = nowQpc;
            // The residual present for these counts ran in the previous
            // frame, before this entry time drain stamp.
            if (g_latLastLatchQpc && LatMs(g_latLastLatchQpc, nowQpc) < 50.0)
                g_latFirstLatchQpc = g_latLastLatchQpc;
        }
        g_latLastDrainQpc = nowQpc;
        return;
    }

    // Idle: report a completed gesture once its tail is fully presented.
    if (g_latGestureQpc && g_latEndFullQpc && !g_latAwaitFull &&
        LatMs(g_latLastDrainQpc, nowQpc) > 250.0) {
        char latch[32];
        if (g_latFirstLatchQpc)
            snprintf(latch, sizeof(latch), "%+.1fms",
                     LatMs(g_latGestureQpc, g_latFirstLatchQpc));
        else
            snprintf(latch, sizeof(latch), "-");
        char buf[320];
        snprintf(buf, sizeof(buf),
            "ME-LAT: drain=0 latch=%s consume=+%.1fms visible=+%.1fms(a=%.2f) | "
            "gestureSpan=%.0fms visibleSpan=%.0fms stretch=%+.0fms "
            "endLat=%.1fms\n",
            latch,
            LatMs(g_latGestureQpc, g_latFirstConsumeQpc),
            LatMs(g_latGestureQpc, g_latFirstVisQpc), g_latFirstVisAlpha,
            LatMs(g_latGestureQpc, g_latLastDrainQpc),
            LatMs(g_latFirstVisQpc, g_latEndFullQpc),
            LatMs(g_latFirstVisQpc, g_latEndFullQpc) -
                LatMs(g_latGestureQpc, g_latLastDrainQpc),
            LatMs(g_latLastDrainQpc, g_latEndFullQpc));
        Log(buf);
        LatProbeReset();
    }
}

// Around each sim tick: detect the consume (accumulators zeroed).
static float g_latPreTickMag = 0.0f;

static void LatProbePreTick(uintptr_t input) {
    float fx = *(float*)(input + UPlayerInputOffsets::aMouseX);
    float fy = *(float*)(input + UPlayerInputOffsets::aMouseY);
    g_latPreTickMag = (fx < 0 ? -fx : fx) + (fy < 0 ? -fy : fy);
}

static void LatProbePostTick(uintptr_t input, LONGLONG nowQpc) {
    if (!g_latGestureQpc || g_latPreTickMag <= 0.5f)
        return;
    float fx = *(float*)(input + UPlayerInputOffsets::aMouseX);
    float fy = *(float*)(input + UPlayerInputOffsets::aMouseY);
    float mag = (fx < 0 ? -fx : fx) + (fy < 0 ? -fy : fy);
    if (mag > 0.5f)
        return;  // not consumed this tick
    g_latSeenX = fx;
    g_latSeenY = fy;
    if (!g_latFirstConsumeQpc) {
        g_latFirstConsumeQpc = nowQpc;
        g_latAwaitVis = true;
    }
    g_latLastConsumeQpc = nowQpc;
    g_latLastConsumeTick = (int)g_simTickCounter;
    g_latAwaitFull = true;
    g_latEndFullQpc = 0;
}

// Fully presented means the first camera write after the sim tick that follows the last consume.
// From that tick on, both interpolation poses hold the gesture's final rotation.
// An alpha threshold misses this at an integer cap ratio, where alpha never reaches 0.95.
static void LatProbeVisible(float alpha, LONGLONG nowQpc) {
    if (g_latAwaitVis) {
        g_latAwaitVis = false;
        g_latFirstVisQpc = nowQpc;
        g_latFirstVisAlpha = alpha;
    }
    if (g_latAwaitFull &&
        (int)g_simTickCounter > g_latLastConsumeTick) {
        g_latAwaitFull = false;
        g_latEndFullQpc = nowQpc;
    }
}

static void RunSimulationTick(void* thisptr) {
    // Wall clock spacing of sim ticks, for the HUD real time fade delta.
    LARGE_INTEGER qpc;
    if (QueryPerformanceCounter(&qpc)) {
        if (g_lastSimTickQpc && g_qpcSecondsPerCount > 0.0) {
            double wall = (double)(qpc.QuadPart - g_lastSimTickQpc) *
                          g_qpcSecondsPerCount;
            if (wall > 0.0 && wall < 0.25)
                g_wallSecondsPerSimTick = (float)wall;
            else
                g_wallSecondsPerSimTick = g_simDtF;
        }
        g_lastSimTickQpc = qpc.QuadPart;
    }

    double saved = *g_pGDeltaTime;
    *g_pGDeltaTime = g_simDt;
    g_skipWorldTick = false;
    g_inSimulationTick = true;
    g_capturedThisSim = false;
    g_meshPushArmed = true;
    ++g_simTickCounter;

    if (g_cfg.logging) {
        uintptr_t latInput = GetPlayerInputPtr();
        if (latInput)
            LatProbePreTick(latInput);
    }

    MouseConsumeForTick();
    StampEngineTickEntry();
    // MouseVerifyAfterTick / LatProbePostTick run inside, right after the world tick.
    g_origEngineTick(thisptr, g_simDtF);

    g_inSimulationTick = false;
    g_meshPushArmed = false;
    *g_pGDeltaTime = saved;

    if (!g_capturedThisSim) {
        CaptureSimulationCamera();
        CaptureViewModelPose();
    }

    g_lastFrameRenderOnly = false;
    ++g_dbgSim;
}

static void __fastcall Hook_WorldTick(void* thisptr, void* edx, int tickType,
                                      float deltaTime);

// One fixed step world tick right after an input pump that requested a
// cinematic skip in a frame whose world tick had already run (or was skipped):
// the tick stock would have run after that pump. Its interval is taken back
// out of the accumulator so game time is unchanged.
static void RunPostSkipWorldTick() {
    g_skipReqThisFrame = false;
    uintptr_t world = 0;
    if (!ReadPtrAt(Addr::GWorld, &world) || !IsReasonableObject(world))
        return;
    double saved = *g_pGDeltaTime;
    *g_pGDeltaTime = g_simDt;
    g_skipWorldTick = false;
    g_inSimulationTick = true;
    g_capturedThisSim = false;
    g_meshPushArmed = true;
    ++g_simTickCounter;
    Hook_WorldTick((void*)world, nullptr, 2 /*LEVELTICK_All*/, g_simDtF);
    g_inSimulationTick = false;
    g_meshPushArmed = false;
    *g_pGDeltaTime = saved;
    g_accumulator -= g_simDt;
    ++g_dbgSim;
}

static void RunRenderOnlyTick(void* thisptr, float deltaSeconds) {
    g_skipWorldTick = true;
    g_meshPushArmed = true;
    g_rtsArmed = true;
    StampEngineTickEntry();
    g_origEngineTick(thisptr, deltaSeconds);
    g_rtsArmed = false;
    g_meshPushArmed = false;
    g_skipWorldTick = false;
    g_lastFrameRenderOnly = true;
    ++g_dbgRdr;
    if (g_skipReqThisFrame)
        RunPostSkipWorldTick();
}

// The real frame delta, with no GDeltaTime pin. The engine still clamps it at 0.4 s.
// Interpolation reseeds. The resampler yields for this tick.
static void RunStallTick(void* thisptr, float deltaSeconds) {
    g_accumulator = 0.0;
    g_simCount = 0;         // The camera pair snaps to the post stall pose.
    DropPosePairs();        // Mesh, mover, and light pairs rebuild with no pre stall pose.
    MouseResampleReset();

    g_skipWorldTick = false;
    g_inSimulationTick = true;
    g_capturedThisSim = false;
    g_meshPushArmed = true;
    ++g_simTickCounter;

    uintptr_t latInput = g_cfg.logging ? GetPlayerInputPtr() : 0;
    if (latInput)
        LatProbePreTick(latInput);

    g_origEngineTick(thisptr, deltaSeconds);

    if (latInput) {
        LARGE_INTEGER latQ;
        if (QueryPerformanceCounter(&latQ))
            LatProbePostTick(latInput, latQ.QuadPart);
    }

    g_inSimulationTick = false;
    g_meshPushArmed = false;
    if (!g_capturedThisSim) {
        CaptureSimulationCamera();
        CaptureViewModelPose();
    }
    g_lastFrameRenderOnly = false;
    ++g_dbgSim;
    ++g_dbgStall;
}

// Stock had already ticked the world at the start of the frame that then stalled.
// Run that tick before the input pump, then skip the engine tick's own world tick.
static void RunStallTickAfterRenderOnly(void* thisptr, float deltaSeconds) {
    uintptr_t world = 0;
    if (!ReadPtrAt(Addr::GWorld, &world) || !IsReasonableObject(world)) {
        RunStallTick(thisptr, deltaSeconds);
        return;
    }
    g_accumulator = 0.0;
    g_simCount = 0;
    DropPosePairs();
    MouseResampleReset();

    g_skipWorldTick = false;
    g_inSimulationTick = true;
    g_capturedThisSim = false;
    g_meshPushArmed = true;
    ++g_simTickCounter;
    Hook_WorldTick((void*)world, nullptr, 2 /*LEVELTICK_All*/, deltaSeconds);
    g_inSimulationTick = false;
    g_meshPushArmed = false;

    g_skipWorldTick = true;   // This frame's world tick has already run.
    g_origEngineTick(thisptr, deltaSeconds);
    g_skipWorldTick = false;

    g_lastFrameRenderOnly = false;
    ++g_dbgSim;
    ++g_dbgStall;
    if (g_skipReqThisFrame)
        RunPostSkipWorldTick();
}

// Seconds per frame for the mod owned cap.
// Init clears bSmoothFrameRate.
// MaxSmoothedFrameRate is still read live, so the console command changes the cap.
static double EngineCapInterval() {
    if (!g_capOwned)
        return 0.0;
    uintptr_t engine = 0;
    if (!ReadPtrAt(g_pGEngine, &engine) || !engine)
        return 0.0;
    float maxRate = 0.0f;
    if (!ReadAt<float>(engine + UGameEngineOffsets::MaxSmoothedFrameRate,
                       &maxRate) ||
        maxRate <= 1.0f)
        return 0.0;
    return 1.0 / (double)maxRate;
}

// Passthrough frames have no camera and no pace stats.
// Frame starts are held one cap interval apart.
// A heavy frame overflows its slot
// A stale anchor is in the past, so the wait is skipped.
static void CapWaitPassthrough() {
    double capDt = EngineCapInterval();
    LARGE_INTEGER q;
    if (capDt <= 0.0 || g_qpcSecondsPerCount <= 0.0 ||
        !QueryPerformanceCounter(&q)) {
        g_passWaitQpc = 0;
        return;
    }
    if (g_passWaitQpc) {
        double target =
            (double)g_passWaitQpc * g_qpcSecondsPerCount + capDt;
        double wait = target - (double)q.QuadPart * g_qpcSecondsPerCount;
        if (wait > 0.0005) {
            if (wait > 0.025) wait = 0.025;
            if (wait > 0.0025)
                Sleep((DWORD)((wait - 0.0015) * 1000.0));
            for (;;) {
                QueryPerformanceCounter(&q);
                if ((double)q.QuadPart * g_qpcSecondsPerCount >= target)
                    break;
                YieldProcessor();
            }
        }
    }
    QueryPerformanceCounter(&q);
    g_passWaitQpc = q.QuadPart;
}

// Sleeps, then spins, so this present lands one pace interval after the previous one.
// The wait is shortened by the predicted cost of this frame's class.
// Warmup is the strongest layer, then evening, then the cap.
// A frame whose predicted cost exceeds the interval does not wait.
static void PaceWait(bool willSim) {
    if (g_qpcSecondsPerCount <= 0.0 || !g_lastPresentQpc)
        return;

    bool warmup = g_warmupActive;
    bool evening = g_cfg.evenPacing && g_paceEngaged && g_paceP > 0.0;
    double capDt = EngineCapInterval();
    double lastP = (double)g_lastPresentQpc * g_qpcSecondsPerCount;
    double targetPresent, predicted;
    if (warmup) {
        // Warmup presents at the sim rate, or at the cap when the cap is slower.
        // The stats were reset in passthrough, so there is no cost model.
        double p = g_simDt;
        if (capDt > p) p = capDt;
        if (p > 0.050) p = 0.050;
        targetPresent = lastP + p;
        predicted = 0.0;
        g_dbgPaceLayer = 3;
        g_dbgPaceP = p;
    } else if (evening) {
        double p = g_paceP;
        if (p > 0.050) p = 0.050;
        targetPresent = lastP + p;
        predicted = g_paceEma[willSim ? 1 : 0];
        // Evening never targets a present earlier than the cap schedule.
        // in steady evening the schedule is already in the past.
        // A burst spends the debt window, then locks to cap interval spacing.
        // Consecutive schedule targets differ by exactly one cap interval, so a prediction error cancels.
        if (g_capOwned && g_capSchedule > targetPresent)
            targetPresent = g_capSchedule;
        g_dbgPaceLayer = 2;
        g_dbgPaceP = p;
    } else if (capDt > 0.0) {
        // Present no earlier than the token bucket schedule.
        // An over budget frame pushes the schedule behind real time.
        // Later frames run early against the credit.
        if (g_capSchedule <= 0.0)
            return;  // PaceRecord seeds the schedule on the first present.
        targetPresent = g_capSchedule;
        predicted = g_paceEma[willSim ? 1 : 0];
        g_dbgPaceLayer = 1;
        g_dbgPaceP = capDt;
    } else {
        g_dbgPaceLayer = 0;
        g_dbgPaceP = 0.0;
        return;
    }

    LARGE_INTEGER q;
    if (!QueryPerformanceCounter(&q)) return;
    double target = targetPresent - predicted;
    double wait = target - (double)q.QuadPart * g_qpcSecondsPerCount;
    if (wait <= 0.0005)
        return;
    if (wait > 0.025) wait = 0.025;
    g_dbgPaceWaitMs += wait * 1000.0;

    // timeBeginPeriod(1) is set at init.
    // Leave about 1.5 ms for the spin.
    if (wait > 0.0025)
        Sleep((DWORD)((wait - 0.0015) * 1000.0));
    for (;;) {
        QueryPerformanceCounter(&q);
        if ((double)q.QuadPart * g_qpcSecondsPerCount >= target)
            break;
        YieldProcessor();
    }
}

// Record this frame's wall cost into its class envelope, stamp the present
// time, and run the engagement/slew state machine. Costs measured post wait,
// so the pacer's own sleep never inflates the stats. Hitch outliers
// (blocking loads) are excluded from the stats but still advance the present
// stamp. Catch up frames (simSteps > 1) record per step cost: the backlog is
// transient and must not raise the pace.
static void PaceRecord(bool wasSim, int simSteps, LONGLONG startQpc) {
    LARGE_INTEGER q;
    if (!QueryPerformanceCounter(&q) || g_qpcSecondsPerCount <= 0.0)
        return;
    g_lastPresentQpc = q.QuadPart;
    double now = (double)q.QuadPart * g_qpcSecondsPerCount;
    double cost = (double)(q.QuadPart - startQpc) * g_qpcSecondsPerCount;

    // Advance the cap's token bucket schedule: one slot per present, debt
    // clamped to the burst window (a hitch banks at most ~2 frames of
    // credit). Runs on every gameplay present so credit accrued during
    // over budget sim frames is reclaimable immediately.
    {
        double capDt = EngineCapInterval();
        if (capDt > 0.0) {
            double floorT = now - capDt * CAP_DEBT_FACTOR;
            if (g_capSchedule < floorT)
                g_capSchedule = floorT;
            g_capSchedule += capDt;
        } else {
            g_capSchedule = 0.0;
        }
    }

    if (cost <= 0.0 || cost > 0.1)
        return;
    if (wasSim && simSteps > 1)
        cost /= (double)simSteps;
    int cls = wasSim ? 1 : 0;

    // A lone sample above 1.5 times the class peak does not move the pace.
    // A second elevated sample within 0.3 s does.
    // A rise below that margin commits immediately.
    if (g_pacePeak[cls] > 0.0 && cost > g_pacePeak[cls] * 1.5 + 0.002) {
        if (g_paceSpikeAt[cls] <= 0.0 || now - g_paceSpikeAt[cls] > 0.3) {
            g_paceSpikeAt[cls] = now;
            return;
        }
        g_paceSpikeAt[cls] = 0.0;  // A second elevated sample commits.
    }

    // The EMA is the per frame predictor.
    // The peak relaxes towards the EMA and supplies the pace interval.
    if (g_paceEma[cls] <= 0.0) {
        g_paceEma[cls] = cost;
        g_pacePeak[cls] = cost;
    } else {
        g_paceEma[cls] += (cost - g_paceEma[cls]) * PACE_EMA_ALPHA;
        double relaxed = g_pacePeak[cls] -
                         (g_pacePeak[cls] - g_paceEma[cls]) * PACE_PEAK_RELAX;
        g_pacePeak[cls] = cost > relaxed ? cost : relaxed;
    }

    // Pace only while the sim peak exceeds the render peak and the cap interval,
    // and evening stays inside the benefit bound.
    // A GPU bound scene raises both peaks together, so it does not engage.
    // Natural pace is 62 sim frames a second plus the render frames the remaining time affords,
    // and it is at least the cap interval.
    double capDt = EngineCapInterval();
    double target = g_pacePeak[1] > g_pacePeak[0] ? g_pacePeak[1]
                                                  : g_pacePeak[0];
    bool primed = g_paceEma[0] > 0.0 && g_paceEma[1] > 0.0;
    double gap = g_pacePeak[1] - g_pacePeak[0];
    double natural = g_paceEma[1];  // Fallback when every frame is a sim frame.
    {
        // The unpaced rate is the slower of the cap and the work feasible rate.
        // The bucket reclaims sim frame overshoot, so a render frame costs its real time.
        double simPerSec = 1.0 / g_simDt;
        double tSim = simPerSec * g_paceEma[1];
        if (tSim < 1.0 && g_paceEma[0] > 0.0002) {
            double frames = simPerSec + (1.0 - tSim) / g_paceEma[0];
            natural = 1.0 / frames;
        }
        if (capDt > natural)
            natural = capDt;
    }
    bool shouldPace = g_paceEngaged
        ? primed && gap > PACE_GAP_MIN * 0.5 &&
              (capDt <= 0.0 || target > capDt * 1.03) &&
              target <= natural * PACE_MAX_TANK_STAY
        : primed && gap > PACE_GAP_MIN &&
              (capDt <= 0.0 || target > capDt * PACE_OVER_CAP) &&
              target <= natural * PACE_MAX_TANK_ENGAGE;
    if (shouldPace != g_paceEngaged) {
        if (g_paceEdgeSince <= 0.0)
            g_paceEdgeSince = now;
        if (now - g_paceEdgeSince >=
            (g_paceEngaged ? PACE_DISENGAGE_SEC : PACE_ENGAGE_SEC)) {
            g_paceEngaged = shouldPace;
            g_paceEdgeSince = 0.0;
            g_paceP = shouldPace ? target : 0.0;
        }
    } else {
        g_paceEdgeSince = 0.0;
    }

    // Slew rate limited interval: the grid never steps, it glides.
    if (g_paceEngaged && g_paceP > 0.0) {
        double lim = g_paceP *
                     (target > g_paceP ? PACE_SLEW_UP : PACE_SLEW_DOWN);
        double d = target - g_paceP;
        if (d > lim)  d = lim;
        if (d < -lim) d = -lim;
        g_paceP += d;
    }
}

// Post load trace. It logs only while Logging is on.
// Times are seconds since the camera disappeared.
// The trace recrods key edges, input gates, warmup ticks, hitches, and player speed.
static LONGLONG g_ltEpochQpc = 0;       // Passthrough start. The timeline zero.
static LONGLONG g_ltSeedQpc = 0;        // Fixed step hand over.
static LONGLONG g_ltMonitorUntil = 0;   // per present polling stops here
static bool  g_ltInPassthrough = false;
static int   g_ltCine = -1, g_ltMove = -1, g_ltLook = -1;
static bool  g_ltSpace = false, g_ltW = false;
static float g_ltPeakSpeed = 0.0f;
static bool  g_ltPeakLogged = false;
static constexpr double LT_MONITOR_LOAD_S = 60.0;  // bound while loading
static constexpr double LT_MONITOR_PLAY_S = 15.0;  // after hand over
static constexpr double LT_PEAK_REPORT_S  = 3.0;   // peak speed report after hand over

static double LtSeconds(LONGLONG q) { return LatMs(g_ltEpochQpc, q) / 1000.0; }

// WorldInfo.TimeSeconds, the simulation clock.
// Logged beside the real time stamp so a rate error is visible
static float GameTimeSeconds() {
    uintptr_t wi = GetWorldInfoPtr();
    float t = -1.0f;
    if (wi)
        ReadAt<float>(wi + AWorldInfoOffsets::TimeSeconds, &t);
    return t;
}

static float PlayerSpeed() {
    uintptr_t pawn = GetPlayerPawnPtr();
    FVector v{};
    if (!pawn || !ReadAt<FVector>(pawn + AActorOffsets::Velocity, &v))
        return -1.0f;
    return sqrtf(LengthSq(v));
}

static bool ReadPlayerGates(int* cine, int* move, int* look) {
    uintptr_t ctrl = GetLocalPlayerController();
    DWORD flags = 0;
    unsigned char mv = 0, lk = 0;
    if (!ctrl ||
        !ReadAt<DWORD>(ctrl + APlayerControllerOffsets::Flags, &flags) ||
        !ReadAt<unsigned char>(ctrl + APlayerControllerOffsets::bIgnoreMoveInput, &mv) ||
        !ReadAt<unsigned char>(ctrl + APlayerControllerOffsets::bIgnoreLookInput, &lk))
        return false;
    *cine = (flags & APlayerControllerOffsets::bCinematicMode_Mask) ? 1 : 0;
    *move = mv;
    *look = lk;
    return true;
}

// Level load machinery state: cached IsLoadingLevel, high priority (drain fully)
// loading, a pending blocking flush, and whether the Bink loading movie still
// owns (swallows) input.
struct LoadProbes { int ldg, hp, blk, bink, uiBlk; };

// The UTdUIInteraction (GlobalInteractions[1] of the game viewport), or 0.
static uintptr_t GetUIInteraction() {
    uintptr_t engine = 0, viewport = 0, data = 0, ui = 0;
    int count = 0;
    if (!ReadPtrAt(Addr::GEngine, &engine) || !engine ||
        !ReadPtrAt(engine + UGameEngineOffsets::GameViewport, &viewport) ||
        !IsReasonableObject(viewport) ||
        !ReadPtrAt(viewport + UGameViewportClientOffsets::GlobalInteractions_Data, &data) ||
        !ReadAt<int>(viewport + UGameViewportClientOffsets::GlobalInteractions_Num, &count) ||
        count < 2 || !data ||
        !ReadPtrAt(data + sizeof(uintptr_t), &ui) || !IsReasonableObject(ui))
        return 0;
    return ui;
}

// Returns the UI input block semaphore.
// Returns -1 when the chain cannot be resolved.
static int ReadUIInputBlock() {
    uintptr_t ui = GetUIInteraction();
    int sem = -1;
    if (!ui || !ReadAt<int>(ui + UTdUIInteractionOffsets::BlockUIInputSemaphore, &sem))
        return -1;
    return sem;
}

// FName -> narrow string via the engine name table (UTF-16 entries).
static void FNameToString(int index, int number, char* out, size_t cap) {
    uintptr_t data = 0, entry = 0;
    int num = 0;
    out[0] = '\0';
    if (cap < 2 || index < 0 ||
        !ReadPtrAt(Addr::FName_Names_Data, &data) || !data ||
        !ReadAt<int>(Addr::FName_Names_Num, &num) || index >= num ||
        !ReadPtrAt(data + (uintptr_t)index * sizeof(uintptr_t), &entry) || !entry ||
        !CanRead((void*)(entry + FNameEntryOffsets::Name), 2)) {
        snprintf(out, cap, "?%d", index);
        return;
    }
    const wchar_t* w = (const wchar_t*)(entry + FNameEntryOffsets::Name);
    size_t n = 0;
    while (n + 1 < cap - 8 && CanRead((void*)(w + n), 2) && w[n]) {
        out[n] = (w[n] < 0x80) ? (char)w[n] : '?';
        ++n;
    }
    out[n] = '\0';
    if (number > 0)
        snprintf(out + n, cap - n, "_%d", number - 1);
}

static void ObjectName(uintptr_t obj, char* out, size_t cap) {
    int idx = -1, num = 0;
    if (!obj || !ReadAt<int>(obj + UObjectOffsets::Name, &idx) ||
        !ReadAt<int>(obj + UObjectOffsets::NameNumber, &num)) {
        snprintf(out, cap, "?");
        return;
    }
    FNameToString(idx, num, out, cap);
}

// Names every scene on the UI stack when the input block changes.
static void LogUIScenes(LONGLONG now, int uiBlk) {
    uintptr_t ui = GetUIInteraction();
    uintptr_t client = 0, data = 0;
    int count = 0;
    if (!ui || !ReadPtrAt(ui + UTdUIInteractionOffsets::SceneClient, &client) ||
        !IsReasonableObject(client) ||
        !ReadPtrAt(client + UGameUISceneClientOffsets::ActiveScenes_Data, &data) ||
        !ReadAt<int>(client + UGameUISceneClientOffsets::ActiveScenes_Num, &count))
        return;
    char buf[640];
    int len = snprintf(buf, sizeof(buf), "ME-UI: +%.2fs uiBlk=%d scenes(%d):",
                       LtSeconds(now), uiBlk, count);
    for (int i = 0; i < count && i < 8 && len < (int)sizeof(buf) - 96; ++i) {
        uintptr_t scene = 0;
        if (!ReadPtrAt(data + (uintptr_t)i * sizeof(uintptr_t), &scene) ||
            !IsReasonableObject(scene))
            continue;
        char tag[40], cls[40];
        int tagIdx = -1, tagNum = 0;
        ReadAt<int>(scene + UUISceneOffsets::SceneTag, &tagIdx);
        ReadAt<int>(scene + UUISceneOffsets::SceneTag + 4, &tagNum);
        FNameToString(tagIdx, tagNum, tag, sizeof(tag));
        uintptr_t klass = 0;
        ReadPtrAt(scene + UObjectOffsets::Class, &klass);
        ObjectName(klass, cls, sizeof(cls));
        unsigned char inputMode = 0;
        DWORD flags = 0;
        float step = 0.0f, dur = 0.0f;
        ReadAt<unsigned char>(scene + UUISceneOffsets::SceneInputMode, &inputMode);
        ReadAt<DWORD>(scene + UTdUISceneOffsets::Flags, &flags);
        ReadAt<float>(scene + UTdUISceneOffsets::SceneAnimStepPos, &step);
        ReadAt<float>(scene + UTdUISceneOffsets::SceneAnimDuration, &dur);
        len += snprintf(buf + len, sizeof(buf) - len,
                        " [%s(%s) im=%d %s%sanim=%.2f/%.2f]", tag, cls, inputMode,
                        (flags & UTdUISceneOffsets::bShowingScene_Mask) ? "showing " : "",
                        (flags & UTdUISceneOffsets::bHidingScene_Mask) ? "hiding " : "",
                        step, dur);
    }
    snprintf(buf + len, sizeof(buf) - len, "\n");
    Log(buf);
}

static LoadProbes ReadLoadProbes() {
    LoadProbes p{};
    ReadAt<int>(Addr::G_IsLoadingLevelCached, &p.ldg);
    ReadAt<int>(Addr::G_PriorityAsyncLoading, &p.hp);
    uintptr_t wi = GetWorldInfoPtr();
    DWORD f = 0;
    if (wi && ReadAt<DWORD>(wi + AWorldInfoOffsets::Flags1D0, &f))
        p.blk = (f & AWorldInfoOffsets::bRequestedBlockOnAsyncLoading_Mask) ? 1 : 0;
    uintptr_t bink = 0;
    int hbink = 0;
    if (ReadPtrAt(Addr::G_FullScreenMovieBink, &bink) && bink &&
        ReadAt<int>(bink + 0x50, &hbink))
        p.bink = hbink ? 1 : 0;
    p.uiBlk = ReadUIInputBlock();
    return p;
}
static LoadProbes g_ltProbes = { -1, -1, -1, -1, -1 };  // last logged state

// Physical Space press that never reached UInput::InputKey within a couple of
// presents was consumed above it (UI semaphore / focused scene / movie).
static bool g_ltSpaceDelivered = true;
static int  g_ltSpacePresentsSince = 0;

static void LoadTraceEnterPassthrough(LONGLONG now, const char* reason) {
    if (!g_cfg.logging || g_ltInPassthrough) return;
    g_ltInPassthrough = true;
    g_ltEpochQpc = now;
    g_ltSeedQpc = 0;
    g_ltMonitorUntil = now + (LONGLONG)(LT_MONITOR_LOAD_S / g_qpcSecondsPerCount);
    g_ltCine = g_ltMove = g_ltLook = -1;
    g_ltProbes = { -1, -1, -1, -1, -1 };
    g_ltPeakSpeed = 0.0f;
    g_ltPeakLogged = false;
    g_ltSpaceDelivered = true;
    char buf[96];
    snprintf(buf, sizeof(buf), "ME-LOAD: +0.00s load begins (%s)\n", reason);
    Log(buf);
}

static void LoadTracePassthroughTick(LONGLONG now, float deltaSeconds) {
    if (!g_cfg.logging || !g_ltEpochQpc || deltaSeconds < 0.1f) return;
    char buf[128];
    snprintf(buf, sizeof(buf), "ME-LOAD: +%.2fs passthrough hitch dt=%.0fms\n",
             LtSeconds(now), deltaSeconds * 1000.0f);
    Log(buf);
}

static void LtExtendWindow(LONGLONG now) {
    LONGLONG until = now + (LONGLONG)(LT_MONITOR_PLAY_S / g_qpcSecondsPerCount);
    if (until > g_ltMonitorUntil)
        g_ltMonitorUntil = until;
}

// Cinematics are traced wherever they start, not only after loads: cinematic
// mode turning on opens the monitoring window (a fresh timeline zero when none
// is open), and the window is held open while it is on and for one more span
// after it drops, so the skip and the Kismet that follows it are captured.
static int g_ctCine = -1;

static void CineTraceArm(LONGLONG now) {
    if (!g_cfg.logging || g_qpcSecondsPerCount <= 0.0)
        return;
    int cine = 0, mv = 0, lk = 0;
    if (!ReadPlayerGates(&cine, &mv, &lk))
        return;
    bool windowOpen = g_ltEpochQpc && now <= g_ltMonitorUntil;
    if (cine != g_ctCine) {
        g_ctCine = cine;
        if (cine && !windowOpen) {
            g_ltEpochQpc = now;
            g_ltInPassthrough = false;
            g_ltCine = g_ltMove = g_ltLook = -1;
            g_ltProbes = { -1, -1, -1, -1, -1 };
            g_ltSpaceDelivered = true;
            char buf[96];
            snprintf(buf, sizeof(buf), "ME-CINE: +0.00s cinematic mode on (gt=%.3f)\n",
                     GameTimeSeconds());
            Log(buf);
            windowOpen = true;
        }
        if (windowOpen)
            LtExtendWindow(now);
    } else if (cine && windowOpen) {
        LtExtendWindow(now);
    }
}

// Per present, all phases, while the monitoring window is open.
static void LoadTracePresent(LONGLONG now) {
    if (!g_cfg.logging || !g_ltEpochQpc || now > g_ltMonitorUntil) return;
    char buf[192];

    bool space = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
    if (space != g_ltSpace) {
        g_ltSpace = space;
        snprintf(buf, sizeof(buf), "ME-KEY: +%.2fs Space %s\n", LtSeconds(now),
                 space ? "down" : "up");
        Log(buf);
        if (space) {
            g_ltSpaceDelivered = false;
            g_ltSpacePresentsSince = 0;
        }
    }
    if (!g_ltSpaceDelivered && ++g_ltSpacePresentsSince >= 3) {
        g_ltSpaceDelivered = true;
        int c = 0, m = 0, l = 0;
        ReadPlayerGates(&c, &m, &l);
        LoadProbes sp = ReadLoadProbes();
        snprintf(buf, sizeof(buf),
                 "ME-KEY: +%.2fs Space press SWALLOWED above UInput (uiBlk=%d "
                 "cine=%d bink=%d)\n",
                 LtSeconds(now), sp.uiBlk, c, sp.bink);
        Log(buf);
        if (sp.uiBlk > 0)
            LogUIScenes(now, sp.uiBlk);
    }
    bool w = (GetAsyncKeyState('W') & 0x8000) != 0;
    if (w != g_ltW) {
        g_ltW = w;
        snprintf(buf, sizeof(buf), "ME-KEY: +%.2fs W %s\n", LtSeconds(now),
                 w ? "down" : "up");
        Log(buf);
    }

    int cine = 0, mv = 0, lk = 0;
    LoadProbes p = ReadLoadProbes();
    if (ReadPlayerGates(&cine, &mv, &lk) &&
        (cine != g_ltCine || mv != g_ltMove || lk != g_ltLook ||
         p.ldg != g_ltProbes.ldg || p.hp != g_ltProbes.hp ||
         p.blk != g_ltProbes.blk || p.bink != g_ltProbes.bink ||
         p.uiBlk != g_ltProbes.uiBlk)) {
        bool uiBlkChanged = p.uiBlk != g_ltProbes.uiBlk && g_ltProbes.uiBlk != -1;
        g_ltCine = cine; g_ltMove = mv; g_ltLook = lk; g_ltProbes = p;
        snprintf(buf, sizeof(buf),
                 "ME-GATE: +%.2fs gt=%.3f cine=%d ignMove=%d ignLook=%d ldg=%d hp=%d "
                 "blk=%d bink=%d uiBlk=%d\n",
                 LtSeconds(now), GameTimeSeconds(), cine, mv, lk, p.ldg, p.hp,
                 p.blk, p.bink, p.uiBlk);
        Log(buf);
        if (uiBlkChanged)
            LogUIScenes(now, p.uiBlk);
    }

    // Pawn population: a change timestamps AI spawns/despawns (Kismet actor
    // factories) against the same real time and game time clocks.
    {
        static int s_lastPawns = -1;
        int pawns = 0;
        uintptr_t wi = GetWorldInfoPtr();
        uintptr_t pawn = 0;
        if (wi && ReadPtrAt(wi + AWorldInfoOffsets::PawnList, &pawn)) {
            while (pawn && pawns < MAX_PAWN_WALK && IsReasonableObject(pawn)) {
                ++pawns;
                uintptr_t next = 0;
                if (!ReadPtrAt(pawn + APawnOffsets::NextPawn, &next))
                    break;
                pawn = next;
            }
        }
        if (pawns != s_lastPawns) {
            if (s_lastPawns >= 0) {
                snprintf(buf, sizeof(buf), "ME-PAWNS: +%.2fs gt=%.3f pawns %d -> %d\n",
                         LtSeconds(now), GameTimeSeconds(), s_lastPawns, pawns);
                Log(buf);
            }
            s_lastPawns = pawns;
        }
    }

    float v = PlayerSpeed();
    if (v > g_ltPeakSpeed) g_ltPeakSpeed = v;
    if (g_ltSeedQpc && !g_ltPeakLogged &&
        LatMs(g_ltSeedQpc, now) >= LT_PEAK_REPORT_S * 1000.0) {
        g_ltPeakLogged = true;
        snprintf(buf, sizeof(buf),
                 "ME-LOAD: +%.2fs peak speed since load %.0f uu/s\n",
                 LtSeconds(now), g_ltPeakSpeed);
        Log(buf);
    }
}

static void LoadTraceWarmupTick(LONGLONG now, int n, float deltaSeconds,
                                int stopsPending, bool settled,
                                float speedBefore, float speedAfter) {
    if (!g_cfg.logging || !g_ltEpochQpc) return;
    if (g_ltInPassthrough) {
        g_ltInPassthrough = false;
        char b[96];
        snprintf(b, sizeof(b), "ME-LOAD: +%.2fs camera back, warm-up begins\n",
                 LtSeconds(now));
        Log(b);
    }
    int cine = 0, mv = 0, lk = 0;
    ReadPlayerGates(&cine, &mv, &lk);
    LoadProbes p = ReadLoadProbes();
    char buf[224];
    snprintf(buf, sizeof(buf),
             "ME-WARM: +%.2fs n=%d dt=%.1fms stop=%d settled=%d cine=%d "
             "ignMove=%d ignLook=%d ldg=%d hp=%d blk=%d bink=%d v=%.0f->%.0f\n",
             LtSeconds(now), n, deltaSeconds * 1000.0f, stopsPending,
             settled ? 1 : 0, cine, mv, lk, p.ldg, p.hp, p.blk, p.bink,
             speedBefore, speedAfter);
    Log(buf);
}

static void LoadTraceHandover(LONGLONG now, int ticksRun, float deltaSeconds) {
    if (!g_cfg.logging || !g_ltEpochQpc) return;
    g_ltInPassthrough = false;
    g_ltSeedQpc = now;
    g_ltMonitorUntil = now + (LONGLONG)(LT_MONITOR_PLAY_S / g_qpcSecondsPerCount);
    char buf[128];
    snprintf(buf, sizeof(buf),
             "ME-WARM: +%.2fs handover to fixed-step after %d stock ticks (dt=%.1fms)\n",
             LtSeconds(now), ticksRun, deltaSeconds * 1000.0f);
    Log(buf);
}

// Stalls run as stock ticks are logged whenver logging is on: they are rare
// and every one of them is a potential boost site.
static void LoadTraceStall(LONGLONG now, float deltaSeconds, const char* how,
                           float speedBefore, float speedAfter) {
    if (!g_cfg.logging) return;
    int cine = 0, mv = 0, lk = 0;
    ReadPlayerGates(&cine, &mv, &lk);
    LoadProbes p = ReadLoadProbes();
    char buf[224];
    snprintf(buf, sizeof(buf),
             "ME-STALL: +%.2fs gt=%.3f dt=%.1fms %s cine=%d ignMove=%d "
             "ignLook=%d ldg=%d blk=%d v=%.0f->%.0f\n",
             g_ltEpochQpc ? LtSeconds(now) : 0.0, GameTimeSeconds(),
             deltaSeconds * 1000.0f, how, cine, mv, lk, p.ldg, p.blk,
             speedBefore, speedAfter);
    Log(buf);
}

static void LoadTraceHitch(LONGLONG now, float deltaSeconds, int steps,
                           float speedBefore, float speedAfter) {
    if (!g_cfg.logging || !g_ltEpochQpc || now > g_ltMonitorUntil) return;
    if ((double)deltaSeconds <= 2.0 * g_simDt) return;
    int cine = 0, mv = 0, lk = 0;
    ReadPlayerGates(&cine, &mv, &lk);
    LoadProbes p = ReadLoadProbes();
    double covered = steps * g_simDt * 1000.0;
    double carry = deltaSeconds * 1000.0 - covered;  // stays in the accumulator
    char buf[224];
    snprintf(buf, sizeof(buf),
             "ME-HITCH: +%.2fs gt=%.3f dt=%.1fms steps=%d covered=%.0fms carry=%.0fms "
             "cine=%d ignMove=%d ignLook=%d ldg=%d blk=%d v=%.0f->%.0f\n",
             LtSeconds(now), GameTimeSeconds(), deltaSeconds * 1000.0f, steps, covered,
             carry > 0.0 ? carry : 0.0, cine, mv, lk, p.ldg, p.blk,
             speedBefore, speedAfter);
    Log(buf);
}

// Installed only while logging is on.
// A physical Space edge with no ME-KEYIN line was swallowed before UInput, by the loading movie or the UI.
typedef int (__thiscall *fn_SkipCutscene)(void* pc);
typedef void (__thiscall *fn_InterpSetPosition)(void* interp, float newPosition,
                                                int bJump);
typedef int (__thiscall *fn_InputKey)(void* input, int controllerId, int keyIdx,
                                      int keyNum, int event, float amount,
                                      int bGamepad);
static fn_SkipCutscene      g_origSkipCutscene = nullptr;
static fn_InterpSetPosition g_origInterpSetPosition = nullptr;
static fn_InputKey          g_origInputKey = nullptr;

// SkipCutscene returns 0 even when it jumps a matinee.
// The jump is visible as the SetPosition call while that native is on the stack.
static bool g_inSkipNative = false;
static bool g_skipPerformed = false;

static void __fastcall Hook_InterpSetPosition(void* interp, void* /*edx*/,
                                              float newPosition, int bJump) {
    if (g_inSkipNative)
        g_skipPerformed = true;
    g_origInterpSetPosition(interp, newPosition, bJump);
}

static int __fastcall Hook_SkipCutscene(void* pc, void* /*edx*/) {
    DWORD flags = 0, tdFlags = 0;
    ReadAt<DWORD>((uintptr_t)pc + APlayerControllerOffsets::Flags, &flags);
    ReadAt<DWORD>((uintptr_t)pc + APlayerControllerOffsets::TdFlags, &tdFlags);

    // Only a real skip gets the tick after the pump.
    // A press with nothing playing, such as the TdMove_Cutscene walk in before a matinee starts, does nothing in stock and still does nothing here.
    g_skipPerformed = false;
    g_inSkipNative = true;
    int r = g_origSkipCutscene(pc);
    g_inSkipNative = false;
    bool skipped = g_skipPerformed;
    if (skipped)
        g_skipReqThisFrame = true;

    if (g_cfg.logging && g_ltEpochQpc) {
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        if (q.QuadPart <= g_ltMonitorUntil) {
            LtExtendWindow(q.QuadPart);  // keep tracing through the skip's aftermath
            LoadProbes p = ReadLoadProbes();
            char buf[192];
            snprintf(buf, sizeof(buf),
                     "ME-SKIP: +%.2fs gt=%.3f SkipCutscene -> skipped=%d cine=%d "
                     "disableSkip=%d ldg=%d bink=%d uiBlk=%d\n",
                     LtSeconds(q.QuadPart), GameTimeSeconds(), skipped ? 1 : 0,
                     (flags & APlayerControllerOffsets::bCinematicMode_Mask) ? 1 : 0,
                     (tdFlags & APlayerControllerOffsets::bDisableSkipCutscenes_Mask) ? 1 : 0,
                     p.ldg, p.bink, p.uiBlk);
            Log(buf);
        }
    }
    return r;
}

static int __fastcall Hook_InputKey(void* input, void* /*edx*/, int controllerId,
                                    int keyIdx, int keyNum, int event,
                                    float amount, int bGamepad) {
    int r = g_origInputKey(input, controllerId, keyIdx, keyNum, event, amount,
                           bGamepad);
    if (g_ltEpochQpc && event <= 1) {  // IE_Pressed / IE_Released only
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        if (q.QuadPart <= g_ltMonitorUntil) {
            int spaceIdx = 0;
            ReadAt<int>(Addr::FName_SpaceBar, &spaceIdx);
            if (keyIdx == spaceIdx && event == 0)
                g_ltSpaceDelivered = true;
            char buf[160];
            snprintf(buf, sizeof(buf), "ME-KEYIN: +%.2fs gt=%.3f key=%d%s %s -> %s\n",
                     LtSeconds(q.QuadPart), GameTimeSeconds(), keyIdx,
                     keyIdx == spaceIdx ? "(Space)" : "",
                     event == 0 ? "press" : "release",
                     r ? "accepted" : "DROPPED");
            Log(buf);
        }
    }
    return r;
}

static void __fastcall Hook_EngineTick(void* thisptr, void* /*edx*/,
                                       float deltaSeconds) {
    ++g_total;

    // A new world frees every pooled component, even when the pages stay readable.
    // drop the pools.
    // They refill within two sim ticks
    // Streaming a sublevel keeps GWorld, and the per entry identity check covers it.
    LARGE_INTEGER ltQ;
    QueryPerformanceCounter(&ltQ);

    // A reload can return the new world at the old address, with the camera still valid.
    // World time resets on every load and does not decrease inside a world.
    // A drop in world time is a load.
    static uintptr_t s_lastWorld = 0;
    static float s_lastWorldTime = 0.0f;
    uintptr_t worldNow = 0;
    ReadPtrAt(Addr::GWorld, &worldNow);
    float worldTime = s_lastWorldTime;
    uintptr_t worldInfo = GetWorldInfoPtr();
    if (worldInfo)
        ReadAt<float>(worldInfo + AWorldInfoOffsets::TimeSeconds, &worldTime);
    bool timeReset = worldInfo && worldTime + 0.5f < s_lastWorldTime;
    if (worldNow != s_lastWorld || timeReset) {
        s_lastWorld = worldNow;
        LoadTraceEnterPassthrough(ltQ.QuadPart, timeReset ? "world time reset"
                                                          : "world pointer changed");
        ResetFixedStepState();
    }
    s_lastWorldTime = worldTime;

    // One UI tick per present, at the real frame delta.
    // The delta is not clamped across a stall.
    // The stock viewport tick receives the raw frame delta, and a scene's input block lasts as long as its animation.
    g_uiTickedThisPresent = 0;
    g_uiRealFrameDelta = deltaSeconds > 0.0f ? deltaSeconds : 0.0f;
    g_lateDrainedThisFrame = false;

    CineTraceArm(ltQ.QuadPart);
    LoadTracePresent(ltQ.QuadPart);

    DWORD now = GetTickCount();
    if (g_cfg.logging && now - g_dbgLast >= 2000) {
        char buf[512];
        LONG catchUp  = InterlockedExchange(&g_dbgCatchUp, 0);
        LONG vw       = InterlockedExchange(&g_dbgOverrideWrite, 0);
        LONG draw     = InterlockedExchange(&g_dbgDraw, 0);
        LONG calc     = InterlockedExchange(&g_dbgCalcView, 0);
        LONG calcOv   = InterlockedExchange(&g_dbgCalcViewOv, 0);
        LONG rhiEnd   = InterlockedExchange(&g_dbgRhiEnd, 0);
        LONG vmPush   = InterlockedExchange(&g_dbgVmPush, 0);
        LONG vmSkip   = InterlockedExchange(&g_dbgVmSkip, 0);
        LONG vmSkel   = InterlockedExchange(&g_dbgVmSkel, 0);
        LONG vmCap    = InterlockedExchange(&g_dbgVmCapHit, 0);
        LONG uiTick   = InterlockedExchange(&g_dbgUiTick, 0);
        LONG uiDedup  = InterlockedExchange(&g_dbgUiDedup, 0);
        LONG loadSlice = InterlockedExchange(&g_dbgLoadSlice, 0);
        LONG loadSkip  = InterlockedExchange(&g_dbgLoadSkip, 0);
        LONG vmStale  = InterlockedExchange(&g_dbgVmStale, 0);
        LONG vmFar    = InterlockedExchange(&g_dbgVmFar, 0);
        LONG lsRun    = InterlockedExchange(&g_dbgLsRun, 0);
        LONG lsSkip   = InterlockedExchange(&g_dbgLsSkip, 0);
        LONG mouseRs  = InterlockedExchange(&g_dbgMouseRs, 0);
        LONG mouseRsn = InterlockedExchange(&g_dbgMouseResync, 0);
        LONG cutSnap  = InterlockedExchange(&g_dbgCutSnap, 0);
        LONG vmLead   = InterlockedExchange(&g_dbgVmLead, 0);
        LONG lightPush = InterlockedExchange(&g_dbgLightPush, 0);
        LONG stall    = InterlockedExchange(&g_dbgStall, 0);
        LONG llApply  = InterlockedExchange(&g_dbgLlApply, 0);
        LONG llMoves  = InterlockedExchange(&g_dbgLlMoves, 0);
        LONG llGate   = InterlockedExchange(&g_dbgLlGate, 0);
        LONG llCheck  = InterlockedExchange(&g_dbgLlCheck, 0);
        LONG llMis    = InterlockedExchange(&g_dbgLlMismatch, 0);
        LONG llTrans  = InterlockedExchange(&g_dbgLlTrans, 0);
        LONG llErr    = InterlockedExchange(&g_dbgLlErrMax, 0);
        LONG lateDrain = InterlockedExchange(&g_dbgLateDrain, 0);
        double bRip = g_dbgBatchMeanSum > 0.0
                          ? g_dbgBatchDiffSum / g_dbgBatchMeanSum : 0.0;
        g_dbgBatchDiffSum = g_dbgBatchMeanSum = 0.0;
        float aMin = g_dbgAlphaMin, aMax = g_dbgAlphaMax;
        g_dbgAlphaMin = 2.0f;
        g_dbgAlphaMax = -1.0f;
        snprintf(buf, sizeof(buf),
            "ME-TICK: total=%d sim=%d rdr=%d pass=%d catch=%ld stall=%ld vw=%ld "
            "draw=%ld calc=%ld calcOv=%ld rhiEnd=%ld vmPush=%ld vmSkip=%ld "
            "vmSkel=%ld vmWorld=%ld vmMover=%ld vmLead=%ld vmLight=%ld/%ld pawns=%ld vmCap=%ld "
            "vmStale=%ld vmFar=%ld cut=%ld a=%.2f..%.2f uiTick=%ld "
            "uiDedup=%ld loadSlice=%ld loadSkip=%ld ls=%ld/%ld "
            "mrs=%ld/%ld bRip=%.2f ll=%ld llMv=%ld llGate=%ld llChk=%ld llMis=%ld llTrans=%ld llErr=%ld "
            "ldr=%ld pace=%d/%.2fms paceW=%.0fms costS=%.2f/%.2fms "
            "costR=%.2f/%.2fms age=%.2f/%.2fms dt=%.4fms\n",
            g_total, g_dbgSim, g_dbgRdr, g_dbgPass, catchUp, stall, vw,
            draw, calc, calcOv, rhiEnd, vmPush, vmSkip, vmSkel,
            g_dbgVmWorld, g_dbgVmMover, vmLead, g_dbgVmLight, lightPush,
            g_dbgPawnWalk, vmCap, vmStale,
            vmFar, cutSnap, aMin, aMax, uiTick, uiDedup,
            loadSlice, loadSkip, lsRun, lsSkip, mouseRs, mouseRsn, bRip,
            llApply, llMoves, llGate, llCheck, llMis, llTrans, llErr, lateDrain,
            g_dbgPaceLayer, g_dbgPaceP * 1000.0, g_dbgPaceWaitMs,
            g_paceEma[1] * 1000.0, g_pacePeak[1] * 1000.0,
            g_paceEma[0] * 1000.0, g_pacePeak[0] * 1000.0,
            g_dbgAgeEma[1] * 1000.0, g_dbgAgeEma[0] * 1000.0,
            (double)deltaSeconds * 1000.0);
        Log(buf);
        g_dbgSim = g_dbgRdr = g_dbgPass = 0;
        g_dbgPaceWaitMs = 0.0;
        g_dbgLast = now;
    }

    // No camera (loading, transitions, main menu without world): stock tick
    // under the mod owned cap (the engine limiter is disabled at init).
    if (!GetCameraPOVPtr()) {
        LoadTraceEnterPassthrough(ltQ.QuadPart, "camera gone");
        LoadTracePassthroughTick(ltQ.QuadPart, deltaSeconds);
        ResetFixedStepState();
        // Pacing re-anchors with fresh stats on the next gameplay frame, so
        // reload/respawn spike frames never hold the pace interval down.
        PaceResetStats();
        CapWaitPassthrough();
        ++g_dbgPass;
        g_origEngineTick(thisptr, deltaSeconds);
        return;
    }

    double dt = (double)deltaSeconds;
    if (dt <= 0.0)  dt = 0.0001;
    if (dt > 0.25)  dt = 0.25;

    // Record this present's raw mouse counts + span for the resampler
    // before the pacer sleeps (counts arrived at the message pump, so the
    // timestamp should predate the wait). No-op until the driver is seeded
    // and the pointer chain resolves.
    MouseSamplePresent();

    // One ME-LAT line per completed look gesture.
    if (g_cfg.logging && g_seeded) {
        uintptr_t latInput = GetPlayerInputPtr();
        LARGE_INTEGER latQ;
        if (latInput && QueryPerformanceCounter(&latQ))
            LatProbePresent(latInput, latQ.QuadPart);
    }

    // Predict this frame's class (mirrors the accumulator logic below) and
    // pace the present before any work runs.
    double accPreview = g_accumulator + dt;
    if (accPreview > g_maxAccum) accPreview = g_maxAccum;
    bool willSim = !g_seeded || accPreview >= g_simDt;
    PaceWait(willSim);
    LARGE_INTEGER paceStart;
    QueryPerformanceCounter(&paceStart);

    if (!g_seeded) {
        // Warmup ticks use the real delta, including the engine's 0.4 s clamp.
        // Presents stay at the sim rate.
        // The movie stop countdown marks the white screen.
        // The fixed step driver seeds once the tick delta has settled.
        // That settling tick is itself a normal length tick, and it seeds.
        if (g_warmupActive) {
            int stopsPending = 0;
            ReadAt<int>(Addr::G_MovieStopCountdown, &stopsPending);
            bool settled = stopsPending <= 0 &&
                           (double)deltaSeconds <=
                               g_simDt * WARMUP_SETTLE_FACTOR;
            g_warmupSettled = settled ? g_warmupSettled + 1 : 0;
            if (g_warmupSettled >= WARMUP_SETTLED_NEEDED ||
                g_warmupTicksRun >= WARMUP_MAX_TICKS) {
                g_warmupActive = false;  // hand over: seed on this tick
                LoadTraceHandover(ltQ.QuadPart, g_warmupTicksRun, deltaSeconds);
            } else {
                ++g_warmupTicksRun;
                ++g_dbgPass;
                float vBefore = g_cfg.logging ? PlayerSpeed() : 0.0f;
                g_origEngineTick(thisptr, deltaSeconds);
                LoadTraceWarmupTick(ltQ.QuadPart, g_warmupTicksRun, deltaSeconds,
                                    stopsPending, settled, vBefore,
                                    g_cfg.logging ? PlayerSpeed() : 0.0f);
                LARGE_INTEGER q;
                if (QueryPerformanceCounter(&q))
                    g_lastPresentQpc = q.QuadPart;  // Anchor pacing. This frame is not a cost sample.
                return;
            }
        }
        g_seeded = true;
        RunSimulationTick(thisptr);
        PaceRecord(true, 1, paceStart.QuadPart);
        return;
    }

    bool ltTracing = g_cfg.logging && g_ltEpochQpc &&
                     ltQ.QuadPart <= g_ltMonitorUntil;
    float ltSpeedBefore = ltTracing ? PlayerSpeed() : 0.0f;

    g_skipReqThisFrame = false;

    // Stall parity: a frame the accumulator would have to drop time for runs
    // as one stock variable delta tick instead (see RunStallTick).
    if ((double)deltaSeconds >= g_maxAccum) {
        float v0 = g_cfg.logging ? PlayerSpeed() : 0.0f;
        bool afterRenderOnly = g_lastFrameRenderOnly;
        if (afterRenderOnly)
            RunStallTickAfterRenderOnly(thisptr, deltaSeconds);
        else
            RunStallTick(thisptr, deltaSeconds);
        LoadTraceStall(ltQ.QuadPart, deltaSeconds,
                       afterRenderOnly
                           ? "after render-only frame: world ticked before input"
                           : "run as stock tick",
                       v0, g_cfg.logging ? PlayerSpeed() : 0.0f);
        LARGE_INTEGER q;
        if (QueryPerformanceCounter(&q))
            g_lastPresentQpc = q.QuadPart;  // Anchor pacing. This frame is not a cost sample.
        return;
    }

    g_accumulator += dt;
    if (g_accumulator > g_maxAccum) g_accumulator = g_maxAccum;

    int steps = 0;
    while (g_accumulator >= g_simDt && steps < MAX_SIM_STEPS_PER_FRAME) {
        g_accumulator -= g_simDt;
        RunSimulationTick(thisptr);
        ++steps;
    }

    if (steps > 1)
        InterlockedExchangeAdd(&g_dbgCatchUp, steps - 1);

    if (steps == 0)
        RunRenderOnlyTick(thisptr, deltaSeconds);

    if (ltTracing)
        LoadTraceHitch(ltQ.QuadPart, deltaSeconds, steps, ltSpeedBefore,
                       PlayerSpeed());

    PaceRecord(steps > 0, steps, paceStart.QuadPart);
}

static bool CheckOverrideGlobals() {
    if (!CanWrite((void*)Addr::LP_bOverrideView, sizeof(int)) ||
        !CanWrite((void*)Addr::LP_OverrideLocation, sizeof(FVector)) ||
        !CanWrite((void*)Addr::LP_OverrideRotation, sizeof(FRotator)))
        return false;

    // The shipped game never sets the flag.
    // A nonzero value means this address is some other live global.
    int flag = 0;
    if (!ReadAt<int>(Addr::LP_bOverrideView, &flag) || flag != 0)
        return false;

    return true;
}

static DWORD WINAPI InitThread(LPVOID) {
    char buf[256];

    // Config first: everything below (logging, rates, toggles) depends on it.
    LoadConfig(g_hSelf);
    ResolveExeSections();
    g_simDt    = 1.0 / (double)g_cfg.simTickRate;
    g_simDtF   = (float)g_simDt;
    g_maxAccum = g_simDt * (double)MAX_SIM_STEPS_PER_FRAME;
    g_loadSliceDt = (g_cfg.loadPaceRate > 0.0f)
                        ? 1.0 / (double)g_cfg.loadPaceRate : 0.0;
    g_lsGateEnabled = (g_cfg.loadPaceRate > 0.0f) ? 1 : 0;
    g_wallSecondsPerSimTick = g_simDtF;
    if (g_cfg.worldMeshRangeM > 0.0f) {
        float uu = g_cfg.worldMeshRangeM * 100.0f;  // 1 uu = 1 cm
        g_worldMeshRangeSqUU = uu * uu;
    }
    Log("ME-INIT: build " MOD_VERSION "\n");
    snprintf(buf, sizeof(buf),
        "ME-INIT: config: TickRate=%.1f PaceRate=%.1f FirstPerson=%d "
        "WorldMeshes=%d Movers=%d MoverLead=%d Lights=%d Range=%.0fm Cap=%.1f "
        "EvenPacing=%d MouseResample=%d LateLatchMouse=%d LateLatchMoves=%d Logging=%d\n",
        g_cfg.simTickRate, g_cfg.loadPaceRate,
        g_cfg.smoothFirstPerson ? 1 : 0, g_cfg.smoothWorldMeshes ? 1 : 0,
        g_cfg.smoothMovers ? 1 : 0, g_cfg.moverLead ? 1 : 0,
        g_cfg.smoothLights ? 1 : 0,
        g_cfg.worldMeshRangeM, g_cfg.framerateCap,
        g_cfg.evenPacing ? 1 : 0, g_cfg.mouseResample ? 1 : 0,
        g_cfg.lateLatchMouse ? 1 : 0, g_cfg.lateLatchMoves ? 1 : 0,
        g_cfg.logging ? 1 : 0);
    Log(buf);

    for (int i = 0; i < 300; ++i) {
        uintptr_t engine = 0;
        ReadPtrAt(Addr::GEngine, &engine);
        if (engine) {
            uintptr_t vtable = 0;
            ReadPtrAt(engine, &vtable);
            if (vtable > 0x00400000 && vtable < 0x02000000) break;
        }
        Sleep(100);
    }

    uintptr_t engine = 0;
    ReadPtrAt(Addr::GEngine, &engine);
    uintptr_t vtable = 0;
    if (engine) ReadPtrAt(engine, &vtable);
    uintptr_t tickFn = 0;
    if (vtable) ReadPtrAt(vtable + 0x114, &tickFn);  // slot 69

    snprintf(buf, sizeof(buf), "ME-INIT: GEngine=%08X vtable[69]=%08X\n",
        (unsigned)engine, (unsigned)tickFn);
    Log(buf);

    if (!tickFn || !IsEngineCodePtr(tickFn))
        tickFn = Addr::UTdGameEngine_Tick;

    if (!engine) { Log("ME-INIT: FAILED to resolve GEngine\n"); return 1; }

    // Turning bSmoothFrameRate back on from the console stacks the engine limiter on this one.
    DWORD* smoothBits =
        (DWORD*)(engine + UGameEngineOffsets::bSmoothFrameRate_Bitfield);
    float* maxSmoothed =
        (float*)(engine + UGameEngineOffsets::MaxSmoothedFrameRate);
    float wantedCap = 0.0f;
    if (g_cfg.framerateCap > 0.0f) {
        wantedCap = g_cfg.framerateCap;
    } else if (g_cfg.framerateCap < 0.0f &&
               (*smoothBits & UGameEngineOffsets::bSmoothFrameRate_Mask) &&
               *maxSmoothed > 1.0f) {
        wantedCap = *maxSmoothed;  // The game's own rate.
    }
    *smoothBits &= ~UGameEngineOffsets::bSmoothFrameRate_Mask;
    if (wantedCap > 0.0f)
        *maxSmoothed = wantedCap;
    g_capOwned = wantedCap > 0.0f;
    snprintf(buf, sizeof(buf),
        "ME-INIT: frame cap (%s): %s%.1f FPS; engine smoothing limiter "
        "disabled\n",
        g_cfg.framerateCap < 0.0f ? "game settings" : "ini override",
        g_capOwned ? "mod-owned at " : "none ",
        g_capOwned ? *maxSmoothed : 0.0f);
    Log(buf);

    g_pGEngine    = Addr::GEngine;
    g_pGDeltaTime = (double*)Addr::GDeltaTime;

    g_overrideGlobalsOk = CheckOverrideGlobals();
    snprintf(buf, sizeof(buf),
        "ME-INIT: view override globals flag=%08X loc=%08X rot=%08X ok=%d\n",
        (unsigned)Addr::LP_bOverrideView, (unsigned)Addr::LP_OverrideLocation,
        (unsigned)Addr::LP_OverrideRotation, g_overrideGlobalsOk ? 1 : 0);
    Log(buf);

    uintptr_t worldTickAddr = Pattern::FindPattern(
        Addr::Pat_UWorld_Tick, Addr::Mask_UWorld_Tick);
    snprintf(buf, sizeof(buf), "ME-INIT: UWorld::Tick=%08X\n",
        (unsigned)worldTickAddr);
    Log(buf);
    if (!worldTickAddr) { Log("ME-INIT: FAILED find UWorld::Tick\n"); return 1; }

    if (!Hook::Install((void*)worldTickAddr, (void*)Hook_WorldTick,
                       (void**)&g_origWorldTick)) {
        Log("ME-INIT: FAILED hook UWorld::Tick\n"); return 1;
    }

    if (!Hook::Install((void*)Addr::UGameViewportClient_Draw,
                       (void*)Hook_ViewportClientDraw,
                       &g_origViewportClientDraw)) {
        Log("ME-INIT: WARN failed hook UGameViewportClient::Draw counter\n");
    }
    if (!Hook::Install((void*)Addr::ULocalPlayer_CalcSceneView,
                       (void*)Hook_CalcSceneView,
                       &g_origCalcSceneView)) {
        Log("ME-INIT: WARN failed hook CalcSceneView counter\n");
    }
    if (!Hook::Install((void*)Addr::FD3D9DynamicRHI_EndDrawingViewport,
                       (void*)Hook_EndDrawingViewport,
                       &g_origEndDrawingViewport)) {
        Log("ME-INIT: WARN failed hook EndDrawingViewport counter\n");
    }
    if (!Hook::Install((void*)Addr::USkeletalMeshComponent_UpdateTransform,
                       (void*)Hook_SkelUpdateTransform,
                       (void**)&g_origSkelUpdateTransform)) {
        Log("ME-INIT: WARN failed hook SkelUpdateTransform registry\n");
    }
    if (!Hook::Install((void*)Addr::FScene_UpdatePrimitiveTransform,
                       (void*)Hook_SceneUpdatePrimitiveTransform,
                       (void**)&g_origSceneUpdateTransform)) {
        Log("ME-INIT: WARN failed hook SceneUpdatePrimitiveTransform registry\n");
    }
    if (!Hook::Install((void*)Addr::FScene_UpdateLightTransform,
                       (void*)Hook_SceneUpdateLightTransform,
                       (void**)&g_origSceneUpdateLightTransform)) {
        Log("ME-INIT: WARN failed hook SceneUpdateLightTransform registry\n");
    }

    // SkipCutscene drives the tick after a completed skip, and is traced.
    // UInput::InputKey is traced only.
    if (!Hook::Install((void*)Addr::ATdPlayerController_SkipCutscene,
                       (void*)Hook_SkipCutscene,
                       (void**)&g_origSkipCutscene))
        Log("ME-INIT: WARN failed hook SkipCutscene\n");
    if (!Hook::Install((void*)Addr::USeqAct_Interp_SetPosition,
                       (void*)Hook_InterpSetPosition,
                       (void**)&g_origInterpSetPosition))
        Log("ME-INIT: WARN failed hook USeqAct_Interp::SetPosition\n");
    if (g_cfg.logging) {
        if (!Hook::Install((void*)Addr::UInput_InputKey,
                           (void*)Hook_InputKey, (void**)&g_origInputKey))
            Log("ME-INIT: WARN failed hook UInput::InputKey trace\n");
    }

    // Both call sites of UGameViewportClient::Tick are redirected here.
    // 0xECD833, inside UGameEngine::Tick, is the live per present caller.
    // 0x7C7A7C is the same call in the secondary world engine tick.
    {
        const uintptr_t sites[2] = { Addr::UISceneTick_CallSite,
                                     Addr::UISceneTick_CallSite2 };
        for (int i = 0; i < 2; ++i) {
            uint8_t expected = 0;
            if (ReadAt<uint8_t>(sites[i], &expected) && expected == 0xE8) {
                uint8_t patch[5];
                patch[0] = 0xE8;
                *(int32_t*)(patch + 1) =
                    (int32_t)((uintptr_t)Hook_UISceneTickGate - (sites[i] + 5));
                Hook::Patch(sites[i], patch, 5);
                snprintf(buf, sizeof(buf),
                    "ME-INIT: UI scene tick gate installed at %08X\n",
                    (unsigned)sites[i]);
                Log(buf);
            } else {
                snprintf(buf, sizeof(buf),
                    "ME-INIT: WARN UI tick call site %08X byte mismatch\n",
                    (unsigned)sites[i]);
                Log(buf);
            }
        }
    }

    // HUD real time fade delta repair.
    if (!Hook::Install((void*)Addr::ATdHUD_TickSpecial,
                       (void*)Hook_HudTickSpecial,
                       (void**)&g_origHudTickSpecial)) {
        Log("ME-INIT: WARN failed hook ATdHUD::TickSpecial (HUD fades)\n");
    }

    // Level streaming cadence gate: retarget the three per present stepper
    // call sites to run only on sim presents (stock invocation schedule).
    {
        struct { uintptr_t site; void* gate; } lsSites[3] = {
            { Addr::LS_CallSite_Draw,        (void*)Gate_UpdateLevelStreaming },
            { Addr::LS_CallSite_TickTail,    (void*)Gate_UpdateLevelStreaming },
            { Addr::LS_CallSite_HelperInner, (void*)Gate_UpdateLevelStreaming },
        };
        for (int i = 0; i < 3; ++i) {
            uint8_t opcode = 0;
            if (ReadAt<uint8_t>(lsSites[i].site, &opcode) && opcode == 0xE8) {
                uint8_t patch[5];
                patch[0] = 0xE8;
                *(int32_t*)(patch + 1) = (int32_t)(
                    (uintptr_t)lsSites[i].gate - (lsSites[i].site + 5));
                Hook::Patch(lsSites[i].site, patch, 5);
                snprintf(buf, sizeof(buf),
                    "ME-INIT: streaming cadence gate at %08X\n",
                    (unsigned)lsSites[i].site);
                Log(buf);
            } else {
                snprintf(buf, sizeof(buf),
                    "ME-INIT: WARN streaming gate site %08X unexpected byte\n",
                    (unsigned)lsSites[i].site);
                Log(buf);
            }
        }
    }

    LARGE_INTEGER qpcFreq;
    if (QueryPerformanceFrequency(&qpcFreq) && qpcFreq.QuadPart > 0)
        g_qpcSecondsPerCount = 1.0 / (double)qpcFreq.QuadPart;

    // One millisecond sleep granularity for the pacer.
    // The DLL lives for the process, and the engine requests the same resolution.
    timeBeginPeriod(1);

    // Stock budget slices on the PaceRate schedule. The budget constant is read from rdata.
    float stockBudget = 0.0f;
    if (ReadAt<float>(Addr::Const_StockSliceBudget, &stockBudget) &&
        stockBudget > 0.0001f && stockBudget < 0.1f)
        g_stockSliceBudget = stockBudget;
    if (!Hook::Install((void*)Addr::UObject_StaticTick,
                       (void*)Hook_StaticTick,
                       (void**)&g_origStaticTick)) {
        Log("ME-INIT: WARN failed hook UObject::StaticTick (loading pace)\n");
    }

    uintptr_t engineTickAddr = tickFn ? tickFn : Addr::UTdGameEngine_Tick;
    if (!Hook::Install((void*)engineTickAddr, (void*)Hook_EngineTick,
                       (void**)&g_origEngineTick)) {
        Log("ME-INIT: FAILED hook UTdGameEngine::Tick\n"); return 1;
    }

    Log("ME Framerate Mod: init complete\n");
    return 0;
}

// The game imports dinput8.dll, so this DLL loads in its place.
// The jumps are naked, so the real function sees the original stack and registers.

static FARPROC g_px_DirectInput8Create  = nullptr;
static FARPROC g_px_DllCanUnloadNow     = nullptr;
static FARPROC g_px_DllGetClassObject   = nullptr;
static FARPROC g_px_DllRegisterServer   = nullptr;
static FARPROC g_px_DllUnregisterServer = nullptr;
static FARPROC g_px_GetdfDIJoystick     = nullptr;

static void LoadRealDinput8() {
    char path[MAX_PATH];
    UINT n = GetSystemDirectoryA(path, MAX_PATH - 16);
    if (n == 0 || n > MAX_PATH - 16) return;
    strcpy_s(path + n, sizeof(path) - n, "\\dinput8.dll");
    HMODULE real = LoadLibraryA(path);
    if (!real) return;
    g_px_DirectInput8Create  = GetProcAddress(real, "DirectInput8Create");
    g_px_DllCanUnloadNow     = GetProcAddress(real, "DllCanUnloadNow");
    g_px_DllGetClassObject   = GetProcAddress(real, "DllGetClassObject");
    g_px_DllRegisterServer   = GetProcAddress(real, "DllRegisterServer");
    g_px_DllUnregisterServer = GetProcAddress(real, "DllUnregisterServer");
    g_px_GetdfDIJoystick     = GetProcAddress(real, "GetdfDIJoystick");
}

extern "C" __declspec(naked) void ProxyDirectInput8Create()  { __asm { jmp dword ptr [g_px_DirectInput8Create] } }
extern "C" __declspec(naked) void ProxyDllCanUnloadNow()     { __asm { jmp dword ptr [g_px_DllCanUnloadNow] } }
extern "C" __declspec(naked) void ProxyDllGetClassObject()   { __asm { jmp dword ptr [g_px_DllGetClassObject] } }
extern "C" __declspec(naked) void ProxyDllRegisterServer()   { __asm { jmp dword ptr [g_px_DllRegisterServer] } }
extern "C" __declspec(naked) void ProxyDllUnregisterServer() { __asm { jmp dword ptr [g_px_DllUnregisterServer] } }
extern "C" __declspec(naked) void ProxyGetdfDIJoystick()     { __asm { jmp dword ptr [g_px_GetdfDIJoystick] } }

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hSelf = hModule;  // for locating framerate-mod.ini next to the DLL
        DisableThreadLibraryCalls(hModule);
        LoadRealDinput8();
        HANDLE init = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (init)
            CloseHandle(init);
    }
    return TRUE;
}
