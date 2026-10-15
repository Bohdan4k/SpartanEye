#include "sig.h"

#include <windows.h>

#include <cstring>

bool game_module(GameModule& m)
{
    auto base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    if (!base)
        return false;
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    m.base = base;
    m.size = nt->OptionalHeader.SizeOfImage;
    m.timestamp = nt->FileHeader.TimeDateStamp;
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
    {
        if (memcmp(sec->Name, ".text", 6) == 0)
        {
            m.text = base + sec->VirtualAddress;
            m.text_size = sec->Misc.VirtualSize;
        }
    }
    return m.text != nullptr;
}

static bool parse(const char* pattern, std::vector<int>& out)
{
    out.clear();
    for (const char* p = pattern; *p;)
    {
        if (*p == ' ')
        {
            ++p;
            continue;
        }
        if (p[0] == '?')
        {
            out.push_back(-1);
            p += (p[1] == '?') ? 2 : 1;
            continue;
        }
        auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                                    : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
        int hi = hex(p[0]), lo = p[1] ? hex(p[1]) : -1;
        if (hi < 0 || lo < 0)
            return false;
        out.push_back(hi * 16 + lo);
        p += 2;
    }
    return !out.empty() && out[0] >= 0;
}

std::vector<uint8_t*> sig_find_in(const uint8_t* start, size_t len, const char* pattern)
{
    std::vector<uint8_t*> hits;
    std::vector<int> pat;
    if (!start || !parse(pattern, pat) || pat.size() > len)
        return hits;
    const uint8_t first = static_cast<uint8_t>(pat[0]);
    const uint8_t* end = start + len - pat.size();
    for (const uint8_t* p = start; p <= end;)
    {
        p = static_cast<const uint8_t*>(memchr(p, first, end - p + 1));
        if (!p)
            break;
        size_t k = 1;
        while (k < pat.size() && (pat[k] < 0 || p[k] == pat[k]))
            ++k;
        if (k == pat.size())
            hits.push_back(const_cast<uint8_t*>(p));
        ++p;
    }
    return hits;
}

std::vector<uint8_t*> sig_find_all(const GameModule& m, const char* pattern)
{
    return sig_find_in(m.text, m.text_size, pattern);
}
