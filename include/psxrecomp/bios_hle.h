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
#include <vector>

namespace psxrecomp {

class BiosHle {
public:
    BiosHle(PsxMemory& memory, IrqController& irq, PsxTimers& timers, PsxCdrom& cdrom, PsxPadSio& pad, psxgpu::PsxGpu& gpu);

    void reset();
    void initializeCdrom();
    bool handleVector(r3k::CpuState& cpu);
    bool handleExceptionVector(r3k::CpuState& cpu);

private:
    struct Event {
        Event() : used(false), enabled(false), ready(false), cls(0), spec(0), mode(0), func(0) {}
        bool used, enabled, ready;
        uint32_t cls, spec, mode, func;
    };

    struct Thread {
        Thread() : used(false), cpu() {}
        bool used;
        r3k::CpuState cpu;
    };

    struct FileHandle {
        FileHandle()
            : used(false), slot(-1), firstBlock(0), position(0), size(0),
              accessMode(0), async(false) {}
        bool used;
        int slot;
        uint32_t firstBlock;
        uint32_t position;
        uint32_t size;
        uint32_t accessMode;
        bool async;
        std::string name;
    };

    struct FindState {
        FindState() : active(false), slot(-1), nextBlock(1) {}
        bool active;
        int slot;
        unsigned nextBlock;
        std::string pattern;
    };

    uint32_t arg(const r3k::CpuState& cpu, unsigned index) const;
    std::string readString(uint32_t addr, std::size_t maxLen=4096) const;
    void writeBytes(uint32_t dst, uint32_t src, uint32_t len);
    void fillBytes(uint32_t dst, uint8_t value, uint32_t len);
    uint32_t deliverEvent(uint32_t cls, uint32_t spec);
    void beginEventCallback(r3k::CpuState& cpu, const r3k::CpuState& resumeState, uint32_t func);
    bool finishEventCallback(r3k::CpuState& cpu);
    uint32_t openEvent(uint32_t cls, uint32_t spec, uint32_t mode, uint32_t func);
    Event* eventFromHandle(uint32_t handle);
    bool enqueueIntRp(uint32_t priority, uint32_t struc);
    bool dequeueIntRp(uint32_t priority, uint32_t struc);
    bool startInterruptChain(r3k::CpuState& cpu, const r3k::CpuState& resumeState);
    bool continueInterruptChain(r3k::CpuState& cpu);
    bool beginEntryIntHook(r3k::CpuState& cpu, const r3k::CpuState& resumeState);
    bool serviceCdromInterrupt(uint32_t& callback);
    bool serviceRetailCdromIntRp(uint32_t& callback);

    // BIOS services used by SLPS_027.11.
    int threadIndex(uint32_t handle) const;
    uint32_t openThread(r3k::CpuState& cpu, uint32_t pc, uint32_t sp, uint32_t gp);
    uint32_t closeThread(uint32_t handle);
    uint32_t changeThread(r3k::CpuState& cpu, uint32_t handle);

    static bool wildcardMatch(const std::string& pattern, const std::string& value);
    bool parseCardPath(const std::string& path, int& slot, std::string& name) const;
    void formatCard(unsigned slot);
    void updateCardFrameChecksum(unsigned slot, unsigned sector);
    int cardSlotFromPort(uint32_t port) const;
    bool cardReadSector(uint32_t port, uint32_t sector, uint32_t dst);
    bool cardWriteSector(uint32_t port, uint32_t sector, uint32_t src);
    int findCardFile(int slot, const std::string& pattern, unsigned startBlock, bool includeDeleted=false) const;
    int createCardFile(int slot, const std::string& name, unsigned blocks);
    uint32_t cardFileSize(int slot, unsigned firstBlock) const;
    bool cardFileCopy(FileHandle& file, uint32_t guestAddress, uint32_t length, bool write);
    int allocFileHandle();
    int32_t openBiosFile(const std::string& path, uint32_t accessMode);
    int32_t readBiosFile(uint32_t fd, uint32_t dst, uint32_t length);
    int32_t writeBiosFile(uint32_t fd, uint32_t src, uint32_t length);
    int32_t closeBiosFile(uint32_t fd);
    uint32_t firstCardFile(const std::string& path, uint32_t dirEntry);
    uint32_t nextCardFile(uint32_t dirEntry);
    void fillCardDirEntry(int slot, unsigned block, uint32_t dirEntry) const;
    void signalCardIoSuccess(uint32_t fd=0xFFFFFFFFu);

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

    // libc/kernel state required by the executable's BIOS wrappers.
    uint32_t m_randSeed=1;
    uint32_t m_heapBase=0;
    uint32_t m_heapSize=0;
    std::array<Thread,4> m_threads;
    unsigned m_currentThread=0;
    bool m_threadSwitchPerformed=false;

    // HookEntryInt/ResetEntryInt state. The game records a setjmp-style frame
    // and expects it to run at the end of BIOS exception processing.
    uint32_t m_entryIntHook=0;
    bool m_entryIntHookActive=false;
    r3k::CpuState m_entryIntResumeState;

    // Two formatted in-memory 128 KiB memory cards. File functions B32/B34/B35/
    // B36/B42/B43 operate on the same raw sectors used by B4E/B4F.
    std::array<std::vector<uint8_t>,2> m_cards;
    std::array<uint8_t,2> m_cardStatus;
    std::array<bool,2> m_cardIgnoreChange;
    bool m_cardInitialized=false;
    bool m_cardStarted=false;
    std::array<FileHandle,16> m_files;
    FindState m_find;
    uint32_t m_pendingEventCallback=0;

    // Retail BIOS CD-ROM interrupt service (_96_init / EnqueueCdIntr).
    // It represents the BIOS-owned priority-0 SysIntRP element that direct
    // PS-X EXE loading would otherwise skip.
    bool m_cdBiosIrqInstalled=false;
    uint8_t m_cdLastStatus=0;
    uint8_t m_cdLastError=0;
    bool m_cdAsyncReadActive=false;
    uint32_t m_cdAsyncReadRemaining=0;
    uint32_t m_cdAsyncReadDst=0;
    uint16_t m_cdAsyncReadMode=0;

    // Callback BIOS mode 0x1000 runs guest code and returns through an HLE
    // trampoline.  The interrupted CPU context is restored at the trampoline.
    bool m_eventCallbackActive=false;
    r3k::CpuState m_eventResumeState;
    uint32_t m_eventCallbackFunc=0;

    // BIOS SysIntRP callback chain. FIRST is called for every registered
    // element; SECOND is called only when FIRST returns v0 != 0.
    bool m_irqChainActive=false;
    r3k::CpuState m_irqResumeState;
    uint32_t m_irqChainPriority=0;
    uint32_t m_irqChainStruct=0;
    uint32_t m_irqChainNext=0;
    uint32_t m_irqChainSecond=0;
    uint32_t m_irqChainFunc=0;
    uint16_t m_irqChainPending=0;
    bool m_irqChainInSecond=false;
    static const uint32_t kEventCallbackTrampoline = 0x800000D0u;
};

} // namespace psxrecomp
