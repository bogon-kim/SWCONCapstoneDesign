/***************************************************************************
 # SampleCountPass.cpp  (spec_TSK.md R-5.5)
 #
 # [5-dispatch 구조] SampleCountPass.cs.slang 참조
 #   histMain -> solveMain -> baseMain -> deltaMain -> finalMain
 # 전체 예산 B = sTarget · 픽셀 수 를 가중치 w = ε + I 에 따라 나눈다. 픽셀당 1..maxSpp.
 # [디버그 출력] sampleCountVis: HAS_SAMPLE_COUNT_VIS 가 1 일 때만 쓴다.
 **************************************************************************/
#include "SampleCountPass.h"

namespace
{
    const std::string kShaderFile = "RenderPasses/SampleCountPass/SampleCountPass.cs.slang";

    // 채널 이름 (reflect() 와 일치해야 함)
    const std::string kInputImportance      = "importance";
    const std::string kOutputSampleCount    = "sampleCount";
    const std::string kOutputSampleCountVis = "sampleCountVis"; ///< [디버그 시각화]

    // Property 키
    const std::string kPropSTarget    = "sTarget";
    const std::string kPropMaxSamples = "maxSamples";
    const std::string kPropEpsilon    = "epsilon";
}

// ===========================================================================
// Construction / Properties
// ===========================================================================

SampleCountPass::SampleCountPass(ref<Device> pDevice, const Properties& props)
    : RenderPass(pDevice)
{
    parseProperties(props);

    auto makePass = [&](const char* entry)
    {
        ProgramDesc desc;
        desc.addShaderLibrary(kShaderFile).csEntry(entry);
        return ComputePass::create(mpDevice, desc);
    };
    mpHistPass  = makePass("histMain");
    mpSolvePass = makePass("solveMain");
    mpBasePass  = makePass("baseMain");
    mpDeltaPass = makePass("deltaMain");
    mpFinalPass = makePass("finalMain");

    // 버퍼 크기는 해상도와 무관하다 (히스토그램 bin 수 고정).
    mpHist = mpDevice->createStructuredBuffer(
        sizeof(uint32_t), kWeightBins + 2 * kFracBins + 1,
        ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource,
        MemoryType::DeviceLocal, nullptr, false);
    mpScale = mpDevice->createStructuredBuffer(
        sizeof(float), 1u,
        ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource,
        MemoryType::DeviceLocal, nullptr, false);
    mpDelta = mpDevice->createStructuredBuffer(
        sizeof(int32_t), 4u,
        ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource,
        MemoryType::DeviceLocal, nullptr, false);
}

void SampleCountPass::parseProperties(const Properties& props)
{
    for (const auto& [key, value] : props)
    {
        if      (key == kPropSTarget)    mSTarget    = value;
        else if (key == kPropMaxSamples) mMaxSamples = value;
        else if (key == kPropEpsilon)    mEpsilon    = value;
        else logWarning("SampleCountPass: Unknown property '{}'.", key);
    }
}

Properties SampleCountPass::getProperties() const
{
    Properties props;
    props[kPropSTarget]    = mSTarget;
    props[kPropMaxSamples] = mMaxSamples;
    props[kPropEpsilon]    = mEpsilon;
    return props;
}

// ===========================================================================
// Reflect
// ===========================================================================

RenderPassReflection SampleCountPass::reflect(const CompileData& compileData)
{
    RenderPassReflection reflector;

    // ImportancePass 의 R32Float importance 텍스처 입력.
    reflector.addInput(kInputImportance, "Per-pixel importance [0,1] (R32Float)")
        .format(ResourceFormat::R32Float);

    // R8Uint sampleCount 출력 — PathTracer 가 Texture2D<uint> 로 읽는다.
    reflector.addOutput(kOutputSampleCount, "Per-pixel sample count (R8Uint)")
        .format(ResourceFormat::R8Uint)
        .bindFlags(ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource);

    // [디버그 시각화] spp 를 히트맵으로 나타낸 RGBA8Unorm 텍스처.
    reflector.addOutput(kOutputSampleCountVis, "Sample count visualization (RGBA8Unorm)")
        .format(ResourceFormat::RGBA8Unorm)
        .bindFlags(ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource);

    return reflector;
}

// ===========================================================================
// Execute  —  5-dispatch
// ===========================================================================

void SampleCountPass::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    ref<Texture> pImportance     = renderData.getTexture(kInputImportance);
    ref<Texture> pOutput         = renderData.getTexture(kOutputSampleCount);
    ref<Texture> pSampleCountVis = renderData.getTexture(kOutputSampleCountVis);
    FALCOR_ASSERT(pImportance && pOutput);

    const uint32_t w = pOutput->getWidth();
    const uint32_t h = pOutput->getHeight();

    SppCB cb = {};
    cb.frameDim[0] = w;
    cb.frameDim[1] = h;
    cb.budget      = mSTarget * w * h;          // B
    cb.maxSpp      = mMaxSamples;
    cb.epsilon     = mEpsilon;
    cb.wMax        = 1.f + mEpsilon;

    // 히스토그램과 보정 카운터는 매 프레임 0 으로 시작한다.
    pRenderContext->clearUAV(mpHist->getUAV().get(), uint4(0));
    pRenderContext->clearUAV(mpDelta->getUAV().get(), uint4(0));

    // ---- 1) 가중치 히스토그램 ----
    {
        auto var = mpHistPass->getRootVar();
        var["CB"].setBlob(&cb, sizeof(cb));
        var["gImportance"] = pImportance;
        var["gHist"]       = mpHist;
        mpHistPass->execute(pRenderContext, w, h);
    }
    pRenderContext->uavBarrier(mpHist.get());

    // ---- 2) s 찾기 (1 스레드) ----
    {
        auto var = mpSolvePass->getRootVar();
        var["CB"].setBlob(&cb, sizeof(cb));
        var["gHist"]  = mpHist;
        var["gScale"] = mpScale;
        mpSolvePass->execute(pRenderContext, 1u, 1u);
    }
    pRenderContext->uavBarrier(mpScale.get());

    // ---- 3) 정수 부분 base 와 소수 부분 히스토그램 ----
    {
        auto var = mpBasePass->getRootVar();
        var["CB"].setBlob(&cb, sizeof(cb));
        var["gImportance"] = pImportance;
        var["gOutput"]     = pOutput;
        var["gHist"]       = mpHist;
        var["gScale"]      = mpScale;
        mpBasePass->execute(pRenderContext, w, h);
    }
    pRenderContext->uavBarrier(mpHist.get());
    pRenderContext->uavBarrier(pOutput.get());

    // ---- 4) 보정 기준 (1 스레드) ----
    {
        auto var = mpDeltaPass->getRootVar();
        var["CB"].setBlob(&cb, sizeof(cb));
        var["gHist"]  = mpHist;
        var["gDelta"] = mpDelta;
        mpDeltaPass->execute(pRenderContext, 1u, 1u);
    }
    pRenderContext->uavBarrier(mpDelta.get());

    // ---- 5) 최종 spp ----
    mpFinalPass->getProgram()->addDefine("HAS_SAMPLE_COUNT_VIS", pSampleCountVis ? "1" : "0");
    {
        auto var = mpFinalPass->getRootVar();
        var["CB"].setBlob(&cb, sizeof(cb));
        var["gImportance"]     = pImportance;
        var["gOutput"]         = pOutput;
        var["gScale"]          = mpScale;
        var["gDelta"]          = mpDelta;
        var["gSampleCountVis"] = pSampleCountVis; ///< nullptr 도 허용 (셰이더에서 guard)
        mpFinalPass->execute(pRenderContext, w, h);
    }
}

// ===========================================================================
// UI
// ===========================================================================

void SampleCountPass::renderUI(Gui::Widgets& widget)
{
    // sTarget: 평균 spp. 전체 예산 B = sTarget · 픽셀 수.
    int sTargetInt = static_cast<int>(mSTarget);
    if (widget.var("Average spp (sTarget)", sTargetInt, 1, static_cast<int>(mMaxSamples), 1))
        mSTarget = static_cast<uint32_t>(sTargetInt);
    widget.tooltip("Average spp over all pixels. Total budget B = sTarget * pixel count, met exactly.");

    // maxSamples: 픽셀당 최대 spp (기본 16).
    int maxSamplesInt = static_cast<int>(mMaxSamples);
    if (widget.var("Max spp (maxSamples)", maxSamplesInt, 1, 256, 1))
        mMaxSamples = static_cast<uint32_t>(maxSamplesInt);
    widget.tooltip("Hard upper bound per pixel. Must be <= PathTracer's maxSamplesPerPixel.");

    // epsilon: 가중치 바닥값 w = ε + I.
    widget.var("Weight floor (epsilon)", mEpsilon, 0.f, 1.f, 0.01f);
    widget.tooltip("w = epsilon + importance. Larger values flatten the distribution.");
}

extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, SampleCountPass>();
}
