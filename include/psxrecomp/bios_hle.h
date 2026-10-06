#pragma once

#include "r3000a.h"
#include "psxrecomp/psx_memory.h"
#include "psxrecomp/irq.h"
#include "psxrecomp/timers.h"
#include "psxrecomp/cdrom.h"
#include "psxrecomp/pad_sio.h"
#include "psxgpu/psx_gpu.h"

#include <array>
#include <cstdint>
#include <string>

namespace psxrecomp {

class BiosHle {
public:
    BiosHle(PsxMemory& memory, IrqController& irq, PsxTimers& timers, PsxCdrom& cdrom, PsxPadSio& pad, psxgpu::PsxGpu& gpu);
    void reset();
    bool handleVector(r3k::CpuState& cpu);
    bool handleExceptionVector(r3k::CpuState& cpu);
private:
    struct Event {
        Event() : used(false), enabled(false), ready(false), cls(0), spec(0), mode(0), func(0) {}
        bool used, enabled, ready;
        uint32_t cls, spec, mode, func;
    };
    uint32_t arg(const r3k::CpuState& cpu, unsigned index) const;
    std::string readString(uint32_t addr, std::size_t maxLen=4096) const;
    void writeBytes(uint32_t dst, uint32_t src, uint32_t len);
    void fillBytes(uint32_t dst, uint8_t value, uint32_t len);
    uint32_t deliverEvent(uint32_t cls, uint32_t spec);
    bool enqueueIntRp(uint32_t priority, uint32_t struc);
    bool dequeueIntRp(uint32_t priority, uint32_t struc);
    uint32_t firstInterruptRoutine() const;
    void beginEventCallback(r3k::CpuState& cpu, const r3k::CpuState& resumeState, uint32_t func);
    bool finishEventCallback(r3k::CpuState& cpu);
    uint32_t openEvent(uint32_t cls, uint32_t spec, uint32_t mode, uint32_t func);
    Event* eventFromHandle(uint32_t handle);
    void callA(r3k::CpuState& cpu, uint8_t fn);
    void callB(r3k::CpuState& cpu, uint8_t fn);
    void callC(r3k::CpuState& cpu, uint8_t fn);
    PsxMemory& m_mem;
    IrqController& m_irq;
    PsxTimers& m_timers;
    PsxCdrom& m_cdrom;
    PsxPadSio& m_pad;
    psxgpu::PsxGpu& m_gpu;
    std::array<Event, 32> m_events;
    std::array<bool, 4> m_autoAck;
    std::array<uint32_t, 4> m_intRpHeads;
    uint32_t m_padBuf1=0, m_padBuf2=0, m_padButtonDest=0;
    uint32_t m_padSize1=0, m_padSize2=0;
    bool m_padEnabled=false;
    bool m_clearPad=true;
    bool m_eventCallbackActive=false;
    r3k::CpuState m_eventResumeState;
    uint32_t m_eventCallbackFunc=0;
    static const uint32_t kEventCallbackTrampoline = 0x800000D0u;
};
} // namespace psxrecomp
