#include "psxrecomp/bios_hle.h"
#include "psxrecomp/trace.h"
#include "psxgpu/psx_gpu_mmio.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace psxrecomp {

BiosHle::BiosHle(PsxMemory& memory, IrqController& irq, PsxTimers& timers, PsxCdrom& cdrom, PsxPadSio& pad, psxgpu::PsxGpu& gpu)
    : m_mem(memory), m_irq(irq), m_timers(timers), m_cdrom(cdrom), m_pad(pad), m_gpu(gpu) {}

void BiosHle::reset()
{
    m_events.fill(Event{});
    m_autoAck.fill(true);
    m_intRpHeads.fill(0);
    m_padBuf1=m_padBuf2=m_padButtonDest=0; m_padSize1=m_padSize2=0; m_padEnabled=false; m_clearPad=true;
    m_eventCallbackActive=false; m_eventCallbackFunc=0; m_eventResumeState=r3k::CpuState();
    m_irqChainActive=false; m_irqResumeState=r3k::CpuState();
    m_irqChainPriority=0; m_irqChainStruct=0; m_irqChainNext=0;
    m_irqChainSecond=0; m_irqChainFunc=0; m_irqChainInSecond=false;
}

uint32_t BiosHle::arg(const r3k::CpuState& cpu,unsigned index) const
{
    if(index<4) return cpu.gpr[4+index];
    return m_mem.read32(cpu.gpr[29]+0x10u+(index-4u)*4u);
}

std::string BiosHle::readString(uint32_t addr,std::size_t maxLen) const
{
    std::string out;
    out.reserve(std::min<std::size_t>(maxLen,256));
    for(std::size_t i=0;i<maxLen;++i){
        const char c=static_cast<char>(const_cast<PsxMemory&>(m_mem).read8(addr+uint32_t(i)));
        if(!c) break;
        out.push_back(c);
    }
    return out;
}

void BiosHle::writeBytes(uint32_t dst,uint32_t src,uint32_t len)
{
    if(dst==0||len>0x7FFFFFFFu)return;
    if(dst<src){for(uint32_t i=0;i<len;++i)m_mem.write8(dst+i,m_mem.read8(src+i));}
    else if(dst>src){for(uint32_t i=len;i>0;--i)m_mem.write8(dst+i-1,m_mem.read8(src+i-1));}
}
void BiosHle::fillBytes(uint32_t dst,uint8_t value,uint32_t len){if(dst==0||len>0x7FFFFFFFu)return;for(uint32_t i=0;i<len;++i)m_mem.write8(dst+i,value);}

uint32_t BiosHle::openEvent(uint32_t cls,uint32_t spec,uint32_t mode,uint32_t func)
{
    for(std::size_t i=0;i<m_events.size();++i)if(!m_events[i].used){m_events[i].used=true; m_events[i].enabled=false; m_events[i].ready=false; m_events[i].cls=cls; m_events[i].spec=spec; m_events[i].mode=mode; m_events[i].func=func; const uint32_t h=0xF1000000u+uint32_t(i); tracePrintf("[BIOS EVENT OPEN] h=%08X cls=%08X spec=%08X mode=%04X func=%08X\n",h,cls,spec,mode,func); return h;}
    return 0xFFFFFFFFu;
}
BiosHle::Event* BiosHle::eventFromHandle(uint32_t h){if(h<0xF1000000u)return nullptr;const uint32_t i=h-0xF1000000u;return i<m_events.size()&&m_events[i].used?&m_events[i]:nullptr;}
uint32_t BiosHle::deliverEvent(uint32_t cls,uint32_t spec)
{
    uint32_t callback = 0;
    for(auto& e:m_events){
        if(!e.used || !e.enabled || e.cls!=cls || e.spec!=spec) continue;
        tracePrintf("[BIOS EVENT] deliver cls=%08X spec=%08X mode=%04X func=%08X\n",
                    cls,spec,e.mode,e.func);
        if(e.mode==0x2000u){
            e.ready=true;
        } else if(e.mode==0x1000u && e.func!=0 && callback==0){
            callback=e.func;
        }
    }
    return callback;
}

void BiosHle::beginEventCallback(r3k::CpuState& c,const r3k::CpuState& resumeState,uint32_t func)
{
    if(!func || m_eventCallbackActive) return;
    m_eventResumeState=resumeState;
    m_eventCallbackActive=true;
    m_eventCallbackFunc=func;
    c.pc=func;
    c.gpr[31]=kEventCallbackTrampoline;
    c.gpr[2]=1;
    tracePrintf("[BIOS EVENT CALLBACK BEGIN] func=%08X resumePC=%08X savedRA=%08X sp=%08X cycles=%llu\n",
                func,resumeState.pc,resumeState.gpr[31],resumeState.gpr[29],
                (unsigned long long)c.cycles);
}

bool BiosHle::finishEventCallback(r3k::CpuState& c)
{
    if(!m_eventCallbackActive) return false;
    const uint64_t cycles=c.cycles;
    const uint32_t func=m_eventCallbackFunc;
    c=m_eventResumeState;
    c.cycles=cycles;
    m_eventCallbackActive=false;
    m_eventCallbackFunc=0;
    tracePrintf("[BIOS EVENT CALLBACK END] func=%08X resumePC=%08X restoredRA=%08X cycles=%llu\n",
                func,c.pc,c.gpr[31],(unsigned long long)c.cycles);
    return true;
}

bool BiosHle::enqueueIntRp(uint32_t priority,uint32_t struc)
{
    if(priority>=m_intRpHeads.size() || struc==0) return false;
    m_mem.write32(struc,m_intRpHeads[priority]);
    m_intRpHeads[priority]=struc;
    tracePrintf("[BIOS SysEnqIntRP] prio=%u struc=%08X func2=%08X func1=%08X\n",
        (unsigned)priority,(unsigned)struc,(unsigned)m_mem.read32(struc+4u),(unsigned)m_mem.read32(struc+8u));
    return true;
}

bool BiosHle::dequeueIntRp(uint32_t priority,uint32_t struc)
{
    if(priority>=m_intRpHeads.size() || struc==0) return false;
    uint32_t* head=&m_intRpHeads[priority];
    uint32_t cur=*head, prev=0;
    for(unsigned guard=0;cur && guard<256;++guard){
        const uint32_t next=m_mem.read32(cur);
        if(cur==struc){
            if(prev) m_mem.write32(prev,next); else *head=next;
            m_mem.write32(cur,0);
            tracePrintf("[BIOS SysDeqIntRP] prio=%u struc=%08X ok=1\n",(unsigned)priority,(unsigned)struc);
            return true;
        }
        prev=cur; cur=next;
    }
    tracePrintf("[BIOS SysDeqIntRP] prio=%u struc=%08X ok=0\n",(unsigned)priority,(unsigned)struc);
    return false;
}

bool BiosHle::startInterruptChain(r3k::CpuState& c,const r3k::CpuState& resumeState)
{
    if(m_irqChainActive || m_eventCallbackActive) return false;

    m_irqResumeState=resumeState;
    m_irqChainPriority=0;
    m_irqChainStruct=0;
    m_irqChainNext=0;
    m_irqChainSecond=0;
    m_irqChainFunc=0;
    m_irqChainInSecond=false;

    for(uint32_t p=0;p<m_intRpHeads.size();++p){
        const uint32_t s=m_intRpHeads[p];
        if(!s) continue;
        const uint32_t first=m_mem.read32(s+8u);
        if(!first) continue;

        m_irqChainActive=true;
        m_irqChainPriority=p;
        m_irqChainStruct=s;
        m_irqChainNext=m_mem.read32(s+0u);
        m_irqChainSecond=m_mem.read32(s+4u);
        m_irqChainFunc=first;
        c.pc=first;
        c.gpr[31]=kEventCallbackTrampoline;
        c.gpr[2]=0;
        tracePrintf("[BIOS IRQ CHAIN BEGIN] prio=%u struc=%08X first=%08X second=%08X next=%08X\n",
            (unsigned)p,(unsigned)s,(unsigned)first,(unsigned)m_irqChainSecond,(unsigned)m_irqChainNext);
        return true;
    }
    return false;
}

bool BiosHle::continueInterruptChain(r3k::CpuState& c)
{
    if(!m_irqChainActive) return false;

    const uint32_t result=c.gpr[2];
    tracePrintf("[BIOS IRQ CHAIN RETURN] prio=%u struc=%08X func=%08X phase=%s v0=%08X\n",
        (unsigned)m_irqChainPriority,(unsigned)m_irqChainStruct,(unsigned)m_irqChainFunc,
        m_irqChainInSecond?"SECOND":"FIRST",(unsigned)result);

    if(!m_irqChainInSecond && result!=0 && m_irqChainSecond!=0){
        m_irqChainInSecond=true;
        m_irqChainFunc=m_irqChainSecond;
        c.pc=m_irqChainSecond;
        c.gpr[31]=kEventCallbackTrampoline;
        tracePrintf("[BIOS IRQ CHAIN SECOND] prio=%u struc=%08X func=%08X\n",
            (unsigned)m_irqChainPriority,(unsigned)m_irqChainStruct,(unsigned)m_irqChainSecond);
        return true;
    }

    uint32_t next=m_irqChainNext;
    uint32_t priority=m_irqChainPriority;
    for(;;){
        if(next){
            const uint32_t s=next;
            next=m_mem.read32(s+0u);
            const uint32_t first=m_mem.read32(s+8u);
            if(first){
                m_irqChainStruct=s;
                m_irqChainNext=next;
                m_irqChainSecond=m_mem.read32(s+4u);
                m_irqChainFunc=first;
                m_irqChainInSecond=false;
                c.pc=first;
                c.gpr[31]=kEventCallbackTrampoline;
                c.gpr[2]=0;
                tracePrintf("[BIOS IRQ CHAIN NEXT] prio=%u struc=%08X first=%08X second=%08X next=%08X\n",
                    (unsigned)priority,(unsigned)s,(unsigned)first,(unsigned)m_irqChainSecond,(unsigned)next);
                return true;
            }
            continue;
        }

        ++priority;
        if(priority>=m_intRpHeads.size()) break;
        next=m_intRpHeads[priority];
    }

    const uint64_t cycles=c.cycles;
    c=m_irqResumeState;
    c.cycles=cycles;
    m_irqChainActive=false;
    m_irqChainPriority=0; m_irqChainStruct=0; m_irqChainNext=0;
    m_irqChainSecond=0; m_irqChainFunc=0; m_irqChainInSecond=false;
    tracePrintf("[BIOS IRQ CHAIN END] resumePC=%08X cycles=%llu\n",
        (unsigned)c.pc,(unsigned long long)c.cycles);
    return true;
}

void BiosHle::callA(r3k::CpuState& c,uint8_t fn)
{
    switch(fn){
        case 0x0E: c.gpr[2]=uint32_t(std::abs(static_cast<int32_t>(arg(c,0)))); break;
        case 0x2A: writeBytes(arg(c,0),arg(c,1),arg(c,2)); c.gpr[2]=arg(c,0); break; // memcpy
        case 0x2B: fillBytes(arg(c,0),uint8_t(arg(c,1)),arg(c,2)); c.gpr[2]=arg(c,0); break; // memset
        case 0x2C: writeBytes(arg(c,0),arg(c,1),arg(c,2)); c.gpr[2]=arg(c,0); break; // memmove
        case 0x3C: std::putchar(int(arg(c,0)&0xFFu)); c.gpr[2]=arg(c,0)&0xFFu; break;
        case 0x3E: { const auto s=readString(arg(c,0)); std::fputs(s.c_str(),stdout); std::fputc('\n',stdout); c.gpr[2]=0; break; }
        case 0x3F: { // Lightweight printf HLE: print format literally; preserves boot logs safely.
            const auto s=readString(arg(c,0)); std::fputs(s.c_str(),stdout); c.gpr[2]=uint32_t(s.size()); break;
        }
        case 0x44: c.gpr[2]=0; break; // FlushCache
        case 0x48: m_mem.write32(psxgpu::PsxGpuMmio::GP1,arg(c,0)); c.gpr[2]=0; break;
        case 0x49: m_mem.write32(psxgpu::PsxGpuMmio::GP0,arg(c,0)); c.gpr[2]=0; break;
        case 0x4A: { const uint32_t p=arg(c,0),n=arg(c,1); for(uint32_t i=0;i<n;++i)m_mem.write32(psxgpu::PsxGpuMmio::GP0,m_mem.read32(p+i*4)); c.gpr[2]=0; break; }
        case 0x4B: { // GPU linked list via DMA2
            m_mem.write32(psxgpu::PsxGpuMmio::DMA2_MADR,arg(c,0));
            m_mem.write32(psxgpu::PsxGpuMmio::DMA2_CHCR,0x01000401u); c.gpr[2]=0; break;
        }
        case 0x4D: c.gpr[2]=m_mem.read32(psxgpu::PsxGpuMmio::GP1); break;
        case 0x4E: c.gpr[2]=0; break; // gpu_sync: immediate in HLE
        case 0xA5: { // CdReadSector(count, sector, buffer)
            const uint32_t count=arg(c,0),sector=arg(c,1),dst=arg(c,2);
            std::vector<uint8_t> buf(std::size_t(count)*2048u);
            const bool ok=m_cdrom.readUserSectors(sector,count,buf.data());
            if(ok)for(std::size_t i=0;i<buf.size();++i)m_mem.write8(dst+uint32_t(i),buf[i]);
            c.gpr[2]=ok?1u:0u; break;
        }
        case 0xA6: c.gpr[2]=m_cdrom.statusByte(); break;
        case 0x78: c.gpr[2]=1; break; // CdAsyncSeekL accepted; low-level CD controller handles actual seeks.
        case 0x7C: if(arg(c,0))m_mem.write8(arg(c,0),m_cdrom.statusByte()); c.gpr[2]=1; break;
        case 0x7E: { // CdAsyncReadSector(count,dst,mode), from current low-level Setloc in full BIOS; use current data path as success.
            c.gpr[2]=m_cdrom.mounted()?1u:0u; break;
        }
        default: c.gpr[2]=0; break;
    }
}

void BiosHle::callB(r3k::CpuState& c,uint8_t fn)
{
    switch(fn){
        case 0x02: { // init_timer
            const uint32_t t=arg(c,0); if(t<3){m_mem.write16(0x1F801104u+t*0x10u,0);m_mem.write16(0x1F801108u+t*0x10u,uint16_t(arg(c,1)));uint16_t mode=(arg(c,2)&0x10u)?0x49u:0x48u;if((arg(c,2)&1u)==0)mode|=0x100u;if(arg(c,2)&0x1000u)mode|=0x10u;m_mem.write16(0x1F801104u+t*0x10u,mode);c.gpr[2]=1;}else c.gpr[2]=0; break; }
        case 0x03: {const uint32_t t=arg(c,0);c.gpr[2]=t<3?m_mem.read16(0x1F801100u+t*0x10u):0;break;}
        case 0x04: case 0x05: {const uint32_t t=arg(c,0);uint16_t mask=m_irq.mask();uint16_t bit=t<3?uint16_t(1u<<(4u+t)):(t==3?1u:0u);if(fn==0x04)mask|=bit;else mask&=~bit;m_irq.setMask(mask);c.gpr[2]=(fn==0x05||t<3)?1u:0u;break;}
        case 0x06: {const uint32_t t=arg(c,0);if(t<3){m_mem.write16(0x1F801100u+t*0x10u,0);c.gpr[2]=1;}else c.gpr[2]=0;break;}
        case 0x07: deliverEvent(arg(c,0),arg(c,1)); c.gpr[2]=1; break;
        case 0x08: c.gpr[2]=openEvent(arg(c,0),arg(c,1),arg(c,2),arg(c,3)); break;
        case 0x09: if(auto*e=eventFromHandle(arg(c,0)))*e=Event(); c.gpr[2]=1; break;
        case 0x0A: {auto*e=eventFromHandle(arg(c,0));c.gpr[2]=(e&&e->enabled&&e->ready)?1u:0u;if(c.gpr[2])e->ready=false;break;}
        case 0x0B: {auto*e=eventFromHandle(arg(c,0));c.gpr[2]=(e&&e->enabled&&e->ready)?1u:0u;if(c.gpr[2])e->ready=false;break;}
        case 0x0C: {const uint32_t h=arg(c,0);auto*e=eventFromHandle(h);if(e)e->enabled=true;tracePrintf("[BIOS EVENT ENABLE] h=%08X ok=%d\n",h,e?1:0);c.gpr[2]=1;break;}
        case 0x0D: {auto*e=eventFromHandle(arg(c,0));if(e)e->enabled=false;c.gpr[2]=1;break;}
        case 0x12: { // InitPAD2
            m_padBuf1=arg(c,0);m_padSize1=arg(c,1);m_padBuf2=arg(c,2);m_padSize2=arg(c,3);
            if(m_padBuf1)fillBytes(m_padBuf1,0,uint32_t(std::min<uint32_t>(m_padSize1,0x22u)));
            if(m_padBuf2)fillBytes(m_padBuf2,0,uint32_t(std::min<uint32_t>(m_padSize2,0x22u)));
            c.gpr[2]=1;break;}
        case 0x13: m_padEnabled=true;c.gpr[2]=1;break; // StartPAD2
        case 0x14: m_padEnabled=false;c.gpr[2]=1;break; // StopPAD2
        case 0x15: { // PAD_init2
            const uint32_t type=arg(c,0);m_padButtonDest=arg(c,1);
            m_padEnabled=(type==0x20000000u||type==0x20000001u);c.gpr[2]=m_padEnabled?2u:0u;break;}
        case 0x16: { // PAD_dr
            const uint16_t al=uint16_t(~m_pad.pressedButtons());
            const uint16_t rev=uint16_t((al>>8)|(al<<8));
            c.gpr[2]=0xFFFF0000u|rev;if(m_padButtonDest)m_mem.write32(m_padButtonDest,c.gpr[2]);break;}
        case 0x17: { // ReturnFromException
            uint32_t low=c.cop0[12]&0x3Fu;c.cop0[12]=(c.cop0[12]&~0x3Fu)|((low>>2)&0x0Fu);c.pc=c.cop0[14];c.gpr[2]=1;return; }
        case 0x18: c.gpr[2]=1; break; // ResetEntryInt
        case 0x19: c.gpr[2]=1; break; // HookEntryInt accepted by simplified HLE
        case 0x5B: {const bool old=m_clearPad;m_clearPad=arg(c,0)!=0;c.gpr[2]=old?1u:0u;break;}
        default: c.gpr[2]=0; break;
    }
}

void BiosHle::callC(r3k::CpuState& c,uint8_t fn)
{
    switch(fn){
        case 0x00: case 0x01: case 0x07: case 0x08: case 0x09: case 0x0C: c.gpr[2]=0; break;
        case 0x02: c.gpr[2]=enqueueIntRp(arg(c,0),arg(c,1))?1u:0u; break;
        case 0x03: c.gpr[2]=dequeueIntRp(arg(c,0),arg(c,1))?1u:0u; break;
        case 0x0A: {const uint32_t t=arg(c,0);if(t<4){const bool old=m_autoAck[t];m_autoAck[t]=arg(c,1)!=0;c.gpr[2]=old?1u:0u;}else c.gpr[2]=0;break;}
        case 0x0D: c.gpr[2]=0; break;
        default: c.gpr[2]=0; break;
    }
}

bool BiosHle::handleVector(r3k::CpuState& c)
{
    const uint32_t p=c.pc&0x1FFFFFFFu;
    if(p==(kEventCallbackTrampoline&0x1FFFFFFFu) && m_irqChainActive) return continueInterruptChain(c);
    if(p==(kEventCallbackTrampoline&0x1FFFFFFFu) && m_eventCallbackActive) return finishEventCallback(c);
    if(p!=0xA0u&&p!=0xB0u&&p!=0xC0u)return false;
    const uint8_t fn=uint8_t(c.gpr[9]);
    if(p==0xA0u)callA(c,fn);else if(p==0xB0u)callB(c,fn);else callC(c,fn);
    // ReturnFromException (B17) chooses EPC itself; normal BIOS vectors return to RA.
    if(!(p==0xB0u && fn==0x17u)) c.pc=c.gpr[31];
    return true;
}

bool BiosHle::handleExceptionVector(r3k::CpuState& c)
{
    const uint32_t p = c.pc & 0x1FFFFFFFu;

    // Vetores gerais de exceção do R3000A:
    // 0x80000080 -> BEV=0
    // 0xBFC00180 -> BEV=1
    if(p != 0x80u && p != 0x180u)
        return false;

    const uint16_t pending =
        m_irq.stat() & m_irq.mask();

    // Preserve the guest context that must be restored after an IRQ callback.
    r3k::CpuState resumeState = c;
    resumeState.pc = c.cop0[14];
    {
        const uint32_t low = resumeState.cop0[12] & 0x3Fu;
        resumeState.cop0[12] = (resumeState.cop0[12] & ~0x3Fu) | ((low >> 2) & 0x0Fu);
    }
    uint32_t irqCallback = 0;

    tracePrintf(
        "[BIOS EXCEPTION HLE] "
        "pc=%08X cause=%08X status=%08X "
        "epc=%08X pending=%04X cycles=%llu\n",
        static_cast<unsigned>(c.pc),
        static_cast<unsigned>(c.cop0[13]),
        static_cast<unsigned>(c.cop0[12]),
        static_cast<unsigned>(c.cop0[14]),
        static_cast<unsigned>(pending),
        static_cast<unsigned long long>(c.cycles)
    );

    std::fflush(stdout);

    /*
     * TESTE TEMPORARIO DE VBLANK
     *
     * O codigo recompilado em 0x80058E2C faz:
     *
     *     value = *(uint32_t*)0x80072FB0;
     *     value++;
     *     *(uint32_t*)0x80072FB0 = value;
     *
     * Como o BIOS HLE ainda nao executa o callback real registrado
     * pelo jogo, fazemos o incremento aqui temporariamente.
     */
    if(pending & IrqController::VBlank)
    {
        const uint32_t oldCounter =
            m_mem.read32(0x80072FB0u);

        const uint32_t newCounter =
            oldCounter + 1u;

        m_mem.write32(
            0x80072FB0u,
            newCounter
        );

        static unsigned vblankLogCount = 0;

        if(vblankLogCount < 120)
        {
            tracePrintf(
                "[VBLANK HLE] "
                "counter=%u -> %u "
                "cycles=%llu\n",
                static_cast<unsigned>(oldCounter),
                static_cast<unsigned>(newCounter),
                static_cast<unsigned long long>(c.cycles)
            );

            std::fflush(stdout);

            ++vblankLogCount;
        }
    }

    /*
     * Atualizacao do PAD durante VBlank.
     */
    if(
        (pending & IrqController::VBlank) &&
        m_padEnabled
    )
    {
        const uint16_t buttons =
            static_cast<uint16_t>(
                ~m_pad.pressedButtons()
            );

        if(m_padBuf1 && m_padSize1 >= 4)
        {
            m_mem.write8(
                m_padBuf1 + 0,
                0x00
            );

            m_mem.write8(
                m_padBuf1 + 1,
                0x41
            );

            m_mem.write8(
                m_padBuf1 + 2,
                static_cast<uint8_t>(
                    buttons
                )
            );

            m_mem.write8(
                m_padBuf1 + 3,
                static_cast<uint8_t>(
                    buttons >> 8
                )
            );
        }

        if(m_padBuf2 && m_padSize2)
        {
            m_mem.write8(
                m_padBuf2,
                0xFF
            );
        }

        if(m_padButtonDest)
        {
            const uint16_t rev =
                static_cast<uint16_t>(
                    (buttons >> 8) |
                    (buttons << 8)
                );

            m_mem.write32(
                m_padButtonDest,
                0xFFFF0000u | rev
            );
        }
    }

    /*
     * Entrega dos eventos BIOS.
     */
    if(pending & IrqController::VBlank)
    {
        const uint32_t cb=deliverEvent(0xF0000001u,0x1000u); if(!irqCallback) irqCallback=cb;
    }

    if(pending & IrqController::Cdrom)
    {
        const uint32_t cb=deliverEvent(0xF0000003u,0x1000u); if(!irqCallback) irqCallback=cb;
    }

    if(pending & IrqController::Dma)
    {
        const uint32_t cb=deliverEvent(0xF0000004u,0x1000u); if(!irqCallback) irqCallback=cb;
    }

    if(pending & IrqController::Timer0)
    {
        const uint32_t cb=deliverEvent(0xF0000005u,0x1000u); if(!irqCallback) irqCallback=cb;
    }

    if(
        pending &
        (
            IrqController::Timer1 |
            IrqController::Timer2
        )
    )
    {
        const uint32_t cb=deliverEvent(0xF0000006u,0x1000u); if(!irqCallback) irqCallback=cb;
    }

    if(pending & IrqController::Pad)
    {
        const uint32_t cb=deliverEvent(0xF0000008u,0x1000u); if(!irqCallback) irqCallback=cb;
    }

    if(pending & IrqController::Spu)
    {
        const uint32_t cb=deliverEvent(0xF0000009u,0x1000u); if(!irqCallback) irqCallback=cb;
    }

    if(pending & IrqController::Sio)
    {
        const uint32_t cb=deliverEvent(0xF000000Bu,0x1000u); if(!irqCallback) irqCallback=cb;
    }

    /*
     * ACK das IRQs.
     *
     * I_STAT usa bits mantidos em 1.
     * Para reconhecer uma fonte, limpamos seu bit em "keep".
     */
    uint16_t keep = 0x07FFu;

    /*
     * VBlank precisa ser reconhecido neste fallback porque
     * nao estamos executando o exception handler real da BIOS.
     */
    if(pending & IrqController::VBlank)
    {
        keep &= static_cast<uint16_t>(
            ~IrqController::VBlank
        );
    }

    if(
        m_autoAck[0] &&
        (pending & IrqController::Timer0)
    )
    {
        keep &= static_cast<uint16_t>(
            ~IrqController::Timer0
        );
    }

    if(
        m_autoAck[1] &&
        (pending & IrqController::Timer1)
    )
    {
        keep &= static_cast<uint16_t>(
            ~IrqController::Timer1
        );
    }

    if(
        m_autoAck[2] &&
        (pending & IrqController::Timer2)
    )
    {
        keep &= static_cast<uint16_t>(
            ~IrqController::Timer2
        );
    }

    /*
     * No caminho BIOS-HLE atual estas IRQs tambem precisam
     * ser reconhecidas para evitar reentrada infinita.
     */
    keep &= static_cast<uint16_t>(
        ~(
            pending &
            (
                IrqController::Cdrom |
                IrqController::Dma |
                IrqController::Pad |
                IrqController::Spu |
                IrqController::Sio
            )
        )
    );

    /*
     * IMPORTANTE:
     * somente UM acknowledge().
     */
    m_irq.acknowledge(keep);

    /*
     * RFE simplificado.
     *
     * R3000A Status:
     *
     * KUc/IEc <- KUp/IEp
     * KUp/IEp <- KUo/IEo
     *
     * Equivalente ao deslocamento dos 6 bits inferiores.
     */
    const uint32_t low =
        c.cop0[12] & 0x3Fu;

    c.cop0[12] =
        (c.cop0[12] & ~0x3Fu) |
        ((low >> 2) & 0x0Fu);

    /*
     * Retorna para EPC, ou executa primeiro o callback BIOS mode=1000h.
     * O trampoline restaura todo o contexto interrompido depois do jr ra.
     */
    c.pc = c.cop0[14];
    if(pending && startInterruptChain(c,resumeState)){
        tracePrintf("[BIOS IRQ CHAIN DISPATCH] pending=%04X\n",(unsigned)pending);
    } else if(irqCallback){
        beginEventCallback(c,resumeState,irqCallback);
    }

    return true;
}

} // namespace psxrecomp
