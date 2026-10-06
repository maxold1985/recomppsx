#pragma once

#include "psxrecomp/irq.h"
#include <cstdint>
#include <deque>
#include <fstream>
#include <functional>
#include <array>
#include <string>
#include <vector>

namespace psxrecomp {

class PsxCdrom {
public:
    explicit PsxCdrom(IrqController& irq) : m_irq(irq) {}
    void reset();
    bool mount(const std::string& path);
    bool mountImage(const std::string& path);
    bool mountDirectory(const std::string& path);
    bool mounted() const { return m_source != SourceKind::None; }
    bool directoryMode() const { return m_source == SourceKind::Directory; }
    uint32_t sectorSize() const { return m_sectorSize; }
    bool handles(uint32_t paddr) const { return paddr >= 0x1F801800u && paddr <= 0x1F801803u; }
    uint8_t read8(uint32_t paddr);
    void write8(uint32_t paddr, uint8_t value);
    void tick(uint32_t cpuCycles);
    using AudioSink = std::function<void(int16_t,int16_t)>;
    void setAudioSink(AudioSink sink) { m_audioSink = std::move(sink); }
    uint32_t readDataWord();
    std::size_t dataBytesAvailable() const { return m_data.size(); }
    bool readUserSector(uint32_t lba, uint8_t* dst2048);
    bool readUserSectors(uint32_t lba, uint32_t count, uint8_t* dst);
    uint8_t statusByte() const;
private:
    enum class SourceKind { None, Image, Directory };
    struct FileExtent {
        FileExtent() : lba(0), size(0) {}
        uint32_t lba; uint64_t size; std::string relativePath;
        uint64_t sectorCount() const { return (size + 2047u) / 2048u; }
    };
    static uint8_t fromBcd(uint8_t v); static uint8_t toBcd(uint8_t v);
    static uint32_t msfToLba(uint8_t m, uint8_t s, uint8_t f);
    void executeCommand(uint8_t cmd); void queueResponse(uint8_t value); void raiseCdInterrupt(uint8_t type);
    void loadNextSector(); bool detectLayout(); bool loadDirectoryManifest(const std::string& manifestPath);
    bool readDirectorySector(uint32_t lba, uint8_t* dst2048); bool readRawSector(uint32_t lba, std::array<uint8_t,2352>& raw);
    bool tryDeliverXa(const std::array<uint8_t,2352>& raw); void deliverCdda(const std::array<uint8_t,2352>& raw);
    void decodeXaGroup(const uint8_t* group, bool stereo, bool eightBit, unsigned rate);
    static int16_t decodeXaSample(int sample, unsigned shift, unsigned filter, int32_t& h1, int32_t& h2);
    IrqController& m_irq; SourceKind m_source = SourceKind::None; std::ifstream m_disc;
    std::string m_path,m_directoryRoot; std::vector<FileExtent> m_extents;
    uint32_t m_sectorSize=2048,m_userOffset=0; uint64_t m_discBytes=0;
    uint8_t m_index=0,m_mode=0x20,m_irqEnable=0,m_irqFlags=0,m_request=0,m_filterFile=0,m_filterChannel=0;
    uint32_t m_setlocLba=0,m_currentLba=0; bool m_reading=false,m_playing=false; uint64_t m_cycleAccumulator=0;
    std::deque<uint8_t> m_params,m_response,m_data; AudioSink m_audioSink;
    int32_t m_xaHist1[2]; int32_t m_xaHist2[2]; uint32_t m_xaResamplePhase=0;
};
} // namespace psxrecomp
