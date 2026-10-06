#pragma once

#include "psxrecomp/irq.h"
#include <array>
#include <cstdint>

namespace psxrecomp {

class PsxTimers {
public:
    explicit PsxTimers(IrqController& irq) : m_irq(irq) {}

    void reset();
    void tick(uint32_t cpuCycles);
    void tickDotClock(uint32_t dots);
    void onHblank();
    void setHblank(bool active) { m_hblank = active; }
    void setVblank(bool active);

    uint16_t read16(uint32_t paddr);
    void write16(uint32_t paddr, uint16_t value);
    bool handles(uint32_t paddr) const;

private:
    struct Timer {
        Timer() : count(0), mode(0x0400), target(0), prescaleRemainder(0),
                  oneShotFired(false), irqLine(true), syncSeen(false), writeHold(0) {}
        uint16_t count;
        uint16_t mode;
        uint16_t target;
        uint32_t prescaleRemainder;
        bool oneShotFired;
        bool irqLine;
        bool syncSeen;
        uint8_t writeHold;
    };

    void tickOne(unsigned index, uint32_t ticks);
    void triggerIrq(unsigned index, Timer& timer);
    bool clockEnabled(unsigned index, const Timer& timer) const;
    void syncEdge(unsigned index, bool enteringBlank);
    static uint32_t base(unsigned index) { return 0x1F801100u + index * 0x10u; }

    IrqController& m_irq;
    std::array<Timer, 3> m_timer;
    bool m_hblank = false;
    bool m_vblank = false;
};

} // namespace psxrecomp
