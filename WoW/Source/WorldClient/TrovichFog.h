#ifndef WOW_SOURCE_WORLDCLIENT_TROVICHFOG_H
#define WOW_SOURCE_WORLDCLIENT_TROVICHFOG_H

#include <cstdint>

class CMapObjDef;
struct lua_State;

// TROVICH: volumetric fog (wowdecomp client only).
// Stage 1: a camera depth pass - the visible terrain, WMO groups, models and units within the fog range drawn
// with the shadow-map effect into a screen-sized R32F texture (linear view depth / range) - and a debug view of it.
// The back buffer is multisampled (gxMultisample), so its own depth buffer cannot be read (INTZ has no MSAA).
//
// cvar volumetricFog: 0 off, 1 on (stage 1: depth pass only), 2 show the depth pass on screen
// cvar volumetricFogRange: yards the fog is computed over (default 400)
namespace TrovichFog {
    void Initialize();

    // CWorldScene::Render, right after CShadowQuery::Update (models animated, shadow buffers free)
    void RenderDepth();

    // CGWorldFrame, after the opaque models (M2PASS_0), BEFORE the detail doodads and the liquid pass: the depth of
    // the transparent water and of the grass (their instances are queued now and gone after their passes)
    void PrepareWater();

    // CGWorldFrame, right AFTER the liquid pass, before weather and the transparent models: the fog over everything
    // opaque and over the water (drawn before the fog, the water stayed clear in front of a fogged shore)
    void Composite();

    // The volumetric fog replaces the stock distance fog outdoors (volumetricFog 1): the stock fog then leaves the
    // fog range to the volumetric one and only adds a far haze beyond it (volumetricFogHaze = visibility in yards),
    // and the far clip may go up to 3200 yd so that distant mountains fade into the sky instead of ending at a wall.
    bool Active();
    float MaxFarClip();
    // DayNight::UpdateFog, outdoor branch: start / end / density (the stock exponent) of the far haze
    void OverrideOutdoorFog(float& start, float& end, float& density);

    // CWorldScene::LocateViewer3: the WMO group the camera is in (nullptr = none). Indoors by the client's own
    // rule (World::QueryOutdoors): the group's exterior flag 0x8 (or exterior-lit 0x40), overridden by its WMOAreaTable row - 0x4 outdoors
    // (Stormwind's districts are interior groups declared streets), 0x2 indoors. Indoors fades the fog out.
    void SetViewerGroup(CMapObjDef* def, uint32_t groupNum);

    // CGGameUI SMSG_WEATHER: the server's weather (Weather.dbc id = WeatherState: 1 fog, 3-5 rain, 6-8 snow,
    // 22 sand storm, 86 thunderstorm ...) and its intensity, published as the cvar volumetricFogWeather
    // ("<id> <intensity>") for the TrovichUIFixes fog presets
    void SetWeather(uint32_t weatherId, float intensity);

    // Lua (registered with the system functions) for the addon's fog banks and presets:
    //   TrovichFogPosition() -> x, y, z, mapId      the player's world position (3.3.5 has no UnitPosition)
    //   TrovichFogLight()    -> sunHeight, sunLight, ambient, indoors
    //       sunHeight: sine of the drawn sun's altitude (-1..1, below 0 = night), sunLight / ambient: the
    //       world light's brightness 0..1 here and now (DayNight: zone, time, weather), indoors 1 / 0
    int32_t ScriptPosition(lua_State* L);
    int32_t ScriptLight(lua_State* L);
}

#endif
