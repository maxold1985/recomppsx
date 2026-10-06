#include "psxrecomp/bios_hle.h"
#include "psxrecomp/trace.h"
#include "psxgpu/psx_gpu_mmio.h"

#include <algorithm>
#include <cctype>
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

    m_randSeed=1;
    m_heapBase=0;
    m_heapSize=0;
    m_threads.fill(Thread());
    m_threads[0].used=true;
    m_currentThread=0;
    m_threadSwitchPerformed=false;

    m_files.fill(FileHandle());
    m_find=FindState();
    m_cardStatus[0]=m_cardStatus[1]=1;
    m_cardIgnoreChange[0]=m_cardIgnoreChange[1]=false;
    m_cardInitialized=false;
    m_cardStarted=false;
    m_pendingEventCallback=0;
    formatCard(0);
    formatCard(1);

    m_cdBiosIrqInstalled=false; m_cdLastStatus=0; m_cdLastError=0;
    m_cdAsyncReadActive=false; m_cdAsyncReadRemaining=0; m_cdAsyncReadDst=0; m_cdAsyncReadMode=0;
    m_eventCallbackActive=false; m_eventCallbackFunc=0; m_eventResumeState=r3k::CpuState();
    m_irqChainActive=false; m_irqResumeState=r3k::CpuState();
    m_irqChainPriority=0; m_irqChainStruct=0; m_irqChainNext=0;
    m_irqChainSecond=0; m_irqChainFunc=0; m_irqChainPending=0; m_irqChainInSecond=false;
}

void BiosHle::initializeCdrom()
{
    m_cdBiosIrqInstalled=true;

    // Loading a PS-X EXE directly skips the retail BIOS boot path. The BIOS
    // normally calls _96_init() before transferring control to the executable,
    // so reproduce the externally visible CD decoder setup here.
    m_cdrom.write8(0x1F801800u, 0x01u); // bank 1
    m_cdrom.write8(0x1F801802u, 0x1Fu); // HINTMSK: INT1..INT5/BF flags
    m_cdrom.write8(0x1F801803u, 0x1Fu); // clear stale HINTSTS low bits
    m_cdrom.write8(0x1F801800u, 0x00u); // bank 0
    m_irq.setMask(static_cast<uint16_t>(m_irq.mask() | IrqController::Cdrom));
    tracePrintf("[BIOS CD INIT] post-boot _96_init HINTMSK=1F I_MASK=%04X priority0=HLE\n",
                (unsigned)m_irq.mask());
}

bool BiosHle::serviceRetailCdromIntRp(uint32_t& callback)
{
    const uint16_t pending=static_cast<uint16_t>(m_irq.stat() & m_irq.mask());
    if(!m_cdBiosIrqInstalled || (pending & IrqController::Cdrom)==0)
        return false;

    // _96_init in the retail BIOS leaves a priority-0 CD SysIntRP service
    // installed. Direct PS-X EXE boot has no ROM routine to execute, so HLE
    // FIRST here: IRQ2/CD pending means SECOND must run.
    tracePrintf(
        "[BIOS CD INTRP FIRST] pending=%04X flags=%02X result=1\n",
        (unsigned)pending,(unsigned)m_cdrom.irqFlags());

    // HLE SECOND: consume decoder response/flags and perform the externally
    // visible BIOS CD side effects even when no F0000003 event was opened.
    const uint8_t flags=m_cdrom.irqFlags();
    const uint8_t type=static_cast<uint8_t>(flags & 0x07u);
    uint8_t response0=0;
    uint8_t response1=0;
    if(m_cdrom.responseBytesAvailable()!=0)
        response0=m_cdrom.read8(0x1F801801u);
    if(m_cdrom.responseBytesAvailable()!=0)
        response1=m_cdrom.read8(0x1F801801u);

    if(response0!=0) m_cdLastStatus=response0;
    if(type==5u) m_cdLastError=response1;

    uint32_t eventSpec=0;
    switch(type){
        case 1: eventSpec=0x10u; break;
        case 2: eventSpec=0x20u; break;
        case 3: eventSpec=0x20u; break;
        case 4: eventSpec=0x80u; break;
        case 5: eventSpec=0x8000u; break;
        default: break;
    }

    const uint32_t irqEvent=deliverEvent(0xF0000003u,0x1000u);
    if(!callback) callback=irqEvent;
    if(eventSpec){
        const uint32_t cb=deliverEvent(0xF0000003u,eventSpec);
        if(!callback) callback=cb;
    }

    m_cdrom.acknowledgeInterrupt(0x1Fu);
    m_irq.acknowledge(static_cast<uint16_t>(0x07FFu & ~IrqController::Cdrom));

    tracePrintf(
        "[BIOS CD INTRP SECOND] type=%u flags=%02X status=%02X error=%02X "
        "spec=%04X I_STAT=%04X\n",
        (unsigned)type,(unsigned)flags,(unsigned)m_cdLastStatus,
        (unsigned)m_cdLastError,(unsigned)eventSpec,(unsigned)m_irq.stat());
    return true;
}

bool BiosHle::serviceCdromInterrupt(uint32_t& callback)
{
    const uint16_t pending=static_cast<uint16_t>(m_irq.stat() & m_irq.mask());

    if(!m_cdBiosIrqInstalled || (pending & IrqController::Cdrom)==0)
        return false;

    // Do not steal a raw decoder IRQ unless software actually uses the BIOS
    // CD event interface. PsyQ/libcd can install its own interrupt path and
    // poll/update command state outside OpenEvent/DeliverEvent. Consuming
    // HINTSTS/I_STAT here would make that code wait until its timeout.
    bool haveCdEvent=false;
    for(const auto& e:m_events){
        if(e.used && e.enabled && e.cls==0xF0000003u){
            haveCdEvent=true;
            break;
        }
    }

    // A game that explicitly installs a priority-0 SysIntRP entry also owns
    // the CD-ROM interrupt path.
    if(m_intRpHeads[0]!=0 || !haveCdEvent){
        tracePrintf(
            "[BIOS CD IRQ] defer to guest path priority0=%u biosEvent=%u "
            "flags=%02X I_STAT=%04X\n",
            m_intRpHeads[0]!=0 ? 1u : 0u,
            haveCdEvent ? 1u : 0u,
            (unsigned)m_cdrom.irqFlags(),
            (unsigned)m_irq.stat()
        );
        return false;
    }

    const uint8_t flags=m_cdrom.irqFlags();
    const uint8_t type=static_cast<uint8_t>(flags & 0x07u);
    uint8_t response0=0;
    uint8_t response1=0;

    if(m_cdrom.responseBytesAvailable()!=0)
        response0=m_cdrom.read8(0x1F801801u);
    if(m_cdrom.responseBytesAvailable()!=0)
        response1=m_cdrom.read8(0x1F801801u);

    if(response0!=0)
        m_cdLastStatus=response0;
    if(type==5u)
        m_cdLastError=response1;

    // The BIOS owns a normal IRQ2 event plus CD-specific completion events.
    // INT3 is the acknowledgement used by commands such as GetStat, and
    // INT2 is the later completion used by multi-phase commands.
    uint32_t eventSpec=0;

    if(type==1u && m_cdAsyncReadActive && m_cdAsyncReadRemaining!=0){
        // BIOS asynchronous sector reads transfer decoder data through DMA3.
        // The current CD core exposes 2048-byte user sectors, hence 512 words.
        m_mem.write32(0x1F8010B0u,m_cdAsyncReadDst);
        m_mem.write32(0x1F8010B4u,0x00000200u);
        m_mem.write32(0x1F8010B8u,0x11000000u);

        m_cdAsyncReadDst += 2048u;
        --m_cdAsyncReadRemaining;

        tracePrintf("[BIOS CD DMA3] dst=%08X remaining=%u mode=%04X\n",
                    (unsigned)(m_cdAsyncReadDst-2048u),
                    (unsigned)m_cdAsyncReadRemaining,
                    (unsigned)m_cdAsyncReadMode);

        if(m_cdAsyncReadRemaining==0){
            m_cdAsyncReadActive=false;
            m_cdrom.stopDataRead();

            const uint32_t dmaDone=deliverEvent(0xF0000003u,0x10u);
            if(!callback) callback=dmaDone;
            const uint32_t readDone=deliverEvent(0xF0000003u,0x20u);
            if(!callback) callback=readDone;
        }
    } else {
        switch(type){
            case 2: eventSpec=0x20u; break;
            case 3:
                // During a BIOS async read, INT3 is only the acknowledgement
                // of SetMode/ReadN/ReadS; completion is signalled after DMA.
                if(!m_cdAsyncReadActive) eventSpec=0x20u;
                break;
            case 4: eventSpec=0x80u; break;
            case 5: eventSpec=0x8000u; break;
            default: break;
        }
    }

    {
        const uint32_t cb=deliverEvent(0xF0000003u,0x1000u);
        if(!callback) callback=cb;
    }
    if(eventSpec){
        const uint32_t cb=deliverEvent(0xF0000003u,eventSpec);
        if(!callback) callback=cb;
    }

    // BIOS priority-0 CD ISR consumes the decoder interrupt before lower
    // priority Card/PAD SysIntRP entries are considered.
    m_cdrom.acknowledgeInterrupt(0x1Fu);
    m_irq.acknowledge(static_cast<uint16_t>(0x07FFu & ~IrqController::Cdrom));

    tracePrintf(
        "[BIOS CD IRQ] type=%u flags=%02X status=%02X error=%02X spec=%04X handled=1\n",
        static_cast<unsigned>(type),
        static_cast<unsigned>(flags),
        static_cast<unsigned>(m_cdLastStatus),
        static_cast<unsigned>(m_cdLastError),
        static_cast<unsigned>(eventSpec)
    );

    return true;
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

int BiosHle::threadIndex(uint32_t handle) const
{
    if(handle<0xFF000000u || handle>=0xFF000004u) return -1;
    return static_cast<int>(handle-0xFF000000u);
}

uint32_t BiosHle::openThread(r3k::CpuState& cpu,uint32_t pc,uint32_t sp,uint32_t gp)
{
    for(unsigned i=1;i<m_threads.size();++i){
        if(m_threads[i].used) continue;
        Thread t;
        t.used=true;
        t.cpu.pc=pc;
        t.cpu.gpr[28]=gp;
        t.cpu.gpr[29]=sp;
        t.cpu.gpr[30]=sp;
        t.cpu.gpr[31]=0;
        t.cpu.cop0[12]=cpu.cop0[12];
        t.cpu.cop0[15]=cpu.cop0[15];
        m_threads[i]=t;
        tracePrintf("[BIOS THREAD OPEN] id=%u handle=%08X pc=%08X sp=%08X gp=%08X\n",
                    i,0xFF000000u+i,(unsigned)pc,(unsigned)sp,(unsigned)gp);
        return 0xFF000000u+i;
    }
    return 0xFFFFFFFFu;
}

uint32_t BiosHle::closeThread(uint32_t handle)
{
    const int idx=threadIndex(handle);
    if(idx>0 && static_cast<unsigned>(idx)!=m_currentThread)
        m_threads[static_cast<unsigned>(idx)]=Thread();
    tracePrintf("[BIOS THREAD CLOSE] handle=%08X idx=%d\n",(unsigned)handle,idx);
    return 1;
}

uint32_t BiosHle::changeThread(r3k::CpuState& cpu,uint32_t handle)
{
    const int idx=threadIndex(handle);
    m_threadSwitchPerformed=false;
    if(idx<0 || !m_threads[static_cast<unsigned>(idx)].used) return 0;
    if(static_cast<unsigned>(idx)==m_currentThread) return 1;

    const uint64_t cycles=cpu.cycles;
    Thread& old=m_threads[m_currentThread];
    old.used=true;
    old.cpu=cpu;
    old.cpu.pc=cpu.gpr[31];
    old.cpu.gpr[2]=1;

    cpu=m_threads[static_cast<unsigned>(idx)].cpu;
    cpu.cycles=cycles;
    m_currentThread=static_cast<unsigned>(idx);
    m_threadSwitchPerformed=true;

    tracePrintf("[BIOS THREAD SWITCH] new=%u pc=%08X sp=%08X\n",
                m_currentThread,(unsigned)cpu.pc,(unsigned)cpu.gpr[29]);
    return 1;
}

bool BiosHle::wildcardMatch(const std::string& pattern,const std::string& value)
{
    std::size_t p=0,v=0;
    while(p<pattern.size()){
        const char pc=pattern[p];
        if(pc=='*') return true;
        if(v>=value.size()) return false;
        if(pc!='?' && pc!=value[v]) return false;
        ++p; ++v;
    }
    return v==value.size();
}

bool BiosHle::parseCardPath(const std::string& path,int& slot,std::string& name) const
{
    if(path.size()<5) return false;
    const char b0=static_cast<char>(std::tolower(static_cast<unsigned char>(path[0])));
    const char b1=static_cast<char>(std::tolower(static_cast<unsigned char>(path[1])));
    if(b0!='b' || b1!='u' || path[3]!='0' || path[4]!=':') return false;
    if(path[2]=='0') slot=0;
    else if(path[2]=='1') slot=1;
    else return false;
    name=path.substr(5);
    return true;
}

void BiosHle::updateCardFrameChecksum(unsigned slot,unsigned sector)
{
    if(slot>=m_cards.size() || sector>=1024u || m_cards[slot].size()!=0x20000u) return;
    const std::size_t base=static_cast<std::size_t>(sector)*128u;
    uint8_t x=0;
    for(unsigned i=0;i<127u;++i) x^=m_cards[slot][base+i];
    m_cards[slot][base+127u]=x;
}

void BiosHle::formatCard(unsigned slot)
{
    if(slot>=m_cards.size()) return;
    std::vector<uint8_t>& card=m_cards[slot];
    card.assign(0x20000u,0xFFu);

    std::fill(card.begin(),card.begin()+128u,0);
    card[0]='M'; card[1]='C';
    updateCardFrameChecksum(slot,0);

    for(unsigned frame=1;frame<=15u;++frame){
        const std::size_t base=static_cast<std::size_t>(frame)*128u;
        std::fill(card.begin()+base,card.begin()+base+128u,0);
        card[base+0]=0xA0u;
        card[base+8]=0xFFu;
        card[base+9]=0xFFu;
        updateCardFrameChecksum(slot,frame);
    }

    for(unsigned frame=16;frame<=35u;++frame){
        const std::size_t base=static_cast<std::size_t>(frame)*128u;
        std::fill(card.begin()+base,card.begin()+base+128u,0);
        card[base+0]=0xFFu; card[base+1]=0xFFu;
        card[base+2]=0xFFu; card[base+3]=0xFFu;
        updateCardFrameChecksum(slot,frame);
    }

    // Frame 63 is a backup of the header on retail cards.
    std::copy(card.begin(),card.begin()+128u,card.begin()+63u*128u);
}

int BiosHle::cardSlotFromPort(uint32_t port) const
{
    if(port==0u) return 0;
    if(port==0x10u || port==1u) return 1;
    return -1;
}

void BiosHle::signalCardIoSuccess(uint32_t fd)
{
    const uint32_t cls=(fd<16u) ? fd : 0xF0000011u;
    const uint32_t cb=deliverEvent(cls,0x00000004u);
    if(cb && m_pendingEventCallback==0) m_pendingEventCallback=cb;
}

bool BiosHle::cardReadSector(uint32_t port,uint32_t sector,uint32_t dst)
{
    const int slot=cardSlotFromPort(port);
    if(slot<0 || sector>0x3FFu || dst==0) return false;
    std::vector<uint8_t>& card=m_cards[static_cast<unsigned>(slot)];
    if(card.size()!=0x20000u) formatCard(static_cast<unsigned>(slot));
    m_cardStatus[static_cast<unsigned>(slot)]=2;
    const std::size_t base=static_cast<std::size_t>(sector)*128u;
    for(unsigned i=0;i<128u;++i) m_mem.write8(dst+i,card[base+i]);
    m_cardStatus[static_cast<unsigned>(slot)]=1;
    signalCardIoSuccess();
    tracePrintf("[BIOS CARD READ] slot=%d sector=%u dst=%08X\n",slot,(unsigned)sector,(unsigned)dst);
    return true;
}

bool BiosHle::cardWriteSector(uint32_t port,uint32_t sector,uint32_t src)
{
    const int slot=cardSlotFromPort(port);
    if(slot<0 || sector>0x3FFu) return false;
    std::vector<uint8_t>& card=m_cards[static_cast<unsigned>(slot)];
    if(card.size()!=0x20000u) formatCard(static_cast<unsigned>(slot));
    m_cardStatus[static_cast<unsigned>(slot)]=4;
    if(src!=0){
        const std::size_t base=static_cast<std::size_t>(sector)*128u;
        for(unsigned i=0;i<128u;++i) card[base+i]=m_mem.read8(src+i);
    }
    m_cardIgnoreChange[static_cast<unsigned>(slot)]=false;
    m_cardStatus[static_cast<unsigned>(slot)]=1;
    signalCardIoSuccess();
    tracePrintf("[BIOS CARD WRITE] slot=%d sector=%u src=%08X%s\n",
                slot,(unsigned)sector,(unsigned)src,src?"":" dummy");
    return true;
}

int BiosHle::findCardFile(int slot,const std::string& pattern,unsigned startBlock,bool includeDeleted) const
{
    if(slot<0 || slot>=2 || m_cards[static_cast<unsigned>(slot)].size()!=0x20000u) return -1;
    const std::vector<uint8_t>& card=m_cards[static_cast<unsigned>(slot)];
    if(startBlock<1u) startBlock=1u;
    for(unsigned block=startBlock;block<=15u;++block){
        const std::size_t base=static_cast<std::size_t>(block)*128u;
        const uint8_t state=card[base];
        const bool live=state==0x51u;
        const bool deleted=state==0xA1u;
        if(!live && !(includeDeleted && deleted)) continue;
        std::string filename;
        for(unsigned i=0;i<20u;++i){
            const uint8_t ch=card[base+0x0Au+i];
            if(ch==0) break;
            filename.push_back(static_cast<char>(ch));
        }
        if(wildcardMatch(pattern,filename)) return static_cast<int>(block);
    }
    return -1;
}

int BiosHle::createCardFile(int slot,const std::string& name,unsigned blocks)
{
    if(slot<0 || slot>=2 || blocks==0 || blocks>15u) return -1;
    std::vector<uint8_t>& card=m_cards[static_cast<unsigned>(slot)];
    if(card.size()!=0x20000u) formatCard(static_cast<unsigned>(slot));

    std::vector<unsigned> freeBlocks;
    for(unsigned block=1;block<=15u && freeBlocks.size()<blocks;++block){
        const uint8_t state=card[static_cast<std::size_t>(block)*128u];
        if(state>=0xA0u && state<=0xA3u) freeBlocks.push_back(block);
    }
    if(freeBlocks.size()!=blocks) return -1;

    for(unsigned n=0;n<blocks;++n){
        const unsigned block=freeBlocks[n];
        const std::size_t dir=static_cast<std::size_t>(block)*128u;
        std::fill(card.begin()+dir,card.begin()+dir+128u,0);

        const uint8_t state=(n==0) ? 0x51u : ((n+1u==blocks) ? 0x53u : 0x52u);
        card[dir]=state;

        if(n==0){
            const uint32_t size=blocks*8192u;
            card[dir+4]=static_cast<uint8_t>(size);
            card[dir+5]=static_cast<uint8_t>(size>>8);
            card[dir+6]=static_cast<uint8_t>(size>>16);
            card[dir+7]=static_cast<uint8_t>(size>>24);
            const std::size_t count=std::min<std::size_t>(20u,name.size());
            for(std::size_t i=0;i<count;++i) card[dir+0x0Au+i]=static_cast<uint8_t>(name[i]);
        }

        if(n+1u<blocks){
            const uint16_t next=static_cast<uint16_t>(freeBlocks[n+1u]-1u);
            card[dir+8]=static_cast<uint8_t>(next);
            card[dir+9]=static_cast<uint8_t>(next>>8);
        } else {
            card[dir+8]=0xFFu; card[dir+9]=0xFFu;
        }
        updateCardFrameChecksum(static_cast<unsigned>(slot),block);

        const std::size_t data=static_cast<std::size_t>(block)*8192u;
        std::fill(card.begin()+data,card.begin()+data+8192u,0);
    }

    tracePrintf("[BIOS CARD CREATE] slot=%d name=%s blocks=%u first=%u\n",
                slot,name.c_str(),blocks,freeBlocks[0]);
    return static_cast<int>(freeBlocks[0]);
}

uint32_t BiosHle::cardFileSize(int slot,unsigned firstBlock) const
{
    if(slot<0 || slot>=2 || firstBlock<1u || firstBlock>15u) return 0;
    const std::vector<uint8_t>& card=m_cards[static_cast<unsigned>(slot)];
    if(card.size()!=0x20000u) return 0;
    const std::size_t base=static_cast<std::size_t>(firstBlock)*128u+4u;
    return uint32_t(card[base]) |
           (uint32_t(card[base+1u])<<8) |
           (uint32_t(card[base+2u])<<16) |
           (uint32_t(card[base+3u])<<24);
}

bool BiosHle::cardFileCopy(FileHandle& file,uint32_t guestAddress,uint32_t length,bool write)
{
    if(file.slot<0 || file.slot>=2 || file.firstBlock<1u || file.firstBlock>15u) return false;
    if(guestAddress==0 && length!=0) return false;
    std::vector<uint8_t>& card=m_cards[static_cast<unsigned>(file.slot)];
    if(card.size()!=0x20000u) return false;

    const uint32_t remain=(file.position<file.size) ? (file.size-file.position) : 0u;
    const uint32_t count=std::min<uint32_t>(length,remain);
    uint32_t done=0;
    while(done<count){
        const uint32_t absolute=file.position+done;
        unsigned hops=absolute/8192u;
        unsigned block=file.firstBlock;
        while(hops--){
            const std::size_t dir=static_cast<std::size_t>(block)*128u;
            const uint16_t link=uint16_t(card[dir+8u]) | (uint16_t(card[dir+9u])<<8);
            if(link==0xFFFFu) return false;
            block=static_cast<unsigned>(link)+1u;
            if(block<1u || block>15u) return false;
        }

        const uint32_t inBlock=absolute%8192u;
        const uint32_t chunk=std::min<uint32_t>(count-done,8192u-inBlock);
        const std::size_t cardPos=static_cast<std::size_t>(block)*8192u+inBlock;
        for(uint32_t i=0;i<chunk;++i){
            if(write) card[cardPos+i]=m_mem.read8(guestAddress+done+i);
            else m_mem.write8(guestAddress+done+i,card[cardPos+i]);
        }
        done+=chunk;
    }
    file.position+=done;
    return done==count;
}

int BiosHle::allocFileHandle()
{
    for(unsigned i=2;i<m_files.size();++i)
        if(!m_files[i].used) return static_cast<int>(i);
    return -1;
}

int32_t BiosHle::openBiosFile(const std::string& path,uint32_t accessMode)
{
    int slot=-1;
    std::string name;
    if(!parseCardPath(path,slot,name) || accessMode==0) return -1;

    int block=findCardFile(slot,name,1u,false);
    if(block<0 && (accessMode&0x200u)){
        unsigned blocks=accessMode>>16;
        if(blocks==0) blocks=1;
        block=createCardFile(slot,name,blocks);
    }
    if(block<0) return -1;

    const int fd=allocFileHandle();
    if(fd<0) return -1;

    FileHandle h;
    h.used=true;
    h.slot=slot;
    h.firstBlock=static_cast<uint32_t>(block);
    h.position=0;
    h.size=cardFileSize(slot,static_cast<unsigned>(block));
    h.accessMode=accessMode;
    h.async=(accessMode&0x8000u)!=0;
    h.name=name;
    m_files[static_cast<unsigned>(fd)]=h;

    tracePrintf("[BIOS FILE OPEN] fd=%d slot=%d name=%s mode=%08X size=%u\n",
                fd,slot,name.c_str(),(unsigned)accessMode,(unsigned)h.size);
    return fd;
}

int32_t BiosHle::readBiosFile(uint32_t fd,uint32_t dst,uint32_t length)
{
    if(fd>=m_files.size() || !m_files[fd].used || length==0) return -1;
    FileHandle& h=m_files[fd];
    const uint32_t before=h.position;
    const uint32_t available=(before<h.size)?(h.size-before):0u;
    const uint32_t count=std::min<uint32_t>(length,available);
    if(!cardFileCopy(h,dst,count,false)) return -1;
    if(h.async) signalCardIoSuccess(fd);
    tracePrintf("[BIOS FILE READ] fd=%u dst=%08X request=%u done=%u pos=%u\n",
                (unsigned)fd,(unsigned)dst,(unsigned)length,(unsigned)count,(unsigned)h.position);
    return static_cast<int32_t>(count);
}

int32_t BiosHle::writeBiosFile(uint32_t fd,uint32_t src,uint32_t length)
{
    if(fd>=m_files.size() || !m_files[fd].used || length==0) return -1;
    FileHandle& h=m_files[fd];
    const uint32_t before=h.position;
    const uint32_t available=(before<h.size)?(h.size-before):0u;
    const uint32_t count=std::min<uint32_t>(length,available);
    if(!cardFileCopy(h,src,count,true)) return -1;
    if(h.async) signalCardIoSuccess(fd);
    tracePrintf("[BIOS FILE WRITE] fd=%u src=%08X request=%u done=%u pos=%u\n",
                (unsigned)fd,(unsigned)src,(unsigned)length,(unsigned)count,(unsigned)h.position);
    return static_cast<int32_t>(count);
}

int32_t BiosHle::closeBiosFile(uint32_t fd)
{
    if(fd>=m_files.size() || !m_files[fd].used) return -1;
    m_files[fd]=FileHandle();
    tracePrintf("[BIOS FILE CLOSE] fd=%u\n",(unsigned)fd);
    return static_cast<int32_t>(fd);
}

void BiosHle::fillCardDirEntry(int slot,unsigned block,uint32_t dirEntry) const
{
    if(slot<0 || slot>=2 || block<1u || block>15u || dirEntry==0) return;
    const std::vector<uint8_t>& card=m_cards[static_cast<unsigned>(slot)];
    const std::size_t base=static_cast<std::size_t>(block)*128u;

    for(unsigned i=0;i<0x28u;++i) const_cast<PsxMemory&>(m_mem).write8(dirEntry+i,0);
    for(unsigned i=0;i<20u;++i){
        const uint8_t ch=card[base+0x0Au+i];
        const_cast<PsxMemory&>(m_mem).write8(dirEntry+i,ch);
        if(ch==0) break;
    }
    const_cast<PsxMemory&>(m_mem).write32(dirEntry+0x14u,0x50u);
    const_cast<PsxMemory&>(m_mem).write32(dirEntry+0x18u,cardFileSize(slot,block));
    const_cast<PsxMemory&>(m_mem).write32(dirEntry+0x1Cu,0);
    const_cast<PsxMemory&>(m_mem).write32(dirEntry+0x20u,block*64u);
    const_cast<PsxMemory&>(m_mem).write32(dirEntry+0x24u,0);
}

uint32_t BiosHle::firstCardFile(const std::string& path,uint32_t dirEntry)
{
    int slot=-1;
    std::string pattern;
    if(!parseCardPath(path,slot,pattern) || dirEntry==0) return 0;
    const int block=findCardFile(slot,pattern,1u,false);
    if(block<0){ m_find=FindState(); return 0; }

    m_find.active=true;
    m_find.slot=slot;
    m_find.nextBlock=static_cast<unsigned>(block)+1u;
    m_find.pattern=pattern;
    fillCardDirEntry(slot,static_cast<unsigned>(block),dirEntry);
    tracePrintf("[BIOS FILE FIRST] slot=%d pattern=%s block=%d\n",slot,pattern.c_str(),block);
    return dirEntry;
}

uint32_t BiosHle::nextCardFile(uint32_t dirEntry)
{
    if(!m_find.active || dirEntry==0) return 0;
    const int block=findCardFile(m_find.slot,m_find.pattern,m_find.nextBlock,false);
    if(block<0){ m_find.active=false; return 0; }
    m_find.nextBlock=static_cast<unsigned>(block)+1u;
    fillCardDirEntry(m_find.slot,static_cast<unsigned>(block),dirEntry);
    tracePrintf("[BIOS FILE NEXT] slot=%d block=%d\n",m_find.slot,block);
    return dirEntry;
}

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
    m_irqChainPending=static_cast<uint16_t>(m_irq.stat() & m_irq.mask());
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

        // We are already servicing a hardware exception. Keep I_STAT visible
        // to the guest FIRST/SECOND handlers, but keep COP0 IEc cleared so the
        // same pending source cannot immediately re-enter at the first
        // instruction of the SysIntRP routine.
        c.cop0[12] &= ~1u;

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

    // The real BIOS lets FIRST/SECOND inspect and acknowledge I_STAT.
    // Only clear sources that were pending on entry and are still asserted
    // after the complete SysIntRP chain, preventing an HLE re-entry loop.
    const uint16_t remaining = static_cast<uint16_t>(m_irq.stat() & m_irqChainPending);
    if(remaining){
        const uint16_t keep = static_cast<uint16_t>(0x07FFu & ~remaining);
        tracePrintf("[BIOS IRQ CHAIN FALLBACK ACK] entry=%04X remaining=%04X keep=%04X\n",
            (unsigned)m_irqChainPending,(unsigned)remaining,(unsigned)keep);
        m_irq.acknowledge(keep);
    }

    const uint64_t cycles=c.cycles;
    c=m_irqResumeState;
    c.cycles=cycles;
    m_irqChainActive=false;
    m_irqChainPriority=0; m_irqChainStruct=0; m_irqChainNext=0;
    m_irqChainSecond=0; m_irqChainFunc=0; m_irqChainPending=0; m_irqChainInSecond=false;
    tracePrintf("[BIOS IRQ CHAIN END] resumePC=%08X cycles=%llu\n",
        (unsigned)c.pc,(unsigned long long)c.cycles);
    return true;
}

void BiosHle::callA(r3k::CpuState& c,uint8_t fn)
{
    switch(fn){
        case 0x0E: c.gpr[2]=uint32_t(std::abs(static_cast<int32_t>(arg(c,0)))); break;
        case 0x13: { // setjmp(buf)
            const uint32_t b=arg(c,0);
            if(b){
                m_mem.write32(b+0x00u,c.gpr[31]);
                m_mem.write32(b+0x04u,c.gpr[29]);
                m_mem.write32(b+0x08u,c.gpr[30]);
                for(unsigned i=0;i<8u;++i) m_mem.write32(b+0x0Cu+i*4u,c.gpr[16u+i]);
                m_mem.write32(b+0x2Cu,c.gpr[28]);
            }
            c.gpr[2]=0;
            break;
        }
        case 0x17: { // strcmp
            uint32_t a=arg(c,0),b=arg(c,1);
            int32_t result=0;
            for(unsigned guard=0;guard<0x10000u;++guard){
                const uint8_t ca=m_mem.read8(a++),cb=m_mem.read8(b++);
                if(ca!=cb){ result=int32_t(ca)-int32_t(cb); break; }
                if(ca==0) break;
            }
            c.gpr[2]=static_cast<uint32_t>(result);
            break;
        }
        case 0x18: { // strncmp
            uint32_t a=arg(c,0),b=arg(c,1),n=arg(c,2);
            int32_t result=0;
            for(uint32_t i=0;i<n;++i){
                const uint8_t ca=m_mem.read8(a+i),cb=m_mem.read8(b+i);
                if(ca!=cb){ result=int32_t(ca)-int32_t(cb); break; }
                if(ca==0) break;
            }
            c.gpr[2]=static_cast<uint32_t>(result);
            break;
        }
        case 0x1B: { // strlen
            const uint32_t s=arg(c,0);
            uint32_t n=0;
            while(n<0x100000u && m_mem.read8(s+n)!=0) ++n;
            c.gpr[2]=n;
            break;
        }
        case 0x28: fillBytes(arg(c,0),0,arg(c,1)); c.gpr[2]=arg(c,0); break; // bzero
        case 0x2A: writeBytes(arg(c,0),arg(c,1),arg(c,2)); c.gpr[2]=arg(c,0); break; // memcpy
        case 0x2B: fillBytes(arg(c,0),uint8_t(arg(c,1)),arg(c,2)); c.gpr[2]=arg(c,0); break; // memset
        case 0x2C: writeBytes(arg(c,0),arg(c,1),arg(c,2)); c.gpr[2]=arg(c,0); break; // memmove
        case 0x2F: { // rand
            m_randSeed=m_randSeed*0x41C64E6Du+0x3039u;
            c.gpr[2]=(m_randSeed>>16)&0x7FFFu;
            break;
        }
        case 0x30: m_randSeed=arg(c,0); c.gpr[2]=0; break; // srand
        case 0x39: { // InitHeap
            m_heapBase=arg(c,0);
            m_heapSize=arg(c,1);
            tracePrintf("[BIOS HEAP INIT] base=%08X size=%u\n",
                        (unsigned)m_heapBase,(unsigned)m_heapSize);
            c.gpr[2]=0;
            break;
        }
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
        case 0x55: // _bu_init (alias)
        case 0x70: { // _bu_init
            m_cardInitialized=true;
            m_cardStarted=true;
            m_cardStatus[0]=m_cardStatus[1]=1;
            m_cardIgnoreChange[0]=m_cardIgnoreChange[1]=false;
            tracePrintf("[BIOS CARD INIT] _bu_init\n");
            c.gpr[2]=1;
            break;
        }
        case 0x54: // _96_init (alias)
        case 0x71: { // _96_init
            initializeCdrom();
            c.gpr[2]=1;
            break;
        }
        case 0x56: // _96_remove (alias)
        case 0x72: { // _96_remove
            /*
             * Retail PS1 BIOS quirk:
             * _96_remove() calls DequeueCdIntr(), but SysDeqIntRP is bugged
             * and this removal does not work. Games can call A(72h) during
             * startup and still rely on the BIOS CD IRQ service afterwards.
             *
             * Do NOT disable HINTMSK or remove the priority-0 CD service here.
             */
            tracePrintf(
                "[BIOS CD REMOVE] _96_remove ignored (retail SysDeqIntRP bug); "
                "HINTMSK=%02X installed=%u\n",
                (unsigned)m_cdrom.irqEnable(),
                m_cdBiosIrqInstalled ? 1u : 0u
            );
            c.gpr[2]=1;
            break;
        }
        case 0x90: { // CdromIoIrqFunc1 (FIRST)
            c.gpr[2]=(m_cdBiosIrqInstalled &&
                      ((m_irq.stat() & m_irq.mask() & IrqController::Cdrom)!=0)) ? 1u : 0u;
            break;
        }
        case 0x91: { // CdromDmaIrqFunc1 (FIRST)
            c.gpr[2]=((m_irq.stat() & m_irq.mask() & IrqController::Dma)!=0) ? 1u : 0u;
            break;
        }
        case 0x92: { // CdromIoIrqFunc2 (SECOND)
            uint32_t ignoredCallback=0;
            c.gpr[2]=serviceCdromInterrupt(ignoredCallback) ? 1u : 0u;
            break;
        }
        case 0x93: { // CdromDmaIrqFunc2 (SECOND)
            // DMA-specific DICR acknowledgement belongs to the DMA controller;
            // keep this HLE conservative and only report whether IRQ3 is live.
            c.gpr[2]=((m_irq.stat() & m_irq.mask() & IrqController::Dma)!=0) ? 1u : 0u;
            break;
        }
        case 0x94: { // CdromGetInt5errCode(dst1,dst2)
            if(arg(c,0)) m_mem.write8(arg(c,0),m_cdLastStatus);
            if(arg(c,1)) m_mem.write8(arg(c,1),m_cdLastError);
            c.gpr[2]=1;
            break;
        }
        case 0x95: { // CdInitSubFunc
            initializeCdrom();
            tracePrintf("[BIOS CD INIT SUB] CdInitSubFunc\n");
            c.gpr[2]=1;
            break;
        }
        case 0x9E: { // SetCdromIrqAutoAbort(type,flag)
            // The current HLE completes decoder IRQs synchronously; retain API
            // compatibility and log the requested policy.
            tracePrintf("[BIOS CD AUTOABORT] type=%08X flag=%08X\n",
                        (unsigned)arg(c,0),(unsigned)arg(c,1));
            c.gpr[2]=1;
            break;
        }
        case 0xAB: { // _card_info(port)
            const int slot=cardSlotFromPort(arg(c,0));
            if(slot<0){ c.gpr[2]=0; break; }
            m_cardStatus[static_cast<unsigned>(slot)]=8;
            m_cardStatus[static_cast<unsigned>(slot)]=1;
            const uint32_t cb=deliverEvent(0xF4000001u,0x00000004u);
            if(cb && m_pendingEventCallback==0) m_pendingEventCallback=cb;
            tracePrintf("[BIOS CARD INFO] slot=%d port=%08X cb=%08X\n",
                        slot,(unsigned)arg(c,0),(unsigned)cb);
            c.gpr[2]=1;
            break;
        }
        case 0xAC: { // _card_load(port)
            const int slot=cardSlotFromPort(arg(c,0));
            if(slot<0){ c.gpr[2]=0; break; }
            if(m_cards[static_cast<unsigned>(slot)].size()!=0x20000u)
                formatCard(static_cast<unsigned>(slot));
            const bool valid=m_cards[static_cast<unsigned>(slot)][0]=='M' &&
                             m_cards[static_cast<unsigned>(slot)][1]=='C';
            m_cardStatus[static_cast<unsigned>(slot)]=valid ? 1u : 0x21u;
            const uint32_t cb=deliverEvent(0xF4000001u,valid?0x00000004u:0x00008000u);
            if(cb && m_pendingEventCallback==0) m_pendingEventCallback=cb;
            tracePrintf("[BIOS CARD LOAD] slot=%d valid=%u cb=%08X\n",
                        slot,valid?1u:0u,(unsigned)cb);
            c.gpr[2]=valid?1u:0u;
            break;
        }
        case 0xA2: { // EnqueueCdIntr -- BIOS priority 0
            m_cdBiosIrqInstalled=true;
            const uint32_t struc=arg(c,0);
            bool ok=true;
            if(struc!=0)
                ok=enqueueIntRp(0u,struc);
            tracePrintf(
                "[BIOS CD ENQUEUE] priority=0 installed=1 struc=%08X "
                "first=%08X second=%08X ok=%u\n",
                (unsigned)struc,
                (unsigned)(struc ? m_mem.read32(struc+8u) : 0u),
                (unsigned)(struc ? m_mem.read32(struc+4u) : 0u),
                ok ? 1u : 0u
            );
            c.gpr[2]=ok ? 1u : 0u;
            break;
        }
        case 0xA3: { // DequeueCdIntr
            const uint32_t struc=arg(c,0);
            bool ok=true;
            if(struc!=0)
                ok=dequeueIntRp(0u,struc);
            m_cdBiosIrqInstalled=false;
            tracePrintf("[BIOS CD DEQUEUE] priority=0 struc=%08X ok=%u installed=0\n",
                        (unsigned)struc,ok ? 1u : 0u);
            c.gpr[2]=ok ? 1u : 0u;
            break;
        }
        case 0xA5: { // CdReadSector(count, sector, buffer)
            const uint32_t count=arg(c,0),sector=arg(c,1),dst=arg(c,2);
            std::vector<uint8_t> buf(std::size_t(count)*2048u);
            const bool ok=m_cdrom.readUserSectors(sector,count,buf.data());
            if(ok)for(std::size_t i=0;i<buf.size();++i)m_mem.write8(dst+uint32_t(i),buf[i]);
            c.gpr[2]=ok?count:0xFFFFFFFFu; break;
        }
        case 0xA6: c.gpr[2]=m_cdrom.statusByte(); break;
        case 0x78: { // CdAsyncSeekL(src[3] = BCD MM:SS:FF)
            const uint32_t src=arg(c,0);
            if(!m_cdrom.mounted() || !src){ c.gpr[2]=0; break; }

            m_cdrom.write8(0x1F801800u,0x00u);
            m_cdrom.write8(0x1F801802u,m_mem.read8(src+0u));
            m_cdrom.write8(0x1F801802u,m_mem.read8(src+1u));
            m_cdrom.write8(0x1F801802u,m_mem.read8(src+2u));
            m_cdrom.write8(0x1F801801u,0x02u); // Setloc
            { uint32_t cb=0; serviceCdromInterrupt(cb); }

            m_cdrom.write8(0x1F801800u,0x00u);
            m_cdrom.write8(0x1F801801u,0x15u); // SeekL
            { uint32_t cb=0; serviceCdromInterrupt(cb); }

            c.gpr[2]=1;
            break;
        }
        case 0x7C: { // CdAsyncGetStatus(dst)
            const uint32_t dst=arg(c,0);
            if(!m_cdrom.mounted() || !dst){ c.gpr[2]=0; break; }

            m_cdrom.write8(0x1F801800u,0x00u);
            m_cdrom.write8(0x1F801801u,0x01u); // GetStat
            { uint32_t cb=0; serviceCdromInterrupt(cb); }
            m_mem.write8(dst,m_cdLastStatus ? m_cdLastStatus : m_cdrom.statusByte());

            c.gpr[2]=1;
            break;
        }
        case 0x7E: { // CdAsyncReadSector(count,dst,mode)
            const uint32_t count=arg(c,0);
            const uint32_t dst=arg(c,1);
            const uint16_t mode=static_cast<uint16_t>(arg(c,2));
            if(!m_cdrom.mounted() || count==0 || dst==0){ c.gpr[2]=0; break; }

            m_cdAsyncReadActive=true;
            m_cdAsyncReadRemaining=count;
            m_cdAsyncReadDst=dst;
            m_cdAsyncReadMode=mode;

            m_cdrom.write8(0x1F801800u,0x00u);
            m_cdrom.write8(0x1F801802u,static_cast<uint8_t>(mode));
            m_cdrom.write8(0x1F801801u,0x0Eu); // SetMode
            { uint32_t cb=0; serviceCdromInterrupt(cb); }

            m_cdrom.write8(0x1F801800u,0x00u);
            m_cdrom.write8(0x1F801801u,(mode & 0x0100u) ? 0x1Bu : 0x06u);
            { uint32_t cb=0; serviceCdromInterrupt(cb); }

            tracePrintf("[BIOS CD ASYNC READ] count=%u dst=%08X mode=%04X\n",
                        (unsigned)count,(unsigned)dst,(unsigned)mode);
            c.gpr[2]=1;
            break;
        }
        case 0x81: { // CdAsyncSetMode(mode)
            const uint16_t mode=static_cast<uint16_t>(arg(c,0));
            if(!m_cdrom.mounted()){ c.gpr[2]=0; break; }
            m_cdrom.write8(0x1F801800u,0x00u);
            m_cdrom.write8(0x1F801802u,static_cast<uint8_t>(mode));
            m_cdrom.write8(0x1F801801u,0x0Eu);
            { uint32_t cb=0; serviceCdromInterrupt(cb); }
            c.gpr[2]=1;
            break;
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
        case 0x0E: c.gpr[2]=openThread(c,arg(c,0),arg(c,1),arg(c,2)); break; // OpenTh
        case 0x0F: c.gpr[2]=closeThread(arg(c,0)); break; // CloseTh
        case 0x10: { // ChangeTh
            const uint32_t h=arg(c,0);
            const uint32_t ok=changeThread(c,h);
            if(!m_threadSwitchPerformed) c.gpr[2]=ok;
            break;
        }
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
        case 0x32: { // open
            const std::string path=readString(arg(c,0));
            c.gpr[2]=static_cast<uint32_t>(openBiosFile(path,arg(c,1)));
            break;
        }
        case 0x34: c.gpr[2]=static_cast<uint32_t>(readBiosFile(arg(c,0),arg(c,1),arg(c,2))); break;
        case 0x35: c.gpr[2]=static_cast<uint32_t>(writeBiosFile(arg(c,0),arg(c,1),arg(c,2))); break;
        case 0x36: c.gpr[2]=static_cast<uint32_t>(closeBiosFile(arg(c,0))); break;
        case 0x3F: {
            const std::string s=readString(arg(c,0));
            std::fputs(s.c_str(),stdout);
            std::fputc('\n',stdout);
            c.gpr[2]=0;
            break;
        }
        case 0x42: c.gpr[2]=firstCardFile(readString(arg(c,0)),arg(c,1)); break;
        case 0x43: c.gpr[2]=nextCardFile(arg(c,0)); break;
        case 0x4A: { // InitCARD2
            m_cardInitialized=true;
            m_cardStatus[0]=m_cardStatus[1]=1;
            tracePrintf("[BIOS CARD InitCARD2] pad_enable=%08X\n",(unsigned)arg(c,0));
            c.gpr[2]=1;
            break;
        }
        case 0x4B: m_cardStarted=true; c.gpr[2]=1; tracePrintf("[BIOS CARD StartCARD2]\n"); break;
        case 0x4C: m_cardStarted=false; c.gpr[2]=1; tracePrintf("[BIOS CARD StopCARD2]\n"); break;
        case 0x4E: c.gpr[2]=cardWriteSector(arg(c,0),arg(c,1),arg(c,2))?1u:0u; break;
        case 0x4F: c.gpr[2]=cardReadSector(arg(c,0),arg(c,1),arg(c,2))?1u:0u; break;
        case 0x50:
            m_cardIgnoreChange[0]=m_cardIgnoreChange[1]=true;
            c.gpr[2]=1;
            tracePrintf("[BIOS CARD NEW] ignore-change-once\n");
            break;
        case 0x5B: {const bool old=m_clearPad;m_clearPad=arg(c,0)!=0;c.gpr[2]=old?1u:0u;break;}
        case 0x5C: {
            const uint32_t slot=arg(c,0);
            c.gpr[2]=(slot<2u)?m_cardStatus[slot]:0x11u;
            break;
        }
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
    if(p==0xA0u &&
       (fn==0x54u || fn==0x56u || fn==0x71u || fn==0x72u ||
        fn==0x78u || fn==0x7Cu || fn==0x7Eu || fn==0x81u ||
        (fn>=0x90u && fn<=0x95u) || fn==0x9Eu ||
        (fn>=0xA2u && fn<=0xA6u))){
        tracePrintf("[BIOS CD CALL] fn=%02X a0=%08X a1=%08X a2=%08X a3=%08X\n",
            (unsigned)fn,(unsigned)c.gpr[4],(unsigned)c.gpr[5],
            (unsigned)c.gpr[6],(unsigned)c.gpr[7]);
    }
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

    uint16_t pending =
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

    // The retail BIOS CD handler is a priority-0 SysIntRP entry. Service it
    // before guest Card/PAD chains (typically priorities 1/2), then recompute
    // pending sources so an already-consumed CD IRQ is not misrouted.
    if(pending & IrqController::Cdrom){
        // Direct PS-X EXE boot skips the ROM's _96_init SysIntRP element.
        // Reproduce that persistent priority-0 FIRST/SECOND service in HLE.
        bool cdConsumed=serviceRetailCdromIntRp(irqCallback);
        if(!cdConsumed)
            cdConsumed=serviceCdromInterrupt(irqCallback);
        if(cdConsumed)
            pending=static_cast<uint16_t>(m_irq.stat() & m_irq.mask());
    }

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
     * SysIntRP FIRST precisa observar I_STAT ainda pendente. O ACK antecipado
     * fazia o driver retornar zero antes de SECOND. Quando existe uma cadeia,
     * o ACK fica a cargo do handler guest; continueInterruptChain() limpa
     * somente fontes residuais para impedir reentrada infinita.
     */
    const bool haveIntRp = pending && (m_intRpHeads[0] || m_intRpHeads[1] ||
                                      m_intRpHeads[2] || m_intRpHeads[3]);
    if(!haveIntRp)
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
    /*
     * Nao execute RFE antes da cadeia SysIntRP. Durante o handler a CPU deve
     * permanecer em estado de excecao (IEc=0), enquanto I_STAT continua
     * legivel pelo FIRST/SECOND. O estado pos-RFE ja foi calculado em
     * resumeState e sera restaurado quando a cadeia terminar.
     */
    if(!haveIntRp){
        const uint32_t low = c.cop0[12] & 0x3Fu;
        c.cop0[12] =
            (c.cop0[12] & ~0x3Fu) |
            ((low >> 2) & 0x0Fu);
    }

    /*
     * Retorna para EPC, ou executa primeiro o callback BIOS mode=1000h.
     * O trampoline restaura todo o contexto interrompido depois do jr ra.
     */
    c.pc = c.cop0[14];

    // A raw CD decoder IRQ must not be routed through an unrelated Card/PAD
    // SysIntRP entry merely because some lower-priority chain exists. The
    // trace showed priority 2 FIRST=80065564 returning zero for CD INT3,
    // after which the generic fallback erased I_STAT and libcd timed out.
    //
    // When no BIOS CD event/callback consumed this source, leave HINTSTS and
    // the response FIFO intact for libcd polling, acknowledge only the global
    // I_STAT latch, and resume the interrupted instruction. A later decoder
    // register ACK will clear HINTSTS in the normal guest path.
    const bool rawCdOnly =
        (pending & IrqController::Cdrom) != 0 &&
        irqCallback == 0 &&
        m_intRpHeads[0] == 0;

    if(rawCdOnly){
        // Keep both decoder HINTSTS and the global CD bit visible. The guest
        // libcd command engine polls/acks these registers itself. Do not
        // manufacture an unrelated priority-2 interrupt and do not erase
        // I_STAT before that code observes it.
        c=resumeState;
        c.cop0[12] &= ~1u;
        tracePrintf(
            "[BIOS CD RAW HOLD] pending=%04X flags=%02X I_STAT=%04X "
            "skip-unrelated-SysIntRP=1 resumePC=%08X\n",
            (unsigned)pending,
            (unsigned)m_cdrom.irqFlags(),
            (unsigned)m_irq.stat(),
            (unsigned)c.pc
        );
    } else if(pending && startInterruptChain(c,resumeState)){
        tracePrintf("[BIOS IRQ CHAIN DISPATCH] pending=%04X\n",(unsigned)pending);
    } else if(irqCallback){
        beginEventCallback(c,resumeState,irqCallback);
    }

    return true;
}

} // namespace psxrecomp
