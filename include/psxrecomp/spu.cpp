#include "psxrecomp/spu.h"

#include <algorithm>
#include <cstring>

namespace psxrecomp {

namespace {
const uint32_t kCpuClock = 33868800u;
const uint32_t kSampleRate = 44100u;
const int kAdpcmCoef[5][2] = {
    { 0, 0 }, { 60, 0 }, { 115, -52 }, { 98, -55 }, { 122, -60 }
};

const uint32_t kMainVolL = 0x1F801D80u;
const uint32_t kMainVolR = 0x1F801D82u;
const uint32_t kReverbVolL = 0x1F801D84u;
const uint32_t kReverbVolR = 0x1F801D86u;
const uint32_t kKeyOnLo = 0x1F801D88u;
const uint32_t kKeyOnHi = 0x1F801D8Au;
const uint32_t kKeyOffLo = 0x1F801D8Cu;
const uint32_t kKeyOffHi = 0x1F801D8Eu;
const uint32_t kPmonLo = 0x1F801D90u;
const uint32_t kPmonHi = 0x1F801D92u;
const uint32_t kNonLo = 0x1F801D94u;
const uint32_t kNonHi = 0x1F801D96u;
const uint32_t kEonLo = 0x1F801D98u;
const uint32_t kEonHi = 0x1F801D9Au;
const uint32_t kEndxLo = 0x1F801D9Cu;
const uint32_t kEndxHi = 0x1F801D9Eu;
const uint32_t kIrqa = 0x1F801DA4u;
const uint32_t kTsa = 0x1F801DA6u;
const uint32_t kFifo = 0x1F801DA8u;
const uint32_t kAttr = 0x1F801DAAu;
const uint32_t kStatx = 0x1F801DAEu;
const uint32_t kCdVolL = 0x1F801DB0u;
const uint32_t kCdVolR = 0x1F801DB2u;
const uint32_t kExtVolL = 0x1F801DB4u;
const uint32_t kExtVolR = 0x1F801DB6u;
}

int16_t PsxSpu::clamp16(int32_t v)
{
    if (v < -32768) return -32768;
    if (v > 32767) return 32767;
    return static_cast<int16_t>(v);
}

int32_t PsxSpu::signedVolume(uint16_t v)
{
    // Direct volume mode is signed 15-bit magnitude. Sweep mode is approximated
    // by using its current encoded level, which is sufficient for most games.
    if (v & 0x8000u) {
        const int32_t mag = int32_t(v & 0x7FFFu);
        return (v & 0x4000u) ? -mag : mag;
    }
    return int16_t(v);
}

void PsxSpu::reset()
{
    m_ram.fill(0);
    for (auto& v : m_voice) v = Voice();
    m_mainVolL = m_mainVolR = 0;
    m_reverbVolL = m_reverbVolR = 0;
    m_keyOn = m_keyOff = m_pmon = m_non = m_eon = 0;
    m_endx = 0x00FFFFFFu;
    m_irqAddr = m_transferAddr = 0;
    m_transferByteAddr = 0;
    m_control = 0;
    m_status = 0;
    m_cdVolL = m_cdVolR = 0x7FFFu;
    m_extVolL = m_extVolR = 0;
    m_sampleCycleRemainder = 0;
    m_cdQueue.clear();
    m_mixQueue.clear();
}

void PsxSpu::checkIrqAddress(uint32_t byteAddr)
{
    if ((m_control & (1u << 6)) == 0 || (m_control & (1u << 15)) == 0) return;
    const uint32_t irqByte = (uint32_t(m_irqAddr) * 8u) & (RamSize - 1u);
    if ((byteAddr & (RamSize - 1u)) == irqByte) {
        m_status |= 1u << 6;
        m_irq.request(IrqController::Spu);
    }
}

void PsxSpu::keyOn(unsigned index)
{
    if (index >= m_voice.size()) return;
    Voice& v = m_voice[index];
    v.currentByteAddr = (uint32_t(v.startAddr) * 8u) & (RamSize - 1u);
    v.repeatByteAddr = (uint32_t(v.loopAddr) * 8u) & (RamSize - 1u);
    v.phase = 0;
    v.decodedPos = 28;
    v.hist1 = v.hist2 = 0;
    v.envx = 0x7FFFu; // envelope timing is approximated, register semantics remain visible.
    v.keyed = true;
    v.releasing = false;
    m_endx &= ~(1u << index);
}

void PsxSpu::keyOff(unsigned index)
{
    if (index >= m_voice.size()) return;
    Voice& v = m_voice[index];
    v.releasing = true;
}

bool PsxSpu::decodeNextBlock(unsigned index)
{
    Voice& v = m_voice[index];
    const uint32_t a = v.currentByteAddr & (RamSize - 1u);
    checkIrqAddress(a);

    const uint8_t header = m_ram[a];
    const uint8_t flags = m_ram[(a + 1u) & (RamSize - 1u)];
    unsigned shift = header & 0x0Fu;
    if (shift > 12) shift = 9;
    unsigned filter = (header >> 4) & 7u;
    if (filter > 4) filter = 0;

    int out = 0;
    for (unsigned i = 0; i < 14; ++i) {
        const uint8_t packed = m_ram[(a + 2u + i) & (RamSize - 1u)];
        for (unsigned n = 0; n < 2; ++n) {
            int32_t nib = (n == 0) ? (packed & 0x0F) : (packed >> 4);
            if (nib & 8) nib -= 16;
            int32_t s = (nib << 12) >> shift;
            s += (v.hist1 * kAdpcmCoef[filter][0] + v.hist2 * kAdpcmCoef[filter][1] + 32) >> 6;
            s = clamp16(s);
            v.hist2 = v.hist1;
            v.hist1 = s;
            v.decoded[out++] = static_cast<int16_t>(s);
        }
    }

    if (flags & 0x04u) {
        v.repeatByteAddr = a;
        v.loopAddr = static_cast<uint16_t>(a / 8u);
    }

    uint32_t next = (a + 16u) & (RamSize - 1u);
    if (flags & 0x01u) {
        m_endx |= 1u << index;
        next = v.repeatByteAddr;
        if ((flags & 0x02u) == 0) {
            v.keyed = false;
            v.envx = 0;
        }
    }
    v.currentByteAddr = next;
    v.decodedPos = 0;
    return true;
}

int16_t PsxSpu::nextVoiceSample(unsigned index)
{
    Voice& v = m_voice[index];
    if (!v.keyed || v.pitch == 0) return 0;

    if (v.decodedPos >= 28 && !decodeNextBlock(index)) return 0;
    const int16_t sample = v.decoded[v.decodedPos];

    // Pitch 0x1000 equals 44.1 kHz. Linear interpolation is intentionally omitted;
    // phase behavior and source stepping are preserved for compatibility.
    v.phase += std::min<uint32_t>(v.pitch, 0x4000u);
    while (v.phase >= 0x1000u) {
        v.phase -= 0x1000u;
        ++v.decodedPos;
        if (v.decodedPos >= 28) {
            if (!decodeNextBlock(index)) break;
        }
    }

    if (v.releasing) {
        v.envx = static_cast<uint16_t>((uint32_t(v.envx) * 63u) / 64u);
        if (v.envx < 8) {
            v.envx = 0;
            v.keyed = false;
            v.releasing = false;
        }
    }

    return static_cast<int16_t>((int32_t(sample) * int32_t(v.envx)) >> 15);
}

void PsxSpu::mixOneSample()
{
    int64_t left = 0;
    int64_t right = 0;

    if ((m_control & (1u << 15)) != 0 && (m_control & (1u << 14)) != 0) {
        for (unsigned i = 0; i < 24; ++i) {
            const int32_t s = nextVoiceSample(i);
            left += (int64_t(s) * signedVolume(m_voice[i].volL)) >> 15;
            right += (int64_t(s) * signedVolume(m_voice[i].volR)) >> 15;
        }
    }

    // CD/XA input bypasses voice channels. Control bit0 gates CD input.
    if (m_cdQueue.size() >= 2) {
        const int16_t cl = m_cdQueue.front(); m_cdQueue.pop_front();
        const int16_t cr = m_cdQueue.front(); m_cdQueue.pop_front();
        if (m_control & 1u) {
            left += (int64_t(cl) * int16_t(m_cdVolL)) >> 15;
            right += (int64_t(cr) * int16_t(m_cdVolR)) >> 15;
        }
    }

    left = (left * signedVolume(m_mainVolL)) >> 15;
    right = (right * signedVolume(m_mainVolR)) >> 15;

    m_mixQueue.push_back(clamp16(static_cast<int32_t>(left)));
    m_mixQueue.push_back(clamp16(static_cast<int32_t>(right)));
    // Keep at most roughly one second to avoid unbounded growth if no backend drains it.
    while (m_mixQueue.size() > 44100u * 2u) {
        m_mixQueue.pop_front();
        m_mixQueue.pop_front();
    }
}

void PsxSpu::tick(uint32_t cpuCycles)
{
    m_sampleCycleRemainder += uint64_t(cpuCycles) * kSampleRate;
    while (m_sampleCycleRemainder >= kCpuClock) {
        m_sampleCycleRemainder -= kCpuClock;
        mixOneSample();
    }
    m_status = static_cast<uint16_t>((m_status & ~0x003Fu) | (m_control & 0x003Fu));
}

void PsxSpu::pushCdSample(int16_t left, int16_t right)
{
    m_cdQueue.push_back(left);
    m_cdQueue.push_back(right);
    while (m_cdQueue.size() > 44100u * 4u) {
        m_cdQueue.pop_front();
        m_cdQueue.pop_front();
    }
}

std::size_t PsxSpu::drainAudio(int16_t* out, std::size_t frames)
{
    if (!out) return 0;
    const std::size_t available = std::min(frames, m_mixQueue.size() / 2u);
    for (std::size_t i = 0; i < available * 2u; ++i) {
        out[i] = m_mixQueue.front();
        m_mixQueue.pop_front();
    }
    return available;
}

void PsxSpu::dmaWriteWord(uint32_t value)
{
    const uint16_t lo = static_cast<uint16_t>(value);
    const uint16_t hi = static_cast<uint16_t>(value >> 16);
    write16(kFifo, lo);
    write16(kFifo, hi);
}

uint32_t PsxSpu::dmaReadWord()
{
    const uint16_t lo = read16(kFifo);
    const uint16_t hi = read16(kFifo);
    return uint32_t(lo) | (uint32_t(hi) << 16);
}

uint16_t PsxSpu::globalRead(uint32_t p) const
{
    switch (p) {
        case kMainVolL: return m_mainVolL;
        case kMainVolR: return m_mainVolR;
        case kReverbVolL: return m_reverbVolL;
        case kReverbVolR: return m_reverbVolR;
        case kKeyOnLo: return uint16_t(m_keyOn);
        case kKeyOnHi: return uint16_t(m_keyOn >> 16);
        case kKeyOffLo: return uint16_t(m_keyOff);
        case kKeyOffHi: return uint16_t(m_keyOff >> 16);
        case kPmonLo: return uint16_t(m_pmon);
        case kPmonHi: return uint16_t(m_pmon >> 16);
        case kNonLo: return uint16_t(m_non);
        case kNonHi: return uint16_t(m_non >> 16);
        case kEonLo: return uint16_t(m_eon);
        case kEonHi: return uint16_t(m_eon >> 16);
        case kEndxLo: return uint16_t(m_endx);
        case kEndxHi: return uint16_t(m_endx >> 16);
        case kIrqa: return m_irqAddr;
        case kTsa: return m_transferAddr;
        case kAttr: return m_control;
        case kStatx: return m_status;
        case kCdVolL: return m_cdVolL;
        case kCdVolR: return m_cdVolR;
        case kExtVolL: return m_extVolL;
        case kExtVolR: return m_extVolR;
        default: return 0;
    }
}

void PsxSpu::globalWrite(uint32_t p, uint16_t value)
{
    auto writePair = [value](uint32_t& reg, bool high) {
        if (high) reg = (reg & 0x0000FFFFu) | (uint32_t(value) << 16);
        else reg = (reg & 0xFFFF0000u) | value;
    };

    switch (p) {
        case kMainVolL: m_mainVolL = value; break;
        case kMainVolR: m_mainVolR = value; break;
        case kReverbVolL: m_reverbVolL = value; break;
        case kReverbVolR: m_reverbVolR = value; break;
        case kKeyOnLo:
            writePair(m_keyOn, false);
            for (unsigned i = 0; i < 16; ++i) if (value & (1u << i)) keyOn(i);
            break;
        case kKeyOnHi:
            writePair(m_keyOn, true);
            for (unsigned i = 0; i < 8; ++i) if (value & (1u << i)) keyOn(i + 16);
            break;
        case kKeyOffLo:
            writePair(m_keyOff, false);
            for (unsigned i = 0; i < 16; ++i) if (value & (1u << i)) keyOff(i);
            break;
        case kKeyOffHi:
            writePair(m_keyOff, true);
            for (unsigned i = 0; i < 8; ++i) if (value & (1u << i)) keyOff(i + 16);
            break;
        case kPmonLo: writePair(m_pmon, false); break;
        case kPmonHi: writePair(m_pmon, true); break;
        case kNonLo: writePair(m_non, false); break;
        case kNonHi: writePair(m_non, true); break;
        case kEonLo: writePair(m_eon, false); break;
        case kEonHi: writePair(m_eon, true); break;
        case kIrqa: m_irqAddr = value; break;
        case kTsa:
            m_transferAddr = value;
            m_transferByteAddr = (uint32_t(value) * 8u) & (RamSize - 1u);
            break;
        case kFifo: {
            checkIrqAddress(m_transferByteAddr);
            m_ram[m_transferByteAddr] = uint8_t(value);
            m_ram[(m_transferByteAddr + 1u) & (RamSize - 1u)] = uint8_t(value >> 8);
            m_transferByteAddr = (m_transferByteAddr + 2u) & (RamSize - 1u);
            m_transferAddr = static_cast<uint16_t>(m_transferByteAddr / 8u);
            break;
        }
        case kAttr:
            m_control = value;
            if ((value & (1u << 6)) == 0) m_status &= static_cast<uint16_t>(~(1u << 6));
            break;
        case kCdVolL: m_cdVolL = value; break;
        case kCdVolR: m_cdVolR = value; break;
        case kExtVolL: m_extVolL = value; break;
        case kExtVolR: m_extVolR = value; break;
        default: break;
    }
}

uint16_t PsxSpu::read16(uint32_t paddr)
{
    const uint32_t p = paddr & ~1u;
    if (p >= Base && p < Base + 24u * 0x10u) {
        const unsigned vi = (p - Base) / 0x10u;
        const unsigned off = (p - Base) & 0x0Fu;
        const Voice& v = m_voice[vi];
        switch (off) {
            case 0x0: return v.volL;
            case 0x2: return v.volR;
            case 0x4: return v.pitch;
            case 0x6: return v.startAddr;
            case 0x8: return v.adsr1;
            case 0xA: return v.adsr2;
            case 0xC: return v.envx;
            case 0xE: return v.loopAddr;
            default: return 0;
        }
    }
    if (p == kFifo) {
        checkIrqAddress(m_transferByteAddr);
        const uint16_t value = uint16_t(m_ram[m_transferByteAddr]) |
                               (uint16_t(m_ram[(m_transferByteAddr + 1u) & (RamSize - 1u)]) << 8);
        m_transferByteAddr = (m_transferByteAddr + 2u) & (RamSize - 1u);
        m_transferAddr = static_cast<uint16_t>(m_transferByteAddr / 8u);
        return value;
    }
    return globalRead(p);
}

void PsxSpu::write16(uint32_t paddr, uint16_t value)
{
    const uint32_t p = paddr & ~1u;
    if (p >= Base && p < Base + 24u * 0x10u) {
        const unsigned vi = (p - Base) / 0x10u;
        const unsigned off = (p - Base) & 0x0Fu;
        Voice& v = m_voice[vi];
        switch (off) {
            case 0x0: v.volL = value; break;
            case 0x2: v.volR = value; break;
            case 0x4: v.pitch = value; break;
            case 0x6: v.startAddr = value; break;
            case 0x8: v.adsr1 = value; break;
            case 0xA: v.adsr2 = value; break;
            case 0xC: v.envx = value; break;
            case 0xE: v.loopAddr = value; v.repeatByteAddr = (uint32_t(value) * 8u) & (RamSize - 1u); break;
            default: break;
        }
        return;
    }
    globalWrite(p, value);
}

} // namespace psxrecomp
