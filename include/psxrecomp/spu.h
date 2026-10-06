#pragma once

#include "psxrecomp/irq.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace psxrecomp {

class PsxSpu {
public:
    static const uint32_t Base = 0x1F801C00u;
    static const uint32_t End  = 0x1F801E00u;
    static const std::size_t RamSize = 512u * 1024u;

    explicit PsxSpu(IrqController& irq) : m_irq(irq) {}

    void reset();
    bool handles(uint32_t paddr) const { return paddr >= Base && paddr < End; }
    uint16_t read16(uint32_t paddr);
    void write16(uint32_t paddr, uint16_t value);

    void tick(uint32_t cpuCycles);

    // DMA4 transfers are 32-bit on the CPU bus, but SPU RAM itself is 16-bit.
    void dmaWriteWord(uint32_t value);
    uint32_t dmaReadWord();

    // CD controller feeds already-decoded PCM here. Values are native 16-bit.
    void pushCdSample(int16_t left, int16_t right);

    // Optional host audio backend can drain interleaved stereo 44.1kHz samples.
    std::size_t drainAudio(int16_t* interleavedStereo, std::size_t frames);
    std::size_t queuedAudioFrames() const { return m_mixQueue.size() / 2u; }

    const std::array<uint8_t, RamSize>& ram() const { return m_ram; }

private:
    struct Voice {
        Voice() : volL(0), volR(0), pitch(0), startAddr(0), adsr1(0), adsr2(0),
                  envx(0), loopAddr(0), currentByteAddr(0), repeatByteAddr(0), phase(0),
                  decodedPos(28), hist1(0), hist2(0), keyed(false), releasing(false) { decoded.fill(0); }
        uint16_t volL, volR, pitch, startAddr, adsr1, adsr2, envx, loopAddr;
        uint32_t currentByteAddr, repeatByteAddr, phase;
        std::array<int16_t, 28> decoded;
        unsigned decodedPos;
        int32_t hist1, hist2;
        bool keyed, releasing;
    };

    static int16_t clamp16(int32_t v);
    static int32_t signedVolume(uint16_t v);
    void keyOn(unsigned voice);
    void keyOff(unsigned voice);
    bool decodeNextBlock(unsigned voice);
    int16_t nextVoiceSample(unsigned voice);
    void mixOneSample();
    void checkIrqAddress(uint32_t byteAddr);
    uint16_t globalRead(uint32_t paddr) const;
    void globalWrite(uint32_t paddr, uint16_t value);

    IrqController& m_irq;
    std::array<uint8_t, RamSize> m_ram;
    std::array<Voice, 24> m_voice;

    uint16_t m_mainVolL = 0;
    uint16_t m_mainVolR = 0;
    uint16_t m_reverbVolL = 0;
    uint16_t m_reverbVolR = 0;
    uint32_t m_keyOn = 0;
    uint32_t m_keyOff = 0;
    uint32_t m_pmon = 0;
    uint32_t m_non = 0;
    uint32_t m_eon = 0;
    uint32_t m_endx = 0x00FFFFFFu;
    uint16_t m_irqAddr = 0;
    uint16_t m_transferAddr = 0;
    uint32_t m_transferByteAddr = 0;
    uint16_t m_control = 0;
    uint16_t m_status = 0;
    uint16_t m_cdVolL = 0x7FFFu;
    uint16_t m_cdVolR = 0x7FFFu;
    uint16_t m_extVolL = 0;
    uint16_t m_extVolR = 0;

    uint64_t m_sampleCycleRemainder = 0;
    std::deque<int16_t> m_cdQueue;  // interleaved stereo
    std::deque<int16_t> m_mixQueue; // interleaved stereo
};

} // namespace psxrecomp
