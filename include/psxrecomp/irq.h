#pragma once

#include "r3000a.h"
#include <cstdint>

namespace psxrecomp {

class IrqController {
public:
    enum Source : uint16_t {
        VBlank = 1u << 0, Gpu = 1u << 1, Cdrom = 1u << 2, Dma = 1u << 3,
        Timer0 = 1u << 4, Timer1 = 1u << 5, Timer2 = 1u << 6,
        Pad = 1u << 7, Sio = 1u << 8, Spu = 1u << 9, Pio = 1u << 10
    };
    void reset();
    void request(Source source);
    void requestMask(uint16_t bits);
    void acknowledge(uint16_t value);
    uint16_t stat() const { return m_stat; }
    uint16_t mask() const { return m_mask; }
    void setMask(uint16_t value) { m_mask = value & 0x07FFu; }
    bool pending() const { return (m_stat & m_mask) != 0; }
    void updateCop0(r3k::CpuState& cpu) const;
    bool takeInterrupt(r3k::CpuState& cpu) const;
private:
    uint16_t m_stat = 0;
    uint16_t m_mask = 0;
};

} // namespace psxrecomp
