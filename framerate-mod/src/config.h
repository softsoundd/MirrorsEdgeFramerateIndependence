#pragma once
#include <windows.h>
#include <cstdlib>
#include <cstring>

// Settings read from framerate-mod.ini beside this DLL.
// A missing file or a missing key keeps the defaults below.
// Values are clamped here so a bad edit cannot break the game.
struct ModConfig {
    float simTickRate;       // Fixed simulation rate, in Hz.

    // Loader slices per second.
    // Zero leaves loading scaled with the framerate
    float loadPaceRate;

    bool  smoothFirstPerson; // Interpolates the first person arms, legs, and weapon.
    bool  smoothWorldMeshes; // Interpolates NPCs, the shadow mesh, and cutscene actors.
    bool  smoothMovers;      // Interpolates elevators, doors, and props.
    bool  moverLead;         // Presents a verified constant velocity mover one sim interval ahead, so the mesh meets its collision.
    bool  smoothLights;      // Interpolates moving lights, such as an elevator interior or a lamp on a mover.

    // Metres around the camera inside which world skeletal meshes are interpolated.
    // Zero leaves the range unlimited.
    float worldMeshRangeM;

    // Render cap, in frames per second.
    // Negative one reads the rate from the game settings.
    // Zero leaves rendering uncapped.
    float framerateCap;

    bool  evenPacing;        // Holds each present to the pace of the heavest recent frame class.
    bool  mouseResample;     // Feeds each sim tick one sim interval of real time mouse counts.

    // Shows pending mouse counts on the newest tick's rotation.
    // the simulation still consumes those counts on the tick.
    bool  lateLatchMouse;

    bool  lateLatchMoves;    // Extends the late latch through look constrained moves and per axis states.
    bool  logging;           // Writes framerate-mod.log.
};

static ModConfig g_cfg = {
    62.0f,  // simTickRate
    62.0f,  // loadPaceRate
    true,   // smoothFirstPerson
    true,   // smoothWorldMeshes
    true,   // smoothMovers
    true,   // moverLead
    true,   // smoothLights
    0.0f,   // worldMeshRangeM (unlimited)
    -1.0f,  // framerateCap
    true,   // evenPacing
    true,   // mouseResample. The late latch shows the pending counts, so the motion the resampler holds back stays off screen.
    true,   // lateLatchMouse
    true,   // lateLatchMoves
    false,  // logging
};

static float ReadIniFloat(const char* iniPath, const char* section,
                          const char* key, float fallback) {
    char buf[64];
    GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), iniPath);
    if (!buf[0]) return fallback;
    char* end = nullptr;
    double v = strtod(buf, &end);
    return (end == buf) ? fallback : (float)v;
}

static bool ReadIniBool(const char* iniPath, const char* section,
                        const char* key, bool fallback) {
    return GetPrivateProfileIntA(section, key, fallback ? 1 : 0, iniPath) != 0;
}

static float ClampF(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Reads framerate-mod.ini from the directory that contains `self`.
static void LoadConfig(HMODULE self) {
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    char* slash = strrchr(path, '\\');
    if (!slash) return;
    slash[1] = '\0';

    if (strcat_s(path, sizeof(path), "framerate-mod.ini") != 0)
        return;

    g_cfg.simTickRate =
        ClampF(ReadIniFloat(path, "Simulation", "TickRate", g_cfg.simTickRate),
               10.0f, 1000.0f);

    float pace = ReadIniFloat(path, "Loading", "PaceRate", g_cfg.loadPaceRate);
    g_cfg.loadPaceRate = (pace <= 0.0f) ? 0.0f : ClampF(pace, 10.0f, 1000.0f);

    g_cfg.smoothFirstPerson =
        ReadIniBool(path, "Smoothing", "FirstPerson", g_cfg.smoothFirstPerson);
    g_cfg.smoothWorldMeshes =
        ReadIniBool(path, "Smoothing", "WorldMeshes", g_cfg.smoothWorldMeshes);
    g_cfg.smoothMovers =
        ReadIniBool(path, "Smoothing", "Movers", g_cfg.smoothMovers);
    g_cfg.moverLead =
        ReadIniBool(path, "Smoothing", "MoverLead", g_cfg.moverLead);
    g_cfg.smoothLights =
        ReadIniBool(path, "Smoothing", "Lights", g_cfg.smoothLights);

    float range = ReadIniFloat(path, "Smoothing", "WorldMeshRange",
                               g_cfg.worldMeshRangeM);
    g_cfg.worldMeshRangeM = (range <= 0.0f) ? 0.0f : ClampF(range, 5.0f, 1000.0f);

    float cap = ReadIniFloat(path, "Framerate", "Cap", g_cfg.framerateCap);
    if (cap < 0.0f)       g_cfg.framerateCap = -1.0f;
    else if (cap == 0.0f) g_cfg.framerateCap = 0.0f;
    else                  g_cfg.framerateCap = ClampF(cap, 10.0f, 1000.0f);

    g_cfg.evenPacing =
        ReadIniBool(path, "Framerate", "EvenPacing", g_cfg.evenPacing);

    g_cfg.mouseResample =
        ReadIniBool(path, "Input", "MouseResample", g_cfg.mouseResample);
    g_cfg.lateLatchMouse =
        ReadIniBool(path, "Input", "LateLatchMouse", g_cfg.lateLatchMouse);
    g_cfg.lateLatchMoves =
        ReadIniBool(path, "Input", "LateLatchMoves", g_cfg.lateLatchMoves);

    g_cfg.logging = ReadIniBool(path, "Diagnostics", "Logging", g_cfg.logging);
}
