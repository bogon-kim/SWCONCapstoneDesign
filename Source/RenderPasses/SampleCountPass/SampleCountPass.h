/***************************************************************************
 # SampleCountPass.h  (spec_TSK.md R-5.5)
 #
 # 배분 규칙 (SppAllocation.h 와 같다):
 #   B     = sTarget · 픽셀 수                      전체 예산
 #   w_p   = ε + I_p                                가중치
 #   x_p   = clamp(s · w_p, 1, maxSpp)              Σ x_p = B 가 되도록 s 를 정한다
 #   spp_p = clamp(floor(x_p) + δ_p, 1, maxSpp)     Σ spp_p = B (가능한 범위일 때)
 #
 # GPU 구성 (5 dispatch, SampleCountPass.cs.slang):
 #   histMain / solveMain / baseMain / deltaMain / finalMain
 # [디버그 출력] sampleCountVis: spp 를 파랑(1) → 빨강(maxSpp) 으로 보여 준다.
 **************************************************************************/
#pragma once
#include "Falcor.h"
#include "RenderGraph/RenderPass.h"
#include "RenderGraph/RenderPassHelpers.h"
#include "SppAllocation.h"
using namespace Falcor;

class SampleCountPass : public RenderPass
{
public:
    FALCOR_PLUGIN_CLASS(SampleCountPass, "SampleCountPass",
        "Outputs per-pixel sample count: total budget B distributed by importance (R-5.5).");

    static ref<SampleCountPass> create(ref<Device> pDevice, const Properties& props)
    {
        return make_ref<SampleCountPass>(pDevice, props);
    }

    SampleCountPass(ref<Device> pDevice, const Properties& props);

    virtual Properties           getProperties()  const override;
    virtual RenderPassReflection reflect(const CompileData& compileData) override;
    virtual void                 execute(RenderContext* pRenderContext, const RenderData& renderData) override;
    virtual void                 renderUI(Gui::Widgets& widget) override;

private:
    void parseProperties(const Properties& props);

    // 상수 버퍼 (SampleCountPass.cs.slang 의 CB 와 같아야 한다)
    struct alignas(16) SppCB
    {
        uint32_t frameDim[2];  //  0
        uint32_t budget;       //  8
        uint32_t maxSpp;       // 12
        float    epsilon;      // 16
        float    wMax;         // 20
        uint32_t _p0, _p1;     // 24, 28
    };  // 32 bytes

    // 설정 (속성·UI)
    uint32_t mSTarget    = 4u;    ///< 평균 spp. 예산 B = mSTarget · 픽셀 수
    uint32_t mMaxSamples = 16u;   ///< 픽셀당 최대 spp
    float    mEpsilon    = 0.1f;  ///< ε: 가중치 바닥값

    // 5 개의 compute 패스 (같은 셰이더 파일, 진입점만 다르다)
    ref<ComputePass> mpHistPass;
    ref<ComputePass> mpSolvePass;
    ref<ComputePass> mpBasePass;
    ref<ComputePass> mpDeltaPass;
    ref<ComputePass> mpFinalPass;

    // 공유 버퍼
    ref<Buffer> mpHist;   ///< uint × HIST_SIZE (가중치·소수 부분 히스토그램, Σbase)
    ref<Buffer> mpScale;  ///< float × 1 (s)
    ref<Buffer> mpDelta;  ///< int × 4 (mode, thr, rem, ticket)
};
