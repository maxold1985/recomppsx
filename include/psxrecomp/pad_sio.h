#pragma once

#include "psxrecomp/irq.h"

#include <cstdint>
#include <deque>

namespace psxrecomp {

class PsxPadSio {
public:
    enum Button : uint16_t {
        Select   = 1u << 0,
        L3       = 1u << 1,
        R3       = 1u << 2,
        Start    = 1u << 3,
        Up       = 1u << 4,
        Right    = 1u << 5,
        Down     = 1u << 6,
        Left     = 1u << 7,
        L2       = 1u << 8,
        R2       = 1u << 9,
        L1       = 1u << 10,
        R1       = 1u << 11,
        Triangle = 1u << 12,
        Circle   = 1u << 13,
        Cross    = 1u << 14,
        Square   = 1u << 15
    };

    explicit PsxPadSio(IrqController& irq) : m_irq(irq) {}

    void reset();
    bool handles(uint32_t paddr) const { return paddr >= 0x1F801040u && paddr <= 0x1F80105Fu; }

    uint8_t read8(uint32_t paddr);
    uint16_t read16(uint32_t paddr);
    uint32_t read32(uint32_t paddr);
    void write8(uint32_t paddr, uint8_t value);
    void write16(uint32_t paddr, uint16_t value);
    void write32(uint32_t paddr, uint32_t value);

    void setButton(Button button, bool pressed);
    void setButtons(uint16_t pressedMask) { m_pressed = pressedMask; }
    uint16_t pressedButtons() const { return m_pressed; }

private:
    void transferByte(uint8_t value);
    void pushRx(uint8_t value);
    uint16_t status() const;

    IrqController& m_irq;
    uint16_t m_mode = 0;
    uint16_t m_control = 0;
    uint16_t m_baud = 0x0088;
    uint16_t m_pressed = 0;
    std::deque<uint8_t> m_rx;
    unsigned m_phase = 0;
    uint16_t m_sio1Mode=0, m_sio1Control=0, m_sio1Baud=0;
    uint8_t m_sio1LastRx=0;
    bool m_sio1RxReady=false;
};

} // namespace psxrecomp
