#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "common.hpp"

class LibTable
{
public:
    int32_t intern(const char* name, uint64_t addr = 0, uint32_t size = 0)
    {
        if (!name || !name[0]) return -1;

        std::string key(name);
        auto it = m_map.find(key);
        if (it != m_map.end())
        {
            auto& lib = m_libs[it->second];
            if (addr != 0)
            {
                uint64_t end = addr + size;
                if (lib.start == 0 || addr < lib.start) lib.start = addr;
                if (end > lib.end) lib.end = end;
            }
            return it->second;
        }

        int32_t idx = static_cast<int32_t>(m_libs.size());
        uint64_t start = addr;
        uint64_t end = addr + size;
        m_libs.push_back({ key, start, end });
        m_map[key] = idx;
        return idx;
    }

    json to_json() const
    {
        json arr = json::array();
        for (const auto& lib : m_libs)
        {
            arr.push_back({
                {"arch", nullptr},
                {"name", lib.name},
                {"path", lib.name},
                {"debugName", lib.name},
                {"debugPath", lib.name},
                {"start", lib.start},
                {"end", lib.end},
                {"breakpadId", nullptr},
                {"codeId", nullptr},
            });
        }
        return arr;
    }

private:
    struct LibEntry
    {
        std::string name;
        uint64_t start;
        uint64_t end;
    };

    std::vector<LibEntry> m_libs;
    std::unordered_map<std::string, int32_t> m_map;
};
