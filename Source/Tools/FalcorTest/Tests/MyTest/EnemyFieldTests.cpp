#include "Testing/UnitTest.h"

// EnemyField 는 ImportancePass 플러그인 DLL 안에 있어 링크할 수 없으므로 소스를 직접 포함한다.
#include "../../../../RenderPasses/ImportancePass/EnemyField.cpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace Falcor
{
namespace
{
// w x h 격자에 중심 (cx, cy), 반지름 r 인 원을 적 마스크로 만든다
std::vector<uint8_t> diskMask(uint32_t w, uint32_t h, float cx, float cy, float r)
{
    std::vector<uint8_t> mask(static_cast<size_t>(w) * h, 0);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
        {
            const float dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy <= r * r)
                mask[static_cast<size_t>(y) * w + x] = 1;
        }
    return mask;
}

// 검사 기준: 모든 마스크 픽셀에 대한 최소 거리
float bruteDistance(const std::vector<uint8_t>& mask, uint32_t w, uint32_t h, uint32_t x, uint32_t y)
{
    float best = std::numeric_limits<float>::infinity();
    for (uint32_t j = 0; j < h; j++)
        for (uint32_t i = 0; i < w; i++)
            if (mask[static_cast<size_t>(j) * w + i])
            {
                const float dx = float(i) - float(x), dy = float(j) - float(y);
                best = std::min(best, std::sqrt(dx * dx + dy * dy));
            }
    return best;
}
} // namespace

// R-5.2: 마스크 안 1, 경계 바깥 0.5, σ 에서 0.5·e^-1, 멀면 0
CPU_TEST(EnemyField_FalloffShape)
{
    EXPECT(enemyFalloff(true, 0.f, 10.f) == 1.f);
    EXPECT(std::fabs(enemyFalloff(false, 0.f, 10.f) - 0.5f) < 1e-6f);           // 경계 바로 바깥
    EXPECT(std::fabs(enemyFalloff(false, 10.f, 10.f) - 0.5f * std::exp(-1.f)) < 1e-6f);
    EXPECT(enemyFalloff(false, 100.f, 10.f) < 1e-6f);                          // 멀면 0
    EXPECT(enemyFalloff(false, std::numeric_limits<float>::infinity(), 10.f) == 0.f);
}

// 경계에서 0 으로 떨어지지 않는다 (명세 R-5.2 의 하한 0.5)
CPU_TEST(EnemyField_EdgeKeepsHalfWeight)
{
    EXPECT(enemyFalloff(false, 1.f, 10.f) > 0.45f);
}

CPU_TEST(EnemyField_JfaStepsCoverThreeSigma)
{
    const std::vector<uint32_t> s = jfaSteps(10.8f); // 3σ = 32.4 -> 최대 단계 64
    EXPECT(s.size() == 7);
    EXPECT(s.front() == 64u);
    EXPECT(s.back() == 1u);
    for (size_t i = 1; i < s.size(); i++)
        EXPECT(s[i] * 2 == s[i - 1]);

    const std::vector<uint32_t> tiny = jfaSteps(0.f);
    EXPECT(tiny.size() == 1 && tiny[0] == 1u);
}

CPU_TEST(EnemyField_JfaNoEnemyIsInfinite)
{
    const std::vector<uint8_t> empty(32 * 24, 0);
    const std::vector<float> d = jfaDistance(empty, 32, 24, jfaSteps(4.f));
    for (float v : d)
        EXPECT(std::isinf(v));
}

// JFA 거리는 전수 탐색과 거의 같아야 한다. 감쇠가 의미 있는 3σ 안에서 오차 1px 이하를 기준으로 한다.
CPU_TEST(EnemyField_JfaMatchesBruteForceNearEnemy)
{
    const uint32_t w = 64, h = 48;
    const float sigma = 4.f;
    const std::vector<uint8_t> mask = diskMask(w, h, 30.f, 22.f, 6.f);
    const std::vector<float> jfa = jfaDistance(mask, w, h, jfaSteps(sigma));

    float worst = 0.f;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
        {
            const float exact = bruteDistance(mask, w, h, x, y);
            if (exact > 3.f * sigma)
                continue;
            worst = std::max(worst, std::fabs(jfa[static_cast<size_t>(y) * w + x] - exact));
        }
    EXPECT(worst <= 1.f);
}

// R-5.4: 벽시계 기준. 60 fps 12프레임과 30 fps 6프레임은 같은 0.2초이므로 같은 α 가 나온다.
CPU_TEST(EnemyField_AlphaRampIsWallClock)
{
    const AlphaRampParams p; // alphaMax 0.4, rampSec 0.2
    float a60 = 0.f, a30 = 0.f;
    for (int i = 0; i < 12; i++)
        a60 = updateAlphaRamp(a60, true, 1.f / 60.f, p);
    for (int i = 0; i < 6; i++)
        a30 = updateAlphaRamp(a30, true, 1.f / 30.f, p);
    EXPECT(std::fabs(a60 - 0.4f) < 1e-4f);
    EXPECT(std::fabs(a30 - 0.4f) < 1e-4f);
}

CPU_TEST(EnemyField_AlphaRampRisesThenFalls)
{
    const AlphaRampParams p;
    float a = 0.f;
    a = updateAlphaRamp(a, true, 0.1f, p);
    EXPECT(std::fabs(a - 0.2f) < 1e-4f);      // 0.1초 = 절반
    a = updateAlphaRamp(a, true, 0.5f, p);
    EXPECT(std::fabs(a - 0.4f) < 1e-6f);      // 넘지 않는다
    a = updateAlphaRamp(a, false, 0.1f, p);
    EXPECT(std::fabs(a - 0.2f) < 1e-4f);      // 같은 시간으로 내린다
    a = updateAlphaRamp(a, false, 0.5f, p);
    EXPECT(a == 0.f);                          // 0 아래로 내려가지 않는다
}
} // namespace Falcor
