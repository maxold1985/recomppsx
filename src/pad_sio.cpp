#include "psxrecomp/pad_sio.h"

namespace psxrecomp {

namespace {
const uint32_t kData = 0x1F801040u;
const uint32_t kStat = 0x1F801044u;
const uint32_t kMode = 0x1F801048u;
const uint32_t kCtrl = 0x1F80104Au;
const uint32_t kBaud = 0x1F80104Eu;
const uint32_t kSio1Data = 0x1F801050u;
const uint32_t kSio1Stat = 0x1F801054u;
const uint32_t kSio1Mode = 0x1F801058u;
const uint32_t kSio1Ctrl = 0x1F80105Au;
const uint32_t kSio1Misc = 0x1F80105Cu;
const uint32_t kSio1Baud = 0x1F80105Eu;
}

void PsxPadSio::reset()
{
    m_mode = 0;
    m_control = 0;
    m_baud = 0x0088;
    m_pressed = 0;
    m_rx.clear();
    m_phase = 0;
    m_sio1Mode=m_sio1Control=m_sio1Baud=0;m_sio1LastRx=0;m_sio1RxReady=false;
}

void PsxPadSio::setButton(Button button, bool pressed)
{
    if (pressed) m_pressed |= static_cast<uint16_t>(button);
    else m_pressed &= static_cast<uint16_t>(~static_cast<uint16_t>(button));
}

uint16_t PsxPadSio::status() const
{
    uint16_t s = 0;
    s |= 1u << 0; // TX ready
    if (!m_rx.empty()) s |= 1u << 1; // RX FIFO non-empty
    s |= 1u << 2; // TX idle
    if (!m_rx.empty()) s |= 1u << 7; // ACK input approximation
    return s;
}

void PsxPadSio::pushRx(uint8_t v)
{
    if (m_rx.size() < 8) m_rx.push_back(v);
    // SIO0 IRQ enable is control bit12 on PS1. Requesting slightly early is
    // harmless for BIOS/pad polling and avoids host timing dependence.
    if (m_control & (1u << 12)) m_irq.request(IrqController::Pad);
}

void PsxPadSio::transferByte(uint8_t value)
{
    // Port select/attention is CR bit1. If deasserted, peripheral is idle.
    if ((m_control & (1u << 1)) == 0) {
        m_phase = 0;
        pushRx(0xFF);
        return;
    }

    switch (m_phase) {
        case 0:
            // Device address 01h starts controller transaction.
            pushRx(0xFF);
            m_phase = (value == 0x01) ? 1 : 0;
            break;
        case 1:
            // 42h = poll/read data. Digital pad ID is 41h.
            pushRx(value == 0x42 ? 0x41 : 0xFF);
            m_phase = (value == 0x42) ? 2 : 0;
            break;
        case 2:
            pushRx(0x5A);
            m_phase = 3;
            break;
        case 3: {
            const uint16_t activeLow = static_cast<uint16_t>(~m_pressed);
            pushRx(uint8_t(activeLow));
            m_phase = 4;
            break;
        }
        case 4: {
            const uint16_t activeLow = static_cast<uint16_t>(~m_pressed);
            pushRx(uint8_t(activeLow >> 8));
            m_phase = 5;
            break;
        }
        default:
            pushRx(0xFF);
            ++m_phase;
            break;
    }
}

uint8_t PsxPadSio::read8(uint32_t paddr)
{
    if (paddr == kData) {
        if (m_rx.empty()) return 0xFF;
        const uint8_t v = m_rx.front();
        m_rx.pop_front();
        return v;
    }
    if(paddr==kSio1Data){const uint8_t v=m_sio1LastRx;m_sio1RxReady=false;return v;}
    const uint32_t even = paddr & ~1u;
    const uint16_t v = read16(even);
    return uint8_t(v >> ((paddr & 1u) * 8u));
}

uint16_t PsxPadSio::read16(uint32_t paddr)
{
    switch (paddr & ~1u) {
        case kData: return read8(kData);
        case kStat: return status();
        case kMode: return m_mode;
        case kCtrl: return m_control;
        case kBaud: return m_baud;
        case kSio1Data: return read8(kSio1Data);
        case kSio1Stat: return uint16_t(0x0005u | (m_sio1RxReady?0x0002u:0));
        case kSio1Mode: return m_sio1Mode;
        case kSio1Ctrl: return m_sio1Control;
        case kSio1Misc: return 0;
        case kSio1Baud: return m_sio1Baud;
        default: return 0;
    }
}

uint32_t PsxPadSio::read32(uint32_t paddr)
{
    return uint32_t(read16(paddr)) | (uint32_t(read16(paddr + 2u)) << 16);
}

void PsxPadSio::write8(uint32_t paddr, uint8_t value)
{
    if (paddr == kData) {
        transferByte(value);
        return;
    }
    if(paddr==kSio1Data){m_sio1LastRx=value;m_sio1RxReady=true;if(m_sio1Control&(1u<<10))m_irq.request(IrqController::Sio);return;}
    const uint32_t even = paddr & ~1u;
    uint16_t old = read16(even);
    const unsigned shift = (paddr & 1u) * 8u;
    old = uint16_t((old & ~(0xFFu << shift)) | (uint16_t(value) << shift));
    write16(even, old);
}

void PsxPadSio::write16(uint32_t paddr, uint16_t value)
{
    switch (paddr & ~1u) {
        case kData: transferByte(uint8_t(value)); break;
        case kMode: m_mode = value; break;
        case kCtrl:
            m_control = value;
            if (value & (1u << 6)) { // reset
                m_rx.clear();
                m_phase = 0;
            }
            if ((value & (1u << 1)) == 0) m_phase = 0;
            break;
        case kBaud: m_baud = value; break;
        case kSio1Data: write8(kSio1Data,uint8_t(value)); break;
        case kSio1Mode: m_sio1Mode=value; break;
        case kSio1Ctrl: m_sio1Control=value;if(value&(1u<<6)){m_sio1RxReady=false;}break;
        case kSio1Baud: m_sio1Baud=value; break;
        default: break;
    }
}

void PsxPadSio::write32(uint32_t paddr, uint32_t value)
{
    write16(paddr, uint16_t(value));
    write16(paddr + 2u, uint16_t(value >> 16));
}

} // namespace psxrecomp
