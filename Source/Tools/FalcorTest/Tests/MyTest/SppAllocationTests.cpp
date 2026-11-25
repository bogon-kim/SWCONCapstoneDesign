#include "Testing/UnitTest.h"

// SppAllocation 은 SampleCountPass 플러그인 DLL 안에 있어 링크할 수 없으므로 소스를 직접 포함한다.
#include "../../../../RenderPasses/SampleCountPass/SppAllocation.cpp"

#include <cstdlib>
#include <numeric>
#include <vector>

namespace Falcor
{
namespace
{
uint64_t total(const std::vector<uint32_t>& v)
{
    return std::accumulate(v.begin(), v.end(), uint64_t(0));
}

// 결정적 난수 (테스트 재현성)
float lcg(uint32_t& state)
{
    state = state * 1664525u + 1013904223u;
    return static_cast<float>(state >> 8) / static_cast<float>(1u << 24);
}
} // namespace

// 명세 예: 중요도 1 픽셀과 0 픽셀, 예산 8 -> 7 과 1 (w = 1.1 : 0.1)
CPU_TEST(SppAllocation_TwoPixelExample)
{
    SppParams p;
    p.budget = 8;
    const std::vector<uint32_t> spp = allocateSpp({1.f, 0.f}, p);
    EXPECT(spp.size() == 2);
    EXPECT(spp[0] == 7u);
    EXPECT(spp[1] == 1u);
    EXPECT(total(spp) == 8u);
}

// 전체 예산을 정확히 맞춘다 (R-5.5). 무작위 중요도로 여러 예산을 검사한다.
CPU_TEST(SppAllocation_TotalMatchesBudget)
{
    uint32_t seed = 12345u;
    std::vector<float> imp(3000);
    for (auto& v : imp)
        v = lcg(seed);
    for (uint32_t budgetPerPixel : {1u, 2u, 4u, 8u, 15u})
    {
        SppParams p;
        p.budget = budgetPerPixel * static_cast<uint32_t>(imp.size());
        const std::vector<uint32_t> spp = allocateSpp(imp, p);
        EXPECT(total(spp) == p.budget);
    }
}

// 픽셀당 상한 16, 하한 1 을 지킨다. 중요도가 극단적이어도 마찬가지다.
CPU_TEST(SppAllocation_BoundsAreRespected)
{
    std::vector<float> imp(500, 0.f);
    imp[0] = 1.f;
    imp[1] = 1.f;
    SppParams p;
    p.budget = 4 * 500;
    const std::vector<uint32_t> spp = allocateSpp(imp, p);
    for (uint32_t v : spp)
    {
        EXPECT(v >= 1u);
        EXPECT(v <= 16u);
    }
    EXPECT(total(spp) == p.budget);
}

// 상한에 걸린 픽셀은 16 이고, 나머지는 남은 예산을 나눠 받는다.
CPU_TEST(SppAllocation_CapRedistributesBudget)
{
    std::vector<float> imp(10, 0.f);
    imp[0] = 1.f;
    SppParams p;
    p.budget = 40;
    const std::vector<uint32_t> spp = allocateSpp(imp, p);
    EXPECT(spp[0] == 16u);
    EXPECT(total(spp) == 40u);
}

// 예산이 최소·최대와 같으면 모든 픽셀이 같은 값을 받는다.
CPU_TEST(SppAllocation_BudgetAtLimits)
{
    std::vector<float> imp = {0.2f, 0.9f, 0.5f, 0.0f};
    SppParams p;
    p.budget = 4;                        // 최소 (픽셀당 1)
    std::vector<uint32_t> spp = allocateSpp(imp, p);
    for (uint32_t v : spp) EXPECT(v == 1u);

    p.budget = 4 * 16;                   // 최대 (픽셀당 16)
    spp = allocateSpp(imp, p);
    for (uint32_t v : spp) EXPECT(v == 16u);
}

// 중요도가 커지면 spp 가 줄지 않는다 (ε 와 보정이 순서를 뒤집지 않는다).
CPU_TEST(SppAllocation_MonotoneInImportance)
{
    uint32_t seed = 777u;
    std::vector<float> imp(2000);
    for (auto& v : imp)
        v = lcg(seed);
    SppParams p;
    p.budget = 4 * 2000;
    const std::vector<uint32_t> spp = allocateSpp(imp, p);

    for (size_t i = 0; i < imp.size(); i++)
        for (size_t j = 0; j < imp.size(); j++)
            if (imp[i] > imp[j] + 0.01f)
                EXPECT(spp[i] + 1 >= spp[j]); // 중요도 차이가 bin 보다 큰 쌍
}

// 바닥값 ε: 중요도 0 픽셀도 1 spp 이상을 받고, 중요도 1 픽셀보다 적게 받는다.
CPU_TEST(SppAllocation_EpsilonKeepsFloor)
{
    std::vector<float> imp(100, 0.5f);
    imp[0] = 0.f;
    imp[1] = 1.f;
    SppParams p;
    p.budget = 4 * 100;
    const std::vector<uint32_t> spp = allocateSpp(imp, p);
    EXPECT(spp[0] >= 1u);
    EXPECT(spp[0] < spp[1]);
    EXPECT(total(spp) == p.budget);
}
} // namespace Falcor
