#pragma once

#include "psxexe.h"
#include "r3000a.h"
#include "psxrecomp/irq.h"
#include "psxrecomp/timers.h"
#include "psxrecomp/cdrom.h"
#include "psxrecomp/gte.h"
#include "psxrecomp/spu.h"
#include "psxrecomp/mdec.h"
#include "psxrecomp/pad_sio.h"
#include "psxrecomp/psx_memory.h"
#include "psxrecomp/bios_hle.h"
#include "psxgpu/psx_gpu.h"

#include <cstdint>
#include <string>

namespace psxrecomp {

enum BiosBackend {
    BiosBackendHle = 0,
    BiosBackendOpenBios
};

class PsxRuntime {
public:
    PsxRuntime();

    void reset();
    void loadExecutable(const PsxExeImage& image);
    bool mountDisc(const std::string& path);
    bool enableOpenBios(const std::string& path);
    void useHleBios();
    BiosBackend biosBackend() const { return m_biosBackend; }
    bool openBiosLoaded() const { return m_memory.hasBiosRom(); }

    // Advances devices by R3000A CPU clocks and services interrupt boundaries.
    void advance(uint32_t cpuCycles);
    bool handleHle();
    bool stepOpenBios();

    r3k::CpuState& cpu() { return m_cpu; }
    PsxMemory& memory() { return m_memory; }
    psxgpu::PsxGpu& gpu() { return m_gpu; }
    PsxGte& gte() { return m_gte; }
    IrqController& irq() { return m_irq; }
    PsxTimers& timers() { return m_timers; }
    PsxCdrom& cdrom() { return m_cdrom; }
    PsxSpu& spu() { return m_spu; }
    PsxMdec& mdec() { return m_mdec; }
    PsxPadSio& pad() { return m_pad; }
    BiosHle& bios() { return m_bios; }

private:
    psxgpu::PsxGpu m_gpu;
    IrqController m_irq;
    PsxTimers m_timers;
    PsxSpu m_spu;
    PsxCdrom m_cdrom;
    PsxMdec m_mdec;
    PsxPadSio m_pad;
    PsxMemory m_memory;
    PsxGte m_gte;
    BiosHle m_bios;
    r3k::CpuState m_cpu;
    BiosBackend m_biosBackend = BiosBackendHle;

    uint64_t m_scanlineCycles = 0;
    uint64_t m_dotClockNumerator = 0;
    unsigned m_scanline = 0;
    bool m_inVblank = false;
};

} // namespace psxrecomp
