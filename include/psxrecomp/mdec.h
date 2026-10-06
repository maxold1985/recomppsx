#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <vector>

namespace psxrecomp {

class PsxMdec {
public:
    static const uint32_t DataPort = 0x1F801820u;
    static const uint32_t StatusPort = 0x1F801824u;

    void reset();
    bool handles(uint32_t paddr) const { return paddr == DataPort || paddr == StatusPort; }

    uint32_t read32(uint32_t paddr);
    void write32(uint32_t paddr, uint32_t value);

    void dmaWriteWord(uint32_t value) { writeData(value); }
    uint32_t dmaReadWord() { return readData(); }
    std::size_t outputWords() const { return m_output.size(); }

private:
    enum class CommandType { None, Decode, Quant, Scale };

    void writeData(uint32_t value);
    uint32_t readData();
    uint32_t status() const;
    void beginCommand(uint32_t word);
    void finishCommandIfReady();
    void executeDecode();
    void executeQuant();
    void executeScale();

    bool decodeBlock(const std::vector<uint16_t>& src, std::size_t& pos,
                     const std::array<uint8_t,64>& qt, std::array<int32_t,64>& out) const;
    static void idct(const std::array<int32_t,64>& in, std::array<int16_t,64>& out);
    void outputMono(const std::array<int16_t,64>& y);
    void outputColor(const std::array<int16_t,64>& cr,
                     const std::array<int16_t,64>& cb,
                     const std::array<int16_t,64>& y1,
                     const std::array<int16_t,64>& y2,
                     const std::array<int16_t,64>& y3,
                     const std::array<int16_t,64>& y4);
    void pushBytes(const uint8_t* data, std::size_t size);

    CommandType m_command = CommandType::None;
    uint32_t m_commandWord = 0;
    uint32_t m_expectedWords = 0;
    std::vector<uint32_t> m_params;
    std::deque<uint32_t> m_output;

    std::array<uint8_t,64> m_quantY;
    std::array<uint8_t,64> m_quantUv;
    std::array<int16_t,64> m_scale;

    uint8_t m_depth = 0;
    bool m_signedOutput = false;
    bool m_setBit15 = false;
    bool m_dmaIn = false;
    bool m_dmaOut = false;
};

} // namespace psxrecomp
