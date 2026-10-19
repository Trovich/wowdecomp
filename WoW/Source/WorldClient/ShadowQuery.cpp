#include "ShadowQuery.h"
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/Types.h"
#include "Engine/Source/Storm/H/storm.h"
#include <cmath>
#include "Engine/Source/Model2/M2Light.h"
#include "Engine/Source/ShaderEffect/ShadowCache.h"
#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"
#include "WoW/Source/Object/ObjectClient/Object_C.h"
#include "WoW/Source/UIUtil/Camera.h"
#include "WoW/Source/Ui/WorldFrame.h"
#include "WoW/Source/WorldClient/CPortalView.h"
#include "WoW/Source/WorldClient/DayNight__DNInfo.h"
#include "WoW/Source/WorldClient/Map.h"
#include "WoW/Source/WorldClient/MapLight.h"
#include "WoW/Source/WorldClient/World.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gxu/Gxu.h"
#include "Engine/Source/ShaderEffect/ShaderEffect.h"
#include "Engine/Source/Tempest/CAaBox.h"
#include "WoW/Source/WorldClient/CWorldOcclusion.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/ShaderEffect/ShaderEffectManager.h"
#include "Engine/Source/Tempest/CRect.h"
#include "WoW/Source/WorldClient/MapObj.h"
#include "Engine/Source/Model2/M2Shared.h"
#include "Engine/Source/Tempest/CFrustum.h"
#include "WoW/Source/WorldClient/MapEntity.h"
#include "Engine/Source/Tempest/CiRect.h"
#include "WoW/Source/WorldClient/MapArea.h"
#include "WoW/Source/WorldClient/MapChunk.h"
#include "WoW/Source/WorldClient/MapDoodadDef.h"
#include "WoW/Source/WorldClient/MapObjDef.h"
#include "WoW/Source/WorldClient/MapObjGroup.h"
#include "WoW/Source/WorldClient/CMapBaseObjLink.h"
#include "Engine/Source/Gx/CGxVertexPT.h"
#include "WoW/Source/WorldClient/TrovichFog.h"

// STATIC: 0x00D25300
uint8_t CShadowQuery::s_stamp;

// STATIC: 0x00D2538C
static uint32_t s_maxTexturedBatches;

// STATIC: 0x00D25390
static uint32_t s_maxUntexturedBatches;

// STATIC: 0x00D25304
float CShadowQuery::s_cutoffHeight;

// STATIC: 0x00D25320
ShadowQueryBuffers CShadowQuery::s_bandBuffers[3];

// TROVICH: the terrain chunks each band casts from (QueryTerrainCasters -> RenderTerrainCasters)
static TSGrowableArray<CMapChunk*> s_terrainCasters[3];
// TROVICH: QueryMapObjDefsBand for the fog's camera depth pass: interior groups too (the shadow bands skip them -
// they cast no sun shadow - but the camera sees into a temple through its door), and groups without their doodads
static int32_t s_viewAllGroups;
static int32_t s_viewGroupsOnly;
// TROVICH: QueryTerrainBand for the camera pass's far band: only doodads of at least this radius, and none the near
// band (within s_viewNearRange of s_viewCamera) already took
static float s_viewMinRadius;
static float s_viewNearRange;
static C3Vector s_viewCamera;

// VERIFY: a0192ad6 auto
// OFFSET: 0x007BD3A0
void CShadowQuery::Initialize() {
    CShadowCache::SetMatrixCallback(CShadowQuery::MatrixCallback);
    CShadowCache::SetFrustumCallback(CShadowQuery::FrustumCallback);
    CShadowCache::SetQueryCallback(CShadowQuery::QueryCallback);
    CShadowCache::SetRenderCallback(CShadowQuery::RenderCallback);

    g_shadowCull = CVar::Register("shadowCull", "enable shadow frustum culling", 0, "1", nullptr, CATEGORY_DEFAULT, false, nullptr, false);
    g_shadowScissor = CVar::Register("shadowScissor", "enable scissoring when rendering shadowmaps", 0, "1", nullptr, CATEGORY_DEFAULT, false, nullptr, false);
    g_shadowInstancing = CVar::Register("shadowInstancing", "enable instancing when rendering shadowmaps", 0, "1", nullptr, CATEGORY_DEFAULT, false, nullptr, false);
    g_shadowTerrain = CVar::Register("shadowTerrain", "terrain casts dynamic shadows (Trovich)", 0, "1", nullptr, CATEGORY_DEFAULT, false, nullptr, false);  // TROVICH

    CShadowCache::Initialize();
    TrovichFog::Initialize();  // TROVICH
}

// OFFSET: 0x007BA7D0
void CShadowQuery::FreeBuffers() {
    for (ShadowQueryBuffers* buffers = CShadowQuery::s_bandBuffers; buffers < CShadowQuery::s_bandBuffers + sizeof(CShadowQuery::s_bandBuffers) / sizeof(CShadowQuery::s_bandBuffers[0]); buffers++) {
        if (buffers->m_groups) {
            SMemFree(buffers->m_groups, nullptr, 0, 0);
        }

        if (buffers->m_groupData) {
            SMemFree(buffers->m_groupData, nullptr, 0, 0);
        }

        delete[] buffers->m_untextured.m_batches;
        delete[] buffers->m_textured.m_batches;
    }
}

// VERIFY: 11a5d7a4 manual
// OFFSET: 0x007BB3E0
static void GetViewerPosition(C3Vector* position) {
    *position = DayNight::GetInfo()->m_cameraPos;
    CGCamera* camera = CGWorldFrame::GetActiveCamera();

    if (!camera) {
        return;
    }

    CGObject_C* object = ClntObjMgrObjectPtr(camera->m_targetGUID, TYPEMASK_OBJECT, __FILE__, __LINE__);

    if (object) {
        *position = object->GetPosition();
    }
}

// VERIFY: aa0465cc auto
// OFFSET: 0x007BB570
void CShadowQuery::Update() {
    C3Vector direction = CMap::s_mapLight->m_light.m_dir;
    int32_t useCullFrustum = CWorldScene::frustumPortalView.m_maxViewDepth >= 0.0f || CWorldScene::s_hasPendingExteriorGroups;
    float z = direction.z * 5.0f;

    if (z < -1.2f) {
        z = -1.2f;
    }

    // TROVICH: with sunPath the direction is already the real (held >= 15 degrees) one - no stock steepening
    static CVar* s_sunPath = nullptr;

    if (!s_sunPath) {
        s_sunPath = CVar::Lookup("sunPath");
    }

    if (s_sunPath && s_sunPath->m_intValue) {
        z = direction.z;
    }

    float inv = 1.0f / std::sqrt(direction.y * direction.y + direction.x * direction.x + z * z);
    direction.x = direction.x * inv;
    direction.y = direction.y * inv;
    direction.z = z * inv;
    CShadowCache::PreUpdate(direction);
    C3Vector viewer(0.0f, 0.0f, 0.0f);
    GetViewerPosition(&viewer);
    CShadowCache::SetDebugParams(1.0f, 0);
    CShadowCache::Update(viewer, useCullFrustum);
    CShadowCache::PostUpdate();
}

// VERIFY: 7421bb67 manual
// OFFSET: 0x007BAC10
void CShadowQuery::MatrixCallback(C3Vector& center, float radius, C44Matrix& matrix, C3Vector& direction, int32_t band) {
    if (radius <= 0.0f) {
        return;
    }

    C44Matrix ortho;
    C44Matrix view;
    C44Matrix inverseView;
    C44Matrix lookAt;
    C3Vector eye(0.0f, 0.0f, 0.0f);
    C3Vector target(0.0f, 0.0f, 0.0f);
    C3Vector camera(0.0f, 0.0f, 0.0f);
    CGWorldFrame::GetCameraPosition(&camera);

    eye.x = center.x - CShadowCache::s_sunDir.x * 2000.0f - camera.x;
    eye.y = center.y - CShadowCache::s_sunDir.y * 2000.0f - camera.y;
    eye.z = center.z - 2000.0f * CShadowCache::s_sunDir.z - camera.z;
    target.x = center.x - camera.x;
    target.y = center.y - camera.y;
    target.z = center.z - camera.z;

    CGxMatrixStack& stack = g_theGxDevicePtr->m_xforms[GxXform_View];
    view = stack.m_mtx[stack.m_level];
    C44Matrix copy = view;
    float determinant = copy.Determinant();
    view = copy.Inverse(determinant);
    inverseView = view;

    GxuXformCreateOrtho(-radius, radius, -radius, radius, 1.0f, 4000.0f, ortho);
    GxuXformCreateLookAtXXX(eye, target, direction, lookAt);
    matrix = inverseView * lookAt * ortho;

    C44Matrix depth = inverseView * lookAt;
    matrix.a2 = depth.a2;
    matrix.b2 = depth.b2;
    matrix.c2 = depth.c2;
    matrix.d2 = depth.d2;

    float bias = CShaderEffect::s_usePcfFiltering ? 0.5f : 0.0f;
    float scale;

    if (band == 0) {
        scale = 2.0f;
    } else if (band == 1) {
        matrix.d2 = depth.d2 - (bias + 4.0f * 0.2f);
        return;
    } else if (band == 2) {
        matrix.d2 = depth.d2 - (bias + 8.0f * 0.2f);
        return;
    } else {
        scale = 0.5f;
    }

    matrix.d2 = depth.d2 - (bias + scale * 0.2f);
}

// VERIFY: de87a831 manual
// OFFSET: 0x007BAFD0
void CShadowQuery::FrustumCallback(CFrustum& frustum, C44Matrix& matrix, const C3Vector& direction, CShadowCacheQueryData& data, int32_t band) {
    C3Vector& pos = data.m_pos[band];
    float* bounds = data.m_bounds[band];
    C3Vector eye;
    eye.x = pos.x - CShadowCache::s_sunDir.x * 2000.0f;
    eye.y = pos.y - CShadowCache::s_sunDir.y * 2000.0f;
    eye.z = pos.z - 2000.0f * CShadowCache::s_sunDir.z;

    GxuXformCreateOrtho(bounds[0], bounds[1], bounds[2], bounds[3], 1.0f, 4000.0f, matrix);

    C44Matrix view;
    C3Vector corners[8];

    for (uint32_t i = 0; i < 8; i++) {
        corners[i] = C3Vector(0.0f, 0.0f, 0.0f);
    }

    GxuXformCreateLookAtXXX(eye, pos, direction, view);
    GxuXformCalcFrustumCorners(view, matrix, corners);
    frustum.CalcPlanesFromCorners(corners);
    frustum.InvertPlanes();
    frustum.SwapCorners();

    float* rect = data.m_cullRects[band];
    rect[0] = -1.0f;
    rect[1] = -1.0f;
    rect[2] = 1.0f;
    rect[3] = 1.0f;

    CFrustum& cull = data.m_bandFrustums[band];
    cull = frustum;

    if (data.m_queryFlags[band] == 8 || !g_shadowCull->m_intValue) {
        return;
    }

    CFrustum clip = CWorldScene::s_clipFrustum;
    clip.Transform(view * matrix);
    CAaBox box = CAaBox::Bounding(clip.corners, 8);
    box.b = C3Vector::Max(box.b, C3Vector(-1.0f, -1.0f, -1.0f));
    box.b = C3Vector::Min(box.b, C3Vector(1.0f, 1.0f, 1.0f));
    box.t = C3Vector::Max(box.t, C3Vector(-1.0f, -1.0f, -1.0f));
    box.t = C3Vector::Min(box.t, C3Vector(1.0f, 1.0f, 1.0f));

    if (box.t.x > box.b.x && box.t.y > box.b.y && box.t.z > box.b.z) {
        C44Matrix ortho;
        GxuXformCreateOrtho(-(bounds[0] * box.b.x), bounds[1] * box.t.x, -(bounds[2] * box.b.y), box.t.y * bounds[3], 1.0f, 4000.0f, ortho);
        GxuXformCalcFrustumCorners(view, ortho, corners);
        cull.CalcPlanesFromCorners(corners);
        cull.InvertPlanes();
        cull.SwapCorners();
        rect[1] = box.b.x;
        rect[3] = box.t.x;
        rect[0] = -box.t.y;
        rect[2] = -box.b.y;
    }
}

// VERIFY: dc5e2519 manual
// OFFSET: 0x007BA840
void CShadowQuery::AllocBuffers() {
    for (uint32_t i = 0; i < 3; i++) {
        ShadowQueryBuffers& buffers = CShadowQuery::s_bandBuffers[i];
        buffers.m_groupCount = 0;
        buffers.m_untextured.m_count = 0;
        buffers.m_untextured.m_maxCount = 0x2000;
        buffers.m_textured.m_count = 0;
        buffers.m_textured.m_maxCount = 0x2000;

        if (!buffers.m_groups) {
            buffers.m_groups = SMemAlloc(0x800 * sizeof(CMapObjGroup*), __FILE__, __LINE__, 0);
        }

        if (!buffers.m_groupData) {
            buffers.m_groupData = SMemAlloc(0x800 * sizeof(C44Matrix*), __FILE__, __LINE__, 0);
        }

        if (!buffers.m_untextured.m_batches) {
            buffers.m_untextured.m_batches = static_cast<CModelBatch*>(SMemAlloc(sizeof(CModelBatch) * 0x2000, __FILE__, __LINE__, 0));
        }

        if (!buffers.m_textured.m_batches) {
            buffers.m_textured.m_batches = static_cast<CModelBatch*>(SMemAlloc(sizeof(CModelBatch) * 0x2000, __FILE__, __LINE__, 0));
        }
    }
}

// VERIFY: 3e3dfd04 manual
// OFFSET: 0x007BD200
int32_t CShadowQuery::QueryCallback(CShadowCacheQueryData& data, uint32_t frame) {
    CShadowQuery::AllocBuffers();

    if (!++CShadowQuery::s_stamp) {
        CShadowQuery::s_stamp = 1;
    }

    int32_t flags = 0;

    for (int32_t i = 0; i <= data.m_lastBand; i++) {
        flags |= data.m_queryFlags[i];
    }

    CShadowQuery::GatherShadowCasters(data, 0, data.m_lastBand);
    CShadowQuery::QueryTerrainCasters(data, 0, data.m_lastBand);  // TROVICH

    if (data.m_useFrustum) {
        if (flags & 0xC) {
            CFrustum& frustum = data.m_frustums[data.m_lastBand];
            C3Vector center(0.0f, 0.0f, 0.0f);
            center.x = frustum.corners[0].x;
            center.y = frustum.corners[0].y;
            center.z = frustum.corners[0].z;
            center.x = center.x + frustum.corners[3].x;
            center.y = center.y + frustum.corners[3].y;
            center.z = center.z + frustum.corners[3].z;
            center.x = center.x + frustum.corners[1].x;
            center.y = center.y + frustum.corners[1].y;
            center.z = center.z + frustum.corners[1].z;
            center.x = center.x + frustum.corners[2].x;
            center.y = center.y + frustum.corners[2].y;
            center.z = center.z + frustum.corners[2].z;
            center.x = center.x * 0.25f;
            center.y = center.y * 0.25f;
            center.z = center.z * 0.25f;
            CWorldScene::CreateOcclusionVolumes(&center, frustum.corners, 1);
            CShadowQuery::QueryMapObjDefs(data, 0, data.m_lastBand);
            CShadowQuery::QueryTerrain(data, 0, data.m_lastBand);
        }
    } else {
        for (int32_t band = 0; band <= data.m_lastBand; band++) {
            if (flags & 0xC) {
                CWorldOcclusion::ClearVolumes();
                CShadowQuery::QueryMapObjDefsBand(data, band);
                CShadowQuery::QueryTerrainBand(data, band);
            }
        }
    }

    return 1;
}

// VERIFY: 2abab737 manual
// OFFSET: 0x007BBC50
int32_t CShadowQuery::RenderCallback(CShadowCacheQueryData& data, int32_t band, CGxTex* target, CGxTex* colorTarget, C3Vector& direction) {
    ShadowQueryBuffers& buffers = CShadowQuery::s_bandBuffers[band];

    if (!buffers.m_groupCount && !buffers.m_untextured.m_count && !buffers.m_textured.m_count && !CShadowQuery::TerrainCasterCount(band)) {  // TROVICH: + terrain
        CShadowQuery::Clear(target, colorTarget, data.m_texBounds[band]);
        return 1;
    }

    if (colorTarget) {
        GxRenderTargetSet(GxBuffers_Depth, colorTarget, 0);
    }

    g_theGxDevicePtr->RsPush();
    GxRsSet(GxRs_Culling, 0);
    CShaderEffect::SetTexMtx_Identity(0);

    C44Matrix projection;
    C3Vector eye(0.0f, 0.0f, 0.0f);
    C3Vector center(0.0f, 0.0f, 0.0f);
    C44Matrix lookAt;
    C44Matrix view;
    C44Matrix inverseView;
    C3Vector camera(0.0f, 0.0f, 0.0f);
    CGxTex* oldTarget;
    g_theGxDevicePtr->DeviceGetRenderTarget(GxBuffers_Color, oldTarget);
    GxRsSet(GxRs_Fog, 0);

    CShaderEffect* effect = CShaderEffectManager::GetEffect("ShadowMapRenderSL");
    CGWorldFrame::GetCameraPosition(&camera);
    C3Vector& pos = data.m_pos[band];
    eye.x = pos.x - CShadowCache::s_sunDir.x * 2000.0f - camera.x;
    eye.y = pos.y - CShadowCache::s_sunDir.y * 2000.0f - camera.y;
    eye.z = pos.z - 2000.0f * CShadowCache::s_sunDir.z - camera.z;
    center.x = pos.x - camera.x;
    center.y = pos.y - camera.y;
    center.z = pos.z - camera.z;

    C44Matrix translation;
    translation.d0 = -camera.x;
    translation.d1 = -camera.y;
    translation.d2 = -camera.z;

    CGxDevice* device = g_theGxDevicePtr;
    projection = device->m_projection;
    float viewportMinX = device->m_viewport.m_x.m_l;
    float viewportMaxX = device->m_viewport.m_x.m_h;
    float viewportMinY = device->m_viewport.m_y.m_l;
    float viewportMaxY = device->m_viewport.m_y.m_h;
    float viewportMinZ = device->m_viewport.m_z.m_l;
    float viewportMaxZ = device->m_viewport.m_z.m_h;
    GxXformSetViewport(data.m_texBounds[band][0], data.m_texBounds[band][1], data.m_texBounds[band][2], data.m_texBounds[band][3], 0.0f, 1.0f);

    CGxMatrixStack& stack = g_theGxDevicePtr->m_xforms[GxXform_View];
    view = stack.m_mtx[stack.m_level];
    C44Matrix copy = view;
    float determinant = copy.Determinant();
    inverseView = copy.Inverse(determinant);

    effect->SetCurrent();
    float constants[4] = { 0.0f, 0.0f, 0.0f, 0.00025000001f };
    CShadowCache::SetShadowScale(0.00025000001f);
    g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 0, constants, 1);
    CShaderEffect::s_shadowValue = CShadowCache::GetShadowValue();
    g_theGxDevicePtr->XformSetProjection(data.m_matrices[band]);
    CShaderEffect::UpdateProjMatrix();
    GxRsSet(GxRs_ScissorTest, 0);
    g_theGxDevicePtr->DeviceSetRenderTarget(GxBuffers_Color, target, 0);
    GxSceneClear(3, CImVector(0xFF, 0xFF, 0xFF, 0xFF));

    if (g_shadowScissor->m_intValue) {
        GxRsSet(GxRs_ScissorTest, 1);
        float* rect = data.m_cullRects[band];
        CRect scissor;
        scissor.minY = (rect[0] + 1.0f) * 0.5f;
        scissor.minX = (rect[1] + 1.0f) * 0.5f;
        scissor.maxY = (rect[2] + 1.0f) * 0.5f;
        scissor.maxX = 0.5f * (rect[3] + 1.0f);
        g_theGxDevicePtr->SetScissor(scissor);
    }

    GxuXformCreateLookAtXXX(eye, center, direction, lookAt);
    g_theGxDevicePtr->XformSetView(lookAt);
    C44Matrix texMatrix = (inverseView * lookAt).Transpose();
    g_theGxDevicePtr->ShaderConstantsSet(GxSh_Vertex, 14, &texMatrix.a0, 3);

    if (buffers.m_groupCount) {
        CMapObj::RenderMapObjGroupsShadowMap(buffers.m_groups, buffers.m_groupCount, buffers.m_groupData, translation, data.m_frustums[band]);
    }

    CShadowQuery::RenderTerrainCasters(band, translation);  // TROVICH

    if (buffers.m_untextured.m_count || buffers.m_textured.m_count) {
        if (g_shadowInstancing->m_intValue) {
            buffers.m_untextured.Optimize();
            buffers.m_textured.Optimize();
        }

        if (buffers.m_untextured.m_count > s_maxUntexturedBatches) {
            s_maxUntexturedBatches = buffers.m_untextured.m_count;
        }

        if (buffers.m_textured.m_count > s_maxTexturedBatches) {
            s_maxTexturedBatches = buffers.m_textured.m_count;
        }

        CM2Model::RenderModelBatchesShadowMap(buffers.m_untextured, buffers.m_textured);
    }

    g_theGxDevicePtr->RsPop();
    GxXformSetViewport(viewportMinX, viewportMaxX, viewportMinY, viewportMaxY, viewportMinZ, viewportMaxZ);
    g_theGxDevicePtr->XformSetView(view);
    GxRsSet(GxRs_ScissorTest, 0);
    g_theGxDevicePtr->DeviceSetRenderTarget(GxBuffers_Color, oldTarget, 0);
    g_theGxDevicePtr->XformSetProjection(projection);
    CShaderEffect::UpdateProjMatrix();
    return 1;
}

// VERIFY: f464b8c3 manual
// OFFSET: 0x007BB9D0
void CShadowQuery::GatherShadowCasters(CShadowCacheQueryData& data, int32_t firstBand, int32_t lastBand) {
    float maxRadius[3];
    float minRadius[3];
    maxRadius[0] = 10000.0f;

    if ((data.m_queryFlags[0] & 0xC) == 4) {
        maxRadius[0] = 25.0f;
    }

    minRadius[0] = 0.25f;
    maxRadius[1] = 10000.0f;
    minRadius[1] = 2.0f;
    maxRadius[2] = 10000.0f;
    minRadius[2] = 10.0f;

    for (CMapEntity* entity = CMap::entityList.Head(); entity; entity = CMap::entityList.Next(entity)) {
        if (entity->m_staticFlags & 0x4) {
            continue;
        }

        CM2Model* model = entity->m_model;

        if (!model) {
            continue;
        }

        int32_t isUnit = 1;

        if (!(entity->m_type & 0x20) || (entity->m_staticFlags & 0x2000)) {
            isUnit = 0;
        }

        uint32_t doodad = (model->m_fFlags & 0x1) ? model->m_shared->m_billboardBoneCount : 0;

        for (int32_t band = firstBand; band <= lastBand; band++) {
            int32_t queryFlags = data.m_queryFlags[band];
            uint32_t flags;

            if (queryFlags & 0x2) {
                if (!isUnit) {
                    continue;
                }

                flags = entity->m_flags;

                if (flags & 0x4) {
                    continue;
                }

                if (CShadowQuery::s_cutoffHeight < entity->m_position.z) {
                    continue;
                }
            } else {
                flags = entity->m_flags;

                if (flags & 0x2) {
                    if (!(entity->m_staticFlags & 0x1000)) {
                        continue;
                    }

                    if (CShadowQuery::s_cutoffHeight < entity->m_position.z) {
                        continue;
                    }
                }
            }

            int32_t wanted;

            if (isUnit) {
                wanted = queryFlags & 0x1;
            } else if (doodad) {
                wanted = queryFlags & 0x4;
            } else {
                wanted = queryFlags & 0x8;
            }

            if (!wanted) {
                continue;
            }

            CAaBox& extents = data.m_extents[band];
            CAaBox& bbox = entity->m_bbox;

            if (extents.t.x >= bbox.b.x && extents.t.y >= bbox.b.y && extents.t.z >= bbox.b.z && extents.b.x <= bbox.t.x && extents.b.y <= bbox.t.y && extents.b.z <= bbox.t.z && minRadius[band] <= entity->m_sphere.r && ((flags & 0x20000) || maxRadius[band] >= entity->m_sphere.r)) {
                if (data.m_bandFrustums[band].Cull(bbox) && (!data.m_useCullFrustum[band] || data.m_cullFrustums[band].CullInv(bbox) != 3)) {
                    model->GatherModelBatchesShadowMap(CShadowQuery::s_bandBuffers[band].m_untextured, CShadowQuery::s_bandBuffers[band].m_textured);
                }
            }
        }
    }
}

// VERIFY: 7278bb56 manual
// OFFSET: 0x007BB830
void CShadowQuery::Clear(CGxTex* target, CGxTex* depthTarget, float* rect) {
    CGxTex* oldTarget = nullptr;
    g_theGxDevicePtr->DeviceGetRenderTarget(GxBuffers_Color, oldTarget);
    CGxDevice* device = g_theGxDevicePtr;
    float viewportMinX = device->m_viewport.m_x.m_l;
    float viewportMaxX = device->m_viewport.m_x.m_h;
    float viewportMinY = device->m_viewport.m_y.m_l;
    float viewportMaxY = device->m_viewport.m_y.m_h;
    float viewportMinZ = device->m_viewport.m_z.m_l;
    float viewportMaxZ = device->m_viewport.m_z.m_h;
    GxRenderTargetSet(GxBuffers_Depth, depthTarget, 0);
    GxRenderTargetSet(GxBuffers_Color, target, 0);
    GxXformSetViewport(rect[0], rect[1], rect[2], rect[3], 0.0f, 1.0f);
    GxSceneClear(3, CImVector(0xFF, 0xFF, 0xFF, 0xFF));
    GxXformSetViewport(viewportMinX, viewportMaxX, viewportMinY, viewportMaxY, viewportMinZ, viewportMaxZ);
    GxRenderTargetSet(GxBuffers_Color, oldTarget, 0);
}

// VERIFY: 89a7afcf auto
// OFFSET: 0x007BA8F0
void CShadowQuery::GetSize(CShadowCacheQueryData& data, int32_t band, float* minSize, float* maxSize) {
    *maxSize = 10000.0f;

    if (band == 0) {
        if ((data.m_queryFlags[0] & 0xC) == 4) {
            *maxSize = 25.0f;
            *minSize = 0.25f;
            return;
        }

        *minSize = 0.25f;
        return;
    }

    if (band < 1) {
        *minSize = 0.25f;
        return;
    }

    *minSize = band >= 2 ? 10.0f : 2.0f;
}

// VERIFY: 69b3a5e2 auto
// OFFSET: 0x007BABC0
bool CWorldScene::IsBeyondFadeDistance(const C3Vector& position, uint32_t fadeLevel) {
    float dz = position.z - CWorldScene::s_activeWorldView.z;
    float dy = position.y - CWorldScene::s_activeWorldView.y;
    float dx = position.x - CWorldScene::s_activeWorldView.x;
    return dx * dx + (dy * dy + dz * dz) > CWorldScene::s_fadeDistMinSqr[fadeLevel];
}

// VERIFY: 18313b4e auto
// OFFSET: 0x007BB460
static void GetChunkRect(const CAaBox& box, CiRect& rect) {
    float minY = -(box.t.x - 17066.666f);
    float minX = -(box.t.y - 17066.666f);
    float maxY = -(box.b.x - 17066.666f);
    float maxX = -(box.b.y - 17066.666f);

    if (minX < 0.0f) {
        minX = 0.0f;
    }

    if (minY < 0.0f) {
        minY = 0.0f;
    }

    if (maxX >= 34133.332f) {
        maxX = 34132.332f;
    }

    if (maxY >= 34133.332f) {
        maxY = 34132.332f;
    }

    int32_t cellMaxX = CMath::fint_(maxX * 0.23999999f - 0.5f);
    int32_t cellMaxY = CMath::fint_(maxY * 0.23999999f - 0.5f);
    int32_t cellMinX = CMath::fint_(minX * 0.23999999f - 0.5f);
    int32_t cellMinY = CMath::fint_(minY * 0.23999999f - 0.5f);
    rect.minY = cellMinY >> 3;
    rect.minX = cellMinX >> 3;
    rect.maxY = cellMaxY >> 3;
    rect.maxX = cellMaxX >> 3;
}

// VERIFY: 9b4413e5 manual
// OFFSET: 0x007BC890
void CShadowQuery::QueryMapObjDefsBand(CShadowCacheQueryData& data, int32_t band) {
    float minSize;
    float maxSize;
    CShadowQuery::GetSize(data, band, &minSize, &maxSize);

    for (CMapObjDef* def = CMap::mapObjDefHash.m_fulllist.Head(); def; def = CMap::mapObjDefHash.Next(def)) {
        uint32_t flags = def->m_flags;

        if (flags & 0x20) {
            continue;
        }

        float maxDoodadSize = maxSize;
        int32_t interior = 0;
        int32_t castDoodads = 0;
        int32_t castOther = 0;

        if (flags & 0x400) {
            if (!(data.m_queryFlags[band] & 0x4)) {
                continue;
            }

            maxDoodadSize = 100000.0f;
            castDoodads = 1;
            castOther = 1;
            interior = 1;
        } else {
            int32_t queryFlags = data.m_queryFlags[band];

            if (queryFlags & 0x8) {
                castOther = 1;
            }

            if (queryFlags & 0x4) {
                castDoodads = 1;
            }
        }

        CAaBox& extents = data.m_extents[band];

        if (!extents.Intersects(def->m_bbox)) {
            continue;
        }

        if (!data.m_bandFrustums[band].Cull(def->m_bbox) || (data.m_useCullFrustum[band] && data.m_cullFrustums[band].CullInv(def->m_bbox) == 3)) {
            continue;
        }

        uint32_t groupCount = def->m_defGroups.m_count == 0xFFFFFFFF ? def->m_defGroups.m_dynamic.m_count : def->m_defGroups.m_count;

        for (uint32_t i = 0; i < groupCount; i++) {
            CMapObjDefGroup* defGroup = def->m_defGroups[i];
            CMapObjGroup* group = def->m_owner->GetGroup(i, 0);

            // TROVICH: the camera pass (s_viewAllGroups) takes the groups the main view draws this frame - the cull
            // (CullSortTable / CullMapObjDefGroup, through the portals) ran before it and left them linked in the
            // sort table: interiors seen through a door, but not Orgrimmar's inner gate towers seen from outside
            if (!group || (s_viewAllGroups ? !defGroup->m_sortTableLink.m_next : !(group->m_flags & 0x48))) {
                continue;
            }

            if (!extents.Intersects(defGroup->m_bbox) || !data.m_bandFrustums[band].Cull(defGroup->m_bbox)) {
                continue;
            }

            if (data.m_useCullFrustum[band] && data.m_cullFrustums[band].CullInv(defGroup->m_bbox) == 3) {
                continue;
            }

            if (interior ? (data.m_queryFlags[band] & 0x4) : (data.m_queryFlags[band] & 0x8)) {
                ShadowQueryBuffers& buffers = CShadowQuery::s_bandBuffers[band];

                if (buffers.m_groupCount >= 0x800) {
                    break;
                }

                static_cast<C44Matrix**>(buffers.m_groupData)[buffers.m_groupCount] = &def->m_mat;
                static_cast<CMapObjGroup**>(buffers.m_groups)[buffers.m_groupCount++] = group;
            }

            for (CMapBaseObjLink* link = s_viewGroupsOnly ? nullptr : defGroup->m_doodadDefLinkList.Head(); link; link = defGroup->m_doodadDefLinkList.Next(link)) {  // TROVICH: s_viewGroupsOnly
                CMapDoodadDef* doodad = static_cast<CMapDoodadDef*>(link->m_owner);

                if (doodad->m_shadowStamps[band] == CShadowQuery::s_stamp) {
                    continue;
                }

                doodad->m_shadowStamps[band] = CShadowQuery::s_stamp;
                CM2Model* model = doodad->m_model;

                if (!model || !(doodad->m_flags & 0x80) || CWorldScene::IsBeyondFadeDistance(doodad->m_sphere.c, doodad->m_fadeLevel) || maxDoodadSize < doodad->m_sphere.r) {
                    continue;
                }

                int32_t wanted = model->GetBillboardBoneCount() ? castDoodads : castOther;

                if (wanted && extents.Intersects(doodad->m_bbox) && data.m_bandFrustums[band].Cull(doodad->m_bbox) && (!data.m_useCullFrustum[band] || data.m_cullFrustums[band].CullInv(doodad->m_bbox) != 3)) {
                    model->GatherModelBatchesShadowMap(CShadowQuery::s_bandBuffers[band].m_untextured, CShadowQuery::s_bandBuffers[band].m_textured);
                }
            }
        }
    }
}

// VERIFY: 34693e83 manual
// OFFSET: 0x007BC490
void CShadowQuery::QueryMapObjDefs(CShadowCacheQueryData& data, int32_t firstBand, int32_t lastBand) {
    for (CMapObjDef* def = CMap::mapObjDefHash.m_fulllist.Head(); def; def = CMap::mapObjDefHash.Next(def)) {
        if (def->m_flags & 0x20) {
            continue;
        }

        CAaBox& lastExtents = data.m_extents[lastBand];

        if (!(lastExtents.t.x >= def->m_bbox.b.x && lastExtents.t.y >= def->m_bbox.b.y && lastExtents.t.z >= def->m_bbox.b.z && lastExtents.b.x <= def->m_bbox.t.x && lastExtents.b.y <= def->m_bbox.t.y && lastExtents.b.z <= def->m_bbox.t.z)) {
            continue;
        }

        CFrustum& lastFrustum = data.m_bandFrustums[lastBand];

        if (!lastFrustum.Cull(def->m_bbox)) {
            continue;
        }

        uint32_t groupCount = def->m_defGroups.m_count == 0xFFFFFFFF ? def->m_defGroups.m_dynamic.m_count : def->m_defGroups.m_count;

        for (uint32_t i = 0; i < groupCount; i++) {
            CMapObjDefGroup* defGroup = def->m_defGroups[i];
            CMapObjGroup* group = def->m_owner->GetGroup(i, 0);

            if (!group || !(group->m_flags & 0x48)) {
                continue;
            }

            if (!lastExtents.Intersects(defGroup->m_bbox) || !lastFrustum.Cull(defGroup->m_bbox)) {
                continue;
            }

            for (int32_t band = firstBand; band <= lastBand; band++) {
                if (data.m_extents[band].Intersects(defGroup->m_bbox) && data.m_bandFrustums[band].Cull(defGroup->m_bbox) && (!data.m_useCullFrustum[band] || data.m_cullFrustums[band].CullInv(defGroup->m_bbox) != 3)) {
                    ShadowQueryBuffers& buffers = CShadowQuery::s_bandBuffers[band];

                    if (buffers.m_groupCount >= 0x800) {
                        break;
                    }

                    static_cast<C44Matrix**>(buffers.m_groupData)[buffers.m_groupCount] = &def->m_mat;
                    static_cast<CMapObjGroup**>(buffers.m_groups)[buffers.m_groupCount++] = group;
                }
            }

            for (CMapBaseObjLink* link = defGroup->m_doodadDefLinkList.Head(); link; link = defGroup->m_doodadDefLinkList.Next(link)) {
                CMapDoodadDef* doodad = static_cast<CMapDoodadDef*>(link->m_owner);

                if (!doodad->m_model || !(doodad->m_flags & 0x80)) {
                    continue;
                }

                for (int32_t band = firstBand; band <= lastBand; band++) {
                    if (doodad->m_shadowStamps[band] == CShadowQuery::s_stamp) {
                        continue;
                    }

                    doodad->m_shadowStamps[band] = CShadowQuery::s_stamp;

                    if (!CWorldScene::IsBeyondFadeDistance(doodad->m_sphere.c, doodad->m_fadeLevel) && data.m_extents[band].Intersects(doodad->m_bbox) && data.m_bandFrustums[band].Cull(doodad->m_bbox) && (!data.m_useCullFrustum[band] || data.m_cullFrustums[band].CullInv(doodad->m_bbox) != 3)) {
                        doodad->m_model->GatherModelBatchesShadowMap(CShadowQuery::s_bandBuffers[band].m_untextured, CShadowQuery::s_bandBuffers[band].m_textured);
                    }
                }
            }
        }
    }
}

// VERIFY: 896bfb08 manual
// OFFSET: 0x007BCF20
void CShadowQuery::QueryTerrainBand(CShadowCacheQueryData& data, int32_t band) {
    float minSize;
    float maxSize;
    CShadowQuery::GetSize(data, band, &minSize, &maxSize);
    CAaBox& extents = data.m_extents[band];
    CiRect rect;
    rect.minY = 0;
    rect.minX = 0;
    rect.maxY = 0;
    rect.maxX = 0;
    GetChunkRect(extents, rect);

    for (int32_t row = rect.minY; row <= rect.maxY; row++) {
        for (int32_t col = rect.minX; col <= rect.maxX; col++) {
            CMapArea* area = CMap::m_areaTable[(((row >> 4) & 0x3F) << 6) + ((col >> 4) & 0x3F)];

            if (!area || area->m_asyncObject) {
                continue;
            }

            CMapChunk* chunk = area->m_mapChunks[(col & 0xF) + 16 * (row & 0xF)];

            if (!chunk) {
                continue;
            }

            if (!(extents.t.x >= chunk->m_bbox.b.x && extents.t.y >= chunk->m_bbox.b.y && extents.t.z >= chunk->m_bbox.b.z && extents.b.x <= chunk->m_bbox.t.x && extents.b.y <= chunk->m_bbox.t.y && extents.b.z <= chunk->m_bbox.t.z)) {
                continue;
            }

            if (!data.m_bandFrustums[band].Cull(chunk->m_bbox) || (data.m_useCullFrustum[band] && data.m_cullFrustums[band].CullInv(chunk->m_bbox) == 3)) {
                continue;
            }

            for (CMapBaseObjLink* link = chunk->m_doodadDefLinkList.Head(); link; link = chunk->m_doodadDefLinkList.Next(link)) {
                CMapDoodadDef* doodad = static_cast<CMapDoodadDef*>(link->m_owner);

                if (doodad->m_shadowStamps[band] == CShadowQuery::s_stamp) {
                    continue;
                }

                doodad->m_shadowStamps[band] = CShadowQuery::s_stamp;
                CM2Model* model = doodad->m_model;

                if (!model || !(doodad->m_flags & 0x80) || CWorldScene::IsBeyondFadeDistance(doodad->m_sphere.c, doodad->m_fadeLevel) || maxSize < doodad->m_sphere.r) {
                    continue;
                }

                // TROVICH: the camera pass's far band - big doodads only, beyond the near band
                if (s_viewMinRadius > 0.0f) {
                    if (doodad->m_sphere.r < s_viewMinRadius) {
                        continue;
                    }

                    C3Vector d(doodad->m_sphere.c.x - s_viewCamera.x, doodad->m_sphere.c.y - s_viewCamera.y, doodad->m_sphere.c.z - s_viewCamera.z);
                    float nearEdge = s_viewNearRange - doodad->m_sphere.r;

                    if (nearEdge > 0.0f && d.x * d.x + d.y * d.y + d.z * d.z < nearEdge * nearEdge) {
                        continue;
                    }
                }

                int32_t wanted = model->GetBillboardBoneCount() ? (data.m_queryFlags[band] & 0x4) : (data.m_queryFlags[band] & 0x8);

                if (wanted && extents.Intersects(doodad->m_bbox) && data.m_bandFrustums[band].Cull(doodad->m_bbox) && (!data.m_useCullFrustum[band] || data.m_cullFrustums[band].CullInv(doodad->m_bbox) != 3)) {
                    model->GatherModelBatchesShadowMap(CShadowQuery::s_bandBuffers[band].m_untextured, CShadowQuery::s_bandBuffers[band].m_textured);
                }
            }
        }
    }
}

// VERIFY: ba8a3951 manual
// OFFSET: 0x007BCC00
void CShadowQuery::QueryTerrain(CShadowCacheQueryData& data, int32_t firstBand, int32_t lastBand) {
    CiRect rect;
    rect.minY = 0;
    rect.minX = 0;
    rect.maxY = 0;
    rect.maxX = 0;
    GetChunkRect(data.m_extents[data.m_lastBand], rect);

    for (int32_t row = rect.minY; row <= rect.maxY; row++) {
        for (int32_t col = rect.minX; col <= rect.maxX; col++) {
            CMapArea* area = CMap::m_areaTable[(((row >> 4) & 0x3F) << 6) + ((col >> 4) & 0x3F)];

            if (!area || area->m_asyncObject) {
                continue;
            }

            CMapChunk* chunk = area->m_mapChunks[(col & 0xF) + 16 * (row & 0xF)];

            if (!chunk) {
                continue;
            }

            CAaBox& lastExtents = data.m_extents[lastBand];

            if (!(lastExtents.t.x >= chunk->m_bbox.b.x && lastExtents.t.y >= chunk->m_bbox.b.y && lastExtents.t.z >= chunk->m_bbox.b.z && lastExtents.b.x <= chunk->m_bbox.t.x && lastExtents.b.y <= chunk->m_bbox.t.y && lastExtents.b.z <= chunk->m_bbox.t.z)) {
                continue;
            }

            if (!data.m_bandFrustums[lastBand].Cull(chunk->m_bbox) || CWorldOcclusion::QueryVolumes(&chunk->m_sphere)) {
                continue;
            }

            for (CMapBaseObjLink* link = chunk->m_doodadDefLinkList.Head(); link; link = chunk->m_doodadDefLinkList.Next(link)) {
                CMapDoodadDef* doodad = static_cast<CMapDoodadDef*>(link->m_owner);
                CM2Model* model = doodad->m_model;

                if (!model || !(doodad->m_flags & 0x80)) {
                    continue;
                }

                uint32_t doodadFlag = model->GetBillboardBoneCount();

                for (int32_t band = firstBand; band <= lastBand; band++) {
                    if (doodad->m_shadowStamps[band] == CShadowQuery::s_stamp) {
                        continue;
                    }

                    doodad->m_shadowStamps[band] = CShadowQuery::s_stamp;

                    if (CWorldScene::IsBeyondFadeDistance(doodad->m_sphere.c, doodad->m_fadeLevel)) {
                        continue;
                    }

                    if (!(doodadFlag ? (data.m_queryFlags[band] & 0x4) : (data.m_queryFlags[band] & 0x8))) {
                        continue;
                    }

                    if (data.m_extents[band].Intersects(doodad->m_bbox) && data.m_bandFrustums[band].Cull(doodad->m_bbox) && (!data.m_useCullFrustum[band] || data.m_cullFrustums[band].CullInv(doodad->m_bbox) != 3)) {
                        model->GatherModelBatchesShadowMap(CShadowQuery::s_bandBuffers[band].m_untextured, CShadowQuery::s_bandBuffers[band].m_textured);
                    }
                }
            }
        }
    }
}

// TROVICH: terrain chunks as shadow casters. Stock gathers only the doodads standing on a chunk (QueryTerrain),
// never the ground itself: mountains cast no dynamic shadow, and the shadow of a tree in a mountain's shade still
// lands on the ground. Every loaded chunk that can throw a shadow into a band goes into that band's list:
//   - inside the band's box and light-space frustum, not wholly inside the previous band (as the doodads);
//   - not too far toward the sun for its height: the shadow of a top h above the ground reaches h * cot(sun
//     elevation) - the ground taken 500 yd under the band centre - and nothing down-sun of the band.
// The light-space depth range (2000 yd toward the sun) would otherwise take thousands of chunks into band 2.
void CShadowQuery::QueryTerrainCasters(CShadowCacheQueryData& data, int32_t firstBand, int32_t lastBand) {
    for (int32_t band = 0; band < 3; band++) {
        s_terrainCasters[band].SetCount(0);
    }

    if (!g_shadowTerrain || !g_shadowTerrain->m_intValue) {
        return;
    }

    // the light travels along s_sunDir (eye = centre - 2000 * s_sunDir): toward the sun = -s_sunDir
    const C3Vector& sun = CShadowCache::s_sunDir;
    float horizontal = sqrtf(sun.x * sun.x + sun.y * sun.y);
    float towardX = horizontal > 0.0001f ? -sun.x / horizontal : 0.0f;
    float towardY = horizontal > 0.0001f ? -sun.y / horizontal : 0.0f;
    float reachPerHeight = -sun.z > 0.05f ? horizontal / -sun.z : 20.0f;

    for (int32_t band = firstBand; band <= lastBand; band++) {
        if (!(data.m_queryFlags[band] & 0x8)) {
            continue;
        }

        CAaBox& extents = data.m_extents[band];
        const C3Vector& centre = data.m_pos[band];
        // the band's ground footprint along the sun axis: its light-space half-size (square + chunk half-diagonal)
        // stretched by 1 / sin(elevation)
        // the band's ground footprint along the sun axis: its light-space half-size stretched by 1 / sin(elevation)
        // - but no farther than anything drawn (the far clip): a low sun stretched band 2 over kilometres of
        // empty ground and took thousands of chunks (the sunset slowdown)
        float footprint = data.m_radius[band] * 1.4142135f;
        float alongHalf = footprint / (-sun.z > 0.25f ? -sun.z : 0.25f);
        float alongLimit = World::s_farClip > footprint ? World::s_farClip : footprint;
        alongHalf = (alongHalf < alongLimit ? alongHalf : alongLimit) + 24.0f;
        // how far toward the sun a caster may stand - the SAME for every band: a point takes its shadow from the
        // finest band that holds it, and a caster missing from that band only leaves a square hole there
        const float maxReach = 2000.0f;
        CiRect rect;
        rect.minY = 0;
        rect.minX = 0;
        rect.maxY = 0;
        rect.maxX = 0;
        GetChunkRect(extents, rect);

        for (int32_t row = rect.minY; row <= rect.maxY; row++) {
            for (int32_t col = rect.minX; col <= rect.maxX; col++) {
                CMapArea* area = CMap::m_areaTable[(((row >> 4) & 0x3F) << 6) + ((col >> 4) & 0x3F)];

                if (!area || area->m_asyncObject) {
                    continue;
                }

                CMapChunk* chunk = area->m_mapChunks[(col & 0xF) + 16 * (row & 0xF)];

                if (!chunk || !chunk->m_height || !chunk->m_header) {
                    continue;
                }

                CAaBox& box = chunk->m_bbox;

                if (!(extents.t.x >= box.b.x && extents.t.y >= box.b.y && extents.t.z >= box.b.z && extents.b.x <= box.t.x && extents.b.y <= box.t.y && extents.b.z <= box.t.z)) {
                    continue;
                }

                float along = (chunk->m_sphere.c.x - centre.x) * towardX + (chunk->m_sphere.c.y - centre.y) * towardY;
                float rise = box.t.z - (centre.z - 500.0f);

                float reach = (rise > 0.0f ? rise : 0.0f) * reachPerHeight;
                reach = reach < maxReach ? reach : maxReach;

                if (along < -alongHalf || along > alongHalf + reach) {
                    continue;
                }

                if (!data.m_bandFrustums[band].Cull(box) || (data.m_useCullFrustum[band] && data.m_cullFrustums[band].CullInv(box) == 3)) {
                    continue;
                }

                s_terrainCasters[band].Add(1, &chunk);
            }
        }
    }
}

uint32_t CShadowQuery::TerrainCasterCount(int32_t band) {
    return s_terrainCasters[band].Count();
}

// TROVICH: draws the band's terrain casters into its shadow map with the ShadowMapRenderSL effect already set
// (stock ShadowMap VS: position + texcoord = the GxVBF_PT stream; no texture, no alpha test - as the untextured
// WMO groups in RenderMapObjGroupsShadowMap). Chunk vertices in world space, world = -camera like the WMO
// groups; the chunk's own 145-vertex mesh with its holes (CMapChunk::CreateIndices), 64 chunks a draw.
void CShadowQuery::RenderTerrainCasters(int32_t band, const C44Matrix& translation, int32_t slopeBias, float coarseBeyond) {
    TSGrowableArray<CMapChunk*>& casters = s_terrainCasters[band];
    uint32_t count = casters.Count();

    if (!count) {
        return;
    }

    g_theGxDevicePtr->RsPush();

    CGxMatrixStack& stack = g_theGxDevicePtr->m_xforms[GxXform_World];
    stack.m_dirty = 1;
    stack.m_flags[stack.m_level] &= ~CGxMatrixStack::F_Identity;
    stack.m_mtx[stack.m_level] = translation;
    CShaderEffect::UpdateWorldViewMatrix();

    CShaderEffect::SetAlphaRef(0.0f);
    CShaderEffect::SetDefaultShaders(0);
    GxRsSet(GxRs_Texture0, 0);
    float constants[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
    g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 2, constants, 1);

    const uint32_t chunksPerDraw = 64;  // 64 * 145 vertices, 64 * 768 indices: fits 16-bit indices

    // Slope-scaled depth bias: every caster vertex is pushed away from the light along the light ray by
    //   texel * (1.5 + 2 * tan(angle between its normal and the light)),  at most 16 texels.
    // The stock receiver bias is a constant (about 2 yd in the far band) and a lit slope, or flat ground under a
    // low sun, compares its depth with its own rasterised copy across the 2-texel PCF footprint: noisy, banded
    // self-shadowing. Back faces (turned from the light) get the 16 texels - they are dark from N.L anyway.
    const C3Vector& light = CShadowCache::s_sunDir;   // the direction the light travels, unit
    float texel = 2.0f * CShadowCache::s_bandRadius[band] / static_cast<float>(CShadowCache::s_shadowSize);
    float maxBias = 16.0f * texel;

    for (uint32_t first = 0; first < count; first += chunksPerDraw) {
        uint32_t n = count - first < chunksPerDraw ? count - first : chunksPerDraw;
        CGxBuf* vertexBuf = g_theGxDevicePtr->BufStream(GxPoolTarget_Vertex, sizeof(CGxVertexPT), n * 145);
        CGxVertexPT* vertices = reinterpret_cast<CGxVertexPT*>(g_theGxDevicePtr->BufLock(vertexBuf));
        CGxBuf* indexBuf = g_theGxDevicePtr->BufStream(GxPoolTarget_Index, sizeof(uint16_t), n * 768);
        uint16_t* indices = reinterpret_cast<uint16_t*>(g_theGxDevicePtr->BufLock(indexBuf));
        uint32_t vertexCount = 0;
        uint32_t indexCount = 0;

        for (uint32_t i = first; i < first + n; i++) {
            CMapChunk* chunk = casters.m_data[i];
            float top = 17066.666f - chunk->m_cOffset.y * 33.333332f;   // as CMapChunk::CreateVerticesWorld
            float left = 17066.666f - chunk->m_cOffset.x * 33.333332f;
            const float* height = chunk->m_height;
            const int8_t* normals = chunk->m_normals;   // MCNR: 145 x (x, y, z) / 127, same order as the heights

            for (uint32_t k = 0; k < 145; k++) {
                float bias = slopeBias ? maxBias : 0.0f;

                if (normals && slopeBias) {
                    float facing = -(normals[3 * k] * light.x + normals[3 * k + 1] * light.y + normals[3 * k + 2] * light.z) * (1.0f / 127.0f);

                    if (facing > 0.05f) {
                        bias = texel * (1.5f + 2.0f * sqrtf(1.0f - (facing > 1.0f ? 1.0f : facing * facing)) / facing);
                        bias = bias < maxBias ? bias : maxBias;
                    }
                }

                CGxVertexPT& v = vertices[vertexCount + k];
                v.p.x = top + CMapChunk::vertexList[k].x + light.x * bias;
                v.p.y = left + CMapChunk::vertexList[k].y + light.y * bias;
                v.p.z = chunk->m_topLeftCoords.z + height[k] + light.z * bias;
                v.tc.x = 0.0f;
                v.tc.y = 0.0f;
            }

            // coarseBeyond (the fog's camera pass): far chunks as 8 x 8 quads of their outer vertices only - half
            // the triangles, plenty for a depth the far haze reads; holes kept (a quad in a hole is left out)
            float dx = chunk->m_sphere.c.x + translation.d0;
            float dy = chunk->m_sphere.c.y + translation.d1;

            if (coarseBeyond > 0.0f && dx * dx + dy * dy > coarseBeyond * coarseBeyond) {
                uint16_t* out = &indices[indexCount];

                for (int32_t row = 0; row < 8; row++) {
                    for (int32_t col = 0; col < 8; col++) {
                        if (chunk->m_header->m_holes & CMap::s_holeMask[(row / 2) * 4 + (col / 2)]) {
                            continue;
                        }

                        uint16_t tl = static_cast<uint16_t>(vertexCount + row * 17 + col);
                        uint16_t tr = tl + 1;
                        uint16_t bl = tl + 17;
                        uint16_t br = bl + 1;
                        out[0] = tl;
                        out[1] = bl;
                        out[2] = tr;
                        out[3] = tr;
                        out[4] = bl;
                        out[5] = br;
                        out += 6;
                        indexCount += 6;
                    }
                }
            } else {
                indexCount += chunk->CreateIndices(reinterpret_cast<char*>(&indices[indexCount]), vertexCount);
            }

            vertexCount += 145;
        }

        g_theGxDevicePtr->BufUnlock(vertexBuf, 0);
        vertexBuf->m_hasData = 1;
        GxPrimVertexPtr(vertexBuf, GxVBF_PT);
        g_theGxDevicePtr->BufUnlock(indexBuf, 0);
        indexBuf->m_hasData = 1;
        g_theGxDevicePtr->PrimIndexPtr(indexBuf);

        if (!indexCount) {
            continue;
        }

        CGxBatch batch;
        batch.m_primType = GxPrim_Triangles;
        batch.m_start = 0;
        batch.m_count = indexCount;
        batch.m_minIndex = 0;
        batch.m_maxIndex = static_cast<uint16_t>(vertexCount - 1);
        g_theGxDevicePtr->Draw(&batch, 1);
    }

    g_theGxDevicePtr->RsPop();
}

// TROVICH: the camera's visible terrain chunks within the fog range (box + camera frustum of the query band)
void CShadowQuery::QueryTerrainView(CShadowCacheQueryData& data, int32_t band) {
    s_terrainCasters[band].SetCount(0);

    CAaBox& extents = data.m_extents[band];
    CiRect rect;
    rect.minY = 0;
    rect.minX = 0;
    rect.maxY = 0;
    rect.maxX = 0;
    GetChunkRect(extents, rect);

    for (int32_t row = rect.minY; row <= rect.maxY; row++) {
        for (int32_t col = rect.minX; col <= rect.maxX; col++) {
            CMapArea* area = CMap::m_areaTable[(((row >> 4) & 0x3F) << 6) + ((col >> 4) & 0x3F)];

            if (!area || area->m_asyncObject) {
                continue;
            }

            CMapChunk* chunk = area->m_mapChunks[(col & 0xF) + 16 * (row & 0xF)];

            if (!chunk || !chunk->m_height || !chunk->m_header) {
                continue;
            }

            CAaBox& box = chunk->m_bbox;

            if (!(extents.t.x >= box.b.x && extents.t.y >= box.b.y && extents.t.z >= box.b.z && extents.b.x <= box.t.x && extents.b.y <= box.t.y && extents.b.z <= box.t.z)) {
                continue;
            }

            if (!data.m_bandFrustums[band].Cull(box)) {
                continue;
            }

            s_terrainCasters[band].Add(1, &chunk);
        }
    }
}

static void SetPlane(C4Plane& plane, C3Vector n, const C3Vector& point) {
    float length = sqrtf(n.x * n.x + n.y * n.y + n.z * n.z);

    if (length > 0.0f) {
        n.x /= length;
        n.y /= length;
        n.z /= length;
    }

    plane.n = n;
    plane.d = -(n.x * point.x + n.y * point.y + n.z * point.z);
}

// TROVICH: camera depth pass for the volumetric fog. The same casters as a shadow band (GatherShadowCasters:
// units and entity models, QueryMapObjDefsBand: WMO groups and their doodads, QueryTerrainBand: the chunks'
// doodads, QueryTerrainView: the chunks themselves) inside the camera frustum cut at `range`, drawn with the
// ShadowMapRenderSL effect from the CAMERA: its pixel shader writes view z * c0.w = view depth / range (the
// camera's view z points forward, GxuXformCreateLookAtSgCompat). Cleared to 1 = nothing nearer than the range.
// Runs after CShadowQuery::Update: the shadow bands are drawn, their buffers are free, the models animated.
void CShadowQuery::RenderViewDepth(CGxTex* target, float depthRange, float modelRange) {
    float range = modelRange;
    // the camera: the device view is its camera-relative rotation (see RenderCallback), the position the world view
    C44Matrix view;
    GxXformView(view);
    C3Vector right(view.a0, view.b0, view.c0);
    C3Vector up(view.a1, view.b1, view.c1);
    C3Vector forward(view.a2, view.b2, view.c2);
    const C44Matrix& projection = g_theGxDevicePtr->m_projection;
    float tanX = projection.a0 != 0.0f ? 1.0f / projection.a0 : 1.0f;
    float tanY = projection.b1 != 0.0f ? 1.0f / projection.b1 : 1.0f;
    const C3Vector camera = CWorldScene::s_activeWorldView;

    CShadowCacheQueryData data;

    for (int32_t i = 0; i < 3; i++) {
        data.m_queryFlags[i] = 0;
        data.m_active[i] = 0;
        data.m_useCullFrustum[i] = 0;
    }

    data.m_queryFlags[0] = 0x1 | 0x4 | 0x8;   // units, doodads, other models
    data.m_active[0] = 1;
    data.m_pos[0] = camera;
    data.m_radius[0] = range;
    data.m_lastBand = 0;
    data.m_useFrustum = 0;

    // inward planes: n . p + d >= 0 inside (CFrustum::Cull)
    CFrustum& frustum = data.m_bandFrustums[0];
    C3Vector farPoint(camera.x + forward.x * range, camera.y + forward.y * range, camera.z + forward.z * range);
    SetPlane(frustum.planes[0], forward, camera);
    SetPlane(frustum.planes[1], C3Vector(-forward.x, -forward.y, -forward.z), farPoint);
    SetPlane(frustum.planes[2], C3Vector(forward.x * tanX - right.x, forward.y * tanX - right.y, forward.z * tanX - right.z), camera);
    SetPlane(frustum.planes[3], C3Vector(forward.x * tanX + right.x, forward.y * tanX + right.y, forward.z * tanX + right.z), camera);
    SetPlane(frustum.planes[4], C3Vector(forward.x * tanY - up.x, forward.y * tanY - up.y, forward.z * tanY - up.z), camera);
    SetPlane(frustum.planes[5], C3Vector(forward.x * tanY + up.x, forward.y * tanY + up.y, forward.z * tanY + up.z), camera);

    CAaBox& extents = data.m_extents[0];
    extents.b = camera;
    extents.t = camera;

    for (int32_t sx = -1; sx <= 1; sx += 2) {
        for (int32_t sy = -1; sy <= 1; sy += 2) {
            C3Vector corner(
                camera.x + (forward.x + right.x * tanX * sx + up.x * tanY * sy) * range,
                camera.y + (forward.y + right.y * tanX * sx + up.y * tanY * sy) * range,
                camera.z + (forward.z + right.z * tanX * sx + up.z * tanY * sy) * range);
            extents.b.x = corner.x < extents.b.x ? corner.x : extents.b.x;
            extents.b.y = corner.y < extents.b.y ? corner.y : extents.b.y;
            extents.b.z = corner.z < extents.b.z ? corner.z : extents.b.z;
            extents.t.x = corner.x > extents.t.x ? corner.x : extents.t.x;
            extents.t.y = corner.y > extents.t.y ? corner.y : extents.t.y;
            extents.t.z = corner.z > extents.t.z ? corner.z : extents.t.z;
        }
    }

    // band 1: the same frustum out to depthRange - terrain chunks and WMO groups only (the far haze needs the
    // distance of far mountains and buildings, not of every far tree)
    CFrustum& farFrustum = data.m_bandFrustums[1];
    farFrustum = frustum;
    C3Vector farEnd(camera.x + forward.x * depthRange, camera.y + forward.y * depthRange, camera.z + forward.z * depthRange);
    SetPlane(farFrustum.planes[1], C3Vector(-forward.x, -forward.y, -forward.z), farEnd);
    data.m_extents[1].b = camera;
    data.m_extents[1].t = camera;

    for (int32_t sx = -1; sx <= 1; sx += 2) {
        for (int32_t sy = -1; sy <= 1; sy += 2) {
            C3Vector corner(
                camera.x + (forward.x + right.x * tanX * sx + up.x * tanY * sy) * depthRange,
                camera.y + (forward.y + right.y * tanX * sx + up.y * tanY * sy) * depthRange,
                camera.z + (forward.z + right.z * tanX * sx + up.z * tanY * sy) * depthRange);
            CAaBox& box = data.m_extents[1];
            box.b.x = corner.x < box.b.x ? corner.x : box.b.x;
            box.b.y = corner.y < box.b.y ? corner.y : box.b.y;
            box.b.z = corner.z < box.b.z ? corner.z : box.b.z;
            box.t.x = corner.x > box.t.x ? corner.x : box.t.x;
            box.t.y = corner.y > box.t.y ? corner.y : box.t.y;
            box.t.z = corner.z > box.t.z ? corner.z : box.t.z;
        }
    }

    data.m_queryFlags[1] = 0x4 | 0x8;   // every kind of WMO def and its groups; doodads off (s_viewGroupsOnly)
    data.m_pos[1] = camera;
    data.m_radius[1] = depthRange;

    // band 2: the same far frustum for the BIG doodads beyond the model range - trees, rocks, ships (radius 6 yd and
    // up, s_viewMinRadius: QueryTerrainBand does not apply GetSize's minimum). Without them a tree switched from its own fog to the fog of the ground
    // behind it when it crossed the model range: steps of fog on the trees while walking toward them.
    data.m_bandFrustums[2] = farFrustum;
    data.m_extents[2] = data.m_extents[1];
    data.m_queryFlags[2] = 0x4 | 0x8;
    data.m_pos[2] = camera;
    data.m_radius[2] = depthRange;

    // gather (fresh stamp: the shadow query already stamped the doodads it took this frame)
    CShadowQuery::AllocBuffers();

    if (!++CShadowQuery::s_stamp) {
        CShadowQuery::s_stamp = 1;
    }

    CM2Model::s_cameraDepthPass = 1;               // models in this frame's pose (AnimateSM)
    CShadowQuery::GatherShadowCasters(data, 0, 0);   // units and entity models, band 0 (modelRange)
    CShadowQuery::QueryTerrainBand(data, 0);         // the chunks' doodads, band 0
    s_viewMinRadius = 6.0f;                          // ... and the big ones (trees, rocks) out to depthRange, band 2
    s_viewNearRange = modelRange;
    s_viewCamera = camera;
    CShadowQuery::QueryTerrainBand(data, 2);
    s_viewMinRadius = 0.0f;
    s_viewAllGroups = 1;
    s_viewGroupsOnly = 1;
    CShadowQuery::QueryMapObjDefsBand(data, 1);      // every WMO group to depthRange, interiors too, no doodads
    s_viewGroupsOnly = 0;
    CShadowQuery::QueryMapObjDefsBand(data, 0);      // ... and the WMO doodads within modelRange (groups again)
    s_viewAllGroups = 0;
    CM2Model::s_cameraDepthPass = 0;
    CShadowQuery::QueryTerrainView(data, 1);         // terrain to depthRange

    // draw: band 1's WMO groups and terrain, band 0's models
    ShadowQueryBuffers& buffers = CShadowQuery::s_bandBuffers[0];
    ShadowQueryBuffers& farBuffers = CShadowQuery::s_bandBuffers[1];
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
    GxRsSet(GxRs_Culling, 0);
    GxRsSet(GxRs_Fog, 0);
    GxRsSet(GxRs_ScissorTest, 0);
    CShaderEffect::SetTexMtx_Identity(0);

    CShaderEffect* effect = CShaderEffectManager::GetEffect("ShadowMapRenderSL");
    effect->SetCurrent();
    float constants[4] = { 0.0f, 0.0f, 0.0f, 1.0f / depthRange };
    g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 0, constants, 1);

    GxXformSetViewport(0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f);
    g_theGxDevicePtr->DeviceSetRenderTarget(GxBuffers_Color, target, 0);
    GxSceneClear(3, CImVector(0xFF, 0xFF, 0xFF, 0xFF));

    C44Matrix translation;
    translation.d0 = -camera.x;
    translation.d1 = -camera.y;
    translation.d2 = -camera.z;

    // foliage: the main pass keeps leaf edges the shadow thresholds (0.878 WMO, 0.502 M2) cut away - those edge
    // pixels then got the fog of the background behind the tree (a bright rim). Lower thresholds for the camera.
    float wmoAlphaRef = CMapObj::s_shadowAlphaRef;
    float m2AlphaRef = CM2Model::s_shadowAlphaRef;
    CMapObj::s_shadowAlphaRef = 0.3f;
    CM2Model::s_shadowAlphaRef = 0.25f;

    if (farBuffers.m_groupCount) {
        CMapObj::RenderMapObjGroupsShadowMap(farBuffers.m_groups, farBuffers.m_groupCount, farBuffers.m_groupData, translation, farFrustum);
    }

    CShadowQuery::RenderTerrainCasters(1, translation, 0, modelRange);

    ShadowQueryBuffers& bigBuffers = CShadowQuery::s_bandBuffers[2];
    int32_t haveNear = buffers.m_untextured.m_count || buffers.m_textured.m_count;
    int32_t haveBig = bigBuffers.m_untextured.m_count || bigBuffers.m_textured.m_count;

    if (haveNear || haveBig) {
        // The skinned ShadowMap permutations (every model) take the bones to CAMERA view space and then through
        // vertex c14..c16, the camera-to-light matrix RenderCallback sets for the shadow bands - left as it is,
        // the models land in light space and off the screen. Identity for the camera pass, restored after.
        float saved[12];
        const float* current = g_theGxDevicePtr->ShaderConstantsLock(GxSh_Vertex) + 14 * 4;   // the CPU copy, read only

        for (uint32_t i = 0; i < 12; i++) {
            saved[i] = current[i];
        }

        float identity[12] = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
        g_theGxDevicePtr->ShaderConstantsSet(GxSh_Vertex, 14, identity, 3);

        if (haveNear) {
            if (g_shadowInstancing->m_intValue) {
                buffers.m_untextured.Optimize();
                buffers.m_textured.Optimize();
            }

            CM2Model::RenderModelBatchesShadowMap(buffers.m_untextured, buffers.m_textured);
        }

        if (haveBig) {
            if (g_shadowInstancing->m_intValue) {
                bigBuffers.m_untextured.Optimize();
                bigBuffers.m_textured.Optimize();
            }

            CM2Model::RenderModelBatchesShadowMap(bigBuffers.m_untextured, bigBuffers.m_textured);
        }

        g_theGxDevicePtr->ShaderConstantsSet(GxSh_Vertex, 14, saved, 3);
    }

    CMapObj::s_shadowAlphaRef = wmoAlphaRef;
    CM2Model::s_shadowAlphaRef = m2AlphaRef;

    g_theGxDevicePtr->RsPop();
    GxXformSetViewport(viewportMinX, viewportMaxX, viewportMinY, viewportMaxY, viewportMinZ, viewportMaxZ);
    g_theGxDevicePtr->DeviceSetRenderTarget(GxBuffers_Color, oldTarget, 0);
    CShaderEffect::UpdateProjMatrix();
}
