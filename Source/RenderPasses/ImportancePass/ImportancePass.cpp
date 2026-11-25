#include "ImportancePass.h"
#include "RenderGraph/RenderPassStandardFlags.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>

namespace
{
const std::string kShaderFile = "RenderPasses/ImportancePass/ImportancePass.cs.slang";
const std::string kTagShaderFile   = "RenderPasses/ImportancePass/TagPass.cs.slang";
const std::string kFieldShaderFile = "RenderPasses/ImportancePass/EnemyField.cs.slang";
const std::string kConfigFile = "Source/RenderPasses/ImportancePass/TaskConfig.json";

// Input channel names (must match GBufferRT output channel names exactly)
const std::string kInDiffuseOpacity = "diffuseOpacity";
const std::string kInGuideNormalW   = "guideNormalW";
const std::string kInFaceNormalW    = "faceNormalW";
const std::string kInSpecRough      = "specRough";
const std::string kInMotionVectorW  = "mvecW";
const std::string kInLinearZ        = "linearZ";
const std::string kInShadowCount    = "shadowCount";
const std::string kInObjID          = "objectID";
const std::string kInLuminance      = "luminance";
const std::string kInCenterDist     = "centerDist";
const std::string kInMetalness      = "metalness";

// Output channel names
const std::string kOutImportance    = "importance";
const std::string kOutImportanceVis = "importanceVis";
const std::string kOutTagMap        = "tagMap";
} // namespace

// ---------------------------------------------------------------------------
ImportancePass::ImportancePass(ref<Device> pDevice, const Properties& props)
    : RenderPass(pDevice)
{
    mpComputePass = ComputePass::create(mpDevice, kShaderFile, "main");
    mpTagPass = ComputePass::create(mpDevice, kTagShaderFile, "tagMain");
    mpFieldInitPass = ComputePass::create(mpDevice, kFieldShaderFile, "jfaInitMain");
    mpFieldFinalPass = ComputePass::create(mpDevice, kFieldShaderFile, "jfaFinalMain");

    // 적 마스크 통계: [0] 픽셀 수 [1] x 합 [2] y 합 [3] 조준점 적중
    mpEnemyStatsBuf = mpDevice->createStructuredBuffer(sizeof(uint32_t), kEnemyStatCount);

    // 인스턴스 태그 표: 씬이 없어도 바인딩할 수 있도록 최소 1개로 만든다 (R-2.2)
    mpInstanceTagBuf = mpDevice->createStructuredBuffer(sizeof(uint32_t), 1);

    // 비동기 readback 링 (R-4.7)
    for (auto& slot : mReadback)
        slot.pStaging = mpDevice->createBuffer(kEnemyStatCount * sizeof(uint32_t), ResourceBindFlags::None, MemoryType::ReadBack);
    mpReadbackFence = mpDevice->createFence();

    mConfigPath = getProjectDirectory() / kConfigFile;
    loadConfig();
}

// ---------------------------------------------------------------------------
Properties ImportancePass::getProperties() const { return Properties(); }

// ---------------------------------------------------------------------------
RenderPassReflection ImportancePass::reflect(const CompileData&)
{
    RenderPassReflection r;
    const auto opt = RenderPassReflection::Field::Flags::Optional;

    r.addInput(kInDiffuseOpacity, "Diffuse albedo + opacity").flags(opt);
    r.addInput(kInGuideNormalW,   "Detail (shading) normal W").flags(opt);
    r.addInput(kInFaceNormalW,    "Geometry face normal W").flags(opt);
    r.addInput(kInSpecRough,      "Specular + roughness").flags(opt);
    r.addInput(kInMotionVectorW,  "World-space motion vector").flags(opt);
    r.addInput(kInLinearZ,        "Linear Z + slope").flags(opt);
    r.addInput(kInShadowCount,    "Shadow count").flags(opt);
    r.addInput(kInObjID,          "Object/instance ID").flags(opt);
    r.addInput(kInLuminance,      "Pre-computed luminance").flags(opt);
    r.addInput(kInCenterDist,     "Distance to screen center").flags(opt);
    r.addInput(kInMetalness,      "Per-pixel metalness").flags(opt);

    r.addOutput(kOutImportance, "Per-pixel importance [0,1]")
        .format(ResourceFormat::R32Float)
        .bindFlags(ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource);
    r.addOutput(kOutImportanceVis, "Importance visualization")
        .format(ResourceFormat::RGBA8Unorm)
        .bindFlags(ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource);
    r.addOutput(kOutTagMap, "Per-pixel object tag (0 None, 1 Enemy, 2 Item, 3 Wall, 4 Floor)")
        .format(ResourceFormat::R8Uint)
        .bindFlags(ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource);
    return r;
}

// ---------------------------------------------------------------------------
bool ImportancePass::onKeyEvent(const KeyboardEvent& e)
{
    const bool down = (e.type == KeyboardEvent::Type::KeyPressed ||
                       e.type == KeyboardEvent::Type::KeyRepeated);
    const bool up   = (e.type == KeyboardEvent::Type::KeyReleased);
    if (!down && !up) return false;

    const bool v = down;
    switch (e.key)
    {
    case Input::Key::W: mKeyW = v; break;
    case Input::Key::A: mKeyA = v; break;
    case Input::Key::S: mKeyS = v; break;
    case Input::Key::D: mKeyD = v; break;
    default: return false;
    }
    return false; // do not consume; CameraController also needs it
}

// ---------------------------------------------------------------------------
// Left click fires only in custom camera mode. In that mode the mouse look needs no button,
// so the click is free. Not consumed; the camera still gets the event.
bool ImportancePass::onMouseEvent(const MouseEvent& e)
{
    if (isCustomCameraActive() && e.type == MouseEvent::Type::ButtonDown && e.button == Input::MouseButton::Left)
        mFirePressed = true;
    return false;
}

// ---------------------------------------------------------------------------
// Read-only: the mode is owned by CameraController (mUseCustomBehavior). This pass never writes it.
bool ImportancePass::isCustomCameraActive() const
{
    if (!mpScene) return false;
    const CameraController* pCtrl = mpScene->getCameraController();
    if (!pCtrl) return false;

    switch (mpScene->getCameraControllerType())
    {
    case Scene::CameraControllerType::FirstPerson:
        return static_cast<const FirstPersonCameraController*>(pCtrl)->getUseCustomBehavior();
    case Scene::CameraControllerType::SixDOF:
        return static_cast<const SixDoFCameraController*>(pCtrl)->getUseCustomBehavior();
    default:
        return false; // Orbiter has no custom behavior
    }
}

// ---------------------------------------------------------------------------
// TaskConfig.json 을 읽는다 (R-2.1, R-4.6). 없는 키는 현재 값을 유지한다.
void ImportancePass::loadConfig()
{
    std::ifstream file(mConfigPath);
    if (!file)
    {
        mConfigStatus = "not found, using defaults: " + mConfigPath.string();
        logWarning("ImportancePass: task config {}", mConfigStatus);
        return;
    }

    try
    {
        const nlohmann::json j = nlohmann::json::parse(file);
        TaskConfig c = mClassifier.getConfig();
        const nlohmann::json enemy = j.value("enemy", nlohmann::json::object());
        const nlohmann::json task  = j.value("task", nlohmann::json::object());
        const nlohmann::json fire  = j.value("fire", nlohmann::json::object());

        c.enemyRedThreshold    = enemy.value("redThreshold", c.enemyRedThreshold);
        c.enemyRedDominance    = enemy.value("redDominance", c.enemyRedDominance);
        c.enemyGreenMaxRatio   = enemy.value("greenMaxRatio", c.enemyGreenMaxRatio);
        c.enemyMinPixels       = enemy.value("minPixels", c.enemyMinPixels);
        c.centroidEmaAlpha     = enemy.value("centroidEmaAlpha", c.centroidEmaAlpha);

        c.omegaScan            = task.value("omegaScan", c.omegaScan);
        c.omegaFlick           = task.value("omegaFlick", c.omegaFlick);
        c.omegaFlickExit       = task.value("omegaFlickExit", c.omegaFlickExit);
        c.k                    = task.value("k", c.k);
        c.thetaTrack           = task.value("thetaTrack", c.thetaTrack);
        c.thetaTrackFire       = task.value("thetaTrackFire", c.thetaTrackFire);
        c.thetaTrackExitMargin = task.value("thetaTrackExitMargin", c.thetaTrackExitMargin);
        c.vMove                = task.value("vMove", c.vMove);
        c.phiHead              = task.value("phiHead", c.phiHead);
        c.minHoldSec           = task.value("minHoldSec", c.minHoldSec);

        c.fireChunkGapSec      = fire.value("chunkGapSec", c.fireChunkGapSec);
        c.recentFireWindowSec  = fire.value("recentWindowSec", c.recentFireWindowSec);

        // 객체 태그 (R-2.1): 클래스 이름 -> 머티리얼 이름 목록, 식별 모드 (tag | color)
        std::map<std::string, std::vector<std::string>> classToMaterials;
        const nlohmann::json tags = j.value("tags", nlohmann::json::object());
        for (auto it = tags.begin(); it != tags.end(); ++it)
            classToMaterials[it.key()] = it.value().get<std::vector<std::string>>();
        std::vector<std::string> unknownClasses;
        const MaterialTagMap newTags = buildMaterialTagMap(classToMaterials, &unknownClasses);
        for (const auto& name : unknownClasses)
            logWarning("ImportancePass: task config: unknown tag class '{}' ignored", name);
        // 적 영역 맵 (R-5.2 ~ R-5.4). 값이 없으면 현재 값을 유지한다.
        const nlohmann::json enemyRoi = j.value("enemyRoi", nlohmann::json::object());
        const float newSigmaRatio = enemyRoi.value("sigmaRatio", mEnemySigmaRatio);
        AlphaRampParams newAlpha = mAlphaParams;
        newAlpha.alphaMax = enemyRoi.value("alphaMax", newAlpha.alphaMax);
        newAlpha.rampSec  = enemyRoi.value("rampSec", newAlpha.rampSec);

        const std::string mode = j.value("identifyMode", std::string("tag"));
        if (mode != "tag" && mode != "color")
            logWarning("ImportancePass: task config: identifyMode '{}' is not tag/color, using tag", mode);

        mClassifier.setConfig(c);
        mEnemySigmaRatio = newSigmaRatio;
        mAlphaParams     = newAlpha;
        mMaterialTags  = newTags;
        mIdentifyColor = (mode == "color");
        mTagTableDirty = true;
        mConfigStatus = "loaded: " + mConfigPath.string();
        logInfo("ImportancePass: task config {}", mConfigStatus);
    }
    catch (const nlohmann::json::exception& ex)
    {
        mConfigStatus = std::string("parse error, keeping previous values: ") + ex.what();
        logWarning("ImportancePass: task config {}", mConfigStatus);
    }
}

// ---------------------------------------------------------------------------
// 적 영역 맵용 텍스처를 해상도에 맞춘다 (R-5.2). 해상도가 바뀔 때만 새로 만든다.
void ImportancePass::ensureFieldTextures(uint32_t width, uint32_t height)
{
    if (mpSeedA && mFieldW == width && mFieldH == height)
        return;
    mFieldW = width;
    mFieldH = height;
    const ResourceBindFlags bind = ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource;
    mpTagMap   = mpDevice->createTexture2D(width, height, ResourceFormat::R8Uint, 1, 1, nullptr, bind);
    mpSeedA    = mpDevice->createTexture2D(width, height, ResourceFormat::RG32Uint, 1, 1, nullptr, bind);
    mpSeedB    = mpDevice->createTexture2D(width, height, ResourceFormat::RG32Uint, 1, 1, nullptr, bind);
    mpEnemyRoi = mpDevice->createTexture2D(width, height, ResourceFormat::R32Float, 1, 1, nullptr, bind);
}

// JFA 단계 크기별 compute 패스. 간격은 define JFA_STEP 으로 넣는다.
ref<ComputePass> ImportancePass::getJfaStepPass(uint32_t step)
{
    auto it = mJfaStepPasses.find(step);
    if (it != mJfaStepPasses.end())
        return it->second;

    ref<ComputePass> pass = ComputePass::create(mpDevice, kFieldShaderFile, "jfaStepMain");
    DefineList defines;
    defines["JFA_STEP"] = std::to_string(step);
    pass->getProgram()->addDefines(defines);
    mJfaStepPasses[step] = pass;
    return pass;
}

// 씬 인스턴스마다 머티리얼 이름을 보고 태그 표를 만든다 (R-2.2). 씬이 바뀌거나 설정을 다시 읽으면 호출한다.
void ImportancePass::rebuildInstanceTags()
{
    mTagTableDirty = false;

    std::vector<std::string> materialNames;
    if (mpScene)
    {
        const uint32_t instanceCount = mpScene->getGeometryInstanceCount();
        const uint32_t materialCount = mpScene->getMaterialCount();
        materialNames.reserve(instanceCount);
        for (uint32_t i = 0; i < instanceCount; i++)
        {
            const uint32_t materialID = mpScene->getGeometryInstance(i).materialID;
            materialNames.push_back(materialID < materialCount
                ? mpScene->getMaterial(MaterialID{materialID})->getName()
                : std::string());
        }
    }

    std::vector<std::string> unmapped;
    const std::vector<uint32_t> table = buildInstanceTagTable(materialNames, mMaterialTags, &unmapped);
    mInstanceCount   = static_cast<uint32_t>(table.size());
    mpInstanceTagBuf = mpDevice->createStructuredBuffer(sizeof(uint32_t), std::max(1u, mInstanceCount));
    if (mInstanceCount > 0)
        mpInstanceTagBuf->setBlob(table.data(), 0, table.size() * sizeof(uint32_t));

    if (!unmapped.empty())
    {
        std::string names;
        for (const auto& name : unmapped)
            names += (names.empty() ? "" : ", ") + name;
        logWarning("ImportancePass: material(s) without tag, treated as None: {}", names);
    }
    logInfo("ImportancePass: instance tag table built ({} instances)", mInstanceCount);
}

// ---------------------------------------------------------------------------
// 이번 프레임 입력 수집 (R-1). 카메라는 컨트롤러·애니메이션 어느 쪽으로 움직여도 같은 값을 준다.
FrameInput ImportancePass::gatherFrameInput(double time, float dt)
{
    FrameInput in;
    in.frame = mFrame;
    in.time  = time;
    in.dt    = dt;
    in.camFwd[2] = 1.f;
    if (mpScene && mpScene->getCamera())
    {
        const auto& pCamera = mpScene->getCamera();
        const float3 pos = pCamera->getPosition();
        const float3 fwd = normalize(pCamera->getTarget() - pos);
        in.camPos[0] = pos.x; in.camPos[1] = pos.y; in.camPos[2] = pos.z;
        in.camFwd[0] = fwd.x; in.camFwd[1] = fwd.y; in.camFwd[2] = fwd.z;
    }
    in.keyW = mKeyW; in.keyA = mKeyA; in.keyS = mKeyS; in.keyD = mKeyD;
    in.fire = mFirePressed;
    mFirePressed = false;
    return in;
}

// ---------------------------------------------------------------------------
// 복사가 끝난 readback 슬롯 중 가장 최근 것을 읽는다. GPU 를 기다리지 않는다 (R-4.7).
bool ImportancePass::pollEnemyReadback(EnemySample& sample)
{
    const uint64_t completed = mpReadbackFence->getCurrentValue();
    ReadbackSlot* pNewest = nullptr;
    for (auto& slot : mReadback)
    {
        if (!slot.pending || slot.fenceValue > completed) continue;
        slot.pending = false;
        if (!pNewest || slot.meta.frame > pNewest->meta.frame) pNewest = &slot;
    }
    if (!pNewest) return false;

    uint32_t stats[kEnemyStatCount];
    std::memcpy(stats, pNewest->pStaging->map(), sizeof(stats));
    pNewest->pStaging->unmap();

    sample = pNewest->meta;
    sample.pixelCount = stats[0];
    if (stats[0] > 0)
    {
        sample.centroid[0] = static_cast<float>(stats[1]) / stats[0] + 0.5f;
        sample.centroid[1] = static_cast<float>(stats[2]) / stats[0] + 0.5f;

        // 바운딩 박스: 최대값을 뒤집어 저장했으므로 x 최소 = W - [6], y 최소 = H - [7]
        const float W = static_cast<float>(pNewest->frameW);
        const float H = static_cast<float>(pNewest->frameH);
        sample.bboxUV[0] = (W - static_cast<float>(stats[6])) / W;
        sample.bboxUV[1] = (H - static_cast<float>(stats[7])) / H;
        sample.bboxUV[2] = (static_cast<float>(stats[4]) + 1.f) / W;
        sample.bboxUV[3] = (static_cast<float>(stats[5]) + 1.f) / H;
    }
    sample.centerHit = stats[3] != 0;
    return true;
}

// ---------------------------------------------------------------------------
// 이번 프레임 적 통계를 빈 슬롯으로 복사하고 fence 를 건다.
void ImportancePass::issueEnemyReadback(RenderContext* pRenderContext, uint32_t width, uint32_t height, double time)
{
    ReadbackSlot& slot = mReadback[mNextSlot];
    // GPU 가 슬롯 수만큼 밀려 있으면 이번 프레임은 readback 을 건너뛴다 (기다리지 않음).
    if (slot.pending) return;

    pRenderContext->copyResource(slot.pStaging.get(), mpEnemyStatsBuf.get());
    pRenderContext->submit(false);
    slot.fenceValue = pRenderContext->signal(mpReadbackFence.get());
    slot.pending    = true;

    slot.meta = EnemySample();
    slot.frameW = width;
    slot.frameH = height;
    slot.meta.frame = mFrame;
    slot.meta.time  = time;
    slot.meta.aimPoint[0] = std::floor(mCB.centerUV[0] * width) + 0.5f;
    slot.meta.aimPoint[1] = std::floor(mCB.centerUV[1] * height) + 0.5f;
    if (mpScene && mpScene->getCamera())
    {
        const auto& pCamera = mpScene->getCamera();
        slot.meta.focalPx = pCamera->getFocalLength() / pCamera->getFrameHeight() * static_cast<float>(height);
    }
    mNextSlot = (mNextSlot + 1) % kReadbackSlots;
}

// ---------------------------------------------------------------------------
void ImportancePass::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    // Delta-time
    const auto now = std::chrono::steady_clock::now();
    if (mFirstTick) mStartTick = now;
    const float dt = mFirstTick
        ? 0.f
        : std::chrono::duration<float>(now - mLastTick).count();
    const double time = std::chrono::duration<double>(now - mStartTick).count();
    mLastTick  = now;
    mFirstTick = false;

    // 1) 완료된 적 통계 readback (몇 프레임 전 값, 없으면 직전 값 유지)
    EnemySample enemy;
    const bool hasEnemy = pollEnemyReadback(enemy);
    if (hasEnemy)
        mLastEnemy = enemy;

    // 2) Task 판정 (CPU) -> constant buffer 로 GPU 에 전달
    const FrameInput input = gatherFrameInput(time, dt);
    mCB.activeTask = static_cast<uint32_t>(mClassifier.update(input, hasEnemy ? &enemy : nullptr));

    mObjectIdConnected = renderData.getTexture(kInObjID) != nullptr;

    // Fetch output textures
    ref<Texture> pImportance    = renderData.getTexture(kOutImportance);
    ref<Texture> pImportanceVis = renderData.getTexture(kOutImportanceVis);
    FALCOR_ASSERT(pImportance);
    const uint32_t w = pImportance->getWidth();
    const uint32_t h = pImportance->getHeight();

    if (mTagTableDirty)
        rebuildInstanceTags();

    // Fill constant buffer
    mCB.frameDim[0]       = w;
    mCB.frameDim[1]       = h;
    mCB.invFrameDim[0]    = 1.f / static_cast<float>(w);
    mCB.invFrameDim[1]    = 1.f / static_cast<float>(h);
    // mCB.activeTask: mClassifier.update() 에서 설정
    mCB.debugView         = static_cast<uint32_t>(mDebugView);
    mCB.hasImportanceVis  = pImportanceVis ? 1u : 0u;
    mCB.centerUV[0]       = 0.5f;
    mCB.centerUV[1]       = 0.5f;
    mCB.centerRadiusUV    = mCenterRadiusUV;
    mCB.taskImportance    = mTaskImportance;
    const TaskConfig& cfg = mClassifier.getConfig();
    mCB.enemyRedThreshold  = cfg.enemyRedThreshold;
    mCB.enemyRedDominance  = cfg.enemyRedDominance;
    mCB.enemyGreenMaxRatio = cfg.enemyGreenMaxRatio;
    mCB.lumLow            = mLumLow;
    mCB.lumHigh           = mLumHigh;
    mCB.motionA           = mMotionA;
    mCB.motionB           = mMotionB;
    mCB.motionC           = mMotionC;
    mCB.centerWeightMin   = mCenterWeightMin;
    mCB.pathHalfWidthUV     = mPathHalfWidthUV;
    mCB.bandHalfHeightUV    = mBandHalfHeightUV;
    mCB.corridorHalfWidthUV = mCorridorHalfWidthUV;

    // ROI 입력 (R-5.1): 적 바운딩 박스와 중심은 readback 으로 1~3 프레임 늦은 값이다
    const TaskSignals& sig = mClassifier.getSignals();
    mCB.scanDirX        = sig.scanDir;
    mCB.enemyValid      = (sig.enemyVisible && mLastEnemy.pixelCount > 0) ? 1u : 0u;
    for (int i = 0; i < 4; i++)
        mCB.enemyBBox[i] = mLastEnemy.bboxUV[i];
    mCB.enemyCentroidUV[0] = sig.centroid[0] / static_cast<float>(w);
    mCB.enemyCentroidUV[1] = sig.centroid[1] / static_cast<float>(h);
    mCB.identifyMode    = mIdentifyColor ? 1u : 0u;
    mCB.instanceCount   = mInstanceCount;

    // 적 영역 가중치 α (R-5.4): 적이 보이면 올리고 안 보이면 내린다 (벽시계 시간 기준)
    mEnemyAlpha = updateAlphaRamp(mEnemyAlpha, sig.enemyVisible, dt, mAlphaParams);
    mCB.enemyAlpha = mEnemyAlpha;

    // ---- 1) 태그 맵 (R-2.3). 메인 패스와 적 영역 맵이 이 결과를 읽는다 ----
    ensureFieldTextures(w, h);
    {
        auto tagVar = mpTagPass->getRootVar();
        tagVar["CB"].setBlob(&mCB, sizeof(mCB));
        tagVar["gDiffuseOpacity"] = renderData.getTexture(kInDiffuseOpacity);
        tagVar["gObjectID"]       = renderData.getTexture(kInObjID);
        tagVar["gInstanceTag"]    = mpInstanceTagBuf;
        tagVar["gTagOut"]         = mpTagMap;

        DefineList tagDefines;
        tagDefines["HAS_DIFFUSE_OPACITY"] = renderData.getTexture(kInDiffuseOpacity) ? "1" : "0";
        tagDefines["HAS_OBJ_ID"]          = renderData.getTexture(kInObjID) ? "1" : "0";
        mpTagPass->getProgram()->addDefines(tagDefines);
        mpTagPass->execute(pRenderContext, w, h);
    }
    // 출력 채널이 연결되어 있으면 같은 태그 맵을 복사해 내보낸다
    if (ref<Texture> pTagOut = renderData.getTexture(kOutTagMap))
        pRenderContext->copyResource(pTagOut.get(), mpTagMap.get());

    // ---- 2) 적 영역 맵 I_enemy (R-5.2). JFA 로 거리를 구한 뒤 감쇠를 준다 ----
    const float sigma = mEnemySigmaRatio * static_cast<float>(h);
    FieldCB fieldCB = {};
    fieldCB.frameDim[0] = w;
    fieldCB.frameDim[1] = h;
    fieldCB.sigmaPx     = sigma;
    {
        auto initVar = mpFieldInitPass->getRootVar();
        initVar["FieldCB"].setBlob(&fieldCB, sizeof(fieldCB));
        initVar["gTagIn"]   = mpTagMap;
        initVar["gSeedIn"]  = ref<Texture>();
        initVar["gSeedOut"] = mpSeedA;
        initVar["gEnemyRoi"] = ref<Texture>();
        mpFieldInitPass->execute(pRenderContext, w, h);
    }
    ref<Texture> pSeedIn = mpSeedA, pSeedOut = mpSeedB;
    for (const uint32_t step : jfaSteps(sigma))
    {
        ref<ComputePass> pStep = getJfaStepPass(step);
        auto stepVar = pStep->getRootVar();
        stepVar["FieldCB"].setBlob(&fieldCB, sizeof(fieldCB));
        stepVar["gTagIn"]    = ref<Texture>();
        stepVar["gSeedIn"]   = pSeedIn;
        stepVar["gSeedOut"]  = pSeedOut;
        stepVar["gEnemyRoi"] = ref<Texture>();
        pStep->execute(pRenderContext, w, h);
        std::swap(pSeedIn, pSeedOut);
    }
    {
        auto finalVar = mpFieldFinalPass->getRootVar();
        finalVar["FieldCB"].setBlob(&fieldCB, sizeof(fieldCB));
        finalVar["gTagIn"]   = mpTagMap;
        finalVar["gSeedIn"]  = pSeedIn;
        finalVar["gSeedOut"] = ref<Texture>();
        finalVar["gEnemyRoi"] = mpEnemyRoi;
        mpFieldFinalPass->execute(pRenderContext, w, h);
    }

    // ---- 3) 메인 중요도 ----
    auto var = mpComputePass->getRootVar();
    var["CB"].setBlob(&mCB, sizeof(mCB));
    var["gTagIn"]    = mpTagMap;
    var["gEnemyRoi"] = mpEnemyRoi;

    var["gDiffuseOpacity"] = renderData.getTexture(kInDiffuseOpacity);
    var["gGuideNormalW"]   = renderData.getTexture(kInGuideNormalW);
    var["gFaceNormalW"]    = renderData.getTexture(kInFaceNormalW);
    var["gSpecRough"]      = renderData.getTexture(kInSpecRough);
    var["gMotionVectorW"]  = renderData.getTexture(kInMotionVectorW);
    var["gLinearZ"]        = renderData.getTexture(kInLinearZ);
    var["gShadowCount"]    = renderData.getTexture(kInShadowCount);
    var["gLuminanceTex"]   = renderData.getTexture(kInLuminance);
    var["gCenterDist"]     = renderData.getTexture(kInCenterDist);
    var["gMetalness"]      = renderData.getTexture(kInMetalness);

    var["gImportance"]     = pImportance;
    var["gImportanceVis"]  = pImportanceVis;
    var["gEnemyStats"]     = mpEnemyStatsBuf;

    // Preprocessor defines driven by texture availability
    DefineList defines;
    auto addTexDef = [&](const char* name, ref<Texture> tex)
    {
        defines[name] = tex ? "1" : "0";
    };
    addTexDef("HAS_DIFFUSE_OPACITY", renderData.getTexture(kInDiffuseOpacity));
    addTexDef("HAS_GUIDE_NORMAL",    renderData.getTexture(kInGuideNormalW));
    addTexDef("HAS_FACE_NORMAL",     renderData.getTexture(kInFaceNormalW));
    addTexDef("HAS_SPEC_ROUGH",      renderData.getTexture(kInSpecRough));
    addTexDef("HAS_MOTION_VECTOR_W", renderData.getTexture(kInMotionVectorW));
    addTexDef("HAS_LINEAR_Z",        renderData.getTexture(kInLinearZ));
    addTexDef("HAS_SHADOW_COUNT",    renderData.getTexture(kInShadowCount));
    addTexDef("HAS_LUMINANCE",       renderData.getTexture(kInLuminance));
    addTexDef("HAS_CENTER_DIST",     renderData.getTexture(kInCenterDist));
    addTexDef("HAS_METALNESS",       renderData.getTexture(kInMetalness));
    defines["HAS_IMPORTANCE_VIS"] = pImportanceVis ? "1" : "0";
    mpComputePass->getProgram()->addDefines(defines);

    // Dispatch
    pRenderContext->clearUAV(mpEnemyStatsBuf->getUAV().get(), uint4(0));
    mpComputePass->execute(pRenderContext, w, h);

    // 적 통계를 비동기 readback 으로 넘긴다
    issueEnemyReadback(pRenderContext, w, h, time);
    mFrame++;
}

// ---------------------------------------------------------------------------
void ImportancePass::renderUI(Gui::Widgets& widget)
{
    static const Gui::DropdownList kDbgList = {
        { static_cast<uint32_t>(DebugView::Importance),     "Importance (grayscale)" },
        { static_cast<uint32_t>(DebugView::TaskOverlay),    "Task overlay" },
        { static_cast<uint32_t>(DebugView::ImageSpaceOnly), "Image-space only" },
        { static_cast<uint32_t>(DebugView::TaskObjectOnly), "Task-object only" },
    };

    uint32_t dv = static_cast<uint32_t>(mDebugView);
    if (widget.dropdown("Debug view", kDbgList, dv))
        mDebugView = static_cast<DebugView>(dv);

    const TaskSignals& sig = mClassifier.getSignals();
    widget.text(std::string("Active task: ") + toString(mClassifier.getTask()));
    widget.text(fmt::format("omega {:.1f} deg/s  v {:.2f} m/s  phi {:.1f} deg", sig.omega, sig.v, sig.phi));
    widget.text(fmt::format("enemy px {}  visible {}  center {}", sig.enemyPixels, sig.enemyVisible, sig.centerHit));
    widget.text(fmt::format("latency {} f  objectID {}", sig.enemyLatencyFrames, mObjectIdConnected ? "on" : "OFF"));
    widget.text(fmt::format("theta {:.2f} deg  dtheta/dt {:.1f} deg/s", sig.theta, sig.dThetaDt));
    widget.text(fmt::format("fire chunk {}  recent fire {}  ({})", sig.fireChunk, sig.recentFire,
        isCustomCameraActive() ? "left click = fire" : "fire input off"));
    widget.text("Task config " + mConfigStatus);
    if (widget.button("Reload task config")) loadConfig();
    widget.text(std::string("Identify: ") + (mIdentifyColor ? "color (compare only)" : "tag"));
    widget.var("Center radius (height units)", mCenterRadiusUV, 0.0f,  0.5f,  0.005f);
    widget.var("Task importance",    mTaskImportance,     0.0f,  1.0f,  0.01f);
    widget.var("Lum low",            mLumLow,             0.0f,  1.0f,  0.005f);
    widget.var("Lum high",           mLumHigh,            0.0f,  1.0f,  0.005f);
    widget.var("Motion A",           mMotionA,            0.0f,  5.0f,  0.01f);
    widget.var("Motion B",           mMotionB,            0.0f,  8.0f,  0.05f);
    widget.var("Motion C",           mMotionC,            0.0f,  2.0f,  0.01f);
    widget.var("Center weight min",  mCenterWeightMin,    0.0f,  1.0f,  0.01f);
    widget.var("Scan band half height", mBandHalfHeightUV,   0.0f,  0.5f,  0.005f);
    widget.var("Traverse path half width", mPathHalfWidthUV, 0.0f,  0.5f,  0.005f);
    widget.var("Flick corridor half width", mCorridorHalfWidthUV, 0.0f, 0.2f, 0.002f);
}

// ---------------------------------------------------------------------------
// Plugin.h 의 registerClass<BaseT, T>() 오버로드를 사용:
//   BaseT = RenderPass  (plugin base class, defines PluginInfo / PluginCreate)
//   T     = ImportancePass  (concrete class, provides kPluginType / kPluginInfo / create)
extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, ImportancePass>();
}
