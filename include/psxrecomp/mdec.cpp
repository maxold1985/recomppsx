#include "psxrecomp/mdec.h"
#include "psxrecomp/compat.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace psxrecomp {

namespace {
const int kZigZag[64] = {
     0, 1, 5, 6,14,15,27,28,
     2, 4, 7,13,16,26,29,42,
     3, 8,12,17,25,30,41,43,
     9,11,18,24,31,40,44,53,
    10,19,23,32,39,45,52,54,
    20,22,33,38,46,51,55,60,
    21,34,37,47,50,56,59,61,
    35,36,48,49,57,58,62,63
};

inline int32_t sign10(uint16_t v)
{
    int32_t x = v & 0x03FFu;
    if (x & 0x0200) x -= 0x0400;
    return x;
}

inline uint8_t clamp8(int v)
{
    return static_cast<uint8_t>(compat::clamp_value(v, 0, 255));
}

inline int8_t clampS8(int v)
{
    return static_cast<int8_t>(compat::clamp_value(v, -128, 127));
}
}

void PsxMdec::reset()
{
    m_command = CommandType::None;
    m_commandWord = 0;
    m_expectedWords = 0;
    m_params.clear();
    m_output.clear();
    m_quantY.fill(1);
    m_quantUv.fill(1);
    m_scale.fill(0);
    m_depth = 0;
    m_signedOutput = false;
    m_setBit15 = false;
    m_dmaIn = m_dmaOut = false;
}

uint32_t PsxMdec::read32(uint32_t paddr)
{
    return paddr == DataPort ? readData() : status();
}

void PsxMdec::write32(uint32_t paddr, uint32_t value)
{
    if (paddr == DataPort) {
        writeData(value);
        return;
    }
    // MDEC1 control: bit31 reset, bit30 enable DMA input, bit29 enable DMA output.
    if (value & 0x80000000u) reset();
    m_dmaIn = (value & 0x40000000u) != 0;
    m_dmaOut = (value & 0x20000000u) != 0;
}

uint32_t PsxMdec::readData()
{
    if (m_output.empty()) return 0;
    const uint32_t v = m_output.front();
    m_output.pop_front();
    return v;
}

uint32_t PsxMdec::status() const
{
    uint32_t s = 0;
    if (m_output.empty()) s |= 1u << 31;      // output FIFO empty
    if (m_command == CommandType::None) s |= 1u << 29; // command ready
    if (m_dmaIn) s |= 1u << 28;
    if (m_dmaOut && !m_output.empty()) s |= 1u << 27;
    s |= uint32_t(m_depth & 3u) << 25;
    if (m_signedOutput) s |= 1u << 24;
    if (m_setBit15) s |= 1u << 23;
    if (m_command != CommandType::None && m_expectedWords >= m_params.size()) {
        const uint32_t remain = m_expectedWords - uint32_t(m_params.size());
        s |= (remain ? remain - 1u : 0u) & 0xFFFFu;
    } else {
        s |= 0xFFFFu;
    }
    return s;
}

void PsxMdec::writeData(uint32_t value)
{
    if (m_command == CommandType::None) {
        beginCommand(value);
        finishCommandIfReady();
        return;
    }
    m_params.push_back(value);
    finishCommandIfReady();
}

void PsxMdec::beginCommand(uint32_t word)
{
    m_commandWord = word;
    m_params.clear();
    switch ((word >> 29) & 7u) {
        case 1:
            m_command = CommandType::Decode;
            m_depth = uint8_t((word >> 27) & 3u);
            m_signedOutput = (word & (1u << 26)) != 0;
            m_setBit15 = (word & (1u << 25)) != 0;
            m_expectedWords = word & 0xFFFFu;
            break;
        case 2:
            m_command = CommandType::Quant;
            m_expectedWords = (word & 1u) ? 32u : 16u; // 64 or 128 bytes
            break;
        case 3:
            m_command = CommandType::Scale;
            m_expectedWords = 32u; // 64 signed halfwords
            break;
        default:
            m_command = CommandType::None;
            m_expectedWords = 0;
            break;
    }
}

void PsxMdec::finishCommandIfReady()
{
    if (m_command == CommandType::None) return;
    if (m_params.size() < m_expectedWords) return;
    switch (m_command) {
        case CommandType::Decode: executeDecode(); break;
        case CommandType::Quant: executeQuant(); break;
        case CommandType::Scale: executeScale(); break;
        default: break;
    }
    m_command = CommandType::None;
    m_expectedWords = 0;
    m_params.clear();
}

void PsxMdec::executeQuant()
{
    std::vector<uint8_t> bytes;
    bytes.reserve(m_params.size() * 4u);
    for (uint32_t w : m_params) {
        bytes.push_back(uint8_t(w));
        bytes.push_back(uint8_t(w >> 8));
        bytes.push_back(uint8_t(w >> 16));
        bytes.push_back(uint8_t(w >> 24));
    }
    if (bytes.size() >= 64) std::copy_n(bytes.begin(), 64, m_quantY.begin());
    if ((m_commandWord & 1u) && bytes.size() >= 128)
        std::copy_n(bytes.begin() + 64, 64, m_quantUv.begin());
}

void PsxMdec::executeScale()
{
    std::size_t n = 0;
    for (uint32_t w : m_params) {
        if (n < 64) m_scale[n++] = int16_t(w);
        if (n < 64) m_scale[n++] = int16_t(w >> 16);
    }
}

bool PsxMdec::decodeBlock(const std::vector<uint16_t>& src, std::size_t& pos,
                          const std::array<uint8_t,64>& qt, std::array<int32_t,64>& out) const
{
    out.fill(0);
    while (pos < src.size() && src[pos] == 0xFE00u) ++pos;
    if (pos >= src.size()) return false;

    uint16_t n = src[pos++];
    const int qscale = (n >> 10) & 0x3F;
    int k = 0;
    int32_t val = sign10(n) * int32_t(qt[0]);

    while (true) {
        if (qscale == 0) val = sign10(n) * 2;
        val = compat::clamp_value<int32_t>(val, -0x400, 0x3FF);
        if (k >= 0 && k < 64) {
            const int dst = qscale ? kZigZag[k] : k;
            out[dst] = val;
        }
        if (pos >= src.size()) break;
        n = src[pos++];
        if (n == 0xFE00u) break;
        k += ((n >> 10) & 0x3F) + 1;
        if (k > 63) break;
        val = (sign10(n) * int32_t(qt[k]) * qscale + 4) / 8;
    }
    return true;
}

void PsxMdec::idct(const std::array<int32_t,64>& in, std::array<int16_t,64>& out)
{
    // Straight reference IDCT. It is intentionally simple; games normally feed
    // MDEC through DMA, so correctness is more important than host micro-optimization.
    const double pi = 3.14159265358979323846;
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            double sum = 0.0;
            for (int v = 0; v < 8; ++v) {
                for (int u = 0; u < 8; ++u) {
                    const double cu = (u == 0) ? 0.7071067811865476 : 1.0;
                    const double cv = (v == 0) ? 0.7071067811865476 : 1.0;
                    sum += cu * cv * double(in[v * 8 + u]) *
                           std::cos((2 * x + 1) * u * pi / 16.0) *
                           std::cos((2 * y + 1) * v * pi / 16.0);
                }
            }
            out[y * 8 + x] = static_cast<int16_t>(compat::clamp_value<int>(int(std::lround(sum / 4.0)), -128, 127));
        }
    }
}

void PsxMdec::pushBytes(const uint8_t* data, std::size_t size)
{
    for (std::size_t i = 0; i < size; i += 4) {
        uint32_t w = 0;
        for (unsigned b = 0; b < 4 && i + b < size; ++b)
            w |= uint32_t(data[i + b]) << (8u * b);
        m_output.push_back(w);
    }
}

void PsxMdec::outputMono(const std::array<int16_t,64>& y)
{
    if (m_depth == 0) {
        std::array<uint8_t,32> b{};
        for (int i = 0; i < 64; i += 2) {
            int a = compat::clamp_value<int>(y[i], -128, 127);
            int c = compat::clamp_value<int>(y[i + 1], -128, 127);
            if (!m_signedOutput) { a += 128; c += 128; }
            b[i / 2] = uint8_t(((c >> 4) & 0x0F) << 4 | ((a >> 4) & 0x0F));
        }
        pushBytes(b.data(), b.size());
    } else {
        std::array<uint8_t,64> b{};
        for (int i = 0; i < 64; ++i) {
            int v = compat::clamp_value<int>(y[i], -128, 127);
            b[i] = m_signedOutput ? uint8_t(int8_t(v)) : uint8_t(v + 128);
        }
        pushBytes(b.data(), b.size());
    }
}

void PsxMdec::outputColor(const std::array<int16_t,64>& cr,
                          const std::array<int16_t,64>& cb,
                          const std::array<int16_t,64>& y1,
                          const std::array<int16_t,64>& y2,
                          const std::array<int16_t,64>& y3,
                          const std::array<int16_t,64>& y4)
{
    std::vector<uint8_t> rgb;
    rgb.reserve(m_depth == 3 ? 512 : 768);
    for (int py = 0; py < 16; ++py) {
        for (int px = 0; px < 16; ++px) {
            const int bx = px & 7;
            const int by = py & 7;
            const auto& yb = (py < 8) ? ((px < 8) ? y1 : y2) : ((px < 8) ? y3 : y4);
            const int Y = yb[by * 8 + bx] + 128;
            const int Cb = cb[(py / 2) * 8 + (px / 2)];
            const int Cr = cr[(py / 2) * 8 + (px / 2)];
            const int R = compat::clamp_value<int>(int(std::lround(Y + 1.402 * Cr)), 0, 255);
            const int G = compat::clamp_value<int>(int(std::lround(Y - 0.3437 * Cb - 0.7143 * Cr)), 0, 255);
            const int B = compat::clamp_value<int>(int(std::lround(Y + 1.772 * Cb)), 0, 255);

            if (m_depth == 3) {
                uint16_t p = uint16_t((R >> 3) | ((G >> 3) << 5) | ((B >> 3) << 10));
                if (m_setBit15) p |= 0x8000u;
                rgb.push_back(uint8_t(p));
                rgb.push_back(uint8_t(p >> 8));
            } else {
                rgb.push_back(uint8_t(R));
                rgb.push_back(uint8_t(G));
                rgb.push_back(uint8_t(B));
            }
        }
    }
    pushBytes(rgb.data(), rgb.size());
}

void PsxMdec::executeDecode()
{
    std::vector<uint16_t> src;
    src.reserve(m_params.size() * 2u);
    for (uint32_t w : m_params) {
        src.push_back(uint16_t(w));
        src.push_back(uint16_t(w >> 16));
    }

    std::size_t pos = 0;
    while (pos < src.size()) {
        if (m_depth <= 1) {
            std::array<int32_t,64> coeff{};
            std::array<int16_t,64> y{};
            if (!decodeBlock(src, pos, m_quantY, coeff)) break;
            idct(coeff, y);
            outputMono(y);
        } else {
            std::array<int32_t,64> c{};
            std::array<int16_t,64> cr{}, cb{}, y1{}, y2{}, y3{}, y4{};
            if (!decodeBlock(src, pos, m_quantUv, c)) break; idct(c, cr);
            if (!decodeBlock(src, pos, m_quantUv, c)) break; idct(c, cb);
            if (!decodeBlock(src, pos, m_quantY, c)) break; idct(c, y1);
            if (!decodeBlock(src, pos, m_quantY, c)) break; idct(c, y2);
            if (!decodeBlock(src, pos, m_quantY, c)) break; idct(c, y3);
            if (!decodeBlock(src, pos, m_quantY, c)) break; idct(c, y4);
            outputColor(cr, cb, y1, y2, y3, y4);
        }
    }
}

} // namespace psxrecomp
