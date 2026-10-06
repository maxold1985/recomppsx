#include "psxrecomp/cdrom.h"
#include "psxrecomp/trace.h"
#include "psxrecomp/compat.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <sstream>
#include <cstdio>

namespace psxrecomp {

namespace {
const uint32_t kCdBase = 0x1F801800u;
const uint32_t kCpuCyclesPerSector = 33868800u / 75u;
}

void PsxCdrom::reset()
{
    m_index = 0;
    m_mode = 0x20;
    m_irqEnable = 0;
    m_irqFlags = 0;
    m_request = 0;
    m_filterFile = 0;
    m_filterChannel = 0;
    m_setlocLba = 0;
    m_currentLba = 0;
    m_reading = false;
    m_playing = false;
    m_cycleAccumulator = 0;
    m_xaHist1[0]=m_xaHist1[1]=m_xaHist2[0]=m_xaHist2[1]=0;
    m_xaResamplePhase=0;
    m_params.clear();
    m_response.clear();
    m_data.clear();
}

bool PsxCdrom::mount(const std::string& path)
{
    if (compat::is_directory(path))
        return mountDirectory(path);
    return mountImage(path);
}

bool PsxCdrom::mountImage(const std::string& path)
{
    m_disc.close();
    m_disc.clear();
    m_extents.clear();
    m_directoryRoot.clear();
    m_source = SourceKind::None;

    m_disc.open(path, std::ios::binary);
    if (!m_disc) return false;
    m_path = path;
    m_disc.seekg(0, std::ios::end);
    m_discBytes = static_cast<uint64_t>(m_disc.tellg());
    m_disc.seekg(0, std::ios::beg);
    if (!detectLayout()) {
        m_disc.close();
        return false;
    }
    m_source = SourceKind::Image;
    return true;
}

bool PsxCdrom::loadDirectoryManifest(const std::string& manifestPath)
{
    std::ifstream f(manifestPath);
    if (!f) return false;

    m_extents.clear();
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;

        const std::size_t t1 = line.find('\t');
        if (t1 == std::string::npos) continue;
        const std::size_t t2 = line.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;

        try {
            FileExtent e;
            e.lba = static_cast<uint32_t>(std::stoul(line.substr(0, t1), nullptr, 0));
            e.size = static_cast<uint64_t>(std::stoull(line.substr(t1 + 1, t2 - t1 - 1), nullptr, 0));
            e.relativePath = line.substr(t2 + 1);
            if (!e.relativePath.empty() && e.size != 0)
                m_extents.push_back(std::move(e));
        } catch (...) {
            return false;
        }
    }

    std::sort(m_extents.begin(), m_extents.end(),
              [](const FileExtent& a, const FileExtent& b) { return a.lba < b.lba; });
    return !m_extents.empty();
}

bool PsxCdrom::mountDirectory(const std::string& path)
{
    m_disc.close();
    m_disc.clear();
    m_source = SourceKind::None;
    m_path = path;
    m_directoryRoot = path;
    m_sectorSize = 2048;
    m_userOffset = 0;
    m_discBytes = 0;

    const std::string manifest = compat::join_path(path, "cd_layout.txt");
    if (!loadDirectoryManifest(manifest)) {
        m_directoryRoot.clear();
        return false;
    }

    m_source = SourceKind::Directory;
    return true;
}

bool PsxCdrom::detectLayout()
{
    // Prefer a positive ISO9660 PVD signature test over filename heuristics.
    auto hasPvd = [this](uint32_t sectorSize, uint32_t userOffset) -> bool {
        const uint64_t off = uint64_t(16) * sectorSize + userOffset;
        if (off + 6 > m_discBytes) return false;
        char b[6]{};
        m_disc.clear();
        m_disc.seekg(static_cast<std::streamoff>(off), std::ios::beg);
        m_disc.read(b, sizeof(b));
        return m_disc.gcount() == 6 && b[0] == 1 && std::memcmp(b + 1, "CD001", 5) == 0;
    };

    if (hasPvd(2352, 24)) {
        m_sectorSize = 2352;
        m_userOffset = 24;
    } else if (hasPvd(2048, 0)) {
        m_sectorSize = 2048;
        m_userOffset = 0;
    } else if ((m_discBytes % 2352u) == 0) {
        m_sectorSize = 2352;
        m_userOffset = 24;
    } else {
        m_sectorSize = 2048;
        m_userOffset = 0;
    }
    m_disc.clear();
    return true;
}

uint8_t PsxCdrom::fromBcd(uint8_t v)
{
    return static_cast<uint8_t>(((v >> 4) * 10u) + (v & 0x0Fu));
}

uint8_t PsxCdrom::toBcd(uint8_t v)
{
    return static_cast<uint8_t>(((v / 10u) << 4) | (v % 10u));
}

uint32_t PsxCdrom::msfToLba(uint8_t m, uint8_t s, uint8_t f)
{
    const uint32_t abs = (uint32_t(fromBcd(m)) * 60u + fromBcd(s)) * 75u + fromBcd(f);
    return abs >= 150u ? abs - 150u : 0u;
}

uint8_t PsxCdrom::statusByte() const
{
    uint8_t s = 0x02; // motor on
    if (m_reading) s |= 0x20;
    if (m_playing) s |= 0x80;
    return s;
}

void PsxCdrom::queueResponse(uint8_t value)
{
    if (m_response.size() < 16) m_response.push_back(value);
}

void PsxCdrom::raiseCdInterrupt(uint8_t type)
{
    m_irqFlags = type & 7u;

    const bool enabled =
        (m_irqEnable & m_irqFlags & 0x1Fu) != 0;

    tracePrintf(
        "[CD IRQ RAISE] type=%u flags=%02X enable=%02X request=%d",
        static_cast<unsigned>(type),
        static_cast<unsigned>(m_irqFlags),
        static_cast<unsigned>(m_irqEnable),
        enabled ? 1 : 0
    );

    // Hardware asserts the global CD-ROM IRQ only when at least one
    // enabled HINTSTS bit is active.
    if(enabled)
        m_irq.request(IrqController::Cdrom);
}

bool PsxCdrom::readDirectorySector(uint32_t lba, uint8_t* dst2048)
{
    if (!dst2048 || m_extents.empty()) return false;

    auto it = std::upper_bound(
        m_extents.begin(), m_extents.end(), lba,
        [](uint32_t key, const FileExtent& e) { return key < e.lba; });
    if (it == m_extents.begin()) return false;
    --it;

    const uint64_t sectorIndex = uint64_t(lba) - it->lba;
    const uint64_t byteOffset = sectorIndex * 2048u;
    if (byteOffset >= it->size) return false;

    const std::string full = compat::join_path(m_directoryRoot, it->relativePath);
    std::ifstream file(full.c_str(), std::ios::binary);
    if (!file) return false;

    std::memset(dst2048, 0, 2048);
    file.seekg(static_cast<std::streamoff>(byteOffset), std::ios::beg);
    const uint64_t remain = it->size - byteOffset;
    const std::streamsize wanted = static_cast<std::streamsize>(std::min<uint64_t>(2048u, remain));
    file.read(reinterpret_cast<char*>(dst2048), wanted);
    return file.gcount() == wanted;
}

bool PsxCdrom::readUserSector(uint32_t lba, uint8_t* dst2048)
{
    if (!dst2048) return false;
    if (m_source == SourceKind::Directory)
        return readDirectorySector(lba, dst2048);
    if (m_source != SourceKind::Image || !m_disc) return false;

    const uint64_t off = uint64_t(lba) * m_sectorSize + m_userOffset;
    if (off + 2048u > m_discBytes) return false;
    m_disc.clear();
    m_disc.seekg(static_cast<std::streamoff>(off), std::ios::beg);
    m_disc.read(reinterpret_cast<char*>(dst2048), 2048);
    return m_disc.gcount() == 2048;
}

bool PsxCdrom::readUserSectors(uint32_t lba, uint32_t count, uint8_t* dst)
{
    for (uint32_t i = 0; i < count; ++i) {
        if (!readUserSector(lba + i, dst + std::size_t(i) * 2048u)) return false;
    }
    return true;
}

bool PsxCdrom::readRawSector(uint32_t lba, std::array<uint8_t,2352>& raw)
{
    if (m_source != SourceKind::Image || !m_disc || m_sectorSize != 2352) return false;
    const uint64_t off = uint64_t(lba) * 2352u;
    if (off + raw.size() > m_discBytes) return false;
    m_disc.clear();
    m_disc.seekg(static_cast<std::streamoff>(off), std::ios::beg);
    m_disc.read(reinterpret_cast<char*>(raw.data()), raw.size());
    return m_disc.gcount() == static_cast<std::streamsize>(raw.size());
}

int16_t PsxCdrom::decodeXaSample(int sample, unsigned shift, unsigned filter, int32_t& h1, int32_t& h2)
{
    static const int f[4][2] = { {0,0}, {60,0}, {115,-52}, {98,-55} };
    if (shift > 12) shift = 9;
    filter &= 3u;
    int32_t v = sample >> shift;
    v += (h1 * f[filter][0] + h2 * f[filter][1] + 32) >> 6;
    v = compat::clamp_value<int32_t>(v, -32768, 32767);
    h2 = h1;
    h1 = v;
    return static_cast<int16_t>(v);
}

void PsxCdrom::decodeXaGroup(const uint8_t* g, bool stereo, bool eightBit, unsigned rate)
{
    if (!m_audioSink) return;
    const unsigned unitCount = eightBit ? 4u : 8u;
    std::array<std::array<int16_t,28>,8> units{};

    for (unsigned u=0; u<unitCount; ++u) {
        const unsigned paramIndex = eightBit ? u : (u + (u >= 4 ? 4u : 0u));
        const uint8_t param = g[paramIndex];
        const unsigned shift = param & 0x0Fu;
        const unsigned filter = (param >> 4) & 3u;
        const unsigned channel = stereo ? (u & 1u) : 0u;
        for (unsigned i=0; i<28; ++i) {
            int raw = 0;
            if (eightBit) {
                raw = int8_t(g[16 + i*4 + u]);
                raw <<= 8;
            } else {
                const uint8_t b = g[16 + i*4 + u/2];
                int nib = (u & 1u) ? (b >> 4) : (b & 0x0F);
                if (nib & 8) nib -= 16;
                raw = nib << 12;
            }
            units[u][i] = decodeXaSample(raw, shift, filter,
                                         m_xaHist1[channel], m_xaHist2[channel]);
        }
    }

    auto emit = [&](int16_t l, int16_t r) {
        // Nearest-neighbour rate conversion to SPU's 44.1kHz input clock.
        m_xaResamplePhase += 44100u;
        while (m_xaResamplePhase >= rate) {
            m_xaResamplePhase -= rate;
            m_audioSink(l,r);
        }
    };

    if (stereo) {
        for (unsigned pair=0; pair<unitCount/2; ++pair)
            for (unsigned i=0; i<28; ++i)
                emit(units[pair*2][i], units[pair*2+1][i]);
    } else {
        for (unsigned u=0; u<unitCount; ++u)
            for (unsigned i=0; i<28; ++i)
                emit(units[u][i], units[u][i]);
    }
}

bool PsxCdrom::tryDeliverXa(const std::array<uint8_t,2352>& raw)
{
    if ((m_mode & 0x40u) == 0) return false;
    if (raw[0x0F] != 2) return false;
    const uint8_t file = raw[0x10];
    const uint8_t channel = raw[0x11];
    const uint8_t submode = raw[0x12];
    const uint8_t coding = raw[0x13];
    if ((submode & 0x44u) != 0x44u) return false; // AUDIO + REALTIME
    if ((m_mode & 0x08u) && (file != m_filterFile || channel != m_filterChannel)) return false;

    const bool stereo = (coding & 0x01u) != 0;
    const bool halfRate = (coding & 0x04u) != 0;
    const bool eightBit = (coding & 0x10u) != 0;
    const unsigned rate = halfRate ? 18900u : 37800u;
    const uint8_t* audio = raw.data() + 0x18;
    for (unsigned group=0; group<18; ++group)
        decodeXaGroup(audio + group*128u, stereo, eightBit, rate);
    return true;
}

void PsxCdrom::deliverCdda(const std::array<uint8_t,2352>& raw)
{
    if (!m_audioSink) return;
    for (unsigned i=0; i<588; ++i) {
        const unsigned o=i*4;
        const int16_t l=int16_t(uint16_t(raw[o]) | (uint16_t(raw[o+1])<<8));
        const int16_t r=int16_t(uint16_t(raw[o+2]) | (uint16_t(raw[o+3])<<8));
        m_audioSink(l,r);
    }
}

void PsxCdrom::loadNextSector()
{
    static unsigned cdSectorLogCount = 0;
    if(cdSectorLogCount < 2000)
    {
        tracePrintf(
            "[CD SECTOR #%u] LBA=%u (0x%08X)\n",
            cdSectorLogCount,
            static_cast<unsigned>(m_currentLba),
            static_cast<unsigned>(m_currentLba)
        );

        std::fflush(stdout);
        ++cdSectorLogCount;
    }
    std::array<uint8_t,2352> raw{};
    const bool haveRaw = readRawSector(m_currentLba, raw);

    if (m_playing) {
        if (!haveRaw) { m_playing=false; return; }
        deliverCdda(raw);
        ++m_currentLba;
        if (m_mode & 0x04u) {
            m_response.clear();
            queueResponse(statusByte());
            raiseCdInterrupt(1);
        }
        return;
    }

    if (haveRaw && tryDeliverXa(raw)) {
        ++m_currentLba;
        // XA sectors delivered to SPU don't enter CPU data FIFO.
        return;
    }

    std::array<uint8_t, 2048> sector{};
    if (!readUserSector(m_currentLba, sector.data())) {
        m_reading = false;
        m_response.clear();
        queueResponse(statusByte() | 0x01u);
        raiseCdInterrupt(5);
        return;
    }
    m_data.clear();
    for (uint8_t b : sector) m_data.push_back(b);
    ++m_currentLba;
    m_response.clear();
    queueResponse(statusByte());
    raiseCdInterrupt(1);
}

void PsxCdrom::executeCommand(uint8_t cmd)
{
    static unsigned commandLog = 0;

    if(commandLog < 500)
    {
        tracePrintf(
            "[CD COMMAND #%u] cmd=%02X setloc=%u current=%u reading=%d\n",
            commandLog,
            static_cast<unsigned>(cmd),
            static_cast<unsigned>(m_setlocLba),
            static_cast<unsigned>(m_currentLba),
            m_reading ? 1 : 0
        );

        std::fflush(stdout);
        ++commandLog;
    }
    m_response.clear();
    switch (cmd) {
        case 0x01: // Nop / GetStat
            queueResponse(statusByte());
            raiseCdInterrupt(3);
            break;
        case 0x02: // Setloc mm:ss:ff
            if (m_params.size() >= 3) {
                const uint8_t m = m_params.front(); m_params.pop_front();
                const uint8_t s = m_params.front(); m_params.pop_front();
                const uint8_t f = m_params.front(); m_params.pop_front();
                m_setlocLba = msfToLba(m, s, f);
            }
            queueResponse(statusByte());
            raiseCdInterrupt(3);
            break;
        case 0x03: // Play (raw CD-DA sectors)
            m_currentLba = m_setlocLba;
            m_playing = true;
            m_reading = false;
            m_cycleAccumulator = kCpuCyclesPerSector;
            queueResponse(statusByte());
            raiseCdInterrupt(3);
            break;
        case 0x06: // ReadN
        case 0x1B: // ReadS (served as 2048-byte user data in HLE)
            m_currentLba = m_setlocLba;
            m_reading = true;
            m_playing = false;
            m_cycleAccumulator = kCpuCyclesPerSector; // first sector can arrive on next tick
            tracePrintf(
                "[CD READ START] cmd=%02X LBA=%u (0x%08X)\n",
                static_cast<unsigned>(cmd),
                static_cast<unsigned>(m_currentLba),
                static_cast<unsigned>(m_currentLba)
            );
            std::fflush(stdout);
            queueResponse(statusByte());
            raiseCdInterrupt(3);
            break;
        case 0x08: // Stop
        case 0x09: // Pause
            m_reading = false;
            m_playing = false;
            queueResponse(statusByte());
            raiseCdInterrupt(3);
            break;
        case 0x0A: // Init
            m_reading = false;
            m_playing = false;
            m_mode = 0x20;
            queueResponse(statusByte());
            raiseCdInterrupt(3);
            break;
        case 0x0D: // Setfilter
            if (!m_params.empty()) { m_filterFile = m_params.front(); m_params.pop_front(); }
            if (!m_params.empty()) { m_filterChannel = m_params.front(); m_params.pop_front(); }
            queueResponse(statusByte());
            raiseCdInterrupt(3);
            break;
        case 0x0E: // Setmode
            if (!m_params.empty()) { m_mode = m_params.front(); m_params.pop_front(); }
            queueResponse(statusByte());
            raiseCdInterrupt(3);
            break;
        case 0x0F: // Getparam
            queueResponse(statusByte());
            queueResponse(m_mode);
            queueResponse(0);
            queueResponse(m_filterFile);
            queueResponse(m_filterChannel);
            raiseCdInterrupt(3);
            break;
        case 0x13: // GetTN
            queueResponse(statusByte());
            queueResponse(toBcd(1));
            queueResponse(toBcd(1));
            raiseCdInterrupt(3);
            break;
        case 0x14: // GetTD
            queueResponse(statusByte());
            queueResponse(toBcd(0));
            queueResponse(toBcd(2));
            raiseCdInterrupt(3);
            break;
        case 0x15: // SeekL
        case 0x16: // SeekP
            m_currentLba = m_setlocLba;
            queueResponse(statusByte());
            raiseCdInterrupt(3);
            break;
        case 0x1A: // GetID -- licensed data disc response
            queueResponse(statusByte());
            raiseCdInterrupt(3);
            break;
        default:
            queueResponse(statusByte() | 0x01u);
            queueResponse(0x40u);
            raiseCdInterrupt(5);
            break;
    }
    m_params.clear();
}

uint8_t PsxCdrom::read8(uint32_t paddr)
{
    const uint32_t reg = paddr - kCdBase;
    static unsigned cdReadRegLog = 0;

    if(cdReadRegLog < 500)
    {
        tracePrintf(
            "[CD REG READ #%u] addr=%08X reg=%u index=%u resp=%u data=%u irqFlags=%02X irqEnable=%02X\n",
            cdReadRegLog,
            static_cast<unsigned>(paddr),
            static_cast<unsigned>(reg),
            static_cast<unsigned>(m_index),
            static_cast<unsigned>(m_response.size()),
            static_cast<unsigned>(m_data.size()),
            static_cast<unsigned>(m_irqFlags),
            static_cast<unsigned>(m_irqEnable)
        );
        std::fflush(stdout);
        ++cdReadRegLog;
    }
    if (reg == 0) {
        // HSTS: index plus FIFO state bits. Bits are intentionally conservative.
        uint8_t s = m_index & 3u;
        if (m_params.empty()) s |= 1u << 3;      // parameter FIFO empty
        if (m_params.size() < 16) s |= 1u << 4; // parameter FIFO writable
        if (!m_response.empty()) s |= 1u << 5;  // response ready
        if (!m_data.empty()) s |= 1u << 6;      // data ready
        return s;
    }
    if (reg == 1) {
        if (m_response.empty()) return 0;
        const uint8_t v = m_response.front();
        m_response.pop_front();
        return v;
    }
    if (reg == 2) {
        if (m_data.empty()) return 0;
        const uint8_t v = m_data.front();
        m_data.pop_front();
        return v;
    }
    if (reg == 3) {
        if (m_index == 0 || m_index == 2) return static_cast<uint8_t>(m_irqEnable | 0xE0u);
        return static_cast<uint8_t>(m_irqFlags | 0xE0u);
    }
    return 0;
}

void PsxCdrom::write8(uint32_t paddr, uint8_t value)
{
    const uint32_t reg = paddr - kCdBase;
    static unsigned cdWriteRegLog = 0;

    if(cdWriteRegLog < 500)
    {
        tracePrintf(
            "[CD REG WRITE #%u] addr=%08X reg=%u index=%u value=%02X\n",
            cdWriteRegLog,
            static_cast<unsigned>(paddr),
            static_cast<unsigned>(reg),
            static_cast<unsigned>(m_index),
            static_cast<unsigned>(value)
        );
        std::fflush(stdout);
        ++cdWriteRegLog;
    }
    if (reg == 0) {
        m_index = value & 3u;
        return;
    }
    if (reg == 1) {
        if (m_index == 0) executeCommand(value);
        return;
    }
    if (reg == 2) {
        if (m_index == 0) {
            if (m_params.size() < 16) m_params.push_back(value);
    } else if(m_index == 1) {
        m_irqEnable = value & 0x1Fu;

        tracePrintf(
            "[CD IRQ ENABLE] value=%02X enable=%02X flags=%02X\n",
            static_cast<unsigned>(value),
            static_cast<unsigned>(m_irqEnable),
            static_cast<unsigned>(m_irqFlags)
        );
        std::fflush(stdout);

        if((m_irqEnable & m_irqFlags & 0x1Fu) != 0)
            m_irq.request(IrqController::Cdrom);
    }
        return;
    }
    if (reg == 3) {
        if (m_index == 0) {
            m_request = value;
            if (value & 0x80u) {
                // Requesting data keeps the current sector FIFO visible.
            } else {
                m_data.clear();
            }
        } else if(m_index == 1) {
            tracePrintf(
                "[CD IRQ ACK] value=%02X flags_before=%02X enable=%02X\n",
                static_cast<unsigned>(value),
                static_cast<unsigned>(m_irqFlags),
                static_cast<unsigned>(m_irqEnable)
            );
            std::fflush(stdout);

            m_irqFlags &= static_cast<uint8_t>(~(value & 0x1Fu));

            if(value & 0x40u)
                m_params.clear();

            tracePrintf(
                "[CD IRQ ACK DONE] flags_after=%02X\n",
                static_cast<unsigned>(m_irqFlags)
            );
            std::fflush(stdout);
        }
    }
}

uint32_t PsxCdrom::readDataWord()
{
    uint32_t v = 0;
    for (unsigned i = 0; i < 4; ++i) {
        if (!m_data.empty()) {
            v |= uint32_t(m_data.front()) << (i * 8u);
            m_data.pop_front();
        }
    }
    return v;
}

void PsxCdrom::tick(uint32_t cpuCycles)
{
    if ((!m_reading && !m_playing) || !mounted()) return;
    m_cycleAccumulator += cpuCycles;
    const uint32_t period = (m_mode & 0x80u) ? (kCpuCyclesPerSector / 2u) : kCpuCyclesPerSector;
    while (m_cycleAccumulator >= period) {
        m_cycleAccumulator -= period;
        if (m_playing || m_data.empty()) loadNextSector();
        else break;
    }
}

} // namespace psxrecomp
