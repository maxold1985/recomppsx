#include "psxexe.h"
#include <fstream>
#include <stdexcept>
#include <cstring>

static uint32_t rd32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

PsxExeImage load_psx_exe(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open input");
    std::vector<uint8_t> all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (all.size() < 0x800) throw std::runtime_error("file too small for PS-X EXE");
    if (std::memcmp(all.data(), "PS-X EXE", 8) != 0) throw std::runtime_error("not a PS-X EXE");

    PsxExeImage out;
    out.initial_pc   = rd32(&all[0x10]);
    out.initial_gp   = rd32(&all[0x14]);
    out.load_address= rd32(&all[0x18]);
    out.load_size   = rd32(&all[0x1C]);
    out.stack_base  = rd32(&all[0x30]);
    out.stack_offset= rd32(&all[0x34]);

    const size_t available = all.size() - 0x800;
    const size_t n = out.load_size < available ? out.load_size : available;
    out.payload.assign(all.begin() + 0x800, all.begin() + 0x800 + n);
    return out;
}
