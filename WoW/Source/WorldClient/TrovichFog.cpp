#include "WoW/Source/WorldClient/TrovichFog.h"
#include "Engine/Source/Base/Handle.h"
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/Types.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxTexFlags.h"
#include "Engine/Source/Gx/CGxVertexPT.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Services/Texture.h"
#include "Engine/Source/ShaderEffect/ShaderEffect.h"
#include "Engine/Source/ShaderEffect/ShadowCache.h"
#include "Engine/Source/Model2/M2Light.h"
#include "Engine/Source/Model2/M2Scene.h"
#include "Engine/Source/Tempest/CRect.h"
#include "WoW/Source/WorldClient/ShadowQuery.h"
#include "WoW/Source/LiquidSystem/Liquid.h"
#include "Engine/Source/ShaderEffect/ShaderEffectManager.h"
#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"
#include "WoW/Source/Object/ObjectClient/Object_C.h"
#include "WoW/Source/UIUtil/Camera.h"
#include "WoW/Source/Ui/WorldFrame.h"
#include "WoW/Source/WorldClient/DayNight__DNInfo.h"
#include "WoW/Source/WorldClient/FrameTime.h"
#include "WoW/Source/WorldClient/World.h"
#include "WoW/Source/WorldClient/Map.h"
#include "WoW/Source/WorldClient/MapObj.h"
#include "WoW/Source/WorldClient/MapObjDef.h"
#include "WoW/Source/WorldClient/MapObjGroup.h"
#include "WoW/Source/DB/Rec/WMOAreaTableRec.h"
#include "WoW/Source/WorldClient/WorldParam.h"
#include "WoW/Source/WorldClient/DayNight.h"
#include "Engine/Source/lua-5.1/src/lua.h"
#include <cstdio>
#include <cstring>
#include "WoW/Source/WorldClient/DetailDoodad.h"
#include "WoW/Source/WorldClient/SortTable.h"
#include "WoW/Source/WorldClient/MapChunk.h"
#include "Engine/Source/Gx/CGxDeviceD3d/CGxDeviceD3d.h"
#include "Engine/Source/Gx/CGxDeviceD3d9Ex/CGxDeviceD3d9Ex.h"
#include "WoW/Source/WorldClient/WorldScene.h"
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

// TROVICH: see TrovichFog.h

static CVar* s_cvFog;
static CVar* s_cvFogRange;
// stage 2 tuning (the weather module will drive these later)
static CVar* s_cvFogDensity;
static CVar* s_cvFogFalloff;
static CVar* s_cvFogBase;
static CVar* s_cvFogNoise;
static CVar* s_cvFogNoiseScale;
static CVar* s_cvFogSteps;
// stage 3
static CVar* s_cvFogSunScatter;
static CVar* s_cvFogPhase;
// stage 4
static CVar* s_cvFogLampScatter;
static CVar* s_cvFogLampSize;
static CVar* s_cvFogSeaDensity;
static CVar* s_cvFogSeaHeight;
static CVar* s_cvFogSeaLevel;
static CVar* s_cvFogSeaNoise;
static CVar* s_cvFogBanks;
static CVar* s_cvFogAmbient;
static CVar* s_cvFogNaturalLight;
static CVar* s_cvFogFast;
static CVar* s_cvFogStats;
static CVar* s_cvFogTreeRange;
static CVar* s_cvFogGrassRange;
static CTexture* s_noiseTexture;     // NoiseTextureCallback, s5 of the fog shaders

// fog banks, parsed from volumetricFogBanks when it changes
#define FOG_BANKS 8
struct FogBank {
    C3Vector center;
    float radius;    // horizontal, yards: density down to 8 % there
    float height;    // vertical half-size, yards
    float density;   // peak per yard
    float noise;     // 0..1
};
static FogBank s_banks[FOG_BANKS];
static uint32_t s_bankCount;
static char s_bankText[1024];
// far haze
static CVar* s_cvFogHaze;
static CVar* s_cvFogHazeHeight;
static float s_depthRange = 400.0f;  // what the depth textures encode (view z / range): the far clip
// weather presets (TrovichUIFixes reads the weather, writes the fog cvars)
static CVar* s_cvFogWeather;
static CVar* s_cvFogWind;
static int32_t s_lastMode = -1;      // to re-validate the far clip when the fog is switched
// indoors: interior WMO groups fade the fog out (0 = full fog, 1 = none)
static int32_t s_viewerIndoors;
static float s_indoorFade;
static uint32_t s_indoorTimeMs;

// the camera of this frame, taken during the world render (RenderDepth) for the composite
static C3Vector s_cameraPos;
static C3Vector s_rayRight;
static C3Vector s_rayUp;
static C3Vector s_rayForward;
static C3Vector s_axisRight;         // the camera axes, unit (the shadow band rows are built on them)
static C3Vector s_axisUp;
static float s_baseZ;                // the fog's base height: the player's z, smoothed over a few seconds
static uint32_t s_lastTimeMs;
static C3Vector s_wind;              // accumulated noise drift

static CTexture* s_depthTexture;
static CTexture* s_waterTexture;     // the water surfaces' depth, same size and encoding
static uint32_t s_depthWidth;
static uint32_t s_depthHeight;
static int32_t s_recreate = 1;
static int32_t s_haveDepth;          // the depth pass ran this frame
static CGxTex* s_waterThisFrame;     // PrepareWater's target, for Composite

static CGxShader* s_quadVertexShader;
static CGxShader* s_debugPixelShader;
static CGxShader* s_fogPixelShader;
static CGxShader* s_lampsPixelShader;
static CGxShader* s_grassVertexShader;   // the detail doodads (grass) into the water/transparent depth
static CGxShader* s_grassPixelShader;
static int32_t s_shadersTried;

// Diagnostics: <game folder>\TrovichFog.log, one line whenever the state of a stage changes (no per-frame spam)
static void Log(const char* format, ...) {
    FILE* file = fopen("TrovichFog.log", "a");

    if (!file) {
        return;
    }

    va_list args;
    va_start(args, format);
    vfprintf(file, format, args);
    va_end(args);
    fputc('\n', file);
    fclose(file);
}

static void LogOnChange(int32_t slot, const char* text) {
    static char s_last[4][256];

    if (strncmp(s_last[slot], text, sizeof(s_last[slot]) - 1) != 0) {
        strncpy(s_last[slot], text, sizeof(s_last[slot]) - 1);
        Log("%s", text);
    }
}

// the D3D9 device behind Gx (nullptr on other APIs)
static IDirect3DDevice9* D3DDevice() {
    if (g_theGxDevicePtr->m_api == GxApi_D3d9) {
        return static_cast<CGxDeviceD3d*>(g_theGxDevicePtr)->m_d3dDevice;
    }

    if (g_theGxDevicePtr->m_api == GxApi_D3d9Ex) {
        return static_cast<CGxDeviceD3d9Ex*>(g_theGxDevicePtr)->m_d3dDevice;
    }

    return nullptr;
}

// ---- volumetricFogStats ------------------------------------------------------------------------------------------
// CPU: QueryPerformanceCounter around each part (the time to issue it). GPU: a timestamp before and after each part,
// inside a TIMESTAMPDISJOINT frame, read back 4 frames later (no stall; a frame not ready is skipped).
#define PROF_FRAMES 4
struct ProfileGpuFrame {
    IDirect3DQuery9* disjoint;
    IDirect3DQuery9* frequency;
    IDirect3DQuery9* stamps[2 * TrovichFog::PROF_COUNT];
    int32_t issued[2 * TrovichFog::PROF_COUNT];
    int32_t open;     // begun, not yet ended
    int32_t pending;  // ended, results not read
};

static ProfileGpuFrame s_gpuFrames[PROF_FRAMES];
static uint32_t s_gpuFrame;
static int32_t s_gpuReady;          // the queries exist
static LARGE_INTEGER s_cpuBegin[TrovichFog::PROF_COUNT];
static double s_cpuSum[TrovichFog::PROF_COUNT];
static double s_gpuSum[TrovichFog::PROF_COUNT];
static uint32_t s_gpuCount[TrovichFog::PROF_COUNT];
static uint32_t s_profFrames;
static LARGE_INTEGER s_profWindowStart;
static LARGE_INTEGER s_profLastFrame;
static double s_frameSum;
static double s_countSum[5];         // depth pass: model batches, far tree batches, WMO groups, terrain chunks; grass
static char s_statsText[512];
static int32_t s_statsNew;

static int32_t Profiling() {
    return s_cvFogStats && s_cvFogStats->m_intValue;
}

static void ProfileRelease() {
    for (uint32_t f = 0; f < PROF_FRAMES; f++) {
        ProfileGpuFrame& frame = s_gpuFrames[f];

        if (frame.disjoint) frame.disjoint->Release();
        if (frame.frequency) frame.frequency->Release();

        for (uint32_t i = 0; i < 2 * TrovichFog::PROF_COUNT; i++) {
            if (frame.stamps[i]) frame.stamps[i]->Release();
        }

        memset(&frame, 0, sizeof(frame));
    }

    s_gpuReady = 0;
}

static void ProfileCreate() {
    IDirect3DDevice9* device = D3DDevice();

    if (s_gpuReady || !device) {
        return;
    }

    bool ok = true;

    for (uint32_t f = 0; f < PROF_FRAMES && ok; f++) {
        ProfileGpuFrame& frame = s_gpuFrames[f];
        ok = SUCCEEDED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &frame.disjoint)) && SUCCEEDED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &frame.frequency));

        for (uint32_t i = 0; i < 2 * TrovichFog::PROF_COUNT && ok; i++) {
            ok = SUCCEEDED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &frame.stamps[i]));
        }
    }

    if (!ok) {
        ProfileRelease();
        Log("stats: the device has no timestamp queries - CPU times only");
        s_gpuReady = -1;   // do not retry
        return;
    }

    s_gpuReady = 1;
}

// the results of a frame issued PROF_FRAMES ago, if the GPU has them
static void ProfileCollect(ProfileGpuFrame& frame) {
    if (!frame.pending) {
        return;
    }

    frame.pending = 0;
    BOOL disjoint = TRUE;
    UINT64 frequency = 0;

    if (frame.disjoint->GetData(&disjoint, sizeof(disjoint), 0) != S_OK || disjoint || frame.frequency->GetData(&frequency, sizeof(frequency), 0) != S_OK || !frequency) {
        return;
    }

    for (uint32_t s = 0; s < TrovichFog::PROF_COUNT; s++) {
        UINT64 begin = 0;
        UINT64 end = 0;

        if (frame.issued[2 * s] && frame.issued[2 * s + 1]
            && frame.stamps[2 * s]->GetData(&begin, sizeof(begin), 0) == S_OK && frame.stamps[2 * s + 1]->GetData(&end, sizeof(end), 0) == S_OK && end >= begin) {
            s_gpuSum[s] += static_cast<double>(end - begin) * 1000.0 / static_cast<double>(frequency);
            s_gpuCount[s]++;
        }
    }
}

static void ProfileReport(double seconds) {
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    double frames = s_profFrames ? s_profFrames : 1;
    double gpu[TrovichFog::PROF_COUNT];

    for (uint32_t i = 0; i < TrovichFog::PROF_COUNT; i++) {
        gpu[i] = s_gpuCount[i] ? s_gpuSum[i] / s_gpuCount[i] : -1.0;
    }

    double frameMs = s_frameSum / frames;
    snprintf(s_statsText, sizeof(s_statsText),
        "%.0f fps (%.2f ms) | GPU ms: shadows %.2f, depth %.2f, water %.2f (grass %.2f), fog %.2f, lamps %.2f | CPU ms: shadows %.2f, depth %.2f, water %.2f (grass %.2f), fog %.2f | depth pass: %.0f + %.0f far batches, %.0f WMO groups, %.0f chunks, %.0f grass | fast %d",
        frameMs > 0.0 ? 1000.0 / frameMs : 0.0, frameMs,
        gpu[TrovichFog::PROF_SHADOWS], gpu[TrovichFog::PROF_DEPTH], gpu[TrovichFog::PROF_WATER], gpu[TrovichFog::PROF_GRASS], gpu[TrovichFog::PROF_FOG], gpu[TrovichFog::PROF_LAMPS],
        s_cpuSum[TrovichFog::PROF_SHADOWS] / frames, s_cpuSum[TrovichFog::PROF_DEPTH] / frames, s_cpuSum[TrovichFog::PROF_WATER] / frames, s_cpuSum[TrovichFog::PROF_GRASS] / frames, s_cpuSum[TrovichFog::PROF_FOG] / frames,
        s_countSum[0] / frames, s_countSum[1] / frames, s_countSum[2] / frames, s_countSum[3] / frames, s_countSum[4] / frames,
        s_cvFogFast ? s_cvFogFast->m_intValue : 0);
    s_statsNew = 1;
    Log("stats %.1f s: %s", seconds, s_statsText);
    (void)frequency;

    memset(s_cpuSum, 0, sizeof(s_cpuSum));
    memset(s_gpuSum, 0, sizeof(s_gpuSum));
    memset(s_gpuCount, 0, sizeof(s_gpuCount));
    memset(s_countSum, 0, sizeof(s_countSum));
    s_frameSum = 0.0;
    s_profFrames = 0;
}

void TrovichFog::ProfileFrame() {
    if (!Profiling()) {
        if (s_gpuReady == 1) {
            ProfileRelease();
        }

        s_profLastFrame.QuadPart = 0;
        return;
    }

    LARGE_INTEGER now;
    LARGE_INTEGER frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);

    if (s_profLastFrame.QuadPart) {
        s_frameSum += static_cast<double>(now.QuadPart - s_profLastFrame.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
        s_profFrames++;
    } else {
        s_profWindowStart = now;
    }

    s_profLastFrame = now;
    double seconds = static_cast<double>(now.QuadPart - s_profWindowStart.QuadPart) / static_cast<double>(frequency.QuadPart);

    if (seconds >= 2.0 && s_profFrames) {
        ProfileReport(seconds);
        s_profWindowStart = now;
    }

    if (!s_gpuReady) {
        ProfileCreate();
    }

    if (s_gpuReady != 1) {
        return;
    }

    // close the last frame, read the oldest, open this one
    ProfileGpuFrame& last = s_gpuFrames[s_gpuFrame % PROF_FRAMES];

    if (last.open) {
        last.frequency->Issue(D3DISSUE_END);
        last.disjoint->Issue(D3DISSUE_END);
        last.open = 0;
        last.pending = 1;
    }

    s_gpuFrame++;
    ProfileGpuFrame& frame = s_gpuFrames[s_gpuFrame % PROF_FRAMES];
    ProfileCollect(frame);
    memset(frame.issued, 0, sizeof(frame.issued));
    frame.disjoint->Issue(D3DISSUE_BEGIN);
    frame.open = 1;
}

static void ProfileStamp(TrovichFog::ProfileSection section, int32_t end) {
    if (!Profiling()) {
        return;
    }

    if (end) {
        LARGE_INTEGER now;
        LARGE_INTEGER frequency;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&frequency);
        s_cpuSum[section] += static_cast<double>(now.QuadPart - s_cpuBegin[section].QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
    } else {
        QueryPerformanceCounter(&s_cpuBegin[section]);
    }

    ProfileGpuFrame& frame = s_gpuFrames[s_gpuFrame % PROF_FRAMES];

    if (s_gpuReady == 1 && frame.open) {
        frame.stamps[2 * section + end]->Issue(D3DISSUE_END);
        frame.issued[2 * section + end] = 1;
    }
}

void TrovichFog::ProfileBegin(ProfileSection section) {
    ProfileStamp(section, 0);
}

void TrovichFog::ProfileEnd(ProfileSection section) {
    ProfileStamp(section, 1);
}

int32_t TrovichFog::ScriptStats(lua_State* L) {
    if (!s_statsNew) {
        return 0;
    }

    s_statsNew = 0;
    lua_pushstring(L, s_statsText);
    return 1;
}

// 256 x 256 value-noise lattice for NoiseFast: r random, g = r at (+37, +17) (the next z slice)
static void NoiseTextureCallback(EGxTexCommand command, uint32_t width, uint32_t height, uint32_t depth, uint32_t mipLevel, void* userArg, uint32_t& texelStrideInBytes, const void*& texels) {
    static uint32_t s_texels[256 * 256];
    static int32_t s_filled;

    if (command != GxTex_Latch) {
        return;
    }

    if (!s_filled) {
        static uint8_t values[256 * 256];
        uint32_t seed = 0x9E3779B9u;

        for (uint32_t i = 0; i < 256 * 256; i++) {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            values[i] = static_cast<uint8_t>(seed >> 24);
        }

        for (uint32_t y = 0; y < 256; y++) {
            for (uint32_t x = 0; x < 256; x++) {
                uint32_t r = values[y * 256 + x];
                uint32_t g = values[((y + 17) & 255) * 256 + ((x + 37) & 255)];
                s_texels[y * 256 + x] = 0xFF000000u | (r << 16) | (g << 8);   // A8R8G8B8: r in R, g in G
            }
        }

        s_filled = 1;
    }

    texelStrideInBytes = 256 * 4;
    texels = s_texels;
}

static CGxTex* NoiseTexture() {
    static CGxTex* s_gxNoise;

    if (!s_gxNoise) {
        CGxTexFlags flags(GxTex_Linear, 1, 1, 0, 0, 0, 1, 0, 0, 0);
        GxTexCreate(256, 256, GxTex_Argb8888, flags, nullptr, NoiseTextureCallback, s_gxNoise);
    }

    return s_gxNoise;
}

static void DepthTextureCallback(EGxTexCommand command, uint32_t width, uint32_t height, uint32_t depth, uint32_t mipLevel, void* userArg, uint32_t& texelStrideInBytes, const void*& texels) {
}

static void __fastcall DepthTexturesLost() {
    s_recreate = 1;
    ProfileRelease();   // the queries go with the device
    s_gpuReady = 0;
}

static int32_t FogMode() {
    return s_cvFog ? s_cvFog->m_intValue : 0;
}

static float CVarFloat(CVar* var, float fallback, float minimum, float maximum) {
    float value = var ? var->m_floatValue : fallback;
    return value < minimum ? minimum : (value > maximum ? maximum : value);
}

static float FogRange() {
    float range = s_cvFogRange ? s_cvFogRange->m_floatValue : 400.0f;
    return range < 50.0f ? 50.0f : (range > 2000.0f ? 2000.0f : range);
}

// the R32F target, as big as the window; rebuilt on a resize or a device reset
static CGxTex* DepthTarget() {
    const CRect& window = g_theGxDevicePtr->DeviceCurWindow();
    uint32_t width = static_cast<uint32_t>(window.maxX - window.minX + 0.5f);
    uint32_t height = static_cast<uint32_t>(window.maxY - window.minY + 0.5f);

    if (!width || !height || !g_theGxDevicePtr->Caps().m_rttFormat[GxTex_R32F]) {
        char text[256];
        snprintf(text, sizeof(text), "depth target: NOT created - window %u x %u, R32F render target %s", width, height, g_theGxDevicePtr->Caps().m_rttFormat[GxTex_R32F] ? "supported" : "NOT supported");
        LogOnChange(1, text);
        return nullptr;
    }

    if (s_recreate || !s_depthTexture || width != s_depthWidth || height != s_depthHeight) {
        if (s_depthTexture) {
            HandleClose(reinterpret_cast<HOBJECT>(s_depthTexture));
            s_depthTexture = nullptr;
        }

        if (s_waterTexture) {
            HandleClose(reinterpret_cast<HOBJECT>(s_waterTexture));
            s_waterTexture = nullptr;
        }

        CGxTexFlags flags(GxTex_Nearest, 0, 0, 0, 0, 0, 1, 0, 0, 0);
        flags.m_filter = GxTex_Nearest;
        flags.m_wrapU = 0;
        flags.m_wrapV = 0;
        flags.m_renderTarget = 1;
        // a window-sized texture is not a power of two: GxTexCreate refuses GxTex_2d for it (and TextureCreate
        // still hands back a CTexture without a GxTex) - the non-pow2 target is a plain D3D9 texture with
        // NONPOW2CONDITIONAL (clamped, no mips: as here)
        if (!g_theGxDevicePtr->Caps().m_texTarget[GxTex_NonPow2]) {
            LogOnChange(1, "depth target: NOT created - the device has no non-power-of-two textures");
            return nullptr;
        }

        s_depthTexture = TextureCreate(GxTex_NonPow2, width, height, 0, GxTex_R32F, GxTex_R32F, flags, nullptr, DepthTextureCallback, "TrovichFogDepth", 0);
        s_waterTexture = TextureCreate(GxTex_NonPow2, width, height, 0, GxTex_R32F, GxTex_R32F, flags, nullptr, DepthTextureCallback, "TrovichFogWater", 0);
        s_depthWidth = width;
        s_depthHeight = height;
        s_recreate = 0;
        Log("depth target: %u x %u R32F texture %s", width, height, s_depthTexture && TextureGetGxTex(s_depthTexture, 1, nullptr) ? "created" : "FAILED");
    }

    return s_depthTexture ? TextureGetGxTex(s_depthTexture, 1, nullptr) : nullptr;
}

void TrovichFog::Initialize() {
    s_cvFog = CVar::Register("volumetricFog", "volumetric fog: 0 off, 1 on, 2 show its depth pass (Trovich)", 0, "0", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvFogRange = CVar::Register("volumetricFogRange", "yards the volumetric fog is computed over (Trovich)", 0, "400", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvFogDensity = CVar::Register("volumetricFogDensity", "volumetric fog density per yard at its base height (Trovich)", 0, "0.01", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvFogFalloff = CVar::Register("volumetricFogFalloff", "yards over which the fog thins by e above its base (Trovich)", 0, "25", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvFogBase = CVar::Register("volumetricFogBase", "fog base height relative to the player (Trovich)", 0, "0", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvFogNoise = CVar::Register("volumetricFogNoise", "how patchy the fog is, 0..1 (Trovich)", 0, "0.7", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvFogNoiseScale = CVar::Register("volumetricFogNoiseScale", "size of the fog patches, 1 / yards (Trovich)", 0, "0.02", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvFogSteps = CVar::Register("volumetricFogSteps", "ray-march steps per pixel, 4..64 (Trovich)", 0, "16", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvFogSunScatter = CVar::Register("volumetricFogSunScatter", "sun / moon light scattered in the fog, 0..4 (Trovich)", 0, "0.6", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvFogPhase = CVar::Register("volumetricFogPhase", "forward scattering g, 0 = even, 0.9 = only toward the sun (Trovich)", 0, "0.45", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvFogLampScatter = CVar::Register("volumetricFogLampScatter", "lamp / torch halos in the fog, 0..8 (Trovich)", 0, "0.5", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvFogWeather = CVar::Register("volumetricFogWeather", "the server weather: \"<Weather.dbc id> <intensity>\", set by the client (Trovich)", 0, "0 0", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvFogWind = CVar::Register("volumetricFogWind", "how fast the fog patches drift, yards per second (Trovich)", 0, "1.5", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvFogHazeHeight = CVar::Register("volumetricFogHazeHeight", "far haze scale height: yards over which it thins upward (Trovich)", 0, "120", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvFogHaze = CVar::Register("volumetricFogHaze", "far haze beyond the fog range: visibility in yards (Trovich)", 0, "2500", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvFogSeaDensity = CVar::Register("volumetricFogSeaDensity", "sea fog: density per yard at sea level, 0 = none (Trovich)", 0, "0.003", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvFogSeaHeight = CVar::Register("volumetricFogSeaHeight", "sea fog: yards over which it thins by e above the sea level (Trovich)", 0, "15", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvFogSeaLevel = CVar::Register("volumetricFogSeaLevel", "sea fog: the sea level, world z (Trovich)", 0, "0", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvFogSeaNoise = CVar::Register("volumetricFogSeaNoise", "sea fog: how patchy, 0..1 (Trovich)", 0, "0.6", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvFogBanks = CVar::Register("volumetricFogBanks", "fog banks \"x y z radius height density noise; ...\" (at most 8), set by the addon (Trovich)", 0, "", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvFogAmbient = CVar::Register("volumetricFogAmbient", "near fog lit by the world's ambient light (1) or the far-fog colour (0) (Trovich)", 0, "0.7", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvFogNaturalLight = CVar::Register("volumetricFogNaturalLight", "fog light: 1 = the world's light with a softened sun glow, 0 = the classic fog colour and full glow (Trovich)", 0, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvFogFast = CVar::Register("volumetricFogFast", "volumetric fog: 1 = the fast paths (noise texture, steps by distance, skipped shadow lookups, lamp pass cut to the halos), 0 = the old ones (Trovich)", 0, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvFogStats = CVar::Register("volumetricFogStats", "volumetric fog timing: 1 = measure every part (CPU / GPU ms) and report every 2 s (Trovich)", 0, "0", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvFogTreeRange = CVar::Register("volumetricFogTreeRange", "yards to which the big doodads (trees, rocks) beyond the fog range go into its depth (Trovich)", 0, "1500", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvFogGrassRange = CVar::Register("volumetricFogGrassRange", "yards to which the grass goes into the fog's depth (Trovich)", 0, "60", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvFogLampSize = CVar::Register("volumetricFogLampSize", "core radius of a lamp halo in yards: softer glow, no bright dot (Trovich)", 0, "3", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    g_theGxDevicePtr->AddTextureRecreationCallback(DepthTexturesLost);
    Log("init: volumetricFog %s, volumetricFogRange %s", s_cvFog ? "registered" : "NOT registered", s_cvFogRange ? "registered" : "NOT registered");
}

void TrovichFog::SetWeather(uint32_t weatherId, float intensity) {
    if (!s_cvFogWeather) {
        return;
    }

    char value[32];
    snprintf(value, sizeof(value), "%u %.3f", weatherId, intensity);
    s_cvFogWeather->Set(value, true, false, false, false);
}

// World::QueryOutdoors' rule for a WMO group, exterior-lit groups counted as outdoors
static int32_t GroupIndoors(CMapObjDef* def, uint32_t groupNum) {
    if (!def || !def->m_owner || groupNum == 0xFFFF) {
        return 0;
    }

    // exterior (0x8) or EXTERIOR-LIT (0x40): Orgrimmar's canyons are interior groups (0x2000) lit by the sky
    // (0x2841, no vertex colours) and their WMOAreaTable rows mostly lack the street flag; QueryOutdoors' 0x8
    // alone left the city unfogged. Real halls (temples, houses: 0x2805) carry neither.
    int32_t outdoors = (def->m_owner->GetGroupFlags(groupNum) & 0x48) ? 1 : 0;
    CMapObjGroup* group = def->m_owner->GetGroup(groupNum, 0);

    if (group) {
        WMOAreaTableRec* rec = GetWMOAreaRec(def->m_owner->m_header->m_wmoID, def->m_nameSet, group->m_wmoGroupId);

        if (rec) {
            if (rec->m_flags & 0x4) {
                outdoors = 1;
            } else if (rec->m_flags & 0x2) {
                outdoors = 0;
            }
        }
    }

    return !outdoors;
}

void TrovichFog::SetViewerGroup(CMapObjDef* def, uint32_t groupNum) {
    s_viewerIndoors = GroupIndoors(def, groupNum);
}

// eases s_indoorFade toward the camera's indoor state, about one second either way
static void UpdateIndoorFade() {
    uint32_t now = FrameTime::s_curTimeMs;
    float dt = s_indoorTimeMs && now > s_indoorTimeMs ? (now - s_indoorTimeMs) * 0.001f : 0.0f;
    s_indoorTimeMs = now;
    float target = s_viewerIndoors ? 1.0f : 0.0f;
    float step = dt * 1.5f;

    if (s_indoorFade < target) {
        s_indoorFade = s_indoorFade + step > target ? target : s_indoorFade + step;
    } else {
        s_indoorFade = s_indoorFade - step < target ? target : s_indoorFade - step;
    }
}

bool TrovichFog::Active() {
    return FogMode() == 1 && CShaderEffect::s_enableShaders;
}

float TrovichFog::MaxFarClip() {
    return 3200.0f;
}

// The stock outdoor fog is switched off (start / end far beyond anything drawn): the fog shader's far haze
// takes over beyond the marched range - an exponential HEIGHT haze in closed form along each ray, up to the
// scene (the depth pass reaches the far clip) or, for the sky, to where the ray meets the ground. The stock
// distance fog had no height: it either ate the distant peaks or left a sharp line where the sea ends.
void TrovichFog::OverrideOutdoorFog(float& start, float& end, float& density) {
    // indoors the fog passes are off: the stock fog stays, so the outside seen through a door keeps its haze
    if (s_indoorFade >= 0.5f) {
        return;
    }

    start = 90000.0f;
    end = 100000.0f;
    density = 1.0f;
}

void TrovichFog::RenderDepth() {
    s_haveDepth = 0;

    // switched on / off: the far clip limit and the outdoor fog depend on it
    if (FogMode() != s_lastMode) {
        s_lastMode = FogMode();

        if (CWorldParam::cvar_farClip) {
            World::SetFarClip(CWorldParam::cvar_farClip->m_floatValue);
        }
    }
    char text[256];
    snprintf(text, sizeof(text), "depth pass: mode %d, shaders %s", FogMode(), CShaderEffect::s_enableShaders ? "on" : "OFF");
    LogOnChange(0, text);

    if (!FogMode() || !CShaderEffect::s_enableShaders) {
        return;
    }

    // indoors (debug view excepted): no fog, no passes
    UpdateIndoorFade();

    if (FogMode() == 1 && s_indoorFade >= 0.999f) {
        return;
    }

    CGxTex* target = DepthTarget();

    if (!target) {
        LogOnChange(2, "depth pass: no target texture");
        return;
    }

    // the camera as RenderViewDepth sees it (device view = camera-relative rotation, rows right / up / forward)
    C44Matrix view;
    GxXformView(view);
    const C44Matrix& projection = g_theGxDevicePtr->m_projection;
    float tanX = projection.a0 != 0.0f ? 1.0f / projection.a0 : 1.0f;
    float tanY = projection.b1 != 0.0f ? 1.0f / projection.b1 : 1.0f;
    s_cameraPos = CWorldScene::s_activeWorldView;
    s_rayRight = C3Vector(view.a0 * tanX, view.b0 * tanX, view.c0 * tanX);
    s_rayUp = C3Vector(view.a1 * tanY, view.b1 * tanY, view.c1 * tanY);
    s_rayForward = C3Vector(view.a2, view.b2, view.c2);
    s_axisRight = C3Vector(view.a0, view.b0, view.c0);
    s_axisUp = C3Vector(view.a1, view.b1, view.c1);

    // the depth textures cover the whole far clip: the far haze needs the true distance of far mountains
    s_depthRange = World::s_farClip > FogRange() ? World::s_farClip : FogRange();
    s_depthRange = s_depthRange > 3200.0f ? 3200.0f : s_depthRange;

    TrovichFog::ProfileBegin(TrovichFog::PROF_DEPTH);
    CShadowQuery::RenderViewDepth(target, s_depthRange, FogRange(), CVarFloat(s_cvFogTreeRange, 1500.0f, 0.0f, 5000.0f));
    TrovichFog::ProfileEnd(TrovichFog::PROF_DEPTH);
    s_haveDepth = 1;

    if (Profiling()) {
        s_countSum[0] += CShadowQuery::s_bandBuffers[0].m_untextured.m_count + CShadowQuery::s_bandBuffers[0].m_textured.m_count;
        s_countSum[1] += CShadowQuery::s_bandBuffers[2].m_untextured.m_count + CShadowQuery::s_bandBuffers[2].m_textured.m_count;
        s_countSum[2] += CShadowQuery::s_bandBuffers[1].m_groupCount;
        s_countSum[3] += CShadowQuery::TerrainCasterCount(1);
    }

    // how much it drew, logged when the counts change by a lot (bucketed)
    ShadowQueryBuffers& buffers = CShadowQuery::s_bandBuffers[0];
    snprintf(text, sizeof(text), "depth pass: drawn - WMO groups ~%u, model batches ~%u + %u textured, terrain chunks ~%u",
        buffers.m_groupCount / 10 * 10, buffers.m_untextured.m_count / 50 * 50, buffers.m_textured.m_count / 50 * 50, CShadowQuery::TerrainCasterCount(0) / 50 * 50);
    LogOnChange(2, text);
}

static int32_t LoadShaders() {
    if (!s_shadersTried) {
        s_shadersTried = 1;
        g_theGxDevicePtr->ShaderCreate(&s_quadVertexShader, GxSh_Vertex, "Shaders\\Vertex", "TrovichFogQuad", 1);
        g_theGxDevicePtr->ShaderCreate(&s_debugPixelShader, GxSh_Pixel, "Shaders\\Pixel", "TrovichFogDebug", 1);
        g_theGxDevicePtr->ShaderCreate(&s_fogPixelShader, GxSh_Pixel, "Shaders\\Pixel", "TrovichFogVolume", 1);
        g_theGxDevicePtr->ShaderCreate(&s_lampsPixelShader, GxSh_Pixel, "Shaders\\Pixel", "TrovichFogLamps", 1);
        g_theGxDevicePtr->ShaderCreate(&s_grassVertexShader, GxSh_Vertex, "Shaders\\Vertex", "TrovichFogGrass", 1);
        g_theGxDevicePtr->ShaderCreate(&s_grassPixelShader, GxSh_Pixel, "Shaders\\Pixel", "TrovichFogGrassPs", 1);
    }

    return s_quadVertexShader && s_quadVertexShader->Valid() && s_debugPixelShader && s_debugPixelShader->Valid() && s_fogPixelShader && s_fogPixelShader->Valid();
}

// the player's (camera target's) position, else the camera (as ShadowQuery's GetViewerPosition)
static C3Vector ViewerPosition() {
    C3Vector position = DayNight::GetInfo()->m_cameraPos;
    CGCamera* camera = CGWorldFrame::GetActiveCamera();

    if (camera) {
        CGObject_C* object = ClntObjMgrObjectPtr(camera->m_targetGUID, TYPEMASK_OBJECT, __FILE__, __LINE__);

        if (object) {
            position = object->GetPosition();
        }
    }

    return position;
}

static int32_t FastPaths() {
    return !s_cvFogFast || s_cvFogFast->m_intValue;
}

// volumetricFogBanks -> s_banks (only when the text changed)
static void ParseBanks() {
    const char* text = s_cvFogBanks && s_cvFogBanks->m_stringValue.m_str ? s_cvFogBanks->m_stringValue.m_str : "";

    if (!strncmp(text, s_bankText, sizeof(s_bankText) - 1)) {
        return;
    }

    strncpy(s_bankText, text, sizeof(s_bankText) - 1);
    s_bankText[sizeof(s_bankText) - 1] = 0;
    s_bankCount = 0;
    const char* cursor = text;

    while (*cursor && s_bankCount < FOG_BANKS) {
        FogBank bank;
        int32_t used = 0;

        if (sscanf(cursor, " %f %f %f %f %f %f %f%n", &bank.center.x, &bank.center.y, &bank.center.z, &bank.radius, &bank.height, &bank.density, &bank.noise, &used) == 7 && used > 0) {
            if (bank.radius >= 1.0f && bank.height >= 1.0f && bank.density > 0.0f) {
                s_banks[s_bankCount++] = bank;
            }

            cursor += used;
        }

        const char* next = strchr(cursor, ';');

        if (!next) {
            break;
        }

        cursor = next + 1;
    }
}

// sea fog and fog banks: pixel c46..c63 of TrovichFogVolume
static void SetBankConstants() {
    ParseBanks();
    float keep = 1.0f - s_indoorFade;

    float sea[2][4] = {
        { CVarFloat(s_cvFogSeaDensity, 0.003f, 0.0f, 1.0f) * keep, 1.0f / CVarFloat(s_cvFogSeaHeight, 15.0f, 1.0f, 1000.0f), CVarFloat(s_cvFogSeaLevel, 0.0f, -5000.0f, 5000.0f), CVarFloat(s_cvFogSeaNoise, 0.6f, 0.0f, 1.0f) },
        { static_cast<float>(s_bankCount), 0.0f, 0.0f, 0.0f },
    };
    g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 46, &sea[0][0], 2);

    // density = peak * exp(-|offset scaled|^2), the scale making it 8 % (e^-2.5) at the radius / height
    float banks[2 * FOG_BANKS][4] = {};

    for (uint32_t i = 0; i < s_bankCount; i++) {
        const FogBank& bank = s_banks[i];
        banks[i][0] = bank.center.x;
        banks[i][1] = bank.center.y;
        banks[i][2] = bank.center.z;
        banks[i][3] = bank.density * keep;
        banks[FOG_BANKS + i][0] = 1.5811388f / bank.radius;
        banks[FOG_BANKS + i][1] = 1.5811388f / bank.height;
        banks[FOG_BANKS + i][2] = bank.noise;
    }

    g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 48, &banks[0][0], 2 * FOG_BANKS);

    const CImVector& ambient = DayNight::GetInfo()->m_light1.m_ambient;
    int32_t natural = !s_cvFogNaturalLight || s_cvFogNaturalLight->m_intValue;
    float light[2][4] = {
        { ambient.r / 255.0f, ambient.g / 255.0f, ambient.b / 255.0f, natural ? CVarFloat(s_cvFogAmbient, 0.7f, 0.0f, 1.0f) : 0.0f },
        { natural ? 0.4f : 0.0f, FastPaths() ? 1.0f : 0.0f, 0.0f, 0.0f },
    };
    g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 64, &light[0][0], 2);
}

static float Luma(const CImVector& c) {
    return (0.299f * c.r + 0.587f * c.g + 0.114f * c.b) / 255.0f;
}

int32_t TrovichFog::ScriptPosition(lua_State* L) {
    C3Vector position = ViewerPosition();
    lua_pushnumber(L, position.x);
    lua_pushnumber(L, position.y);
    lua_pushnumber(L, position.z);
    lua_pushnumber(L, DayNight::s_mapId);
    return 4;
}

int32_t TrovichFog::ScriptLight(lua_State* L) {
    // the SUN's altitude: TrovichLightBody gives the body that is up, the moon's = the sun's negated
    C3Vector body(0.0f, 0.0f, 1.0f);
    bool moon = false;
    float height = -CShadowCache::s_sunDir.z;

    if (DayNight::TrovichLightBody(body, &moon)) {
        height = moon ? -body.z : body.z;
    }

    lua_pushnumber(L, height);
    lua_pushnumber(L, Luma(DayNight::GetInfo()->m_light1.m_diffuse));
    lua_pushnumber(L, Luma(DayNight::GetInfo()->m_light1.m_ambient));
    lua_pushnumber(L, s_viewerIndoors ? 1 : 0);
    return 4;
}

// stage 2 constants of TrovichFogVolume (TrovichFog.hlsl)
static void SetFogConstants() {
    uint32_t now = FrameTime::s_curTimeMs;
    float dt = s_lastTimeMs && now > s_lastTimeMs ? (now - s_lastTimeMs) * 0.001f : 0.0f;
    dt = dt > 0.25f ? 0.25f : dt;
    s_lastTimeMs = now;

    // base height follows the player slowly (a jump or a short climb does not drag the fog along)
    C3Vector viewer = ViewerPosition();
    float target = viewer.z + CVarFloat(s_cvFogBase, 0.0f, -500.0f, 500.0f);

    if (dt == 0.0f || fabsf(target - s_baseZ) > 300.0f) {
        s_baseZ = target;
    } else {
        s_baseZ += (target - s_baseZ) * (dt * 0.4f > 1.0f ? 1.0f : dt * 0.4f);
    }

    // drift: volumetricFogWind yd/s (presets raise it for storms), slowly turning
    float wind = CVarFloat(s_cvFogWind, 1.5f, 0.0f, 60.0f);
    float angle = now * 0.00001f;
    s_wind.x += cosf(angle) * wind * dt;
    s_wind.y += sinf(angle) * wind * dt;
    s_wind.z += 0.07f * wind * dt;

    const CImVector& color = DayNight::GetInfo()->m_fog.m_color;
    float falloff = CVarFloat(s_cvFogFalloff, 25.0f, 1.0f, 1000.0f);
    float steps = static_cast<float>(static_cast<int32_t>(CVarFloat(s_cvFogSteps, 16.0f, 4.0f, 64.0f)));

    float constants[8][4] = {
        { s_cameraPos.x, s_cameraPos.y, s_cameraPos.z, FogRange() },
        { s_rayRight.x, s_rayRight.y, s_rayRight.z, 0.0f },
        { s_rayUp.x, s_rayUp.y, s_rayUp.z, 0.0f },
        { s_rayForward.x, s_rayForward.y, s_rayForward.z, 0.0f },
        { color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, CVarFloat(s_cvFogDensity, 0.01f, 0.0f, 1.0f) * (1.0f - s_indoorFade) },
        { s_baseZ, 1.0f / falloff, CVarFloat(s_cvFogNoise, 0.7f, 0.0f, 1.0f), CVarFloat(s_cvFogNoiseScale, 0.02f, 0.0005f, 1.0f) },
        { s_wind.x, s_wind.y, s_wind.z, 0.0f },
        { steps, 0.97f, 0.0f, 0.0f },
    };
    g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 0, &constants[0][0], 8);

    // ---- stage 3: the sun (or the moon: sunPath) and the cascade bands ----
    // CShadowCache::s_shadowMatrices[3 + 3 * band ..] map a CAMERA VIEW position (x right, y up, z forward,
    // PostUpdate built them on this frame's view) to map xy (-1..1) and the compare depth. Folded with the
    // camera axes into rows for a camera-relative WORLD position, the shader's march variable.
    const C3Vector& light = CShadowCache::s_sunDir;   // the direction the light travels
    const CImVector& diffuse = DayNight::GetInfo()->m_light1.m_diffuse;
    float scatter = CVarFloat(s_cvFogSunScatter, 0.6f, 0.0f, 4.0f);
    int32_t bands = CShadowCache::GetShadowMode() > SHADOW_MEDIUM && !CShadowCache::s_recreateTextures;

    // the glow and the sun's path through the air: where the sun (moon) is DRAWN - the light direction is held 15
    // degrees up for the shadows, the glow then hung above the setting sun and the low sun shone at noon strength
    C3Vector toward(-light.x, -light.y, -light.z);
    DayNight::TrovichLightBody(toward);

    float sun[2][4] = {
        { toward.x, toward.y, toward.z, CVarFloat(s_cvFogPhase, 0.45f, 0.0f, 0.9f) },
        { diffuse.r / 255.0f * scatter, diffuse.g / 255.0f * scatter, diffuse.b / 255.0f * scatter, CShaderEffect::s_usePcfFiltering ? 1.0f : 0.0f },
    };
    g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 8, &sun[0][0], 2);

    // far haze: depth range, density 3 / visibility at the base (e^-3 = 5 % left after V yards there),
    // scale height, how far below the base the sky rays meet the ground. Indoors it fades with the fog.
    float haze[4] = {
        s_depthRange,
        3.0f / CVarFloat(s_cvFogHaze, 2500.0f, 200.0f, 20000.0f) * (1.0f - s_indoorFade),
        CVarFloat(s_cvFogHazeHeight, 120.0f, 10.0f, 2000.0f),
        30.0f,
    };
    g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 45, haze, 1);
    SetBankConstants();

    float rows[10][4] = {};

    for (int32_t i = 0; i < 9; i++) {
        const C4Vector& m = CShadowCache::s_shadowMatrices[3 + i];
        rows[i][0] = m.x * s_axisRight.x + m.y * s_axisUp.x + m.z * s_rayForward.x;
        rows[i][1] = m.x * s_axisRight.y + m.y * s_axisUp.y + m.z * s_rayForward.y;
        rows[i][2] = m.x * s_axisRight.z + m.y * s_axisUp.z + m.z * s_rayForward.z;
        rows[i][3] = m.w;
    }

    rows[9][0] = bands ? 1.0f : 0.0f;
    g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 10, &rows[0][0], 10);

    if (bands) {
        CShadowBand* band = CShadowCache::s_extShadowBands;

        for (int32_t i = 0; i < 3; i++) {
            g_theGxDevicePtr->RsSet(static_cast<EGxRenderState>(GxRs_Texture1 + i), TextureGetGxTex(band[i].m_textures[band[i].m_current], 1, nullptr));
        }
    }
}

// a quad over the viewport (or the part of it in rect: clip-space x / y, -1..1), texture coordinates with the D3D9
// half-texel offset
static void DrawScreenQuad(const CRect* rect = nullptr) {
    float du = 0.5f / static_cast<float>(s_depthWidth);
    float dv = 0.5f / static_cast<float>(s_depthHeight);
    float x0 = rect ? rect->minX : -1.0f;
    float y0 = rect ? rect->minY : -1.0f;
    float x1 = rect ? rect->maxX : 1.0f;
    float y1 = rect ? rect->maxY : 1.0f;
    float u0 = x0 * 0.5f + 0.5f;
    float u1 = x1 * 0.5f + 0.5f;
    float v0 = 0.5f - y1 * 0.5f;   // top
    float v1 = 0.5f - y0 * 0.5f;

    CGxBuf* vertexBuf = g_theGxDevicePtr->BufStream(GxPoolTarget_Vertex, sizeof(CGxVertexPT), 4);
    CGxVertexPT* v = reinterpret_cast<CGxVertexPT*>(g_theGxDevicePtr->BufLock(vertexBuf));
    v[0].p = C3Vector(x0, y1, 0.0f);
    v[0].tc = C2Vector(u0 + du, v0 + dv);
    v[1].p = C3Vector(x1, y1, 0.0f);
    v[1].tc = C2Vector(u1 + du, v0 + dv);
    v[2].p = C3Vector(x0, y0, 0.0f);
    v[2].tc = C2Vector(u0 + du, v1 + dv);
    v[3].p = C3Vector(x1, y0, 0.0f);
    v[3].tc = C2Vector(u1 + du, v1 + dv);
    g_theGxDevicePtr->BufUnlock(vertexBuf, 0);
    vertexBuf->m_hasData = 1;
    GxPrimVertexPtr(vertexBuf, GxVBF_PT);

    CGxBuf* indexBuf = g_theGxDevicePtr->BufStream(GxPoolTarget_Index, sizeof(uint16_t), 6);
    uint16_t* indices = reinterpret_cast<uint16_t*>(g_theGxDevicePtr->BufLock(indexBuf));
    indices[0] = 0;
    indices[1] = 1;
    indices[2] = 2;
    indices[3] = 2;
    indices[4] = 1;
    indices[5] = 3;
    g_theGxDevicePtr->BufUnlock(indexBuf, 0);
    indexBuf->m_hasData = 1;
    g_theGxDevicePtr->PrimIndexPtr(indexBuf);

    CGxBatch batch;
    batch.m_primType = GxPrim_Triangles;
    batch.m_start = 0;
    batch.m_count = 6;
    batch.m_minIndex = 0;
    batch.m_maxIndex = 3;
    g_theGxDevicePtr->Draw(&batch, 1);
}

// Is a lamp inside an interior WMO group (a temple hall, a house)? The same test the client makes for the camera
// (CWorldScene::LocateViewer3): a ray straight down from the lamp, the WMO group whose floor it meets, its
// flags - interior = 0x2000 without the exterior flag 0x8. Lamps do not move: the answer is cached per light
// and position (a few traces when new lamps come into range, none afterwards).
struct LampPlace {
    CM2Light* light;
    C3Vector pos;
    int32_t indoor;
};

static LampPlace s_lampPlaces[256];
static CRect s_lampRect;          // clip-space rectangle the selected lamps' halos can reach (SetLampConstants)
static int32_t s_lampRectEmpty;

// widen s_lampRect by a lamp's halo: the sphere of its reach, projected (conservatively, at its near side)
static void AddLampToRect(const C3Vector& rel, float reach) {
    float tanX = sqrtf(s_rayRight.x * s_rayRight.x + s_rayRight.y * s_rayRight.y + s_rayRight.z * s_rayRight.z);
    float tanY = sqrtf(s_rayUp.x * s_rayUp.x + s_rayUp.y * s_rayUp.y + s_rayUp.z * s_rayUp.z);
    float z = rel.x * s_rayForward.x + rel.y * s_rayForward.y + rel.z * s_rayForward.z;
    float x = rel.x * s_axisRight.x + rel.y * s_axisRight.y + rel.z * s_axisRight.z;
    float y = rel.x * s_axisUp.x + rel.y * s_axisUp.y + rel.z * s_axisUp.z;
    float nearZ = z - reach;
    CRect lamp;

    if (nearZ < 1.0f || tanX <= 0.0f || tanY <= 0.0f) {
        if (z + reach < 0.0f) {
            return;   // wholly behind the camera
        }

        lamp.minX = -1.0f;
        lamp.minY = -1.0f;
        lamp.maxX = 1.0f;
        lamp.maxY = 1.0f;
    } else {
        lamp.minX = (x - reach) / (nearZ * tanX);
        lamp.maxX = (x + reach) / (nearZ * tanX);
        lamp.minY = (y - reach) / (nearZ * tanY);
        lamp.maxY = (y + reach) / (nearZ * tanY);
    }

    if (s_lampRectEmpty) {
        s_lampRect = lamp;
        s_lampRectEmpty = 0;
    } else {
        s_lampRect.minX = lamp.minX < s_lampRect.minX ? lamp.minX : s_lampRect.minX;
        s_lampRect.minY = lamp.minY < s_lampRect.minY ? lamp.minY : s_lampRect.minY;
        s_lampRect.maxX = lamp.maxX > s_lampRect.maxX ? lamp.maxX : s_lampRect.maxX;
        s_lampRect.maxY = lamp.maxY > s_lampRect.maxY ? lamp.maxY : s_lampRect.maxY;
    }
}

// false: no halo on screen
static bool LampScreenRect(CRect& rect) {
    if (s_lampRectEmpty) {
        return false;
    }

    rect.minX = s_lampRect.minX < -1.0f ? -1.0f : s_lampRect.minX;
    rect.minY = s_lampRect.minY < -1.0f ? -1.0f : s_lampRect.minY;
    rect.maxX = s_lampRect.maxX > 1.0f ? 1.0f : s_lampRect.maxX;
    rect.maxY = s_lampRect.maxY > 1.0f ? 1.0f : s_lampRect.maxY;
    return rect.minX < rect.maxX && rect.minY < rect.maxY;
}
static uint32_t s_lampPlaceNext;

static int32_t LampIndoor(CM2Light* light) {
    for (uint32_t i = 0; i < sizeof(s_lampPlaces) / sizeof(s_lampPlaces[0]); i++) {
        LampPlace& place = s_lampPlaces[i];

        if (place.light == light && place.pos.x == light->m_pos.x && place.pos.y == light->m_pos.y && place.pos.z == light->m_pos.z) {
            return place.indoor;
        }
    }

    C3Vector start = light->m_pos;
    C3Vector end(start.x, start.y, start.z - 1760.0f);
    float dist = 1.0f;
    CMapChunk* chunk = nullptr;
    CMap::VectorIntersectTerrain(&start, &end, &dist, 0x100, &chunk);

    CMapObjDef* defs[2];
    uint32_t groups[4];
    int32_t indoor = 0;

    if (CMap::LocateViewerMapObjs(start, end, dist, defs, groups) && defs[0]) {
        indoor = GroupIndoors(defs[0], groups[0]);
    }

    LampPlace& slot = s_lampPlaces[s_lampPlaceNext++ % (sizeof(s_lampPlaces) / sizeof(s_lampPlaces[0]))];
    slot.light = light;
    slot.pos = light->m_pos;
    slot.indoor = indoor;
    return indoor;
}

// stage 4: up to 8 point lights (M2 lamps, torches, the custom light objects) whose light reaches into the
// fog range, nearest / brightest first (CM2LightList keeps them sorted by M2LightSortKey around the camera).
// Constants for TrovichFogLamps: c20..c27 colour, c28..c35 camera-relative position, c36..c43 attenuation,
// c44.x count. Returns the count.
static uint32_t SetLampConstants() {
    float strength = CVarFloat(s_cvFogLampScatter, 0.5f, 0.0f, 8.0f);
    float core = CVarFloat(s_cvFogLampSize, 3.0f, 0.0f, 30.0f);

    if (strength <= 0.0f || !World::s_m2Scene) {
        return 0;
    }

    static CM2LightList s_lamps;
    CAaSphere sphere;
    sphere.c = s_cameraPos;
    sphere.r = FogRange();
    s_lamps.Reset(sphere);
    World::s_m2Scene->SelectPointLights(&s_lamps);

    const uint32_t maxLamps = 8;
    float constants[3 * maxLamps + 1][4] = {};
    uint32_t count = 0;
    s_lampRectEmpty = 1;

    for (uint32_t i = 0; i < s_lamps.m_count && count < maxLamps; i++) {
        CM2Light* lamp = s_lamps.m_lights[i];
        C3Vector rel(lamp->m_pos.x - s_cameraPos.x, lamp->m_pos.y - s_cameraPos.y, lamp->m_pos.z - s_cameraPos.z);
        float distance = sqrtf(rel.x * rel.x + rel.y * rel.y + rel.z * rel.z);

        if (distance > FogRange() + M2LightReach(lamp)) {
            continue;
        }

        // seen from outside, a lamp inside a building lights no fog (it would shine through the walls)
        if (!s_viewerIndoors && LampIndoor(lamp)) {
            continue;
        }

        constants[count][0] = lamp->m_dirColor.x * strength;
        constants[count][1] = lamp->m_dirColor.y * strength;
        constants[count][2] = lamp->m_dirColor.z * strength;
        constants[maxLamps + count][0] = rel.x;
        constants[maxLamps + count][1] = rel.y;
        constants[maxLamps + count][2] = rel.z;
        constants[2 * maxLamps + count][0] = lamp->m_constantAttenuation;
        constants[2 * maxLamps + count][1] = lamp->m_linearAttenuation;
        constants[2 * maxLamps + count][2] = lamp->m_quadraticAttenuation;
        constants[2 * maxLamps + count][3] = core;
        AddLampToRect(rel, 4.0f * M2LightReach(lamp) + core);   // the halo's tail falls as 1 / distance: wide margin
        count++;
    }

    constants[3 * maxLamps][0] = static_cast<float>(count);

    if (count) {
        g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 20, &constants[0][0], 3 * maxLamps + 1);
    }

    return count;
}

// The transparent water (Liquid::CRenderer pass 1, drawn by RenderLiquidPass after the fog) into its own depth
// texture, cleared to 1 every frame: the same geometry and transform the water materials use
// (IGeomFactory::GetBuffers, CMaterialWater::Render), the ShadowMap effect writing view depth / range.
// No depth test: the fog shaders take min(scene, water), so terrain in front of the water still wins.
// D3DRS_BLENDOP MIN / ADD on the D3D9 devices (Gx never touches it - ADD all along). False elsewhere.
static bool SetBlendOpMin(bool min) {
    IDirect3DDevice9* device = D3DDevice();

    if (!device) {
        return false;
    }

    device->SetRenderState(D3DRS_BLENDOP, min ? D3DBLENDOP_MIN : D3DBLENDOP_ADD);
    return true;
}

// The grass (detail doodads, drawn after the depth pass by CWorldScene::RenderDetailDoodads and not in it): their
// own depth into the water target with MIN blending - the nearest of water and every blade, the composite takes
// the nearer of that and the scene. Without it a blade in front of far land had the far land's fog.
// The instances are the ones RenderDetailDoodads draws next (it unlinks them): call before it.
static void RenderGrassDepth() {
    if (!CDetailDoodad::s_bUseShaders || !(CWorld::enables & Enable_DetailDoodads)) {
        return;
    }

    if (!s_grassVertexShader || !s_grassVertexShader->Valid() || !s_grassPixelShader || !s_grassPixelShader->Valid()) {
        return;
    }

    if (!CWorldScene::sortTable.m_detailDoodadInstList.Head() || !SetBlendOpMin(true)) {
        return;
    }

    GxRsSet(GxRs_BlendingMode, GxBlend_Add);
    CDetailDoodad::InitializeShaderConstants();   // projection, fade (RenderDetailDoodads sets them again)
    g_theGxDevicePtr->RsSet(GxRs_VertexShader, s_grassVertexShader);
    g_theGxDevicePtr->RsSet(GxRs_PixelShader, s_grassPixelShader);

    // pixel c0: x alpha test (the stock grass cuts at 128 / 255; lower, as for the leaves), w 1 / depth range
    float pixel[4] = { 0.3f, 0.0f, 0.0f, 1.0f / s_depthRange };
    g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 0, pixel, 1);

    C44Matrix view;
    CGxMatrixStack& stack = g_theGxDevicePtr->m_xforms[GxXform_View];
    view = stack.m_mtx[stack.m_level];

    // the grass near enough to matter (a far blade is seen against the ground behind it anyway): a chunk at a time
    float grassRange = CVarFloat(s_cvFogGrassRange, 60.0f, 0.0f, 1000.0f);

    for (CDetailDoodadInst* inst = CWorldScene::sortTable.m_detailDoodadInstList.Head(); inst; inst = CWorldScene::sortTable.m_detailDoodadInstList.Next(inst)) {
        const CAaSphere& sphere = inst->m_chunk->m_sphere;
        float gx = sphere.c.x - s_cameraPos.x;
        float gy = sphere.c.y - s_cameraPos.y;
        float gz = sphere.c.z - s_cameraPos.z;
        float reach = grassRange + sphere.r;

        if (gx * gx + gy * gy + gz * gz > reach * reach) {
            continue;
        }

        C44Matrix world;
        world.Translate(inst->m_chunk->m_topLeftCoords - CWorldScene::s_activeWorldView);
        CDetailDoodad::s_shaderConstants.m_worldView = world * view;
        // c0..c3 world-view, c4..c7 projection, c8 fog (unused), c9 fade
        g_theGxDevicePtr->ShaderConstantsSet(GxSh_Vertex, 0, reinterpret_cast<const float*>(&CDetailDoodad::s_shaderConstants), 10);
        inst->DrawBatches();
        s_countSum[4] += Profiling() ? 1.0 : 0.0;
    }

    SetBlendOpMin(false);
}

static CGxTex* RenderWaterDepth() {
    CGxTex* target = s_waterTexture ? TextureGetGxTex(s_waterTexture, 1, nullptr) : nullptr;

    if (!target) {
        return nullptr;
    }

    CGxTex* oldTarget = nullptr;
    g_theGxDevicePtr->DeviceGetRenderTarget(GxBuffers_Color, oldTarget);
    CGxDevice* device = g_theGxDevicePtr;
    float viewportMinX = device->m_viewport.m_x.m_l;
    float viewportMaxX = device->m_viewport.m_x.m_h;
    float viewportMinY = device->m_viewport.m_y.m_l;
    float viewportMaxY = device->m_viewport.m_y.m_h;
    float viewportMinZ = device->m_viewport.m_z.m_l;
    float viewportMaxZ = device->m_viewport.m_z.m_h;

    g_theGxDevicePtr->RsPush();
    GxXformPush(GxXform_World);
    GxRsSet(GxRs_Culling, 0);
    GxRsSet(GxRs_Fog, 0);
    GxRsSet(GxRs_BlendingMode, GxBlend_Opaque);
    GxRsSet(GxRs_DepthTest, 0);
    GxRsSet(GxRs_DepthWrite, 0);
    GxRsSet(GxRs_ScissorTest, 0);

    GxXformSetViewport(0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f);
    g_theGxDevicePtr->DeviceSetRenderTarget(GxBuffers_Color, target, 0);
    GxSceneClear(1, CImVector(0xFF, 0xFF, 0xFF, 0xFF));

    Liquid::CRenderer* renderer = CWorldScene::s_liquidRenderer;

    if (renderer && renderer->m_instances[1].Count()) {
        CShaderEffect* effect = CShaderEffectManager::GetEffect("ShadowMapRenderSL");
        effect->SetCurrent();
        float depthScale[4] = { 0.0f, 0.0f, 0.0f, 1.0f / s_depthRange };
        g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 0, depthScale, 1);
        CShaderEffect::SetAlphaRef(0.0f);
        CShaderEffect::SetDefaultShaders(0);
        GxRsSet(GxRs_Texture0, 0);
        float alphaRef[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
        g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 2, alphaRef, 1);
        CShaderEffect::UpdateProjMatrix();

        const C3Vector& camera = CWorldScene::s_activeWorldView;
        TSGrowableArray<Liquid::CInstance*>& instances = renderer->m_instances[1];

        for (uint32_t i = 0; i < instances.Count(); i++) {
            Liquid::CInstance* instance = instances.m_data[i];

            if (!instance || !instance->m_geomFactory) {
                continue;
            }

            // as Liquid::SetShaderTransforms: the factory may have the transform baked into its vertices
            C44Matrix world;

            if (!instance->m_geomFactory->SetTransform(&instance->m_transform)) {
                world = instance->m_transform;
            }

            if (Liquid::UseCameraRelativeCoords()) {
                world.d0 -= camera.x;
                world.d1 -= camera.y;
                world.d2 -= camera.z;
            }

            CGxMatrixStack& stack = g_theGxDevicePtr->m_xforms[GxXform_World];
            stack.m_dirty = 1;
            stack.m_flags[stack.m_level] &= ~CGxMatrixStack::F_Identity;
            stack.m_mtx[stack.m_level] = world;
            CShaderEffect::UpdateWorldViewMatrix();

            CGxBuf* vertexBuf = nullptr;
            CGxBuf* indexBuf = nullptr;
            CGxBatch batch;
            batch.m_primType = GxPrims_Last;
            batch.m_start = 0;
            batch.m_count = 0;
            batch.m_minIndex = 0;
            batch.m_maxIndex = 0;
            instance->m_geomFactory->GetBuffers(GxVBF_PNCT2, &vertexBuf, &indexBuf, &batch);

            if (!vertexBuf || !indexBuf || !batch.m_count) {
                continue;
            }

            GxPrimVertexPtr(vertexBuf, GxVBF_PNCT2);
            g_theGxDevicePtr->PrimIndexPtr(indexBuf);
            g_theGxDevicePtr->Draw(&batch, 1);
        }
    }

    TrovichFog::ProfileBegin(TrovichFog::PROF_GRASS);
    RenderGrassDepth();
    TrovichFog::ProfileEnd(TrovichFog::PROF_GRASS);

    GxXformPop(GxXform_World);
    g_theGxDevicePtr->RsPop();
    GxXformSetViewport(viewportMinX, viewportMaxX, viewportMinY, viewportMaxY, viewportMinZ, viewportMaxZ);
    g_theGxDevicePtr->DeviceSetRenderTarget(GxBuffers_Color, oldTarget, 0);
    CShaderEffect::UpdateProjMatrix();
    CShaderEffect::UpdateWorldViewMatrix();
    return target;
}

void TrovichFog::PrepareWater() {
    s_waterThisFrame = nullptr;
    int32_t mode = FogMode();

    if ((mode != 1 && mode != 2) || !s_haveDepth || !s_depthTexture) {
        return;
    }

    TrovichFog::ProfileBegin(TrovichFog::PROF_WATER);
    s_waterThisFrame = RenderWaterDepth();
    TrovichFog::ProfileEnd(TrovichFog::PROF_WATER);
}

void TrovichFog::Composite() {
    int32_t mode = FogMode();

    if (mode != 1 && mode != 2) {
        return;
    }

    int32_t shaders = LoadShaders();
    char text[256];
    snprintf(text, sizeof(text), "composite: mode %d, depth %s, texture %s, quad VS %s, debug PS %s, fog PS %s", mode, s_haveDepth ? "yes" : "NO", s_depthTexture ? "yes" : "NO",
        s_quadVertexShader && s_quadVertexShader->Valid() ? "ok" : "MISSING", s_debugPixelShader && s_debugPixelShader->Valid() ? "ok" : "MISSING",
        s_fogPixelShader && s_fogPixelShader->Valid() ? "ok" : "MISSING");
    LogOnChange(3, text);

    if (!s_haveDepth || !s_depthTexture || !shaders) {
        return;
    }

    CGxTex* depth = TextureGetGxTex(s_depthTexture, 1, nullptr);

    if (!depth) {
        return;
    }

    // no volumetric fog under water (the stock underwater fog takes over there)
    if (mode == 1 && World::SceneCamLiquidStatus(nullptr)) {
        return;
    }

    CGxTex* water = s_waterThisFrame;
    s_waterThisFrame = nullptr;

    if (!water) {
        return;
    }

    g_theGxDevicePtr->RsPush();
    GxRsSet(GxRs_BlendingMode, mode == 1 ? GxBlend_Alpha : GxBlend_Opaque);
    GxRsSet(GxRs_DepthTest, 0);
    GxRsSet(GxRs_DepthWrite, 0);
    GxRsSet(GxRs_Culling, 0);
    GxRsSet(GxRs_Fog, 0);
    g_theGxDevicePtr->RsSet(GxRs_VertexShader, s_quadVertexShader);
    g_theGxDevicePtr->RsSet(GxRs_PixelShader, mode == 1 ? s_fogPixelShader : s_debugPixelShader);
    g_theGxDevicePtr->RsSet(GxRs_Texture0, depth);
    g_theGxDevicePtr->RsSet(GxRs_Texture4, water);
    g_theGxDevicePtr->RsSet(GxRs_Texture5, NoiseTexture());

    if (mode == 1) {
        SetFogConstants();
    }

    TrovichFog::ProfileBegin(TrovichFog::PROF_FOG);
    DrawScreenQuad();
    TrovichFog::ProfileEnd(TrovichFog::PROF_FOG);

    // stage 4: lamp halos, added on top (the fog constants c0..c7 are still set: the halos read the density).
    // Fast: only over the screen rectangle the halos can reach (often a corner, or nothing at all).
    if (mode == 1 && s_lampsPixelShader && s_lampsPixelShader->Valid() && SetLampConstants()) {
        CRect rect = { -1.0f, -1.0f, 1.0f, 1.0f };

        if (!FastPaths() || LampScreenRect(rect)) {
            TrovichFog::ProfileBegin(TrovichFog::PROF_LAMPS);
            GxRsSet(GxRs_BlendingMode, GxBlend_Add);
            g_theGxDevicePtr->RsSet(GxRs_PixelShader, s_lampsPixelShader);
            DrawScreenQuad(&rect);
            TrovichFog::ProfileEnd(TrovichFog::PROF_LAMPS);
        }
    }
    g_theGxDevicePtr->RsPop();
}
