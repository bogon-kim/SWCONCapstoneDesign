/***************************************************************************
 # TagMapping.cpp  (spec_TSK.md 3.4절, R-2.1 ~ R-2.2)
 ***************************************************************************/
#include "TagMapping.h"

#include <set>

const char* toString(ObjectTag tag)
{
    switch (tag)
    {
    case ObjectTag::Enemy: return "Enemy";
    case ObjectTag::Item:  return "Item";
    case ObjectTag::Wall:  return "Wall";
    case ObjectTag::Floor: return "Floor";
    default:               return "None";
    }
}

bool parseObjectTag(const std::string& name, ObjectTag& tag)
{
    if (name == "Enemy")      tag = ObjectTag::Enemy;
    else if (name == "Item")  tag = ObjectTag::Item;
    else if (name == "Wall")  tag = ObjectTag::Wall;
    else if (name == "Floor") tag = ObjectTag::Floor;
    else return false;
    return true;
}

MaterialTagMap buildMaterialTagMap(const std::map<std::string, std::vector<std::string>>& classToMaterials,
                                   std::vector<std::string>* pUnknownClasses)
{
    MaterialTagMap map;
    for (const auto& [className, materials] : classToMaterials)
    {
        ObjectTag tag;
        if (!parseObjectTag(className, tag))
        {
            if (pUnknownClasses) pUnknownClasses->push_back(className);
            continue;
        }
        // 같은 머티리얼 이름이 여러 클래스에 있으면 나중에 읽은 항목이 이긴다.
        for (const auto& name : materials)
            map[name] = tag;
    }
    return map;
}

std::vector<uint32_t> buildInstanceTagTable(const std::vector<std::string>& instanceMaterialNames,
                                            const MaterialTagMap& map,
                                            std::vector<std::string>* pUnmapped)
{
    std::vector<uint32_t> table(instanceMaterialNames.size(), static_cast<uint32_t>(ObjectTag::None));
    std::set<std::string> unmapped;
    for (size_t i = 0; i < instanceMaterialNames.size(); i++)
    {
        const std::string& name = instanceMaterialNames[i];
        if (name.empty())
            continue;
        const auto it = map.find(name);
        if (it != map.end())
            table[i] = static_cast<uint32_t>(it->second);
        else
            unmapped.insert(name);
    }
    if (pUnmapped)
        pUnmapped->assign(unmapped.begin(), unmapped.end());
    return table;
}

uint32_t lookupObjectTag(const std::vector<uint32_t>& table, uint32_t objectID)
{
    return objectID < table.size() ? table[objectID] : static_cast<uint32_t>(ObjectTag::None);
}
