#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "common.hpp"

class StringTable
{
public:
    uint32_t intern(const std::string& s)
    {
        auto it = m_map.find(s);
        if (it != m_map.end()) return it->second;
        uint32_t idx = static_cast<uint32_t>(m_strings.size());
        m_strings.push_back(s);
        m_map[s] = idx;
        return idx;
    }

    uint32_t intern(const char* s)
    {
        return intern(std::string(s ? s : ""));
    }

    json to_json() const
    {
        return json(m_strings);
    }

private:
    std::vector<std::string> m_strings;
    std::unordered_map<std::string, uint32_t> m_map;
};
