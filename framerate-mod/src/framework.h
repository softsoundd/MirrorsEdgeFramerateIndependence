#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>

// UE3 / Mirror's Edge struct definitions

struct FVector {
    float X, Y, Z;
};

struct FRotator {
    int Pitch, Yaw, Roll;
};

struct FTPOV {
    FVector Location;   // 0x00
    FRotator Rotation;  // 0x0C
    float FOV;          // 0x18
};                      // 0x1C total

struct FMatrix {
    float M[4][4];
};

struct FTCameraCache {
    float TimeStamp;    // 0x00
    FTPOV POV;          // 0x04
};                      // 0x24 total

struct FBoxSphereBounds {
    FVector Origin;     // 0x00
    FVector BoxExtent;  // 0x0C
    float SphereRadius; // 0x18
};                      // 0x1C total

// UGameEngine field offsets
namespace UGameEngineOffsets {
    constexpr int bSmoothFrameRate_Bitfield = 0x0454; // bitfield DWORD
    // Verified against UTdGameEngine::GetMaxTickRate 0xEC6410:
    //   0xEC6578: test byte ptr [this+0x454], 0x10
    constexpr unsigned long bSmoothFrameRate_Mask = 0x10;
    constexpr int MaxSmoothedFrameRate = 0x0480;
    constexpr int MinSmoothedFrameRate = 0x0484; // clamp floor in GetMaxTickRate
    constexpr int Client       = 0x02B8;        // UWindowsClient* (UEngine::Client)
    constexpr int GameViewport = 0x02C8;        // UGameViewportClient*
}

// UWindowsClient (vtable 0x1A99328)
namespace UWindowsClientOffsets {
    constexpr int AllowJoystickInput = 0x01C0; // Gates the joystick block of FWindowsViewport::ProcessInput at 0x41F1EC.
}

// GlobalInteractions, in input order: console, UIController, player manager.
// The FViewportClient subobject sits at +0x3C, so InputKey sees these fields at +0x10 and +0x14.
namespace UGameViewportClientOffsets {
    constexpr int GlobalInteractions_Data = 0x004C;
    constexpr int GlobalInteractions_Num  = 0x0050;
}

// While this is nonzero, every key is swallowed before player input. The check is at 0x126B350.
namespace UTdUIInteractionOffsets {
    constexpr int SceneClient          = 0x0078; // UGameUISceneClient* (UUIInteraction)
    constexpr int BlockUIInputSemaphore = 0x01A8; // int
}

// UObject name fields.
namespace UObjectOffsets {
    constexpr int Name       = 0x002C; // FName.Index
    constexpr int NameNumber = 0x0030; // FName.Number (0 = none, else external+1)
    constexpr int Class      = 0x0034; // UClass*
}

namespace FNameEntryOffsets {
    constexpr int Name = 0x0010; // wchar_t[]
}

// UI scene stack
namespace UGameUISceneClientOffsets {
    constexpr int ActiveScenes_Data = 0x00F8; // TArray<UUIScene*>
    constexpr int ActiveScenes_Num  = 0x00FC;
}
namespace UUISceneOffsets {
    constexpr int SceneTag       = 0x0160; // FName
    constexpr int SceneInputMode = 0x01EC; // Nonzero scenes block UI input while they animate.
}
namespace UTdUISceneOffsets {
    constexpr int Flags             = 0x0248; // bit3 bShowingScene, bit4 bHidingScene
    constexpr unsigned long bShowingScene_Mask = 0x08;
    constexpr unsigned long bHidingScene_Mask  = 0x10;
    constexpr int SceneAnimStepPos  = 0x02A8; // float
    constexpr int SceneAnimDuration = 0x02AC; // float
}

// ATdHUD field offsets (class size 0x4E8, vtable 0x1D13038)
namespace ATdHUDOffsets {
    constexpr int RealTimeRenderDelta = 0x04C4; // Script PostRender writes this every present.
}

namespace UPrimitiveComponentOffsets {
    constexpr int Scene          = 0x0048; // FSceneInterface*
    constexpr int Owner          = 0x004C; // AActor*
    constexpr int ComponentFlags = 0x0050; // bit0 = bAttached
    constexpr int SceneInfo      = 0x0058; // FPrimitiveSceneInfo*
    constexpr int LocalToWorldDeterminant = 0x0060; // float
    constexpr int LocalToWorld   = 0x0070; // FMatrix
    constexpr int Bounds         = 0x00CC; // FBoxSphereBounds
    constexpr int LastRenderTime = 0x01B0; // float, WorldInfo TimeSeconds domain
    constexpr int DetachFence    = 0x01B4; // must be 0 for the transform enqueue to fire
}

namespace USkeletalMeshComponentOffsets {
    constexpr int MeshObject         = 0x0238; // FSkeletalMeshObject*
    constexpr int SpaceBases_Data    = 0x0240; // FMatrix* (component space bone transforms)
    constexpr int SpaceBases_Num     = 0x0244; // int
    constexpr int ParentAnimComponent = 0x0264; // USkeletalMeshComponent* (bone source if set)
    constexpr int ActiveMorphs       = 0x0298; // TArray<FActiveMorph>
    constexpr int PredictedLODLevel  = 0x02C0; // int
    constexpr int RootMotionMode     = 0x043C; // ERootMotionMode. RMM_Ignore is 2, and it is the default.
}

constexpr int RMM_Ignore = 2;

namespace FSkeletalMeshObjectOffsets {
    constexpr int Update_VtblSlot = 0x10;  // Update(INT LOD, USkeletalMeshComponent*, TArray<FActiveMorph>*)
    constexpr int bHasBeenUpdatedAtLeastOnce = 0x20;
}

// AWorldInfo field offsets
namespace AWorldInfoOffsets {
    constexpr int Flags1D0        = 0x01D0; // bitfield DWORD
    constexpr unsigned long bRequestedBlockOnAsyncLoading_Mask = 0x100; // bit 8: blocking flush pump runs before the next UWorld::Tick
    constexpr int TimeDilation    = 0x0C2C; // UWorld::Tick scales the actor dt by this.
    constexpr int TimeSeconds     = 0x0C34;
    constexpr int RealTimeSeconds = 0x0C38;
    constexpr int PawnList        = 0x0CE4; // APawn* linked list head
}

namespace ACameraOffsets {
    constexpr int Flags1D0         = 0x01D0; // Bit 0 is bLockedFOV.
    constexpr unsigned long bLockedFOV_Mask = 1u << 0;
    constexpr int LockedFOV        = 0x01D4; // GetFOVAngle returns this while the FOV is locked.
    constexpr int CameraCache      = 0x04D0; // FTCameraCache
    constexpr int ModifierList_Num = 0x055C; // Count of UCameraModifier pointers. A modifier can reshape the view rotation.
}

// UPlayer field offsets
namespace UPlayerOffsets {
    constexpr int Actor = 0x0040; // APlayerController*
}

// ULocalPlayer inherits UPlayer, no extra offset for Actor

// APlayerController field offsets
namespace APlayerControllerOffsets {
    constexpr int PlayerCamera = 0x02D0; // ACamera*
    constexpr int Flags        = 0x02EC; // bitfield DWORD
    constexpr unsigned long bCinematicMode_Mask = 0x4000; // Bit 14.
    constexpr int bIgnoreMoveInput = 0x02FD; // Byte counter.
    constexpr int bIgnoreLookInput = 0x02FE; // Byte counter.
    constexpr int PlayerInput  = 0x0420; // UTdPlayerInput*.
    constexpr int TdFlags      = 0x052C; // bitfield DWORD (ATdPlayerController)
    constexpr unsigned long bDisableSkipCutscenes_Mask = 0x40; // bit 6
}

// aMouseX and aMouseY accumulate raw counts per present.
// PlayerInput consumes and zeroes them once per world tick.
// The counts are not scaled by dt, so the batch size is the view rotation
namespace UPlayerInputOffsets {
    constexpr int Bits    = 0x00E4; // Bit 0 is gamepad, bit 1 inverts the mouse, and bit 11 enables smoothing.
    constexpr unsigned long Bit_UsingGamepad   = 1u << 0;
    constexpr unsigned long Bit_InvertMouse    = 1u << 1;
    constexpr unsigned long Bit_MouseSmoothing = 1u << 11;
    constexpr int MouseSensitivity = 0x00F8; // 28 times SensitivityMultiplier.
    constexpr int aMouseX = 0x0108;
    constexpr int aMouseY = 0x010C;
}

// UTdPlayerInput class size is 0x188.
namespace UTdPlayerInputOffsets {
    constexpr int Bits = 0x0170;             // bit1 bWalkButtonPressed
    constexpr unsigned long Bit_WalkButton = 1u << 1;
    constexpr int WalkButtonMultiplier = 0x0184; // float (config, default 0.3)
}

namespace AControllerOffsets {
    constexpr int Pawn = 0x01C0; // APawn*
}

namespace AActorOffsets {
    constexpr int CustomTimeDilation = 0x0064; // TickActor dt is the world dt times this.
    constexpr int Base     = 0x0074; // The actor this one stands on.
    constexpr int Location = 0x00E8;
    constexpr int Rotation = 0x00F4;
    constexpr int Velocity = 0x0100;
}

// AddLight copies LightToWorld and WorldToLight into the new scene info.
// An interpolated light is written, pushed through UpdateLightTransform, and restored.
namespace ULightComponentOffsets {
    constexpr int Scene          = 0x0048; // FSceneInterface*
    constexpr int Owner          = 0x004C; // AActor*
    constexpr int ComponentFlags = 0x0050; // bit0 = bAttached
    constexpr int SceneInfo      = 0x0058; // FLightSceneInfo* (null = not in scene)
    constexpr int WorldToLight   = 0x0060; // FMatrix
    constexpr int LightToWorld   = 0x00A0; // FMatrix (origin = light position)
    constexpr int LightFlags     = 0x010C; // bit0 = bEnabled
    constexpr int LightEnvironment = 0x0110; // ULightEnvironmentComponent*. Set when this is the light environment's internal representative.
    constexpr unsigned long bEnabled_Mask = 0x1;
}

namespace APawnOffsets {
    constexpr int Mesh     = 0x0364; // USkeletalMeshComponent*
    constexpr int ViewPitchMin = 0x0384; // float, rotator units (negative)
    constexpr int ViewPitchMax = 0x0388; // float
    constexpr int Weapon   = 0x0398; // AWeapon*
    constexpr int NextPawn = 0x01D0; // APawn* (WorldInfo->PawnList chain)
}

namespace ATdPlayerControllerOffsets {
    // +0x52C bitfield shared with APlayerControllerOffsets::TdFlags:
    // bit4 bRightThumbStickPassedDeadZone (false = UpdateRotation drops DeltaRot)
    constexpr unsigned long bRightThumbStickPassedDeadZone_Mask = 1u << 4;
    constexpr int TargetingPawn            = 0x0548; // Melee soft lock target, from TdMove_Walking.UpdateMeleeAutoLockOn.
    constexpr int CurrentLookAtPoint       = 0x056C; // ATdLookAtPoint*
    constexpr int CurrentForcedLookAtPoint = 0x0570; // ATdLookAtPoint*
    constexpr int myPawn = 0x0624; // ATdPlayerPawn*
}

namespace ATdPawnOffsets {
    constexpr int Bits41C         = 0x041C; // bit2 bConstrainLook (pawn level), bit23 bIsUsingRootRotation
    constexpr unsigned long Bit_ConstrainLook      = 1u << 2;
    constexpr unsigned long Bit_IsUsingRootRotation = 1u << 23; // The animation drives pawn yaw, and FaceRotation returns immediately.
    constexpr int MovementState   = 0x04FE; // byte EMovement
    constexpr int Mesh1p          = 0x05D8; // UTdSkeletalMeshComponent*
    constexpr int Mesh3p          = 0x05DC; // UTdSkeletalMeshComponent*
    constexpr int Mesh1pLowerBody = 0x05E0; // UTdSkeletalMeshComponent*
    constexpr int Moves_Data      = 0x05FC; // TArray<UTdMove*>
    constexpr int Moves_Num       = 0x0600;
    constexpr int TaserDamageLevel = 0x0888; // GetMobilityMultiplier is Clamp(1 - this * 0.01, 0, 1), at 0x12B3000.
    constexpr int MyWeapon        = 0x08F4; // ATdWeapon*
    constexpr int MinLookConstraint = 0x052C; // FRotator, pawn level window
    constexpr int MaxLookConstraint = 0x0538; // FRotator
    constexpr int FaceRotationTimeLeft = 0x04D8; // While this is positive, FaceRotation blends the pawn yaw over time.
}

// Look-at pulls and the camera reset replace the mouse delta on both axes, so those presents interpolate.
// bConstrainLook runs the delta through ConstrainAxis.
// bUsePreciseRotation turns the pawn towards a target, and the controller yaw follows.
namespace UTdMoveOffsets {
    constexpr int Bits = 0x004C;
    constexpr unsigned long Bit_ConstrainLook           = 1u << 6;
    constexpr unsigned long Bit_UseAbsoluteYawConstraint = 1u << 7;
    constexpr unsigned long Bit_LookAtTargetLocation    = 1u << 9;
    constexpr unsigned long Bit_LookAtTargetAngle       = 1u << 10;
    constexpr unsigned long Bit_DisableFaceRotation     = 1u << 11;
    constexpr unsigned long Bit_DisableControllerFacingPawnYawRotation = 1u << 12;
    constexpr unsigned long Bit_UsePreciseRotation      = 1u << 17;
    constexpr unsigned long Bit_ReachedPreciseRotation  = 1u << 18;
    constexpr unsigned long Bit_ResetCameraLook         = 1u << 20;
    constexpr unsigned long SteerFlagsMask =
        Bit_LookAtTargetLocation | Bit_LookAtTargetAngle | Bit_ResetCameraLook;
    constexpr int MoveActiveTime    = 0x0088; // Seconds since the move started.

    // UTdMove::Tick runs after UpdateRotation in the same world tick.
    // While T is greater than dt, pawn yaw and controller yaw step by the truncated fraction of the remaining yaw.
    // Bit 12 skips the controller step.
    // On the final tick both yaws snap to PreciseRotation.Yaw.
    constexpr int PreciseRotationInterpolationTime = 0x00A0; // The countdown, in seconds.
    constexpr int PreciseRotationYaw = 0x00A8; // PreciseRotation.Yaw. The rotator starts at +0xA4.
    constexpr int MinLookConstraint = 0x00D8; // Relative to the pawn. World yaw when the window is absolute.
    constexpr int MaxLookConstraint = 0x00E4;
}

// Move indices whose classes override the look window getters or the input path.
// Every other move uses TdMove.
namespace EMove {
    constexpr int Grab        = 3;
    constexpr int WallRunLeft = 4;
    constexpr int WallRunRight = 5;
    constexpr int WallrunJump = 12;
    constexpr int GrabJump    = 13;
    constexpr int Climb       = 21;
    constexpr int Balance     = 29;
    constexpr int WallRunVertical = 40;
}

namespace UTdMoveGrabOffsets {
    constexpr int Bits1B4          = 0x01B4; // bit5 bHangFreeVertigoEffect (input zeroed)
    constexpr unsigned long Bit_HangFreeVertigoEffect = 1u << 5;
    constexpr int CurrentShimmyMove = 0x01C2; // EShimmyType. 3 is NoShimmy.
    constexpr int CurrentFoldedType = 0x01C3; // Nonzero zeroes look input.
    constexpr int ShimmyType_NoShimmy = 3;
}

namespace UTdMoveClimbOffsets {
    constexpr int Bits194           = 0x0194; // bit0 bIsPlayingAnimation
    constexpr unsigned long Bit_IsPlayingAnimation = 1u << 0;
    constexpr int StartTurningAngle = 0x01C8; // int, rotator units
}

namespace UTdMoveWallRunOffsets {
    constexpr int MinContraintWorld = 0x0228; // int, world yaw (bUseAbsoluteYawConstraint)
    constexpr int MaxContraintWorld = 0x022C;
}

namespace UTdMoveWallrunJumpOffsets {
    constexpr int MinContraintWorld = 0x01A4;
    constexpr int MaxContraintWorld = 0x01A8;
}

namespace ATdWeaponOffsets {
    constexpr int Mesh1p = 0x0470; // UTdSkeletalMeshComponent*
    constexpr int Mesh3p = 0x0474; // UTdSkeletalMeshComponent*
}

// UGameEngine field offsets for GamePlayers array
namespace UGameEngineArrayOffsets {
    constexpr int GamePlayers_Data = 0x02BC; // TArray<ULocalPlayer*>::Data
    constexpr int GamePlayers_Num  = 0x02C0; // TArray<ULocalPlayer*>::Num
}

// Interpolation helpers

inline FVector LerpVector(const FVector& a, const FVector& b, float t) {
    return {
        a.X + (b.X - a.X) * t,
        a.Y + (b.Y - a.Y) * t,
        a.Z + (b.Z - a.Z) * t
    };
}

inline FVector SubVector(const FVector& a, const FVector& b) {
    return { a.X - b.X, a.Y - b.Y, a.Z - b.Z };
}

inline float LerpFloat(float a, float b, float t) {
    return a + (b - a) * t;
}

inline FRotator LerpRotator(const FRotator& a, const FRotator& b, float t) {
    auto shortestAngle = [](int from, int to) -> int {
        int diff = to - from;
        // UE3 rotator units: full circle = 65536 (0x10000)
        while (diff > 32768)  diff -= 65536;
        while (diff < -32768) diff += 65536;
        return diff;
    };

    return {
        a.Pitch + (int)(shortestAngle(a.Pitch, b.Pitch) * t),
        a.Yaw   + (int)(shortestAngle(a.Yaw,   b.Yaw)   * t),
        a.Roll  + (int)(shortestAngle(a.Roll,  b.Roll)  * t)
    };
}

inline FTPOV LerpPOV(const FTPOV& a, const FTPOV& b, float t) {
    return {
        LerpVector(a.Location, b.Location, t),
        LerpRotator(a.Rotation, b.Rotation, t),
        LerpFloat(a.FOV, b.FOV, t)
    };
}

// Matrix helpers. UE3 FMatrix uses row vector convention: v' = v * M, and
// A * B applies A first, then B (LocalToWorld = LocalToParent * ParentToWorld).

inline void MatrixMultiply(const FMatrix& a, const FMatrix& b, FMatrix* out) {
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k)
                sum += a.M[i][k] * b.M[k][j];
            out->M[i][j] = sum;
        }
    }
}

inline FVector TransformPoint(const FMatrix& m, const FVector& p) {
    return {
        m.M[0][0] * p.X + m.M[1][0] * p.Y + m.M[2][0] * p.Z + m.M[3][0],
        m.M[0][1] * p.X + m.M[1][1] * p.Y + m.M[2][1] * p.Z + m.M[3][1],
        m.M[0][2] * p.X + m.M[1][2] * p.Y + m.M[2][2] * p.Z + m.M[3][2]
    };
}

// UE3 FRotationMatrix: rows are the rotated basis axes (X=forward, Y=right,
// Z=up), matching FRotationTranslationMatrix in UnMath.h.
inline void BuildRotationMatrix(const FRotator& rot, FMatrix* out) {
    constexpr float kUnrRotToRad = 3.14159265358979f / 32768.0f;

    float p = rot.Pitch * kUnrRotToRad;
    float y = rot.Yaw   * kUnrRotToRad;
    float r = rot.Roll  * kUnrRotToRad;

    float sp = sinf(p), cp = cosf(p);
    float sy = sinf(y), cy = cosf(y);
    float sr = sinf(r), cr = cosf(r);

    out->M[0][0] = cp * cy;
    out->M[0][1] = cp * sy;
    out->M[0][2] = sp;
    out->M[0][3] = 0.0f;

    out->M[1][0] = sr * sp * cy - cr * sy;
    out->M[1][1] = sr * sp * sy + cr * cy;
    out->M[1][2] = -sr * cp;
    out->M[1][3] = 0.0f;

    out->M[2][0] = -(cr * sp * cy + sr * sy);
    out->M[2][1] = cy * sr - cr * sp * sy;
    out->M[2][2] = cr * cp;
    out->M[2][3] = 0.0f;

    out->M[3][0] = 0.0f;
    out->M[3][1] = 0.0f;
    out->M[3][2] = 0.0f;
    out->M[3][3] = 1.0f;
}

inline void BuildCameraToWorld(const FVector& loc, const FRotator& rot,
                               FMatrix* out) {
    BuildRotationMatrix(rot, out);
    out->M[3][0] = loc.X;
    out->M[3][1] = loc.Y;
    out->M[3][2] = loc.Z;
}

// Quaternion helpers for rotation interpolation between two sim poses.
// per tick deltas are small (a few degrees), so normalised lerp is exact
// enough and free of slerp's edge cases.

struct FQuatSimple {
    float X, Y, Z, W;
};

inline FQuatSimple QuatFromRotationMatrix(const FMatrix& m) {
    // Shepperd's method on the 3x3 block (assumes orthonormal basis rows).
    FQuatSimple q;
    float trace = m.M[0][0] + m.M[1][1] + m.M[2][2];
    if (trace > 0.0f) {
        float s = sqrtf(trace + 1.0f);
        q.W = s * 0.5f;
        s = 0.5f / s;
        q.X = (m.M[1][2] - m.M[2][1]) * s;
        q.Y = (m.M[2][0] - m.M[0][2]) * s;
        q.Z = (m.M[0][1] - m.M[1][0]) * s;
    } else {
        int i = 0;
        if (m.M[1][1] > m.M[0][0]) i = 1;
        if (m.M[2][2] > m.M[i][i]) i = 2;
        int j = (i + 1) % 3;
        int k = (j + 1) % 3;
        float s = sqrtf(m.M[i][i] - m.M[j][j] - m.M[k][k] + 1.0f);
        float* qv = &q.X;
        qv[i] = s * 0.5f;
        s = 0.5f / s;
        q.W   = (m.M[j][k] - m.M[k][j]) * s;
        qv[j] = (m.M[i][j] + m.M[j][i]) * s;
        qv[k] = (m.M[i][k] + m.M[k][i]) * s;
    }
    return q;
}

inline void RotationMatrixFromQuat(const FQuatSimple& q, FMatrix* out) {
    float xx = q.X * q.X, yy = q.Y * q.Y, zz = q.Z * q.Z;
    float xy = q.X * q.Y, xz = q.X * q.Z, yz = q.Y * q.Z;
    float wx = q.W * q.X, wy = q.W * q.Y, wz = q.W * q.Z;

    out->M[0][0] = 1.0f - 2.0f * (yy + zz);
    out->M[0][1] = 2.0f * (xy + wz);
    out->M[0][2] = 2.0f * (xz - wy);
    out->M[1][0] = 2.0f * (xy - wz);
    out->M[1][1] = 1.0f - 2.0f * (xx + zz);
    out->M[1][2] = 2.0f * (yz + wx);
    out->M[2][0] = 2.0f * (xz + wy);
    out->M[2][1] = 2.0f * (yz - wx);
    out->M[2][2] = 1.0f - 2.0f * (xx + yy);
}

inline FQuatSimple QuatNlerp(const FQuatSimple& a, FQuatSimple b, float t) {
    float dot = a.X * b.X + a.Y * b.Y + a.Z * b.Z + a.W * b.W;
    if (dot < 0.0f) { b.X = -b.X; b.Y = -b.Y; b.Z = -b.Z; b.W = -b.W; }

    FQuatSimple r = {
        a.X + (b.X - a.X) * t,
        a.Y + (b.Y - a.Y) * t,
        a.Z + (b.Z - a.Z) * t,
        a.W + (b.W - a.W) * t
    };
    float len = sqrtf(r.X * r.X + r.Y * r.Y + r.Z * r.Z + r.W * r.W);
    if (len > 1e-8f) {
        float inv = 1.0f / len;
        r.X *= inv; r.Y *= inv; r.Z *= inv; r.W *= inv;
    } else {
        r = b;
    }
    return r;
}

// Determinant of the upper 3x3 (basis) block. Negative = mirrored transform.
inline float MatrixDet3(const FMatrix& m) {
    return m.M[0][0] * (m.M[1][1] * m.M[2][2] - m.M[1][2] * m.M[2][1])
         - m.M[0][1] * (m.M[1][0] * m.M[2][2] - m.M[1][2] * m.M[2][0])
         + m.M[0][2] * (m.M[1][0] * m.M[2][1] - m.M[1][1] * m.M[2][0]);
}

// True when each basis row of the two transforms points the same way, within minDot.
// Scale and mirror cancel, because both poses of a real mover carry the same signs
inline bool MatrixBasisDeltaSmall(const FMatrix& a, const FMatrix& b,
                                  float minDot) {
    for (int r = 0; r < 3; ++r) {
        float la2 = a.M[r][0]*a.M[r][0] + a.M[r][1]*a.M[r][1] + a.M[r][2]*a.M[r][2];
        float lb2 = b.M[r][0]*b.M[r][0] + b.M[r][1]*b.M[r][1] + b.M[r][2]*b.M[r][2];
        if (la2 < 1e-12f || lb2 < 1e-12f)
            return false;
        float dot = (a.M[r][0]*b.M[r][0] + a.M[r][1]*b.M[r][1] +
                     a.M[r][2]*b.M[r][2]) / sqrtf(la2 * lb2);
        if (dot < minDot)
            return false;
    }
    return true;
}

// Interpolates rotation, translation, and per axis scale.
// Basis rows are normalised before the quaternion conversion, and the scales are lerped back on.
// Rows 0 through 2 have their W forced to 0, and M[3][3] is forced to 1.
// A t outside 0 to 1 extrapolates. Mover lead uses that.
//
// A negative determinant is a mirrored transform.
// The mirror is folded into the sign of the row-0 scale, so the quaternion sees a proper rotation.
// The raw mirrored basis drops the mirror and turns the mesh the worng way.
// The renderer's cached determinant keeps the original sign and would also flip triangle winding.
// Recomposing with the signed scale reproduces both endpoints, whichever axis is mirrored.
inline void RigidLerpMatrix(const FMatrix& a, const FMatrix& b, float t,
                            FMatrix* out) {
    float scaleA[3], scaleB[3];
    for (int r = 0; r < 3; ++r) {
        scaleA[r] = sqrtf(a.M[r][0]*a.M[r][0] + a.M[r][1]*a.M[r][1] +
                          a.M[r][2]*a.M[r][2]);
        scaleB[r] = sqrtf(b.M[r][0]*b.M[r][0] + b.M[r][1]*b.M[r][1] +
                          b.M[r][2]*b.M[r][2]);
    }

    bool mirA = MatrixDet3(a) < 0.0f;
    bool mirB = MatrixDet3(b) < 0.0f;
    if (mirA != mirB) {  // The two poses have opposite handedness, so there is no sweep. Snap to the new one.
        *out = b;
        return;
    }
    if (mirA) {
        scaleA[0] = -scaleA[0];
        scaleB[0] = -scaleB[0];
    }

    FMatrix na = a, nb = b;
    for (int r = 0; r < 3; ++r) {
        float ia = fabsf(scaleA[r]) > 1e-8f ? 1.0f / scaleA[r] : 0.0f;
        float ib = fabsf(scaleB[r]) > 1e-8f ? 1.0f / scaleB[r] : 0.0f;
        for (int c = 0; c < 3; ++c) {
            na.M[r][c] = a.M[r][c] * ia;
            nb.M[r][c] = b.M[r][c] * ib;
        }
    }

    FQuatSimple qa = QuatFromRotationMatrix(na);
    FQuatSimple qb = QuatFromRotationMatrix(nb);
    RotationMatrixFromQuat(QuatNlerp(qa, qb, t), out);

    for (int r = 0; r < 3; ++r) {
        float s = scaleA[r] + (scaleB[r] - scaleA[r]) * t;
        out->M[r][0] *= s;
        out->M[r][1] *= s;
        out->M[r][2] *= s;
        out->M[r][3] = 0.0f;
    }
    out->M[3][0] = a.M[3][0] + (b.M[3][0] - a.M[3][0]) * t;
    out->M[3][1] = a.M[3][1] + (b.M[3][1] - a.M[3][1]) * t;
    out->M[3][2] = a.M[3][2] + (b.M[3][2] - a.M[3][2]) * t;
    out->M[3][3] = 1.0f;
}

// Inverse of a general affine matrix, in the row vector convention.
// Returns false when the basis is singular.
inline bool MatrixInverseAffine(const FMatrix& m, FMatrix* inv) {
    const float a00 = m.M[0][0], a01 = m.M[0][1], a02 = m.M[0][2];
    const float a10 = m.M[1][0], a11 = m.M[1][1], a12 = m.M[1][2];
    const float a20 = m.M[2][0], a21 = m.M[2][1], a22 = m.M[2][2];
    const float c00 = a11 * a22 - a12 * a21, c01 = a12 * a20 - a10 * a22, c02 = a10 * a21 - a11 * a20;
    const float c10 = a02 * a21 - a01 * a22, c11 = a00 * a22 - a02 * a20, c12 = a01 * a20 - a00 * a21;
    const float c20 = a01 * a12 - a02 * a11, c21 = a02 * a10 - a00 * a12, c22 = a00 * a11 - a01 * a10;
    const float det = a00 * c00 + a01 * c01 + a02 * c02;
    if (fabsf(det) < 1e-12f)
        return false;
    const float id = 1.0f / det;
    inv->M[0][0] = c00 * id; inv->M[0][1] = c10 * id; inv->M[0][2] = c20 * id; inv->M[0][3] = 0.0f;
    inv->M[1][0] = c01 * id; inv->M[1][1] = c11 * id; inv->M[1][2] = c21 * id; inv->M[1][3] = 0.0f;
    inv->M[2][0] = c02 * id; inv->M[2][1] = c12 * id; inv->M[2][2] = c22 * id; inv->M[2][3] = 0.0f;
    const float tx = m.M[3][0], ty = m.M[3][1], tz = m.M[3][2];
    for (int j = 0; j < 3; ++j)
        inv->M[3][j] = -(tx * inv->M[0][j] + ty * inv->M[1][j] + tz * inv->M[2][j]);
    inv->M[3][3] = 1.0f;
    return true;
}

// Inverse of a rigid (rotation + translation) matrix.
inline void MatrixInverseRigid(const FMatrix& m, FMatrix* inv) {
    float r00 = m.M[0][0], r01 = m.M[0][1], r02 = m.M[0][2];
    float r10 = m.M[1][0], r11 = m.M[1][1], r12 = m.M[1][2];
    float r20 = m.M[2][0], r21 = m.M[2][1], r22 = m.M[2][2];

    inv->M[0][0] = r00; inv->M[0][1] = r10; inv->M[0][2] = r20; inv->M[0][3] = 0.0f;
    inv->M[1][0] = r01; inv->M[1][1] = r11; inv->M[1][2] = r21; inv->M[1][3] = 0.0f;
    inv->M[2][0] = r02; inv->M[2][1] = r12; inv->M[2][2] = r22; inv->M[2][3] = 0.0f;

    FVector t = { m.M[3][0], m.M[3][1], m.M[3][2] };
    inv->M[3][0] = -(r00 * t.X + r01 * t.Y + r02 * t.Z);
    inv->M[3][1] = -(r10 * t.X + r11 * t.Y + r12 * t.Z);
    inv->M[3][2] = -(r20 * t.X + r21 * t.Y + r22 * t.Z);
    inv->M[3][3] = 1.0f;
}
