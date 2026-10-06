#include "psxrecomp/psx_runtime.h"
#include "psxrecomp/trace.h"
#include "decoder.h"

#include <climits>

namespace psxrecomp {

namespace {
const uint32_t kCpuClock = 33868800u;
// NTSC 320-pixel dotclock. GPU mode-dependent clocks can be added later; this
// is sufficient to drive Timer0 at the correct order/rate for typical games.
const uint32_t kDotClock320 = 6652800u;
const uint32_t kCpuCyclesPerScanline = 2172u;
const unsigned kNtscScanlines = 263u;
const unsigned kVblankStartLine = 240u;

const uint32_t kBiosPhysBase = 0x1FC00000u;
const uint32_t kBiosPhysEnd  = 0x1FC80000u;

static bool isOpenBiosPc(uint32_t pc)
{
    const uint32_t p=pc&0x1FFFFFFFu;
    return p>=kBiosPhysBase && p<kBiosPhysEnd;
}

static uint32_t biosLwl(PsxMemory& mem,uint32_t a,uint32_t rt)
{
    const uint32_t w=mem.read32(a&~3u);
    switch(a&3u){
        case 0:return (rt&0x00FFFFFFu)|(w<<24);
        case 1:return (rt&0x0000FFFFu)|(w<<16);
        case 2:return (rt&0x000000FFu)|(w<<8);
        default:return w;
    }
}

static uint32_t biosLwr(PsxMemory& mem,uint32_t a,uint32_t rt)
{
    const uint32_t w=mem.read32(a&~3u);
    switch(a&3u){
        case 0:return w;
        case 1:return (rt&0xFF000000u)|(w>>8);
        case 2:return (rt&0xFFFF0000u)|(w>>16);
        default:return (rt&0xFFFFFF00u)|(w>>24);
    }
}

static void biosSwl(PsxMemory& mem,uint32_t a,uint32_t rt)
{
    const uint32_t q=a&~3u;
    uint32_t w=mem.read32(q);
    switch(a&3u){
        case 0:w=(w&0xFFFFFF00u)|(rt>>24);break;
        case 1:w=(w&0xFFFF0000u)|(rt>>16);break;
        case 2:w=(w&0xFF000000u)|(rt>>8);break;
        default:w=rt;break;
    }
    mem.write32(q,w);
}

static void biosSwr(PsxMemory& mem,uint32_t a,uint32_t rt)
{
    const uint32_t q=a&~3u;
    uint32_t w=mem.read32(q);
    switch(a&3u){
        case 0:w=rt;break;
        case 1:w=(w&0x000000FFu)|(rt<<8);break;
        case 2:w=(w&0x0000FFFFu)|(rt<<16);break;
        default:w=(w&0x00FFFFFFu)|(rt<<24);break;
    }
    mem.write32(q,w);
}

static void biosDivSigned(r3k::CpuState& c,uint32_t a,uint32_t b)
{
    const int32_t x=static_cast<int32_t>(a);
    const int32_t y=static_cast<int32_t>(b);
    if(y==0){ c.lo=x>=0?0xFFFFFFFFu:1u; c.hi=a; return; }
    if(x==INT_MIN && y==-1){ c.lo=static_cast<uint32_t>(x); c.hi=0; return; }
    c.lo=static_cast<uint32_t>(x/y);
    c.hi=static_cast<uint32_t>(x%y);
}

static void biosDivUnsigned(r3k::CpuState& c,uint32_t a,uint32_t b)
{
    if(!b){ c.lo=0xFFFFFFFFu; c.hi=a; return; }
    c.lo=a/b;
    c.hi=a%b;
}

static bool biosExecNonControl(r3k::CpuState& c,PsxMemory& mem,PsxGte& gte,const Decoded& d)
{
    const int32_t simm=static_cast<int16_t>(d.imm);
    const uint32_t ea=c.gpr[d.rs]+static_cast<uint32_t>(simm);
    if(d.raw==0) return true;

    if(d.op==0){
        switch(d.funct){
            case 0x00: if(d.rd)c.gpr[d.rd]=c.gpr[d.rt]<<d.sa; return true;
            case 0x02: if(d.rd)c.gpr[d.rd]=c.gpr[d.rt]>>d.sa; return true;
            case 0x03: if(d.rd)c.gpr[d.rd]=static_cast<uint32_t>(static_cast<int32_t>(c.gpr[d.rt])>>d.sa); return true;
            case 0x04: if(d.rd)c.gpr[d.rd]=c.gpr[d.rt]<<(c.gpr[d.rs]&31u); return true;
            case 0x06: if(d.rd)c.gpr[d.rd]=c.gpr[d.rt]>>(c.gpr[d.rs]&31u); return true;
            case 0x07: if(d.rd)c.gpr[d.rd]=static_cast<uint32_t>(static_cast<int32_t>(c.gpr[d.rt])>>(c.gpr[d.rs]&31u)); return true;
            case 0x10: if(d.rd)c.gpr[d.rd]=c.hi; return true;
            case 0x11: c.hi=c.gpr[d.rs]; return true;
            case 0x12: if(d.rd)c.gpr[d.rd]=c.lo; return true;
            case 0x13: c.lo=c.gpr[d.rs]; return true;
            case 0x18: {
                const int64_t q=static_cast<int64_t>(static_cast<int32_t>(c.gpr[d.rs]))*
                                static_cast<int64_t>(static_cast<int32_t>(c.gpr[d.rt]));
                c.lo=static_cast<uint32_t>(q);
                c.hi=static_cast<uint32_t>(static_cast<uint64_t>(q)>>32);
                return true;
            }
            case 0x19: {
                const uint64_t q=static_cast<uint64_t>(c.gpr[d.rs])*static_cast<uint64_t>(c.gpr[d.rt]);
                c.lo=static_cast<uint32_t>(q);
                c.hi=static_cast<uint32_t>(q>>32);
                return true;
            }
            case 0x1A: biosDivSigned(c,c.gpr[d.rs],c.gpr[d.rt]); return true;
            case 0x1B: biosDivUnsigned(c,c.gpr[d.rs],c.gpr[d.rt]); return true;
            case 0x20: case 0x21: if(d.rd)c.gpr[d.rd]=c.gpr[d.rs]+c.gpr[d.rt]; return true;
            case 0x22: case 0x23: if(d.rd)c.gpr[d.rd]=c.gpr[d.rs]-c.gpr[d.rt]; return true;
            case 0x24: if(d.rd)c.gpr[d.rd]=c.gpr[d.rs]&c.gpr[d.rt]; return true;
            case 0x25: if(d.rd)c.gpr[d.rd]=c.gpr[d.rs]|c.gpr[d.rt]; return true;
            case 0x26: if(d.rd)c.gpr[d.rd]=c.gpr[d.rs]^c.gpr[d.rt]; return true;
            case 0x27: if(d.rd)c.gpr[d.rd]=~(c.gpr[d.rs]|c.gpr[d.rt]); return true;
            case 0x2A: if(d.rd)c.gpr[d.rd]=static_cast<int32_t>(c.gpr[d.rs])<static_cast<int32_t>(c.gpr[d.rt]); return true;
            case 0x2B: if(d.rd)c.gpr[d.rd]=c.gpr[d.rs]<c.gpr[d.rt]; return true;
            default: return false;
        }
    }

    switch(d.op){
        case 0x08: case 0x09: if(d.rt)c.gpr[d.rt]=c.gpr[d.rs]+static_cast<uint32_t>(simm); return true;
        case 0x0A: if(d.rt)c.gpr[d.rt]=static_cast<int32_t>(c.gpr[d.rs])<simm; return true;
        case 0x0B: if(d.rt)c.gpr[d.rt]=c.gpr[d.rs]<static_cast<uint32_t>(simm); return true;
        case 0x0C: if(d.rt)c.gpr[d.rt]=c.gpr[d.rs]&d.imm; return true;
        case 0x0D: if(d.rt)c.gpr[d.rt]=c.gpr[d.rs]|d.imm; return true;
        case 0x0E: if(d.rt)c.gpr[d.rt]=c.gpr[d.rs]^d.imm; return true;
        case 0x0F: if(d.rt)c.gpr[d.rt]=static_cast<uint32_t>(d.imm)<<16; return true;
        case 0x20: if(d.rt)c.gpr[d.rt]=static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(mem.read8(ea)))); return true;
        case 0x21: if(d.rt)c.gpr[d.rt]=static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(mem.read16(ea)))); return true;
        case 0x22: if(d.rt)c.gpr[d.rt]=biosLwl(mem,ea,c.gpr[d.rt]); return true;
        case 0x23: if(d.rt)c.gpr[d.rt]=mem.read32(ea); return true;
        case 0x24: if(d.rt)c.gpr[d.rt]=mem.read8(ea); return true;
        case 0x25: if(d.rt)c.gpr[d.rt]=mem.read16(ea); return true;
        case 0x26: if(d.rt)c.gpr[d.rt]=biosLwr(mem,ea,c.gpr[d.rt]); return true;
        case 0x28: mem.write8(ea,static_cast<uint8_t>(c.gpr[d.rt])); return true;
        case 0x29: mem.write16(ea,static_cast<uint16_t>(c.gpr[d.rt])); return true;
        case 0x2A: biosSwl(mem,ea,c.gpr[d.rt]); return true;
        case 0x2B: mem.write32(ea,c.gpr[d.rt]); return true;
        case 0x2E: biosSwr(mem,ea,c.gpr[d.rt]); return true;
        case 0x10:
            if(d.rs==0x00){ if(d.rt)c.gpr[d.rt]=c.cop0[d.rd]; return true; }
            if(d.rs==0x04){ c.cop0[d.rd]=c.gpr[d.rt]; return true; }
            if((d.raw&0x01FFFFFFu)==0x10u){
                const uint32_t low=c.cop0[12]&0x3Fu;
                c.cop0[12]=(c.cop0[12]&~0x3Fu)|((low>>2)&0x0Fu);
                return true;
            }
            return false;
        case 0x12:
            if(d.rs==0x00){ if(d.rt)c.gpr[d.rt]=gte.read_data(d.rd); return true; }
            if(d.rs==0x02){ if(d.rt)c.gpr[d.rt]=gte.read_ctrl(d.rd); return true; }
            if(d.rs==0x04){ gte.write_data(d.rd,c.gpr[d.rt]); return true; }
            if(d.rs==0x06){ gte.write_ctrl(d.rd,c.gpr[d.rt]); return true; }
            if(d.rs&0x10){ gte.execute(d.raw); return true; }
            return false;
        case 0x32: gte.write_data(d.rt,mem.read32(ea)); return true;
        case 0x3A: mem.write32(ea,gte.read_data(d.rt)); return true;
        default: return false;
    }
}

}

PsxRuntime::PsxRuntime()
    : m_gpu(),
      m_irq(),
      m_timers(m_irq),
      m_spu(m_irq),
      m_cdrom(m_irq),
      m_mdec(),
      m_pad(m_irq),
      m_memory(m_gpu,m_irq,m_timers,m_cdrom,m_mdec,m_spu,m_pad),
      m_gte(),
      m_bios(m_memory,m_irq,m_timers,m_cdrom,m_pad,m_gpu)
{
    m_cdrom.setAudioSink([this](int16_t l,int16_t r){ m_spu.pushCdSample(l,r); });
    reset();
}

void PsxRuntime::reset()
{
    tracePrintf("[RUNTIME] reset\n");
    m_cpu = r3k::CpuState();
    m_memory.clear();
    m_gpu.reset();
    m_irq.reset();
    m_timers.reset();
    m_spu.reset();
    m_cdrom.reset();
    m_mdec.reset();
    m_pad.reset();
    m_gte.reset();
    m_bios.reset();
    m_scanlineCycles=0;
    m_dotClockNumerator=0;
    m_scanline=0;
    m_inVblank=false;
    m_timers.setVblank(false);

    // Typical post-BIOS usable state for a PS-X EXE: GTE/COP2 enabled and
    // hardware interrupt line 2 unmasked at COP0. Games may overwrite this.
    m_cpu.cop0[12]=0x40000401u;
}

void PsxRuntime::loadExecutable(const PsxExeImage& image)
{
    tracePrintf("[RUNTIME] loadExecutable entry=%08X load=%08X size=%u\n", image.initial_pc, image.load_address, (unsigned)image.payload.size());
    reset();

    // A PS-X EXE normally starts after the retail BIOS has already initialized
    // the CD-ROM subsystem. Direct EXE loading skips that boot sequence.
    m_bios.initializeCdrom();
    if (!image.payload.empty())
        m_memory.loadBytes(image.load_address, image.payload.data(), image.payload.size());

    m_cpu.pc = image.initial_pc;
    m_cpu.gpr[28] = image.initial_gp;
    if (image.stack_base != 0) {
        const uint32_t sp = image.stack_base + image.stack_offset;
        m_cpu.gpr[29] = sp;
        m_cpu.gpr[30] = sp;
    }
}

bool PsxRuntime::mountDisc(const std::string& path)
{
    const bool ok = m_cdrom.mount(path);
    tracePrintf("[RUNTIME] mountDisc path=%s result=%d\n", path.c_str(), ok ? 1 : 0);
    return ok;
}

bool PsxRuntime::enableOpenBios(const std::string& path)
{
    if(!m_memory.loadBiosRom(path)){
        tracePrintf("[RUNTIME] OpenBIOS load failed path=%s; keeping HLE backend\n",path.c_str());
        m_biosBackend=BiosBackendHle;
        return false;
    }
    m_biosBackend=BiosBackendOpenBios;
    tracePrintf("[RUNTIME] BIOS backend=OpenBIOS ROM mapped at 1FC00000/BFC00000; HLE compatibility retained\n");
    return true;
}

void PsxRuntime::useHleBios()
{
    m_biosBackend=BiosBackendHle;
    tracePrintf("[RUNTIME] BIOS backend=existing HLE\n");
}

void PsxRuntime::advance(uint32_t cpuCycles)
{
    if(cpuCycles==0)cpuCycles=1;

    // CPU-clocked timers first; external dotclock/HBlank paths are fed below.
    m_timers.tick(cpuCycles);

    m_dotClockNumerator += uint64_t(cpuCycles) * kDotClock320;
    const uint32_t dots = uint32_t(m_dotClockNumerator / kCpuClock);
    m_dotClockNumerator %= kCpuClock;
    if(dots)m_timers.tickDotClock(dots);

    m_scanlineCycles += cpuCycles;
    while(m_scanlineCycles >= kCpuCyclesPerScanline){
        m_scanlineCycles -= kCpuCyclesPerScanline;
        m_timers.onHblank();
        ++m_scanline;
        if(m_scanline == kVblankStartLine && !m_inVblank)
        {
            m_inVblank = true;

            m_timers.setVblank(true);

            // GPUSTAT bit 31:
            // alterna o campo de video uma vez por VBlank.
            static bool videoField = false;
            videoField = !videoField;

            m_gpu.setVideoField(videoField);

            tracePrintf("[VBLANK] pc=%08X cycles=%llu scanline=%u field=%d\n", m_cpu.pc, (unsigned long long)m_cpu.cycles, m_scanline, videoField ? 1 : 0);
            m_irq.request(IrqController::VBlank);
        }
        if(m_scanline >= kNtscScanlines){
            m_scanline=0;
            if(m_inVblank){
                m_inVblank=false;
                m_timers.setVblank(false);
            }
        }
    }

    m_cdrom.tick(cpuCycles);
    m_spu.tick(cpuCycles);

    m_irq.updateCop0(m_cpu);
    m_irq.takeInterrupt(m_cpu);
    m_irq.updateCop0(m_cpu);
}


bool PsxRuntime::stepOpenBios()
{
    if(m_biosBackend!=BiosBackendOpenBios || !m_memory.hasBiosRom() || !isOpenBiosPc(m_cpu.pc))
        return false;

    const uint32_t pc=m_cpu.pc;
    const Decoded d=decode(m_memory.read32(pc));
    ++m_cpu.cycles;
    m_cpu.gpr[0]=0;

    static unsigned traceCount=0;
    if(traceCount<256u){
        tracePrintf("[OPENBIOS PC] pc=%08X raw=%08X %s\n",
                    (unsigned)pc,(unsigned)d.raw,disasm(pc,d).c_str());
        ++traceCount;
    }

    bool control=false;
    if(d.op==0 && (d.funct==0x08 || d.funct==0x09)) control=true;
    if(d.op==0x01 || d.op==0x02 || d.op==0x03 || (d.op>=0x04 && d.op<=0x07)) control=true;
    if(d.op==0x12 && d.rs==0x08) control=true;

    if(!control){
        if(!biosExecNonControl(m_cpu,m_memory,m_gte,d)){
            tracePrintf("[OPENBIOS UNSUPPORTED] pc=%08X raw=%08X\n",(unsigned)pc,(unsigned)d.raw);
            return false;
        }
        m_cpu.pc=pc+4u;
        m_cpu.gpr[0]=0;
        return true;
    }

    uint32_t next=pc+8u;
    bool take=false;
    uint32_t target=next;

    if(d.op==0){
        target=m_cpu.gpr[d.rs];
        take=true;
        if(d.funct==0x09 && d.rd) m_cpu.gpr[d.rd]=pc+8u;
    } else if(d.op==0x02 || d.op==0x03){
        target=((pc+4u)&0xF0000000u)|(d.target<<2);
        take=true;
        if(d.op==0x03) m_cpu.gpr[31]=pc+8u;
    } else if(d.op==0x04){
        take=m_cpu.gpr[d.rs]==m_cpu.gpr[d.rt];
        target=pc+4u+(static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(d.imm)))<<2);
    } else if(d.op==0x05){
        take=m_cpu.gpr[d.rs]!=m_cpu.gpr[d.rt];
        target=pc+4u+(static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(d.imm)))<<2);
    } else if(d.op==0x06){
        take=static_cast<int32_t>(m_cpu.gpr[d.rs])<=0;
        target=pc+4u+(static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(d.imm)))<<2);
    } else if(d.op==0x07){
        take=static_cast<int32_t>(m_cpu.gpr[d.rs])>0;
        target=pc+4u+(static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(d.imm)))<<2);
    } else if(d.op==0x01){
        const int32_t v=static_cast<int32_t>(m_cpu.gpr[d.rs]);
        if(d.rt==0x00 || d.rt==0x10) take=v<0;
        else if(d.rt==0x01 || d.rt==0x11) take=v>=0;
        else {
            tracePrintf("[OPENBIOS UNSUPPORTED] pc=%08X REGIMM raw=%08X\n",(unsigned)pc,(unsigned)d.raw);
            return false;
        }
        if(d.rt==0x10 || d.rt==0x11) m_cpu.gpr[31]=pc+8u;
        target=pc+4u+(static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(d.imm)))<<2);
    } else if(d.op==0x12 && d.rs==0x08){
        // PS1 GTE condition flag is treated as false, matching generated game code.
        take=(d.rt&1u)==0u;
        target=pc+4u+(static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(d.imm)))<<2);
    }

    const uint32_t delayPc=pc+4u;
    const Decoded delay=decode(m_memory.read32(delayPc));
    ++m_cpu.cycles;
    if(!biosExecNonControl(m_cpu,m_memory,m_gte,delay)){
        tracePrintf("[OPENBIOS DELAY UNSUPPORTED] pc=%08X raw=%08X\n",
                    (unsigned)delayPc,(unsigned)delay.raw);
        return false;
    }

    m_cpu.pc=take?target:next;
    m_cpu.gpr[0]=0;
    return true;
}

bool PsxRuntime::handleHle()
{
    const uint32_t pcBefore = m_cpu.pc;
    if(stepOpenBios()) return true;
    // Compatibility mode is intentionally retained even with OpenBIOS mapped:
    // generated game code currently cannot execute arbitrary ROM blocks yet.
    // A0/B0/C0 and exception vectors therefore continue through the proven HLE
    // path while software can read the real OpenBIOS image from BIOS address space.
    if(m_biosBackend==BiosBackendOpenBios && !m_memory.hasBiosRom())
        m_biosBackend=BiosBackendHle;
    if(m_bios.handleVector(m_cpu)){ tracePrintf("[HLE] vector pc=%08X -> %08X cycles=%llu\n", pcBefore, m_cpu.pc, (unsigned long long)m_cpu.cycles); return true; }
    if(m_bios.handleExceptionVector(m_cpu)){ tracePrintf("[HLE] exception pc=%08X -> %08X cycles=%llu\n", pcBefore, m_cpu.pc, (unsigned long long)m_cpu.cycles); return true; }
    return false;
}

} // namespace psxrecomp
