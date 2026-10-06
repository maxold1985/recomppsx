#pragma once

#include "r3000a.h"
#include "psxgpu/psx_gpu.h"
#include "psxgpu/psx_gpu_mmio.h"
#include "psxrecomp/irq.h"
#include "psxrecomp/timers.h"
#include "psxrecomp/cdrom.h"
#include "psxrecomp/mdec.h"
#include "psxrecomp/spu.h"
#include "psxrecomp/pad_sio.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace psxrecomp {

class PsxMemory final : public r3k::Memory {
public:
    static const std::size_t MainRamSize = 2 * 1024 * 1024;
    static const std::size_t ScratchpadSize = 1024;
    static const std::size_t BiosRomSize = 512 * 1024;

    PsxMemory(psxgpu::PsxGpu& gpu, IrqController& irq, PsxTimers& timers,
              PsxCdrom& cdrom, PsxMdec& mdec, PsxSpu& spu, PsxPadSio& sio);

    uint8_t read8(uint32_t addr) override;
    uint16_t read16(uint32_t addr) override;
    uint32_t read32(uint32_t addr) override;

    void write8(uint32_t addr, uint8_t value) override;
    void write16(uint32_t addr, uint16_t value) override;
    void write32(uint32_t addr, uint32_t value) override;

    uint8_t rawRead8(uint32_t addr) const;
    uint16_t rawRead16(uint32_t addr) const;
    uint32_t rawRead32(uint32_t addr) const;
    void rawWrite8(uint32_t addr, uint8_t value);
    void rawWrite16(uint32_t addr, uint16_t value);
    void rawWrite32(uint32_t addr, uint32_t value);

    void clear();
    void loadBytes(uint32_t guestAddress, const uint8_t* data, std::size_t size);
    bool loadBiosRom(const std::string& path);
    void clearBiosRom();
    bool hasBiosRom() const { return m_biosLoaded; }

    void setCacheIsolation(bool enabled) { m_cacheIsolated = enabled; }
    bool cacheIsolation() const { return m_cacheIsolated; }
    uint32_t cacheControl() const { return m_cacheControl; }

    psxgpu::PsxGpuMmio& gpuMmio() { return m_gpuMmio; }
    const std::array<uint8_t, MainRamSize>& ram() const { return m_ram; }

private:
    struct DmaChannel { DmaChannel() : madr(0), bcr(0), chcr(0) {} uint32_t madr, bcr, chcr; };

    static uint32_t physical(uint32_t addr);
    static bool isGpuMmio32(uint32_t paddr);
    static bool isScratchpad(uint32_t paddr);
    bool isCoreIo(uint32_t paddr) const;
    static int dmaChannelForReg(uint32_t paddr);

    uint8_t readIo8(uint32_t paddr);
    void writeIo8(uint32_t paddr, uint8_t value);
    uint16_t readIo16(uint32_t paddr);
    void writeIo16(uint32_t paddr, uint16_t value);
    uint32_t readIo32(uint32_t paddr);
    void writeIo32(uint32_t paddr, uint32_t value);
    void runDma(unsigned channel);
    uint32_t dmaWordCount(const DmaChannel& c) const;
    void updateDmaIrq(unsigned channel);

    psxgpu::PsxGpu& m_gpu;
    IrqController& m_irq;
    PsxTimers& m_timers;
    PsxCdrom& m_cdrom;
    PsxMdec& m_mdec;
    PsxSpu& m_spu;
    PsxPadSio& m_sio;
    psxgpu::PsxGpuMmio m_gpuMmio;

    std::array<uint8_t, MainRamSize> m_ram;
    std::array<uint8_t, ScratchpadSize> m_scratchpad;
    std::array<uint8_t, BiosRomSize> m_biosRom;
    bool m_biosLoaded = false;
    std::array<uint8_t, 0x1000> m_io;
    std::array<DmaChannel,7> m_dma;
    uint32_t m_dicr = 0;
    uint32_t m_cacheControl = 0;
    bool m_cacheIsolated = false;
};

} // namespace psxrecomp
