#include "psxgpu/psx_gpu.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <cstdio>

namespace {
template <typename T>
inline T clamp_value(T value, T lo, T hi)
{
	return value < lo ? lo : (value > hi ? hi : value);
}
}

namespace psxgpu {

namespace {

const int kDither[4][4] = {
	{-4,  0, -3,  1},
	{ 2, -2,  3, -1},
	{-3,  1, -4,  0},
	{ 3, -1,  2, -2}
};

inline float edgeFunction(float ax, float ay, float bx, float by, float px, float py)
{
	return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

inline uint32_t rgbWord(uint8_t r, uint8_t g, uint8_t b)
{
	return uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16);
}

} // namespace

PsxGpu::PsxGpu()
	: m_vram(VramWidth * VramHeight, 0)
{
	reset();
}
void PsxGpu::vblank()
{
	m_videoField = !m_videoField;
}
void PsxGpu::setVideoField(bool field)
{
	m_videoField = field;
}
void PsxGpu::reset()
{
	m_videoField = false;
	std::fill(m_vram.begin(), m_vram.end(), 0);
	m_packet.clear();
	m_expectedWords = 0;
	m_polylineMode = false;
	m_cpuToVram = false;
	m_vramToCpu = false;
	m_transferPixelsDone = 0;
	m_transferPixelsTotal = 0;
	m_drawMode = 0;
	m_textureWindow = 0;
	m_drawAreaX1 = 0;
	m_drawAreaY1 = 0;
	m_drawAreaX2 = VramWidth - 1;
	m_drawAreaY2 = VramHeight - 1;
	m_drawOffsetX = 0;
	m_drawOffsetY = 0;
	m_forceMaskBit = false;
	m_checkMaskBit = false;
	m_displayDisabled = true;
	m_displayX = 0;
	m_displayY = 0;
	m_displayMode = 0;
	m_dmaDirection = 0;
	m_gpuReadLatch = 0;
	m_horizontalRange = 0;
	m_verticalRange = 0;
}

int PsxGpu::wrapX(int x)
{
	x %= VramWidth;
	if (x < 0) x += VramWidth;
	return x;
}

int PsxGpu::wrapY(int y)
{
	y %= VramHeight;
	if (y < 0) y += VramHeight;
	return y;
}

uint16_t& PsxGpu::vramAt(int x, int y)
{
	return m_vram[wrapY(y) * VramWidth + wrapX(x)];
}

const uint16_t& PsxGpu::vramAt(int x, int y) const
{
	return m_vram[wrapY(y) * VramWidth + wrapX(x)];
}

int16_t PsxGpu::sign11(uint32_t value)
{
	value &= 0x7FF;
	if (value & 0x400)
		value |= 0xFFFFF800u;
	return static_cast<int16_t>(static_cast<int32_t>(value));
}

uint8_t PsxGpu::clamp8(int value)
{
	return static_cast<uint8_t>(clamp_value(value, 0, 255));
}

int PsxGpu::fixedPacketWords(uint8_t command) const
{
	if (command <= 0x01)
		return 1;
	if (command == 0x02)
		return 3;
	if (command >= 0x20 && command <= 0x3F) {
		const bool textured = (command & 0x04) != 0;
		const bool quad = (command & 0x08) != 0;
		const bool gouraud = (command & 0x10) != 0;
		const int vertices = quad ? 4 : 3;
		return 1 + vertices + (textured ? vertices : 0) + (gouraud ? vertices - 1 : 0);
	}
	if (command >= 0x40 && command <= 0x5F) {
		const bool polyline = (command & 0x08) != 0;
		if (polyline)
			return -1;
		const bool gouraud = (command & 0x10) != 0;
		return gouraud ? 4 : 3;
	}
	if (command >= 0x60 && command <= 0x7F) {
		const bool textured = (command & 0x04) != 0;
		const int sizeCode = (command >> 3) & 0x3;
		return 2 + (textured ? 1 : 0) + (sizeCode == 0 ? 1 : 0);
	}
	if (command >= 0x80 && command <= 0x9F)
		return 4;
	if (command >= 0xA0 && command <= 0xBF)
		return 3;
	if (command >= 0xC0 && command <= 0xDF)
		return 3;
	if (command >= 0xE0)
		return 1;
	return 1;
}

bool PsxGpu::isPolylineTerminator(uint32_t value) const
{
	return (value & 0xF000F000u) == 0x50005000u;
}

void PsxGpu::startGp0Packet(uint32_t firstWord)
{
	m_packet.clear();
	m_packet.push_back(firstWord);
	const uint8_t cmd = static_cast<uint8_t>(firstWord >> 24);
	m_expectedWords = fixedPacketWords(cmd);
	m_polylineMode = (m_expectedWords < 0);
	if (m_expectedWords == 1)
		executeGp0Packet();
}

void PsxGpu::writeGp0(uint32_t value)
{
	static unsigned gp0Count = 0;

	if (gp0Count < 200) {
		std::printf(
			"[GPU GP0 #%u] value=%08X cpuToVram=%d packet=%u expected=%d\n",
			gp0Count,
			static_cast<unsigned>(value),
			m_cpuToVram ? 1 : 0,
			static_cast<unsigned>(m_packet.size()),
			m_expectedWords
		);

		std::fflush(stdout);
	}

	++gp0Count;

	if (m_cpuToVram) {
		for (int half = 0; half < 2 && m_transferPixelsDone < m_transferPixelsTotal; ++half) {
			const uint16_t pixel = static_cast<uint16_t>((value >> (half * 16)) & 0xFFFFu);
			const int index = m_transferPixelsDone++;
			const int x = m_transferX + (index % m_transferW);
			const int y = m_transferY + (index / m_transferW);
			vramAt(x, y) = pixel;
		}
		if (m_transferPixelsDone >= m_transferPixelsTotal) {
			m_cpuToVram = false;
			m_transferPixelsDone = 0;
			m_transferPixelsTotal = 0;
		}
		return;
	}

	if (m_packet.empty()) {
		startGp0Packet(value);
		return;
	}

	if (m_polylineMode) {
		if (isPolylineTerminator(value) && m_packet.size() >= 3) {
			executeGp0Packet();
			return;
		}
		m_packet.push_back(value);
		if (m_packet.size() > 65536) {
			m_packet.clear();
			m_polylineMode = false;
		}
		return;
	}

	m_packet.push_back(value);
	if (static_cast<int>(m_packet.size()) >= m_expectedWords)
		executeGp0Packet();
}

void PsxGpu::executeGp0Packet()
{
	if (m_packet.empty())
		return;

	const uint8_t cmd = static_cast<uint8_t>(m_packet[0] >> 24);
	if (cmd == 0x00 || cmd == 0x01) {
		// NOP / clear texture cache. The software texture path has no cache to flush.
	} else if (cmd == 0x02) {
		commandFillRectangle();
	} else if (cmd >= 0x20 && cmd <= 0x3F) {
		commandPolygon();
	} else if (cmd >= 0x40 && cmd <= 0x5F) {
		commandLine();
	} else if (cmd >= 0x60 && cmd <= 0x7F) {
		commandRectangle();
	} else if (cmd >= 0x80 && cmd <= 0x9F) {
		commandVramCopy();
	} else if (cmd >= 0xA0 && cmd <= 0xBF) {
		commandCpuToVramHeader();
	} else if (cmd >= 0xC0 && cmd <= 0xDF) {
		commandVramToCpuHeader();
	} else if (cmd >= 0xE0) {
		commandEnvironment(cmd, m_packet[0]);
	}

	m_packet.clear();
	m_expectedWords = 0;
	m_polylineMode = false;
}

void PsxGpu::commandEnvironment(uint8_t command, uint32_t word)
{
	switch (command) {
		case 0xE1:
			m_drawMode = word & 0x00FFFFFFu;
			break;
		case 0xE2:
			m_textureWindow = word & 0x000FFFFFu;
			break;
		case 0xE3:
			m_drawAreaX1 = word & 0x3FF;
			m_drawAreaY1 = (word >> 10) & 0x1FF;
			break;
		case 0xE4:
			m_drawAreaX2 = word & 0x3FF;
			m_drawAreaY2 = (word >> 10) & 0x1FF;
			break;
		case 0xE5:
			m_drawOffsetX = sign11(word);
			m_drawOffsetY = sign11(word >> 11);
			break;
		case 0xE6:
			m_forceMaskBit = (word & 1) != 0;
			m_checkMaskBit = (word & 2) != 0;
			break;
		default:
			break;
	}
}

void PsxGpu::commandFillRectangle()
{
	if (m_packet.size() < 3)
		return;
	const uint32_t color = m_packet[0] & 0x00FFFFFFu;
	int x = m_packet[1] & 0x3FF;
	int y = (m_packet[1] >> 16) & 0x1FF;
	int w = m_packet[2] & 0x3FF;
	int h = (m_packet[2] >> 16) & 0x1FF;

	// GP0(02h) aligns the X coordinate and width to 16 pixels.
	x &= ~0xF;
	w = (w + 0xF) & ~0xF;
	if (w == 0) w = 0x400;
	if (h == 0) h = 0x200;
	const uint16_t pixel = color24To555(color, 0, 0, false) & 0x7FFF;

	for (int py = 0; py < h; ++py)
		for (int px = 0; px < w; ++px)
			vramAt(x + px, y + py) = pixel;
}

void PsxGpu::commandVramCopy()
{
	if (m_packet.size() < 4)
		return;
	const int sx = m_packet[1] & 0x3FF;
	const int sy = (m_packet[1] >> 16) & 0x1FF;
	const int dx = m_packet[2] & 0x3FF;
	const int dy = (m_packet[2] >> 16) & 0x1FF;
	int w = m_packet[3] & 0x3FF;
	int h = (m_packet[3] >> 16) & 0x1FF;
	if (w == 0) w = 0x400;
	if (h == 0) h = 0x200;

	std::vector<uint16_t> temp(static_cast<size_t>(w) * static_cast<size_t>(h));
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x)
			temp[static_cast<size_t>(y) * w + x] = vramAt(sx + x, sy + y);
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x)
			vramAt(dx + x, dy + y) = temp[static_cast<size_t>(y) * w + x];
}

void PsxGpu::commandCpuToVramHeader()
{
	if (m_packet.size() < 3)
		return;
	m_transferX = m_packet[1] & 0x3FF;
	m_transferY = (m_packet[1] >> 16) & 0x1FF;
	m_transferW = m_packet[2] & 0x3FF;
	m_transferH = (m_packet[2] >> 16) & 0x1FF;
	if (m_transferW == 0) m_transferW = 0x400;
	if (m_transferH == 0) m_transferH = 0x200;
	m_transferPixelsDone = 0;
	m_transferPixelsTotal = m_transferW * m_transferH;
	m_cpuToVram = m_transferPixelsTotal > 0;
}

void PsxGpu::commandVramToCpuHeader()
{
	if (m_packet.size() < 3)
		return;
	m_transferX = m_packet[1] & 0x3FF;
	m_transferY = (m_packet[1] >> 16) & 0x1FF;
	m_transferW = m_packet[2] & 0x3FF;
	m_transferH = (m_packet[2] >> 16) & 0x1FF;
	if (m_transferW == 0) m_transferW = 0x400;
	if (m_transferH == 0) m_transferH = 0x200;
	m_transferPixelsDone = 0;
	m_transferPixelsTotal = m_transferW * m_transferH;
	m_vramToCpu = m_transferPixelsTotal > 0;
}

uint32_t PsxGpu::readData()
{
	if (!m_vramToCpu)
		return m_gpuReadLatch;

	uint32_t value = 0;
	for (int half = 0; half < 2; ++half) {
		uint16_t pixel = 0;
		if (m_transferPixelsDone < m_transferPixelsTotal) {
			const int index = m_transferPixelsDone++;
			pixel = vramAt(m_transferX + (index % m_transferW), m_transferY + (index / m_transferW));
		}
		value |= uint32_t(pixel) << (half * 16);
	}
	if (m_transferPixelsDone >= m_transferPixelsTotal) {
		m_vramToCpu = false;
		m_transferPixelsDone = 0;
		m_transferPixelsTotal = 0;
	}
	m_gpuReadLatch = value;
	return value;
}

void PsxGpu::writeGp1(uint32_t value)
{
	const uint8_t command = static_cast<uint8_t>(value >> 24);
	switch (command) {
		case 0x00:
			reset();
			break;
		case 0x01:
			m_packet.clear();
			m_expectedWords = 0;
			m_polylineMode = false;
			m_cpuToVram = false;
			break;
		case 0x02:
			// IRQ acknowledge. IRQ generation is intentionally outside this renderer core.
			break;
		case 0x03:
			m_displayDisabled = (value & 1) != 0;
			break;
		case 0x04:
			m_dmaDirection = value & 3;
			break;
		case 0x05:
			m_displayX = value & 0x3FF;
			m_displayY = (value >> 10) & 0x1FF;
			break;
		case 0x06:
			m_horizontalRange = value & 0x00FFFFFFu;
			break;
		case 0x07:
			m_verticalRange = value & 0x00FFFFFFu;
			break;
		case 0x08:
			m_displayMode = value & 0x00FFFFFFu;
			break;
		case 0x10: {
			const uint32_t index = value & 7;

			switch(index) {
				case 2:
					m_gpuReadLatch = m_textureWindow;
					break;

				case 3:
					m_gpuReadLatch =
						uint32_t(m_drawAreaX1) |
						(uint32_t(m_drawAreaY1) << 10);
					break;

				case 4:
					m_gpuReadLatch =
						uint32_t(m_drawAreaX2) |
						(uint32_t(m_drawAreaY2) << 10);
					break;

				case 5:
					m_gpuReadLatch =
						(uint32_t(m_drawOffsetX) & 0x7FF) |
						((uint32_t(m_drawOffsetY) & 0x7FF) << 11);
					break;

				default:
					m_gpuReadLatch = 0;
					break;
			}

			std::printf(
				"[GP1 GETINFO] index=%u latch=%08X\n",
				static_cast<unsigned>(index),
				static_cast<unsigned>(m_gpuReadLatch)
			);
			std::fflush(stdout);

			break;
		}
		default:
			break;
	}
}

uint32_t PsxGpu::readStatus() const
{
	uint32_t status = m_drawMode & 0x7FFu;

	if(m_forceMaskBit)
		status |= (1u << 11);

	if(m_checkMaskBit)
		status |= (1u << 12);

	if(m_displayMode & (1u << 4))
		status |= (1u << 21);

	if(m_displayMode & (1u << 5))
		status |= (1u << 22);

	if(m_displayDisabled)
		status |= (1u << 23);

	if(m_videoField)
		status |= (1u << 31);

	status |= (1u << 26);
	status |= (1u << 27);
	status |= (1u << 28);

	status |= (m_dmaDirection & 3u) << 29;

	bool dmaRequest = false;

	switch(m_dmaDirection & 3u)
	{
		case 0:
			dmaRequest = false;
			break;

		case 1:
			dmaRequest = true;
			break;

		case 2:
			dmaRequest = true;
			break;

		case 3:
			dmaRequest = true;
			break;
	}

	if(dmaRequest)
		status |= (1u << 25);

	if(m_videoField)
		status |= 0x80000000u;
	
	return status;
}
void PsxGpu::dmaLinkedList(uint32_t startAddress, const Read32& read32, uint32_t maxNodes)
{
	if (!read32)
		return;
	uint32_t address = startAddress & 0x1FFFFCu;
	for (uint32_t node = 0; node < maxNodes; ++node) {
		const uint32_t header = read32(address);
		const uint32_t count = header >> 24;
		for (uint32_t i = 0; i < count; ++i)
			writeGp0(read32((address + 4 + i * 4) & 0x1FFFFCu));
		const uint32_t next = header & 0x00FFFFFFu;
		if (next & 0x00800000u)
			break;
		address = next & 0x1FFFFCu;
	}
}

void PsxGpu::dmaBlockToGpu(uint32_t startAddress, uint32_t wordCount, const Read32& read32)
{
	if (!read32)
		return;
	uint32_t address = startAddress & 0x1FFFFCu;
	for (uint32_t i = 0; i < wordCount; ++i) {
		writeGp0(read32(address));
		address = (address + 4) & 0x1FFFFCu;
	}
}

void PsxGpu::dmaGpuToRam(uint32_t startAddress, uint32_t wordCount, const Write32& write32)
{
	if (!write32)
		return;
	uint32_t address = startAddress & 0x1FFFFCu;
	for (uint32_t i = 0; i < wordCount; ++i) {
		write32(address, readData());
		address = (address + 4) & 0x1FFFFCu;
	}
}

void PsxGpu::dmaOtc(uint32_t startAddress, uint32_t wordCount, const Write32& write32)
{
	if (!write32 || wordCount == 0)
		return;
	uint32_t address = startAddress & 0x1FFFFCu;
	for (uint32_t i = 0; i < wordCount; ++i) {
		const bool last = (i + 1) == wordCount;
		const uint32_t value = last ? 0x00FFFFFFu : ((address - 4) & 0x001FFFFFu);
		write32(address, value);
		address = (address - 4) & 0x1FFFFCu;
	}
}

int PsxGpu::displayWidth() const
{
	const uint32_t hr1 = m_displayMode & 3u;
	const bool hr2 = (m_displayMode & (1u << 6)) != 0;
	if (hr2) return 368;
	switch (hr1) {
		case 0: return 256;
		case 1: return 320;
		case 2: return 512;
		case 3: return 640;
	}
	return 320;
}

int PsxGpu::displayHeight() const
{
	return (m_displayMode & (1u << 2)) ? 480 : 240;
}

void PsxGpu::copyDisplayRgba(std::vector<uint8_t>& rgba) const
{
	const int width = displayWidth();
	const int height = displayHeight();
	rgba.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);

	if (!display24Bit()) {
		for (int y = 0; y < height; ++y) {
			for (int x = 0; x < width; ++x) {
				const uint16_t p = vramAt(m_displayX + x, m_displayY + y);
				const uint8_t r = static_cast<uint8_t>((p & 0x1F) * 255 / 31);
				const uint8_t g = static_cast<uint8_t>(((p >> 5) & 0x1F) * 255 / 31);
				const uint8_t b = static_cast<uint8_t>(((p >> 10) & 0x1F) * 255 / 31);
				const size_t o = (static_cast<size_t>(y) * width + x) * 4;
				rgba[o + 0] = r;
				rgba[o + 1] = g;
				rgba[o + 2] = b;
				rgba[o + 3] = 255;
			}
		}
	} else {
		// 24-bit display data is packed as RGB bytes in the 16-bit VRAM byte stream.
		const uint8_t* bytes = reinterpret_cast<const uint8_t*>(m_vram.data());
		const int bytesPerLine = VramWidth * 2;
		for (int y = 0; y < height; ++y) {
			const int srcY = wrapY(m_displayY + y);
			const int baseByte = srcY * bytesPerLine + wrapX(m_displayX) * 2;
			for (int x = 0; x < width; ++x) {
				const int bi = (baseByte + x * 3) % (VramHeight * bytesPerLine);
				const size_t o = (static_cast<size_t>(y) * width + x) * 4;
				rgba[o + 0] = bytes[bi + 0];
				rgba[o + 1] = bytes[(bi + 1) % (VramHeight * bytesPerLine)];
				rgba[o + 2] = bytes[(bi + 2) % (VramHeight * bytesPerLine)];
				rgba[o + 3] = 255;
			}
		}
	}
}

uint16_t PsxGpu::color24To555(uint32_t color, int x, int y, bool dither) const
{
	int r = color & 0xFF;
	int g = (color >> 8) & 0xFF;
	int b = (color >> 16) & 0xFF;
	if (dither && (m_drawMode & (1u << 9))) {
		const int d = kDither[y & 3][x & 3];
		r = clamp_value(r + d, 0, 255);
		g = clamp_value(g + d, 0, 255);
		b = clamp_value(b + d, 0, 255);
	}
	return static_cast<uint16_t>((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
}

void PsxGpu::applyTextureWindow(int& u, int& v) const
{
	const int maskX = (m_textureWindow & 0x1F) * 8;
	const int maskY = ((m_textureWindow >> 5) & 0x1F) * 8;
	const int offX = ((m_textureWindow >> 10) & 0x1F) * 8;
	const int offY = ((m_textureWindow >> 15) & 0x1F) * 8;
	u = (u & ~maskX) | (offX & maskX);
	v = (v & ~maskY) | (offY & maskY);
	u &= 0xFF;
	v &= 0xFF;
}

uint16_t PsxGpu::sampleTexture(int u, int v, uint16_t clut, uint16_t tpage) const
{
	applyTextureWindow(u, v);
	const int pageX = (tpage & 0xF) * 64;
	const int pageY = ((tpage >> 4) & 1) * 256;
	const int depth = (tpage >> 7) & 0x3;

	if (depth == 0) {
		const uint16_t packed = vramAt(pageX + (u >> 2), pageY + v);
		const int index = (packed >> ((u & 3) * 4)) & 0xF;
		const int clutX = (clut & 0x3F) * 16 + index;
		const int clutY = (clut >> 6) & 0x1FF;
		return vramAt(clutX, clutY);
	}
	if (depth == 1) {
		const uint16_t packed = vramAt(pageX + (u >> 1), pageY + v);
		const int index = (packed >> ((u & 1) * 8)) & 0xFF;
		const int clutX = (clut & 0x3F) * 16 + index;
		const int clutY = (clut >> 6) & 0x1FF;
		return vramAt(clutX, clutY);
	}
	return vramAt(pageX + u, pageY + v);
}

uint16_t PsxGpu::modulateTexture(uint16_t texel, float r, float g, float b, int x, int y) const
{
	const int tr = (texel & 0x1F) << 3;
	const int tg = ((texel >> 5) & 0x1F) << 3;
	const int tb = ((texel >> 10) & 0x1F) << 3;
	int rr = clamp_value(static_cast<int>(tr * r / 128.0f), 0, 255);
	int gg = clamp_value(static_cast<int>(tg * g / 128.0f), 0, 255);
	int bb = clamp_value(static_cast<int>(tb * b / 128.0f), 0, 255);
	if (m_drawMode & (1u << 9)) {
		const int d = kDither[y & 3][x & 3];
		rr = clamp_value(rr + d, 0, 255);
		gg = clamp_value(gg + d, 0, 255);
		bb = clamp_value(bb + d, 0, 255);
	}
	return static_cast<uint16_t>((rr >> 3) | ((gg >> 3) << 5) | ((bb >> 3) << 10) | (texel & 0x8000));
}

uint16_t PsxGpu::blendPixel(uint16_t back, uint16_t front, uint16_t tpage) const
{
	const int br = back & 0x1F;
	const int bg = (back >> 5) & 0x1F;
	const int bb = (back >> 10) & 0x1F;
	const int fr = front & 0x1F;
	const int fg = (front >> 5) & 0x1F;
	const int fb = (front >> 10) & 0x1F;
	int r = 0, g = 0, b = 0;
	switch ((tpage >> 5) & 3) {
		case 0: r = br / 2 + fr / 2; g = bg / 2 + fg / 2; b = bb / 2 + fb / 2; break;
		case 1: r = br + fr; g = bg + fg; b = bb + fb; break;
		case 2: r = br - fr; g = bg - fg; b = bb - fb; break;
		case 3: r = br + fr / 4; g = bg + fg / 4; b = bb + fb / 4; break;
	}
	return static_cast<uint16_t>(clamp_value(r, 0, 31) | (clamp_value(g, 0, 31) << 5) | (clamp_value(b, 0, 31) << 10));
}

void PsxGpu::writePixel(int x, int y, uint16_t foreground, bool semiTransparent, uint16_t tpage, bool texturedStp)
{
	if (x < m_drawAreaX1 || x > m_drawAreaX2 || y < m_drawAreaY1 || y > m_drawAreaY2)
		return;
	uint16_t& dst = vramAt(x, y);
	if (m_checkMaskBit && (dst & 0x8000))
		return;
	const bool doBlend = semiTransparent && texturedStp;
	uint16_t out = doBlend ? blendPixel(dst, foreground, tpage) : (foreground & 0x7FFF);
	if (m_forceMaskBit)
		out |= 0x8000;
	dst = out;
}

void PsxGpu::commandPolygon()
{
	const uint8_t cmd = static_cast<uint8_t>(m_packet[0] >> 24);
	const bool rawTexture = (cmd & 0x01) != 0;
	const bool semi = (cmd & 0x02) != 0;
	const bool textured = (cmd & 0x04) != 0;
	const bool quad = (cmd & 0x08) != 0;
	const bool gouraud = (cmd & 0x10) != 0;
	const int count = quad ? 4 : 3;
	std::array<Vertex, 4> vertices{};
	TextureState tex{};
	tex.textured = textured;
	tex.rawTexture = rawTexture;
	tex.semiTransparent = semi;
	uint32_t color = m_packet[0] & 0x00FFFFFFu;
	size_t p = 1;

	for (int i = 0; i < count; ++i) {
		if (i > 0 && gouraud)
			color = m_packet[p++] & 0x00FFFFFFu;
		const uint32_t xy = m_packet[p++];
		vertices[i].x = static_cast<int16_t>(xy & 0xFFFF) + m_drawOffsetX;
		vertices[i].y = static_cast<int16_t>(xy >> 16) + m_drawOffsetY;
		vertices[i].r = static_cast<float>(color & 0xFF);
		vertices[i].g = static_cast<float>((color >> 8) & 0xFF);
		vertices[i].b = static_cast<float>((color >> 16) & 0xFF);
		if (textured) {
			const uint32_t uv = m_packet[p++];
			vertices[i].u = static_cast<float>(uv & 0xFF);
			vertices[i].v = static_cast<float>((uv >> 8) & 0xFF);
			if (i == 0) tex.clut = static_cast<uint16_t>(uv >> 16);
			if (i == 1) tex.tpage = static_cast<uint16_t>(uv >> 16);
		}
	}
	if (!textured)
		tex.tpage = static_cast<uint16_t>(m_drawMode);

	drawTriangle(vertices[0], vertices[1], vertices[2], tex);
	if (quad)
		drawTriangle(vertices[1], vertices[2], vertices[3], tex);
}

void PsxGpu::drawTriangle(const Vertex& a, const Vertex& b, const Vertex& c, const TextureState& tex)
{
	const float area = edgeFunction(a.x, a.y, b.x, b.y, c.x, c.y);
	if (std::abs(area) < 0.0001f)
		return;
	int minX = static_cast<int>(std::floor(std::min({a.x, b.x, c.x})));
	int maxX = static_cast<int>(std::ceil(std::max({a.x, b.x, c.x})));
	int minY = static_cast<int>(std::floor(std::min({a.y, b.y, c.y})));
	int maxY = static_cast<int>(std::ceil(std::max({a.y, b.y, c.y})));
	minX = std::max(minX, m_drawAreaX1);
	maxX = std::min(maxX, m_drawAreaX2);
	minY = std::max(minY, m_drawAreaY1);
	maxY = std::min(maxY, m_drawAreaY2);

	for (int y = minY; y <= maxY; ++y) {
		for (int x = minX; x <= maxX; ++x) {
			const float px = x + 0.5f;
			const float py = y + 0.5f;
			const float w0 = edgeFunction(b.x, b.y, c.x, c.y, px, py) / area;
			const float w1 = edgeFunction(c.x, c.y, a.x, a.y, px, py) / area;
			const float w2 = 1.0f - w0 - w1;
			if (w0 < -0.00001f || w1 < -0.00001f || w2 < -0.00001f)
				continue;
			const float r = a.r * w0 + b.r * w1 + c.r * w2;
			const float g = a.g * w0 + b.g * w1 + c.g * w2;
			const float bl = a.b * w0 + b.b * w1 + c.b * w2;
			uint16_t pixel = 0;
			bool stp = true;
			if (tex.textured) {
				const int u = static_cast<int>(std::floor(a.u * w0 + b.u * w1 + c.u * w2 + 0.5f));
				const int v = static_cast<int>(std::floor(a.v * w0 + b.v * w1 + c.v * w2 + 0.5f));
				const uint16_t texel = sampleTexture(u, v, tex.clut, tex.tpage);
				if ((texel & 0x7FFF) == 0)
					continue;
				stp = (texel & 0x8000) != 0;
				pixel = tex.rawTexture ? texel : modulateTexture(texel, r, g, bl, x, y);
			} else {
				pixel = color24To555(rgbWord(clamp8(static_cast<int>(r)), clamp8(static_cast<int>(g)), clamp8(static_cast<int>(bl))), x, y, true);
			}
			writePixel(x, y, pixel, tex.semiTransparent, tex.tpage, tex.textured ? stp : true);
		}
	}
}

void PsxGpu::commandLine()
{
	const uint8_t cmd = static_cast<uint8_t>(m_packet[0] >> 24);
	const bool semi = (cmd & 0x02) != 0;
	const bool polyline = (cmd & 0x08) != 0;
	const bool gouraud = (cmd & 0x10) != 0;
	uint32_t color = m_packet[0] & 0x00FFFFFFu;
	size_t p = 1;
	std::vector<Vertex> vertices;

	if (!gouraud) {
		while (p < m_packet.size()) {
			const uint32_t xy = m_packet[p++];
			Vertex v;
			v.x = static_cast<int16_t>(xy & 0xFFFF) + m_drawOffsetX;
			v.y = static_cast<int16_t>(xy >> 16) + m_drawOffsetY;
			v.r = color & 0xFF;
			v.g = (color >> 8) & 0xFF;
			v.b = (color >> 16) & 0xFF;
			vertices.push_back(v);
			if (!polyline && vertices.size() == 2) break;
		}
	} else {
		if (p < m_packet.size()) {
			uint32_t xy = m_packet[p++];
			Vertex v;
			v.x = static_cast<int16_t>(xy & 0xFFFF) + m_drawOffsetX;
			v.y = static_cast<int16_t>(xy >> 16) + m_drawOffsetY;
			v.r = color & 0xFF; v.g = (color >> 8) & 0xFF; v.b = (color >> 16) & 0xFF;
			vertices.push_back(v);
		}
		while (p + 1 < m_packet.size()) {
			color = m_packet[p++] & 0x00FFFFFFu;
			const uint32_t xy = m_packet[p++];
			Vertex v;
			v.x = static_cast<int16_t>(xy & 0xFFFF) + m_drawOffsetX;
			v.y = static_cast<int16_t>(xy >> 16) + m_drawOffsetY;
			v.r = color & 0xFF; v.g = (color >> 8) & 0xFF; v.b = (color >> 16) & 0xFF;
			vertices.push_back(v);
			if (!polyline && vertices.size() == 2) break;
		}
	}

	for (size_t i = 1; i < vertices.size(); ++i)
		drawLineSegment(vertices[i - 1], vertices[i], semi, static_cast<uint16_t>(m_drawMode));
}

void PsxGpu::drawLineSegment(const Vertex& a, const Vertex& b, bool semiTransparent, uint16_t tpage)
{
	const float dx = b.x - a.x;
	const float dy = b.y - a.y;
	const int steps = std::max(std::abs(static_cast<int>(std::round(dx))), std::abs(static_cast<int>(std::round(dy))));
	if (steps == 0) {
		const uint16_t p = color24To555(rgbWord(clamp8(static_cast<int>(a.r)), clamp8(static_cast<int>(a.g)), clamp8(static_cast<int>(a.b))), static_cast<int>(a.x), static_cast<int>(a.y), true);
		writePixel(static_cast<int>(a.x), static_cast<int>(a.y), p, semiTransparent, tpage, true);
		return;
	}
	for (int i = 0; i <= steps; ++i) {
		const float t = static_cast<float>(i) / steps;
		const int x = static_cast<int>(std::round(a.x + dx * t));
		const int y = static_cast<int>(std::round(a.y + dy * t));
		const int r = static_cast<int>(a.r + (b.r - a.r) * t);
		const int g = static_cast<int>(a.g + (b.g - a.g) * t);
		const int bl = static_cast<int>(a.b + (b.b - a.b) * t);
		const uint16_t p = color24To555(rgbWord(clamp8(r), clamp8(g), clamp8(bl)), x, y, true);
		writePixel(x, y, p, semiTransparent, tpage, true);
	}
}

void PsxGpu::commandRectangle()
{
	const uint8_t cmd = static_cast<uint8_t>(m_packet[0] >> 24);
	TextureState tex{};
	tex.rawTexture = (cmd & 0x01) != 0;
	tex.semiTransparent = (cmd & 0x02) != 0;
	tex.textured = (cmd & 0x04) != 0;
	const int sizeCode = (cmd >> 3) & 0x3;
	const uint32_t color = m_packet[0] & 0x00FFFFFFu;
	const uint32_t xy = m_packet[1];
	Vertex origin;
	origin.x = static_cast<int16_t>(xy & 0xFFFF) + m_drawOffsetX;
	origin.y = static_cast<int16_t>(xy >> 16) + m_drawOffsetY;
	origin.r = color & 0xFF;
	origin.g = (color >> 8) & 0xFF;
	origin.b = (color >> 16) & 0xFF;
	tex.tpage = static_cast<uint16_t>(m_drawMode);
	size_t p = 2;
	if (tex.textured) {
		const uint32_t uv = m_packet[p++];
		origin.u = uv & 0xFF;
		origin.v = (uv >> 8) & 0xFF;
		tex.clut = static_cast<uint16_t>(uv >> 16);
	}
	int width = 0;
	int height = 0;
	if (sizeCode == 0) {
		const uint32_t wh = m_packet[p];
		width = wh & 0x3FF;
		height = (wh >> 16) & 0x1FF;
	} else if (sizeCode == 1) {
		width = height = 1;
	} else if (sizeCode == 2) {
		width = height = 8;
	} else {
		width = height = 16;
	}
	if (width > 0 && height > 0)
		drawRectanglePrimitive(origin, width, height, tex);
}

void PsxGpu::drawRectanglePrimitive(const Vertex& origin, int width, int height, const TextureState& tex)
{
	const bool flipX = (m_drawMode & (1u << 12)) != 0;
	const bool flipY = (m_drawMode & (1u << 13)) != 0;
	for (int py = 0; py < height; ++py) {
		for (int px = 0; px < width; ++px) {
			const int x = static_cast<int>(origin.x) + px;
			const int y = static_cast<int>(origin.y) + py;
			uint16_t pixel;
			bool stp = true;
			if (tex.textured) {
				const int u = static_cast<int>(origin.u) + (flipX ? -px : px);
				const int v = static_cast<int>(origin.v) + (flipY ? -py : py);
				const uint16_t texel = sampleTexture(u, v, tex.clut, tex.tpage);
				if ((texel & 0x7FFF) == 0)
					continue;
				stp = (texel & 0x8000) != 0;
				pixel = tex.rawTexture ? texel : modulateTexture(texel, origin.r, origin.g, origin.b, x, y);
			} else {
				pixel = color24To555(rgbWord(clamp8(static_cast<int>(origin.r)), clamp8(static_cast<int>(origin.g)), clamp8(static_cast<int>(origin.b))), x, y, false);
			}
			writePixel(x, y, pixel, tex.semiTransparent, tex.tpage, tex.textured ? stp : true);
		}
	}
}

} // namespace psxgpu
