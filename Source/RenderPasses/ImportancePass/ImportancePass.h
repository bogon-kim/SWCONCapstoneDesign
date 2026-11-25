#pragma once
#include "Falcor.h"
#include "RenderGraph/RenderPass.h"
#include "RenderGraph/RenderPassHelpers.h"
#include "Core/API/Buffer.h"
#include "Core/API/Fence.h"
#include "TaskClassifier.h"
#include "TagMapping.h"
#include "EnemyField.h"
#include <array>
#include <map>
#include <chrono>
#include <cmath>
#include <cstring>

using namespace Falcor;

class ImportancePass : public RenderPass
{
public:
    FALCOR_PLUGIN_CLASS(ImportancePass, "ImportancePass",
        {"Computes per-pixel importance from G-buffer + user task state."});

    static ref<ImportancePass> create(ref<Device> pDevice, const Properties& props)
    {
        return make_ref<ImportancePass>(pDevice, props);
    }

    ImportancePass(ref<Device> pDevice, const Properties& props);

    enum class DebugView : uint32_t
    {
        Importance     = 0,
        TaskOverlay    = 1,
        ImageSpaceOnly = 2,
        TaskObjectOnly = 3,
    };

    // GPU constant buffer layout — must match cbuffer CB in the slang shader exactly.
    // All vec2 fields are flattened to float[2] to avoid Falcor/GLM type dependencies.
    struct alignas(16) ImportanceCB
    {
        uint32_t frameDim[2];        //  0
        float    invFrameDim[2];     //  8
        uint32_t activeTask;         // 16
        uint32_t debugView;          // 20
        uint32_t hasImportanceVis;   // 24
        uint32_t _pad0;              // 28
        float    centerUV[2];        // 32
        float    centerRadiusUV;     // 40
        float    taskImportance;     // 44
        float    enemyRedThreshold;  // 48
        float    enemyRedDominance;  // 52
        float    lumLow;             // 56
        float    lumHigh;            // 60
        float    motionA;            // 64
        float    motionB;            // 68
        float    motionC;            // 72
        float    centerWeightMin;    // 76
        float    pathHalfWidthUV;    // 80  Traverse 경로 반너비 (높이 단위)
        float    enemyGreenMaxRatio; // 84
        float    _pad2;              // 88
        float    _pad3;              // 92
        float    enemyBBox[4];       // 96  적 바운딩 박스 x0 y0 x1 y1 (UV)
        float    enemyCentroidUV[2]; // 112 적 중심 (UV)
        uint32_t enemyValid;         // 120
        uint32_t identifyMode;       // 124 0 태그, 1 색상
        float    scanDirX;           // 128 회전 방향 -1..+1
        float    bandHalfHeightUV;   // 132 Scan 띠 반높이
        float    corridorHalfWidthUV; // 136 Flick 회랑 반너비
        uint32_t instanceCount;      // 140 태그 표 항목 수
        float    enemyAlpha;         // 144 R-5.4 α (램프 적용 값)
        float    _pad4;              // 148
        float    _pad5;              // 152
        float    _pad6;              // 156
    };  // total = 160 bytes, multiple of 16

    // 적 영역 맵 상수 버퍼 (EnemyField.cs.slang 의 FieldCB 와 같아야 한다)
    struct alignas(16) FieldCB
    {
        uint32_t frameDim[2];  //  0
        float    sigmaPx;      //  8
        uint32_t _pad;         // 12
    };  // total = 16 bytes

    // 적 마스크 통계 개수: [0] 픽셀 수, [1][2] 좌표 합, [3] 조준점 적중, [4..7] 바운딩 박스
    static constexpr uint32_t kEnemyStatCount = 8;

    // RenderPass interface
    virtual Properties           getProperties()   const override;
    virtual RenderPassReflection reflect(const CompileData& compileData) override;
    virtual void                 execute(RenderContext* pRenderContext, const RenderData& renderData) override;
    virtual void                 renderUI(Gui::Widgets& widget) override;
    virtual bool                 onKeyEvent(const KeyboardEvent& keyEvent) override;
    virtual bool                 onMouseEvent(const MouseEvent& mouseEvent) override;

    virtual void setScene(RenderContext* pRenderContext, const ref<Scene>& pScene) override
    {
        mpScene = pScene;
        mTagTableDirty = true;   // 인스턴스 태그 표를 다시 만든다 (R-2.2)
    }

private:
    // 적 마스크 통계 readback 슬롯. GPU 가 같은 슬롯을 쓰는 중에는 map() 하지 않는다 (R-4.7).
    struct ReadbackSlot
    {
        ref<Buffer> pStaging;          // ReadBack, uint32 x kEnemyStatCount
        uint64_t    fenceValue = 0;    // fence 가 이 값 이상이면 복사 완료
        bool        pending    = false;
        EnemySample meta;              // 복사를 건 프레임의 번호·시각·조준점·초점거리
        uint32_t    frameW = 1;        // 바운딩 박스 UV 변환용 프레임 크기
        uint32_t    frameH = 1;
    };
    static constexpr uint32_t kReadbackSlots = 3;

    void loadConfig();
    void rebuildInstanceTags();
    void ensureFieldTextures(uint32_t width, uint32_t height);
    ref<ComputePass> getJfaStepPass(uint32_t step);
    FrameInput gatherFrameInput(double time, float dt);
    bool pollEnemyReadback(EnemySample& sample);
    void issueEnemyReadback(RenderContext* pRenderContext, uint32_t width, uint32_t height, double time);
    bool isCustomCameraActive() const;

    ref<ComputePass> mpComputePass;
    ref<Scene>       mpScene;

    // Task 판정 (R-1 ~ R-4)
    TaskClassifier        mClassifier;
    std::filesystem::path mConfigPath;
    std::string           mConfigStatus;
    uint64_t              mFrame = 0;

    // Input state (CPU 측 훅만, 렌더링 경로를 막지 않는다)
    bool    mKeyW = false, mKeyA = false, mKeyS = false, mKeyD = false;
    bool    mFirePressed = false;   // left click fire (custom camera mode only), consumed by the next classification

    // Camera change detection
    float4x4 mPrevViewProj = float4x4::identity();

    // Timing
    std::chrono::steady_clock::time_point mStartTick;
    std::chrono::steady_clock::time_point mLastTick;
    bool mFirstTick = true;

    // GPU resources
    ImportanceCB mCB           = {};
    ref<Buffer>  mpEnemyStatsBuf;     // uint32 x kEnemyStatCount, UAV (GPU 가 적 마스크 통계를 누적)
    std::array<ReadbackSlot, kReadbackSlots> mReadback;
    uint32_t     mNextSlot = 0;
    ref<Fence>   mpReadbackFence;
    EnemySample  mLastEnemy;           // 가장 최근에 readback 된 적 통계 (ROI 바운딩 박스용)

    // 객체 태그 (R-2)
    ref<Buffer>    mpInstanceTagBuf;   // uint32 per geometry instance, StructuredBuffer
    uint32_t       mInstanceCount = 0;
    ref<ComputePass> mpTagPass;        // TagPass.cs.slang: 태그 맵 (먼저 실행)
    ref<ComputePass> mpFieldInitPass;  // EnemyField.cs.slang: JFA 초기화
    ref<ComputePass> mpFieldFinalPass; // EnemyField.cs.slang: I_enemy
    std::map<uint32_t, ref<ComputePass>> mJfaStepPasses; // JFA 단계별 (define JFA_STEP)

    // 적 영역 맵 (R-5.2 ~ R-5.4)
    ref<Texture> mpTagMap;             // R8Uint, 픽셀 태그
    ref<Texture> mpSeedA, mpSeedB;     // RG32Uint, JFA ping-pong
    ref<Texture> mpEnemyRoi;           // R32Float, I_enemy
    uint32_t     mFieldW = 0, mFieldH = 0;
    float        mEnemyAlpha = 0.f;    // 램프 상태 (R-5.4)
    float        mEnemySigmaRatio = 0.01f;   // σ / 화면 높이 (R-5.2)
    AlphaRampParams mAlphaParams;             // alphaMax, rampSec (R-5.3, R-5.4)
    bool           mTagTableDirty = true;
    MaterialTagMap mMaterialTags;      // TaskConfig.json "tags" 에서 읽는다
    bool           mIdentifyColor = false; // TaskConfig.json "identifyMode" == "color" (비교용)
    bool           mObjectIdConnected = false; // 그래프에서 objectID 가 연결되었는지 (UI 표시용)

    // UI-tweakable parameters
    DebugView mDebugView     = DebugView::Importance;
    float mTaskImportance    = 0.90f;
    float mCenterRadiusUV    = 0.20f;
    float mLumLow            = 0.15f;
    float mLumHigh           = 0.90f;
    float mMotionA           = 1.05f;
    float mMotionB           = 3.10f;
    float mMotionC           = 0.35f;
    float mCenterWeightMin   = 0.50f;
    float mBandHalfHeightUV  = 0.15f;  // Scan 띠 반높이 (높이 단위)
    float mPathHalfWidthUV   = 0.15f;  // Traverse 경로 반너비 (높이 단위)
    float mCorridorHalfWidthUV = 0.04f; // Flick 회랑 반너비 (높이 단위)

    static constexpr uint32_t kHistBins = 8;
    float mHistogram[kHistBins] = {};
};

static_assert(sizeof(ImportancePass::ImportanceCB) == 160,
    "ImportanceCB size mismatch -- check padding");
static_assert(sizeof(ImportancePass::ImportanceCB) % 16 == 0,
    "ImportanceCB must be 16-byte aligned");
