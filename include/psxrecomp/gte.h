#pragma once

#include "r3000a.h"
#include <array>
#include <cstdint>

namespace psxrecomp {

class PsxGte final : public r3k::Gte {
public:
    void reset();

    uint32_t read_data(uint32_t reg) override;
    uint32_t read_ctrl(uint32_t reg) override;
    void write_data(uint32_t reg, uint32_t value) override;
    void write_ctrl(uint32_t reg, uint32_t value) override;
    void execute(uint32_t instruction) override;

    uint32_t lastInstruction() const { return m_lastInstruction; }

private:
    struct Vec3 {
        Vec3() : x(0), y(0), z(0) {}
        Vec3(int32_t xx, int32_t yy, int32_t zz) : x(xx), y(yy), z(zz) {}
        int32_t x, y, z;
    };
    struct Mat3 {
        Mat3() { for (int y=0; y<3; ++y) for (int x=0; x<3; ++x) m[y][x]=0; }
        int32_t m[3][3];
    };

    static int16_t loS16(uint32_t v) { return static_cast<int16_t>(v & 0xFFFFu); }
    static int16_t hiS16(uint32_t v) { return static_cast<int16_t>(v >> 16); }
    static uint16_t loU16(uint32_t v) { return static_cast<uint16_t>(v); }
    static int32_t clampS16(int64_t v, bool lm, uint32_t& flags, uint32_t bit);
    static uint16_t clampU16(int64_t v, uint32_t& flags, uint32_t bit);
    static int32_t clampScreen(int64_t v, uint32_t& flags, uint32_t bit);
    static uint8_t clampColor(int64_t v, uint32_t& flags, uint32_t bit);
    static int32_t clampIr0(int64_t v, uint32_t& flags);

    Mat3 rotation() const;
    Mat3 light() const;
    Mat3 colorMatrix() const;
    Vec3 vector(unsigned index) const;
    Vec3 irVector() const;
    Vec3 translation(unsigned cv) const;

    void clearFlags();
    void finalizeFlags();
    void setMacIr(const int64_t raw[3], bool sf, bool lm);
    void mvmva(const Mat3& m, const Vec3& v, const Vec3& t, bool sf, bool lm);
    void transformPerspective(unsigned vecIndex, bool sf, bool lm, bool updateIr0);
    void pushSxy(int32_t sx, int32_t sy);
    void pushSz(uint16_t sz);
    void pushColorFromMac(uint8_t code);
    void colorLightStage(const Vec3& normal, bool sf, bool lm);
    void colorMatrixStage(bool sf, bool lm);
    void multiplyPrimaryColor(bool sf, bool lm);
    void depthCue(bool sf, bool lm, bool pushColor);
    void executeColorSingle(unsigned vecIndex, bool sf, bool lm, bool multiplyColor, bool cue);

    std::array<uint32_t, 32> m_data;
    std::array<uint32_t, 32> m_ctrl;
    uint32_t m_flags = 0;
    uint32_t m_lastInstruction = 0;
};

} // namespace psxrecomp
