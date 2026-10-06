#pragma once
#include <cstdint>
#include <string>

struct Decoded {
    Decoded() : raw(0), op(0), rs(0), rt(0), rd(0), sa(0), funct(0), imm(0), target(0) {}
    uint32_t raw;
    uint8_t op, rs, rt, rd, sa, funct;
    uint16_t imm;
    uint32_t target;
};

Decoded decode(uint32_t raw);
std::string disasm(uint32_t pc, const Decoded& d);
