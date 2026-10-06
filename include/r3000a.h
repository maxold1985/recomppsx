#pragma once
#include <cstdint>
#include <cstddef>
#include <array>
#include <stdexcept>

namespace r3k {

struct CpuState {
    CpuState() : hi(0), lo(0), pc(0), cycles(0) {
        gpr.fill(0);
        for (unsigned i = 0; i < 32; ++i) cop0[i] = 0;
    }
    std::array<uint32_t, 32> gpr;
    uint32_t hi;
    uint32_t lo;
    uint32_t pc;
    uint64_t cycles;
    uint32_t cop0[32];
};

class Memory {
public:
    virtual ~Memory() = default;
    virtual uint8_t read8(uint32_t addr) = 0;
    virtual uint16_t read16(uint32_t addr) = 0;
    virtual uint32_t read32(uint32_t addr) = 0;
    virtual void write8(uint32_t addr, uint8_t v) = 0;
    virtual void write16(uint32_t addr, uint16_t v) = 0;
    virtual void write32(uint32_t addr, uint32_t v) = 0;
};

struct Gte {
    virtual ~Gte() = default;
    virtual uint32_t read_data(uint32_t reg) = 0;
    virtual uint32_t read_ctrl(uint32_t reg) = 0;
    virtual void write_data(uint32_t reg, uint32_t value) = 0;
    virtual void write_ctrl(uint32_t reg, uint32_t value) = 0;
    virtual void execute(uint32_t instruction) = 0;
};

inline int32_t sx16(uint16_t v) { return static_cast<int16_t>(v); }
inline uint32_t add32(uint32_t a, uint32_t b) { return a + b; }
inline uint32_t sub32(uint32_t a, uint32_t b) { return a - b; }

} // namespace r3k
