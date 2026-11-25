#include "Testing/UnitTest.h"

// TagMapping 은 ImportancePass 플러그인 DLL 안에 있어 링크할 수 없으므로 소스를 직접 포함한다 (TaskClassifierTests 와 같은 방식).
#include "../../../../RenderPasses/ImportancePass/TagMapping.cpp"

#include <algorithm>
#include <string>
#include <vector>

namespace Falcor
{
namespace
{
// FPS.pyscene 의 머티리얼 이름과 같은 형태의 매핑
std::map<std::string, std::vector<std::string>> fpsClassConfig()
{
    return {
        {"Enemy", {"Enemy"}},
        {"Item",  {"Item"}},
        {"Wall",  {"Wall", "Brick"}},
        {"Floor", {"Floor"}},
    };
}
} // namespace

CPU_TEST(TagMapping_ParsesOnlyKnownClassNames)
{
    ObjectTag tag = ObjectTag::None;
    EXPECT(parseObjectTag("Enemy", tag) && tag == ObjectTag::Enemy);
    EXPECT(parseObjectTag("Item", tag) && tag == ObjectTag::Item);
    EXPECT(parseObjectTag("Wall", tag) && tag == ObjectTag::Wall);
    EXPECT(parseObjectTag("Floor", tag) && tag == ObjectTag::Floor);
    EXPECT(!parseObjectTag("None", tag));  // None 은 설정 항목이 아니다
    EXPECT(!parseObjectTag("Cube", tag));
    EXPECT(!parseObjectTag("enemy", tag)); // 대소문자를 구분한다
}

CPU_TEST(TagMapping_BuildsMaterialMapAndReportsUnknownClasses)
{
    auto config = fpsClassConfig();
    config["Crate"] = {"Crate"};  // 알 수 없는 클래스

    std::vector<std::string> unknown;
    const MaterialTagMap map = buildMaterialTagMap(config, &unknown);

    EXPECT(map.at("Enemy") == ObjectTag::Enemy);
    EXPECT(map.at("Brick") == ObjectTag::Wall);  // 한 클래스에 여러 머티리얼 이름
    EXPECT(map.count("Crate") == 0);
    EXPECT(unknown.size() == 1 && unknown[0] == "Crate");
}

CPU_TEST(TagMapping_UnmappedMaterialIsNoneAndReportedOnce)
{
    const MaterialTagMap map = buildMaterialTagMap(fpsClassConfig(), nullptr);
    const std::vector<std::string> names = {"Enemy", "Cube", "", "Cube", "Floor", "Sphere"};

    std::vector<std::string> unmapped;
    const std::vector<uint32_t> table = buildInstanceTagTable(names, map, &unmapped);

    EXPECT(table.size() == names.size());
    EXPECT(table[0] == static_cast<uint32_t>(ObjectTag::Enemy));
    EXPECT(table[1] == static_cast<uint32_t>(ObjectTag::None)); // Cube 는 매핑에 없다
    EXPECT(table[2] == static_cast<uint32_t>(ObjectTag::None)); // 빈 이름
    EXPECT(table[4] == static_cast<uint32_t>(ObjectTag::Floor));
    EXPECT(table[5] == static_cast<uint32_t>(ObjectTag::None));

    // 중복 없이, 빈 이름 없이 모은다
    std::sort(unmapped.begin(), unmapped.end());
    EXPECT(unmapped.size() == 2 && unmapped[0] == "Cube" && unmapped[1] == "Sphere");
}

CPU_TEST(TagMapping_LookupMissAndOutOfRangeAreNone)
{
    const std::vector<uint32_t> table = {
        static_cast<uint32_t>(ObjectTag::Wall),
        static_cast<uint32_t>(ObjectTag::Enemy),
        static_cast<uint32_t>(ObjectTag::Item),
    };
    EXPECT(lookupObjectTag(table, 0) == static_cast<uint32_t>(ObjectTag::Wall));
    EXPECT(lookupObjectTag(table, 2) == static_cast<uint32_t>(ObjectTag::Item));
    EXPECT(lookupObjectTag(table, 3) == static_cast<uint32_t>(ObjectTag::None));       // 범위 밖
    EXPECT(lookupObjectTag(table, 0xFFFFFFFFu) == static_cast<uint32_t>(ObjectTag::None)); // GBuffer miss
    EXPECT(lookupObjectTag({}, 0) == static_cast<uint32_t>(ObjectTag::None));          // 빈 표
}

// 스펙 S-3.5 의 CPU 쪽 검사: 인스턴스 순서가 바뀌어도 같은 머티리얼은 같은 태그를 받는다.
CPU_TEST(TagMapping_TagFollowsMaterialNotInstanceOrder)
{
    const MaterialTagMap map = buildMaterialTagMap(fpsClassConfig(), nullptr);
    const std::vector<std::string> names = {"Wall", "Enemy", "Floor", "Item", "Brick", "Cube"};
    std::vector<std::string> reversed(names.rbegin(), names.rend());

    const std::vector<uint32_t> a = buildInstanceTagTable(names, map, nullptr);
    const std::vector<uint32_t> b = buildInstanceTagTable(reversed, map, nullptr);

    for (size_t i = 0; i < names.size(); i++)
        EXPECT(a[i] == b[names.size() - 1 - i]);
}
} // namespace Falcor
