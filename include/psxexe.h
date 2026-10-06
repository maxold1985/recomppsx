#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct PsxExeImage {
    PsxExeImage() : initial_pc(0), initial_gp(0), load_address(0), load_size(0), stack_base(0), stack_offset(0) {}
    uint32_t initial_pc;
    uint32_t initial_gp;
    uint32_t load_address;
    uint32_t load_size;
    uint32_t stack_base;
    uint32_t stack_offset;
    std::vector<uint8_t> payload;
};

PsxExeImage load_psx_exe(const std::string& path);
