#include "psxrecomp/psx_memory.h"
#include "psxrecomp/trace.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
namespace psxrecomp {

namespace {
const uint32_t kRamMirrorMask = 0x001FFFFFu;
const uint32_t kScratchpadBase = 0x1F800000u;
const uint32_t kIoBase = 0x1F801000u;
const uint32_t kIoEnd = 0x1F802000u;
const uint32_t kBiosBase = 0x1FC00000u;
const uint32_t kIStat = 0x1F801070u;
const uint32_t kIMask = 0x1F801074u;
const uint32_t kDmaBase = 0x1F801080u;
const uint32_t kDicr = 0x1F8010F4u;
}

PsxMemory::PsxMemory(psxgpu::PsxGpu& gpu, IrqController& irq, PsxTimers& timers,
                     PsxCdrom& cdrom, PsxMdec& mdec, PsxSpu& spu, PsxPadSio& sio)
    : m_gpu(gpu), m_irq(irq), m_timers(timers), m_cdrom(cdrom), m_mdec(mdec),
      m_spu(spu), m_sio(sio),
      m_gpuMmio(gpu,
          [this](uint32_t a) { return this->rawRead32(a); },
          [this](uint32_t a, uint32_t v) { this->rawWrite32(a, v); })
{
}

uint32_t PsxMemory::physical(uint32_t addr) { return addr & 0x1FFFFFFFu; }
bool PsxMemory::isScratchpad(uint32_t p) { return p >= kScratchpadBase && p < kScratchpadBase + ScratchpadSize; }

bool PsxMemory::isGpuMmio32(uint32_t p)
{
    using M = psxgpu::PsxGpuMmio;
    switch (p) {
        case M::GP0: case M::GP1:
        case M::DMA2_MADR: case M::DMA2_BCR: case M::DMA2_CHCR:
        case M::DMA6_MADR: case M::DMA6_BCR: case M::DMA6_CHCR:
        case M::DPCR: return true;
        default: return false;
    }
}

int PsxMemory::dmaChannelForReg(uint32_t p)
{
    if (p < kDmaBase || p >= kDmaBase + 7u * 0x10u) return -1;
    const unsigned off = (p - kDmaBase) & 0x0Fu;
    if (off != 0 && off != 4 && off != 8) return -1;
    return int((p - kDmaBase) / 0x10u);
}

bool PsxMemory::isCoreIo(uint32_t p) const
{
    if (p == kIStat || p == kIMask || p == kDicr) return true;
    if (dmaChannelForReg(p & ~3u) >= 0) return true;
    if (m_timers.handles(p & ~1u) || m_cdrom.handles(p) || m_spu.handles(p) ||
        m_sio.handles(p) || m_mdec.handles(p & ~3u)) return true;
    return false;
}

void PsxMemory::clear()
{
    m_ram.fill(0); m_scratchpad.fill(0); m_io.fill(0);
    for (auto& d : m_dma) d = DmaChannel();
    m_dicr = 0;
}

uint8_t PsxMemory::rawRead8(uint32_t addr) const
{
    const uint32_t p=physical(addr);
    if(p<0x00800000u) return m_ram[p&kRamMirrorMask];
    if(m_biosLoaded && p>=kBiosBase && p<kBiosBase+BiosRomSize) return m_biosRom[p-kBiosBase];
    if(isScratchpad(p)) return m_scratchpad[p-kScratchpadBase];
    if(p>=kIoBase&&p<kIoEnd) return m_io[p-kIoBase];
    return 0;
}
uint16_t PsxMemory::rawRead16(uint32_t a) const { return uint16_t(rawRead8(a))|(uint16_t(rawRead8(a+1))<<8); }
uint32_t PsxMemory::rawRead32(uint32_t a) const { return uint32_t(rawRead16(a))|(uint32_t(rawRead16(a+2))<<16); }
void PsxMemory::rawWrite8(uint32_t addr,uint8_t v){ const uint32_t p=physical(addr); if(p<0x00800000u){m_ram[p&kRamMirrorMask]=v;return;} if(isScratchpad(p)){m_scratchpad[p-kScratchpadBase]=v;return;} if(p>=kIoBase&&p<kIoEnd)m_io[p-kIoBase]=v; }
void PsxMemory::rawWrite16(uint32_t a,uint16_t v){rawWrite8(a,uint8_t(v));rawWrite8(a+1,uint8_t(v>>8));}
void PsxMemory::rawWrite32(uint32_t a,uint32_t v){rawWrite16(a,uint16_t(v));rawWrite16(a+2,uint16_t(v>>16));}

uint32_t PsxMemory::dmaWordCount(const DmaChannel& c) const
{
    const unsigned sync=(c.chcr>>9)&3u;
    uint32_t words = 0;
    if(sync==0) words=c.bcr&0xFFFFu;
    else if(sync==1) words=(c.bcr&0xFFFFu)*((c.bcr>>16)&0xFFFFu);
    else words=c.bcr&0xFFFFu;
    return words ? words : 0x10000u;
}

void PsxMemory::updateDmaIrq(unsigned channel)
{
    if(channel>6) return;
    m_dicr |= 1u<<(24u+channel);
    const bool master=(m_dicr&(1u<<23))!=0;
    const bool enabled=(m_dicr&(1u<<(16u+channel)))!=0;
    const bool force=(m_dicr&(1u<<15))!=0;
    if(force||(master&&enabled)){
        m_dicr|=1u<<31;
        m_irq.request(IrqController::Dma);
    }
}

void PsxMemory::runDma(unsigned channel)
{
    if (channel >= m_dma.size() || channel==2 || channel==6) return;
    DmaChannel& c=m_dma[channel];
    if((c.chcr&(1u<<24))==0) return;
    const uint32_t words=dmaWordCount(c);
    uint32_t addr=c.madr&0x001FFFFCu;
    const bool fromRam=(c.chcr&1u)!=0;
    const bool decrement=(c.chcr&(1u<<1))!=0;

    tracePrintf("[DMA BEGIN] ch=%u MADR=%08X BCR=%08X CHCR=%08X words=%u dir=%s step=%s\n",
                channel, c.madr, c.bcr, c.chcr, words,
                fromRam ? "RAM->DEV" : "DEV->RAM",
                decrement ? "DEC" : "INC");

    for(uint32_t i=0;i<words;++i){
        if(channel==0){ // RAM -> MDEC input
            if(fromRam) m_mdec.dmaWriteWord(rawRead32(addr));
        } else if(channel==1){ // MDEC output -> RAM
            if(!fromRam) rawWrite32(addr,m_mdec.dmaReadWord());
        } else if(channel==3){ // CDROM -> RAM
            if(!fromRam)
            {
                const uint32_t data = m_cdrom.readDataWord();

                static unsigned cdDmaWordLog = 0;

                if(cdDmaWordLog < 64)
                {
                    tracePrintf(
                        "[CD DMA WORD #%u] RAM=%08X DATA=%08X\n",
                        cdDmaWordLog,
                        static_cast<unsigned>(addr),
                        static_cast<unsigned>(data)
                    );
                    std::fflush(stdout);
                    ++cdDmaWordLog;
                }

                rawWrite32(addr, data);
            }
        } else if(channel==4){ // SPU bi-directional
            if(fromRam) m_spu.dmaWriteWord(rawRead32(addr));
            else rawWrite32(addr,m_spu.dmaReadWord());
        }
        addr = decrement ? ((addr-4u)&0x001FFFFCu) : ((addr+4u)&0x001FFFFCu);
    }
    c.madr=addr;
    c.chcr&=~(1u<<24);
    tracePrintf("[DMA END] ch=%u finalMADR=%08X DICR=%08X\n", channel, c.madr, m_dicr);
    updateDmaIrq(channel);
}

uint8_t PsxMemory::readIo8(uint32_t p)
{
    if(m_cdrom.handles(p)) return m_cdrom.read8(p);
    if(m_sio.handles(p)) return m_sio.read8(p);
    if(m_spu.handles(p)) {
        const uint16_t v=m_spu.read16(p&~1u);
        return uint8_t(v>>((p&1u)*8u));
    }
    const uint32_t h=p&~1u;
    if(h==kIStat||h==kIMask||m_timers.handles(h)){const uint16_t v=readIo16(h);return uint8_t(v>>((p&1u)*8u));}
    if(p>=kIoBase&&p<kIoEnd) return m_io[p-kIoBase];
    return 0;
}

void PsxMemory::writeIo8(uint32_t p,uint8_t v)
{
    if(m_cdrom.handles(p)){m_cdrom.write8(p,v);return;}
    if(m_sio.handles(p)){m_sio.write8(p,v);return;}
    if(m_spu.handles(p)){
        const uint32_t h=p&~1u; uint16_t old=m_spu.read16(h); const uint32_t sh=(p&1u)*8u;
        old=uint16_t((old&~(0xFFu<<sh))|(uint16_t(v)<<sh)); m_spu.write16(h,old); return;
    }
    const uint32_t h=p&~1u;
    if(h==kIStat||h==kIMask||m_timers.handles(h)){uint16_t old=readIo16(h);const uint32_t sh=(p&1u)*8u;old=uint16_t((old&~(0xFFu<<sh))|(uint16_t(v)<<sh));writeIo16(h,old);return;}
    if(p>=kIoBase&&p<kIoEnd)m_io[p-kIoBase]=v;
}

uint16_t PsxMemory::readIo16(uint32_t p)
{
    if(p==kIStat) return m_irq.stat();
    if(p==kIMask) return m_irq.mask();
    if(m_timers.handles(p)) return m_timers.read16(p);
    if(m_spu.handles(p)) return m_spu.read16(p);
    if(m_sio.handles(p)) return m_sio.read16(p);
    return uint16_t(readIo8(p))|(uint16_t(readIo8(p+1))<<8);
}

void PsxMemory::writeIo16(uint32_t p,uint16_t v)
{
    if(p==kIStat){tracePrintf("[IRQ MMIO W16] I_STAT=%04X before=%04X\n", v, m_irq.stat());m_irq.acknowledge(v);tracePrintf("[IRQ MMIO W16] I_STAT after=%04X\n", m_irq.stat());return;}
    if(p==kIMask){tracePrintf("[IRQ MMIO W16] I_MASK=%04X before=%04X\n", v, m_irq.mask());m_irq.setMask(v);tracePrintf("[IRQ MMIO W16] I_MASK after=%04X\n", m_irq.mask());return;}
    if(m_timers.handles(p)){m_timers.write16(p,v);return;}
    if(m_spu.handles(p)){m_spu.write16(p,v);return;}
    if(m_sio.handles(p)){m_sio.write16(p,v);return;}
    writeIo8(p,uint8_t(v));writeIo8(p+1,uint8_t(v>>8));
}

uint32_t PsxMemory::readIo32(uint32_t p)
{
    if(p==kIStat) return m_irq.stat();
    if(p==kIMask) return m_irq.mask();
    if(p==kDicr) return m_dicr;
    const int ch=dmaChannelForReg(p);
    if(ch>=0 && ch!=2 && ch!=6){
        const DmaChannel& c=m_dma[ch]; const unsigned off=(p-kDmaBase)&0x0Fu;
        return off==0?c.madr:(off==4?c.bcr:c.chcr);
    }
    if(m_mdec.handles(p)) return m_mdec.read32(p);
    if(m_sio.handles(p)) return m_sio.read32(p);
    if(m_timers.handles(p) || m_spu.handles(p)) return uint32_t(readIo16(p))|(uint32_t(readIo16(p+2))<<16);
    return uint32_t(readIo16(p))|(uint32_t(readIo16(p+2))<<16);
}

void PsxMemory::writeIo32(uint32_t p,uint32_t v)
{
    if(p==kIStat){tracePrintf("[IRQ MMIO W32] I_STAT=%08X before=%04X\n", v, m_irq.stat());m_irq.acknowledge(uint16_t(v));tracePrintf("[IRQ MMIO W32] I_STAT after=%04X\n", m_irq.stat());return;}
    if(p==kIMask){tracePrintf("[IRQ MMIO W32] I_MASK=%08X before=%04X\n", v, m_irq.mask());m_irq.setMask(uint16_t(v));tracePrintf("[IRQ MMIO W32] I_MASK after=%04X\n", m_irq.mask());return;}
    if(p==kDicr){
        const uint32_t ack=v&0x7F000000u; m_dicr&=~ack;
        m_dicr=(m_dicr&0xFF000000u)|(v&0x00FFFFFFu);
        if((m_dicr&0x7F000000u)==0)m_dicr&=~(1u<<31);
        return;
    }
    const int ch=dmaChannelForReg(p);
    if(ch>=0 && ch!=2 && ch!=6){
        DmaChannel& c=m_dma[ch]; const unsigned off=(p-kDmaBase)&0x0Fu;
        if(off==0){ c.madr=v&0x00FFFFFFu; tracePrintf("[DMA REG] ch=%d MADR=%08X\n", ch, c.madr); }
        else if(off==4){ c.bcr=v; tracePrintf("[DMA REG] ch=%d BCR=%08X\n", ch, c.bcr); }
        else { c.chcr=v; tracePrintf("[DMA REG] ch=%d CHCR=%08X\n", ch, c.chcr); runDma(unsigned(ch)); }
        return;
    }
    if(m_mdec.handles(p)){m_mdec.write32(p,v);return;}
    if(m_sio.handles(p)){m_sio.write32(p,v);return;}
    if(m_timers.handles(p)){m_timers.write16(p,uint16_t(v));return;}
    if(m_spu.handles(p)){m_spu.write16(p,uint16_t(v));m_spu.write16(p+2,uint16_t(v>>16));return;}
    writeIo16(p,uint16_t(v));writeIo16(p+2,uint16_t(v>>16));
}

uint8_t PsxMemory::read8(uint32_t addr)
{
    const uint32_t p=physical(addr),base=p&~3u;
    if(isGpuMmio32(base)){const uint32_t v=m_gpuMmio.read32(base);return uint8_t(v>>((p&3u)*8u));}
    if(isCoreIo(p)) return readIo8(p);
    if(p>=kIoBase&&p<kIoEnd)return readIo8(p);
    return rawRead8(p);
}

uint16_t PsxMemory::read16(uint32_t addr)
{
    const uint32_t p=physical(addr),base=p&~3u;
    if((p&3u)<=2u&&isGpuMmio32(base)){const uint32_t v=m_gpuMmio.read32(base);return uint16_t(v>>((p&2u)*8u));}
    if(isCoreIo(p)) return readIo16(p);
    return uint16_t(read8(addr))|(uint16_t(read8(addr+1))<<8);
}

uint32_t PsxMemory::read32(uint32_t addr)
{
    const uint32_t p=physical(addr);
    if((p&3u)==0&&isGpuMmio32(p))return m_gpuMmio.read32(p);
    if(isCoreIo(p))return readIo32(p);
    return uint32_t(read16(addr))|(uint32_t(read16(addr+2))<<16);
}

void PsxMemory::write8(uint32_t addr,uint8_t value)
{
    const uint32_t p=physical(addr),base=p&~3u;
    if(isGpuMmio32(base)){uint32_t old=m_gpuMmio.read32(base);const uint32_t sh=(p&3u)*8u;old=(old&~(0xFFu<<sh))|(uint32_t(value)<<sh);m_gpuMmio.write32(base,old);return;}
    if(isCoreIo(p)|| (p>=kIoBase&&p<kIoEnd)){writeIo8(p,value);return;}
    rawWrite8(p,value);
}

void PsxMemory::write16(uint32_t addr,uint16_t value)
{
    const uint32_t p=physical(addr),base=p&~3u;
    if((p&3u)<=2u&&isGpuMmio32(base)){uint32_t old=m_gpuMmio.read32(base);const uint32_t sh=(p&2u)*8u;old=(old&~(0xFFFFu<<sh))|(uint32_t(value)<<sh);m_gpuMmio.write32(base,old);return;}
    if(isCoreIo(p)){writeIo16(p,value);return;}
    write8(addr,uint8_t(value));write8(addr+1,uint8_t(value>>8));
}

void PsxMemory::write32(uint32_t addr,uint32_t value)
{
    const uint32_t p=physical(addr);
    if((p&3u)==0&&isGpuMmio32(p)){
        m_gpuMmio.write32(p,value);
        if(p==psxgpu::PsxGpuMmio::DMA2_CHCR&&(value&(1u<<24)))updateDmaIrq(2);
        if(p==psxgpu::PsxGpuMmio::DMA6_CHCR&&(value&(1u<<24)))updateDmaIrq(6);
        return;
    }
    if(isCoreIo(p)){writeIo32(p,value);return;}
    write16(addr,uint16_t(value));write16(addr+2,uint16_t(value>>16));
}

void PsxMemory::loadBytes(uint32_t guestAddress,const uint8_t* data,std::size_t size){for(std::size_t i=0;i<size;++i)rawWrite8(guestAddress+uint32_t(i),data[i]);}

bool PsxMemory::loadBiosRom(const std::string& path)
{
    std::ifstream f(path.c_str(),std::ios::binary);
    if(!f) return false;
    f.read(reinterpret_cast<char*>(m_biosRom.data()),BiosRomSize);
    if(f.gcount()!=static_cast<std::streamsize>(BiosRomSize)) return false;
    char extra=0;
    if(f.read(&extra,1)) return false;
    m_biosLoaded=true;
    tracePrintf("[BIOS ROM] loaded %s size=%u base=%08X\n",
                path.c_str(),(unsigned)BiosRomSize,(unsigned)kBiosBase);
    return true;
}

void PsxMemory::clearBiosRom()
{
    m_biosRom.fill(0);
    m_biosLoaded=false;
}

} // namespace psxrecomp
