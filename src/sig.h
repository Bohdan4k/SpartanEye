#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

// The game module as mapped in memory (MetroExodus.exe).
struct GameModule
{
    uint8_t* base = nullptr;
    size_t size = 0;
    uint8_t* text = nullptr;  // .text section
    size_t text_size = 0;
    uint32_t timestamp = 0;   // PE TimeDateStamp (build guard)
};

bool game_module(GameModule& m);

// IDA-style pattern ("48 8B 05 ?? ?? ?? ??") matched against .text. Returns every match address.
std::vector<uint8_t*> sig_find_all(const GameModule& m, const char* pattern);
// Same, inside [start, start + len) (e.g. one function found through a call).
std::vector<uint8_t*> sig_find_in(const uint8_t* start, size_t len, const char* pattern);

inline int32_t rd_i32(const uint8_t* p) { int32_t v; memcpy(&v, p, 4); return v; }
// RIP-relative target: instruction at `insn` with disp32 at `insn + disp_at`, instruction length `len`.
inline uint8_t* rip_target(uint8_t* insn, int disp_at, int len) { return insn + len + rd_i32(insn + disp_at); }
