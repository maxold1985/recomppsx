#include "psxrecomp/psx_runtime.h"
#include "psxrecomp/trace.h"

namespace psxrecomp {

namespace {
const uint32_t kCpuClock = 33868800u;
// NTSC 320-pixel dotclock. GPU mode-dependent clocks can be added later; this
// is sufficient to drive Timer0 at the correct order/rate for typical games.
const uint32_t kDotClock320 = 6652800u;
const uint32_t kCpuCyclesPerScanline = 2172u;
const unsigned kNtscScanlines = 263u;
const unsigned kVblankStartLine = 240u;
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

bool PsxRuntime::handleHle()
{
    const uint32_t pcBefore = m_cpu.pc;
    if(m_bios.handleVector(m_cpu)){ tracePrintf("[HLE] vector pc=%08X -> %08X cycles=%llu\n", pcBefore, m_cpu.pc, (unsigned long long)m_cpu.cycles); return true; }
    if(m_bios.handleExceptionVector(m_cpu)){ tracePrintf("[HLE] exception pc=%08X -> %08X cycles=%llu\n", pcBefore, m_cpu.pc, (unsigned long long)m_cpu.cycles); return true; }
    return false;
}

} // namespace psxrecomp
