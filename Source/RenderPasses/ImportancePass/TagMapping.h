#pragma once
/***************************************************************************
 # TagMapping.h
 # 객체 태그 (spec_TSK.md 3.4절, R-2). Falcor 의존성이 없는 순수 CPU 코드다.
 #   씬 머티리얼 이름 -> 클래스(Enemy / Item / Wall / Floor / None)
 #   인스턴스 인덱스 -> 클래스 표 (GBuffer objectID 로 조회한다)
 ***************************************************************************/
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// GPU 로 넘기는 클래스 ID. ImportancePass.cs.slang 의 tagMap 값과 같아야 한다.
enum class ObjectTag : uint32_t
{
    None  = 0,
    Enemy = 1,
    Item  = 2,
    Wall  = 3,
    Floor = 4,
};

const char* toString(ObjectTag tag);

// 클래스 이름("Enemy" 등)을 ObjectTag 로 바꾼다. "None" 과 모르는 이름은 false.
bool parseObjectTag(const std::string& name, ObjectTag& tag);

// 머티리얼 이름 -> 클래스
using MaterialTagMap = std::map<std::string, ObjectTag>;

// TaskConfig.json 의 "tags" 항목(클래스 이름 -> 머티리얼 이름 목록)을 MaterialTagMap 으로 바꾼다.
// 알 수 없는 클래스 이름은 무시하고 pUnknownClasses 에 모은다.
MaterialTagMap buildMaterialTagMap(const std::map<std::string, std::vector<std::string>>& classToMaterials,
                                   std::vector<std::string>* pUnknownClasses);

// 인스턴스 인덱스 i 의 머티리얼 이름 -> 태그 표.
// 매핑에 없는 머티리얼은 None 이다. 그 이름은 중복 없이 pUnmapped 에 모은다 (빈 이름은 보고하지 않는다).
std::vector<uint32_t> buildInstanceTagTable(const std::vector<std::string>& instanceMaterialNames,
                                            const MaterialTagMap& map,
                                            std::vector<std::string>* pUnmapped);

// objectID 로 태그를 찾는다. 범위 밖이거나 miss(0xFFFFFFFF)이면 None 이다.
uint32_t lookupObjectTag(const std::vector<uint32_t>& table, uint32_t objectID);
