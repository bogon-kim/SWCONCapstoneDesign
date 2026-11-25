/***************************************************************************
 # SppAllocation.cpp  (spec_TSK.md R-5.5)
 #
 # 단계 (GPU 패스와 같은 순서):
 #   1) 가중치 히스토그램: w 를 kWeightBins 로 나눠 개수를 센다
 #   2) s 찾기: 히스토그램 bin 대표값으로 F(s) = Σ clamp(s·w, 1, maxSpp) 를 이분법으로 B 에 맞춘다
 #   3) 정수 부분: base = floor(x), 합 sumBase, 소수 부분 히스토그램(추가 가능 / 감소 가능 픽셀별)
 #   4) 보정: L = B − sumBase 를 소수 부분이 큰(또는 작은) 픽셀부터 ±1 로 나눠 준다
 ***************************************************************************/
#include "SppAllocation.h"

#include <algorithm>
#include <cmath>

namespace
{
uint32_t weightBin(float w, float wMax)
{
    const float t = w / wMax * static_cast<float>(kWeightBins);
    const uint32_t b = static_cast<uint32_t>(std::max(t, 0.f));
    return std::min(b, kWeightBins - 1);
}

uint32_t fracBin(float frac)
{
    const uint32_t b = static_cast<uint32_t>(std::max(frac * static_cast<float>(kFracBins), 0.f));
    return std::min(b, kFracBins - 1);
}
} // namespace

std::vector<uint32_t> allocateSpp(const std::vector<float>& importance, const SppParams& params)
{
    const size_t n = importance.size();
    std::vector<uint32_t> spp(n, 1u);
    if (n == 0)
        return spp;

    const float eps = params.epsilon;
    const float maxSpp = static_cast<float>(params.maxSpp);
    const float wMax = 1.f + eps;

    // ---- 1) 가중치 히스토그램 ----
    std::vector<double> wCount(kWeightBins, 0.0);
    std::vector<float> w(n);
    for (size_t i = 0; i < n; i++)
    {
        w[i] = eps + std::clamp(importance[i], 0.f, 1.f);
        wCount[weightBin(w[i], wMax)] += 1.0;
    }

    // ---- 2) s 찾기: F(s) = Σ count · clamp(s·rep, 1, maxSpp) 가 B 가 되도록 ----
    const double budget = static_cast<double>(params.budget);
    float s = 0.f;
    if (budget <= static_cast<double>(n))
    {
        s = 0.f;                       // 예산이 최소치 이하: 전부 1 spp (아래 보정에서 맞춘다)
    }
    else if (budget >= static_cast<double>(n) * maxSpp)
    {
        s = maxSpp / eps * 2.f;        // 예산이 최대치 이상: 전부 maxSpp (아래 보정에서 맞춘다)
    }
    else
    {
        auto F = [&](float sc) {
            double sum = 0.0;
            for (uint32_t b = 0; b < kWeightBins; b++)
            {
                if (wCount[b] == 0.0) continue;
                const float rep = (static_cast<float>(b) + 0.5f) / static_cast<float>(kWeightBins) * wMax;
                sum += wCount[b] * std::clamp(sc * rep, 1.f, maxSpp);
            }
            return sum;
        };
        float lo = 0.f, hi = maxSpp / eps;   // F(hi) = maxSpp · n ≥ B
        for (uint32_t it = 0; it < kBisectIters; it++)
        {
            const float mid = 0.5f * (lo + hi);
            if (F(mid) < budget) lo = mid;
            else                 hi = mid;
        }
        s = hi;
    }

    // ---- 3) 정수 부분과 소수 부분 히스토그램 ----
    std::vector<float> x(n);
    std::vector<uint32_t> base(n);
    std::vector<uint32_t> addHist(kFracBins, 0u), subHist(kFracBins, 0u);
    uint64_t sumBase = 0;
    for (size_t i = 0; i < n; i++)
    {
        x[i] = std::clamp(s * w[i], 1.f, maxSpp);
        base[i] = static_cast<uint32_t>(std::floor(x[i]));
        sumBase += base[i];
        const float frac = x[i] - static_cast<float>(base[i]);
        const uint32_t fb = fracBin(frac);
        if (base[i] < params.maxSpp) addHist[fb]++;   // +1 을 받을 수 있는 픽셀
        if (base[i] > 1u)            subHist[fb]++;   // −1 을 받을 수 있는 픽셀
    }

    // ---- 4) 보정: L = B − sumBase ----
    const int64_t L = static_cast<int64_t>(params.budget) - static_cast<int64_t>(sumBase);
    int mode = 0;          // +1: 올림, −1: 내림, 0: 없음
    int thr = 0;           // 기준 소수 bin
    int64_t rem = 0;       // thr bin 에서 실제로 보정할 픽셀 수
    if (L > 0)
    {
        mode = +1;
        int64_t cum = 0;
        thr = -1;          // 가능한 픽셀이 부족하면 -1: 전부 +1 (예산을 맞출 수 없는 경우)
        for (int t = static_cast<int>(kFracBins) - 1; t >= 0; t--)
        {
            if (cum + addHist[t] >= L)
            {
                thr = t;
                rem = L - cum;
                break;
            }
            cum += addHist[t];
        }
    }
    else if (L < 0)
    {
        mode = -1;
        const int64_t need = -L;
        int64_t cum = 0;
        thr = static_cast<int>(kFracBins); // 가능한 픽셀이 부족하면 전부 −1
        for (uint32_t t = 0; t < kFracBins; t++)
        {
            if (cum + subHist[t] >= need)
            {
                thr = static_cast<int>(t);
                rem = need - cum;
                break;
            }
            cum += subHist[t];
        }
    }

    // ---- 최종 spp ----
    int64_t ticket = 0;    // thr bin 에서 보정 받을 픽셀 순번 (GPU 에서는 atomic 카운터)
    for (size_t i = 0; i < n; i++)
    {
        int64_t v = base[i];
        const float frac = x[i] - static_cast<float>(base[i]);
        const int fb = static_cast<int>(fracBin(frac));
        if (mode > 0 && base[i] < params.maxSpp)
        {
            if (fb > thr) v += 1;
            else if (fb == thr && ticket++ < rem) v += 1;
        }
        else if (mode < 0 && base[i] > 1u)
        {
            if (fb < thr) v -= 1;
            else if (fb == thr && ticket++ < rem) v -= 1;
        }
        spp[i] = static_cast<uint32_t>(std::clamp<int64_t>(v, 1, params.maxSpp));
    }
    return spp;
}
