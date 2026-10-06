#include "psxrecomp/timers.h"
#include <algorithm>

namespace psxrecomp {

void PsxTimers::reset()
{
    for (auto& t : m_timer) t = Timer();
    m_hblank=false; m_vblank=false;
}

bool PsxTimers::handles(uint32_t paddr) const
{
    for (unsigned i = 0; i < 3; ++i) {
        const uint32_t b = base(i);
        if (paddr == b || paddr == b + 4 || paddr == b + 8) return true;
    }
    return false;
}

uint16_t PsxTimers::read16(uint32_t paddr)
{
    for (unsigned i = 0; i < 3; ++i) {
        const uint32_t b = base(i);
        Timer& t = m_timer[i];
        if (paddr == b) return t.count;
        if (paddr == b + 4) {
            const uint16_t value = t.mode;
            // Reached-target and reached-overflow bits are reset by a mode read.
            t.mode &= static_cast<uint16_t>(~((1u << 11) | (1u << 12)));
            return value;
        }
        if (paddr == b + 8) return t.target;
    }
    return 0;
}

void PsxTimers::write16(uint32_t paddr, uint16_t value)
{
    for (unsigned i = 0; i < 3; ++i) {
        const uint32_t b = base(i);
        Timer& t = m_timer[i];
        if (paddr == b) {
            t.count = value;
            t.writeHold = 2;
            return;
        }
        if (paddr == b + 4) {
            // Writable mode bits are 0..9. Writing mode resets COUNT and state.
            t.mode = static_cast<uint16_t>((value & 0x03FFu) | 0x0400u);
            t.count = 0;
            t.prescaleRemainder = 0;
            t.oneShotFired = false;
            t.irqLine = true;
            t.syncSeen = false;
            t.writeHold = 2;
            return;
        }
        if (paddr == b + 8) {
            t.target = value;
            return;
        }
    }
}

void PsxTimers::triggerIrq(unsigned index, Timer& t)
{
    const bool repeat = (t.mode & (1u << 6)) != 0;
    const bool toggle = (t.mode & (1u << 7)) != 0;
    if (!repeat && t.oneShotFired) return;
    t.oneShotFired = true;

    bool request = true;
    if (toggle) {
        t.irqLine = !t.irqLine;
        request = !t.irqLine;
        if (t.irqLine) t.mode |= 0x0400u;
        else t.mode &= static_cast<uint16_t>(~0x0400u);
    } else {
        // Pulse mode is approximated as one host-side IRQ request.
        t.mode |= 0x0400u;
    }

    if (request) {
        static const IrqController::Source sources[3] = {
            IrqController::Timer0, IrqController::Timer1, IrqController::Timer2
        };
        m_irq.request(sources[index]);
    }
}

void PsxTimers::tickOne(unsigned index, uint32_t ticks)
{
    Timer& t = m_timer[index];
    if (t.writeHold) {
        const uint32_t h=std::min<uint32_t>(ticks,t.writeHold);
        t.writeHold=uint8_t(t.writeHold-h);
        ticks-=h;
        if(!ticks)return;
    }
    uint32_t remaining = ticks;
    while (remaining != 0) {
        const uint32_t count = t.count;
        const uint32_t distOverflow = 0x10000u - count;

        uint32_t distTarget;
        if (t.target > count) distTarget = uint32_t(t.target) - count;
        else distTarget = (0x10000u - count) + uint32_t(t.target);
        if (distTarget == 0) distTarget = 0x10000u;

        uint32_t step = std::min(distOverflow, distTarget);
        if (remaining < step) {
            t.count = static_cast<uint16_t>(count + remaining);
            break;
        }

        remaining -= step;
        const uint32_t next = count + step;
        const bool overflow = next >= 0x10000u;
        uint16_t reached = static_cast<uint16_t>(next & 0xFFFFu);
        const bool hitTarget = (step == distTarget);

        if (overflow) {
            t.mode |= (1u << 12);
            if (t.mode & (1u << 5)) triggerIrq(index, t);
        }
        if (hitTarget) {
            t.mode |= (1u << 11);
            if (t.mode & (1u << 4)) triggerIrq(index, t);
            if (t.mode & (1u << 3)) reached = 0;
        }
        t.count = reached;
    }
}

bool PsxTimers::clockEnabled(unsigned index, const Timer& t) const
{
    if ((t.mode & 1u) == 0) return true;
    const unsigned sm=(t.mode>>1)&3u;
    const bool blank=index==0?m_hblank:(index==1?m_vblank:false);
    if(index==2) return sm==1 || sm==2;
    switch(sm){
        case 0: return !blank;
        case 1: return true;
        case 2: return blank;
        case 3: return t.syncSeen;
    }
    return true;
}

void PsxTimers::syncEdge(unsigned index, bool enteringBlank)
{
    if(index>1)return;
    Timer& t=m_timer[index];
    if((t.mode&1u)==0 || !enteringBlank)return;
    const unsigned sm=(t.mode>>1)&3u;
    if(sm==1 || sm==2){ t.count=0; t.writeHold=2; }
    if(sm==3)t.syncSeen=true;
}

void PsxTimers::setVblank(bool active)
{
    if(active!=m_vblank){
        m_vblank=active;
        if(active)syncEdge(1,true);
    }
}

void PsxTimers::onHblank()
{
    m_hblank=true;
    syncEdge(0,true);
    Timer& t=m_timer[1];
    if((t.mode&(1u<<8)) && clockEnabled(1,t))tickOne(1,1);
    m_hblank=false;
}

void PsxTimers::tickDotClock(uint32_t dots)
{
    Timer& t=m_timer[0];
    if((t.mode&(1u<<8)) && clockEnabled(0,t))tickOne(0,dots);
}

void PsxTimers::tick(uint32_t cpuCycles)
{
    for (unsigned i = 0; i < 3; ++i) {
        Timer& t = m_timer[i];
        if(!clockEnabled(i,t))continue;
        if(i<2 && (t.mode&(1u<<8)))continue; // external dotclock/HBlank paths
        uint32_t ticks = cpuCycles;
        if (i == 2 && (t.mode & (1u << 9))) {
            const uint32_t total = t.prescaleRemainder + cpuCycles;
            ticks = total / 8u;
            t.prescaleRemainder = total % 8u;
        }
        tickOne(i, ticks);
    }
}

} // namespace psxrecomp
