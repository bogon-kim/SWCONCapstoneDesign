#pragma once
/***************************************************************************
 # SppAllocation.h  (spec_TSK.md R-5.5)
 # 픽셀별 spp 배분의 CPU 기준 구현. Falcor 의존성이 없다.
 # SampleCountPass.cs.slang 과 같은 알고리즘이며, 테스트는 이 기준으로 검사한다.
 #
 #   w_p   = ε + I_p                       가중치 (I_p 는 [0,1] 로 자른다)
 #   x_p   = clamp(s · w_p, 1, maxSpp)     s 는 Σ x_p = B 가 되도록 정한다
 #   spp_p = clamp(floor(x_p) + δ_p, 1, maxSpp)
 #           δ_p ∈ {−1, 0, +1} 는 정수 부분의 합을 B 에 맞추는 보정이다
 #   Σ_p spp_p = B                         (B 가 가능한 범위일 때)
 ***************************************************************************/
#include <cstdint>
#include <vector>

// 가중치 히스토그램 bin 수 (s 를 찾을 때 쓴다)
constexpr uint32_t kWeightBins = 1024;
// 소수 부분 히스토그램 bin 수 (δ 를 고를 때 쓴다)
constexpr uint32_t kFracBins = 256;
// s 를 찾는 이분법 반복 횟수
constexpr uint32_t kBisectIters = 32;

struct SppParams
{
    uint32_t budget = 0;   // B: 전체 샘플 예산 (= sTarget · 픽셀 수)
    float    epsilon = 0.1f; // ε: 가중치 바닥값
    uint32_t maxSpp = 16;  // 픽셀당 최대 spp
};

// 픽셀 중요도 I_p (0..1) -> 픽셀별 spp
std::vector<uint32_t> allocateSpp(const std::vector<float>& importance, const SppParams& params);
