/***************************************************************************
 # EnemyField.cpp  (spec_TSK.md R-5.2 ~ R-5.4)
 ***************************************************************************/
#include "EnemyField.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
struct Seed
{
    int32_t x = -1; // -1 이면 seed 없음
    int32_t y = -1;
    bool valid() const { return x >= 0; }
};

// 픽셀 (px, py) 에서 seed 까지 거리 제곱. seed 가 없으면 아주 큰 값.
int64_t dist2(int32_t px, int32_t py, const Seed& s)
{
    if (!s.valid())
        return std::numeric_limits<int64_t>::max();
    const int64_t dx = px - s.x;
    const int64_t dy = py - s.y;
    return dx * dx + dy * dy;
}
} // namespace

float enemyFalloff(bool inside, float d, float sigma)
{
    if (inside)
        return 1.f;
    if (!std::isfinite(d))
        return 0.f;
    if (sigma <= 0.f)
        return d <= 0.f ? 0.5f : 0.f;
    const float r = d / sigma;
    return 0.5f * std::exp(-r * r);
}

std::vector<uint32_t> jfaSteps(float sigmaPx)
{
    const float reach = 3.f * std::max(sigmaPx, 0.f);
    uint32_t maxStep = 1;
    while (static_cast<float>(maxStep) < reach)
        maxStep <<= 1;

    std::vector<uint32_t> steps;
    for (uint32_t s = maxStep; s >= 1; s >>= 1)
        steps.push_back(s);
    return steps;
}

std::vector<float> jfaDistance(const std::vector<uint8_t>& mask, uint32_t w, uint32_t h, const std::vector<uint32_t>& steps)
{
    const size_t count = static_cast<size_t>(w) * h;
    std::vector<Seed> a(count), b(count);
    for (uint32_t y = 0; y < h; y++)
    {
        for (uint32_t x = 0; x < w; x++)
        {
            const size_t i = static_cast<size_t>(y) * w + x;
            if (mask[i])
            {
                a[i].x = static_cast<int32_t>(x);
                a[i].y = static_cast<int32_t>(y);
            }
        }
    }

    for (const uint32_t s : steps)
    {
        const int32_t step = static_cast<int32_t>(s);
        for (uint32_t y = 0; y < h; y++)
        {
            for (uint32_t x = 0; x < w; x++)
            {
                const size_t i = static_cast<size_t>(y) * w + x;
                Seed best = a[i];
                int64_t bestD = dist2(static_cast<int32_t>(x), static_cast<int32_t>(y), best);
                for (int32_t dy = -1; dy <= 1; dy++)
                {
                    for (int32_t dx = -1; dx <= 1; dx++)
                    {
                        const int32_t qx = static_cast<int32_t>(x) + dx * step;
                        const int32_t qy = static_cast<int32_t>(y) + dy * step;
                        if (qx < 0 || qy < 0 || qx >= static_cast<int32_t>(w) || qy >= static_cast<int32_t>(h))
                            continue;
                        const Seed& cand = a[static_cast<size_t>(qy) * w + qx];
                        const int64_t d = dist2(static_cast<int32_t>(x), static_cast<int32_t>(y), cand);
                        if (d < bestD)
                        {
                            bestD = d;
                            best = cand;
                        }
                    }
                }
                b[i] = best;
            }
        }
        std::swap(a, b);
    }

    std::vector<float> distance(count, std::numeric_limits<float>::infinity());
    for (uint32_t y = 0; y < h; y++)
    {
        for (uint32_t x = 0; x < w; x++)
        {
            const size_t i = static_cast<size_t>(y) * w + x;
            const int64_t d = dist2(static_cast<int32_t>(x), static_cast<int32_t>(y), a[i]);
            if (d != std::numeric_limits<int64_t>::max())
                distance[i] = std::sqrt(static_cast<float>(d));
        }
    }
    return distance;
}

float updateAlphaRamp(float alpha, bool enemyVisible, float dtSec, const AlphaRampParams& params)
{
    const float target = enemyVisible ? params.alphaMax : 0.f;
    if (params.rampSec <= 0.f || dtSec <= 0.f)
        return target;

    const float step = params.alphaMax * dtSec / params.rampSec;
    if (alpha < target)
        return std::min(target, alpha + step);
    return std::max(target, alpha - step);
}
