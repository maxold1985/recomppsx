#pragma once

#include "psx_gpu.h"

#include <cstdint>

namespace psxgpu {

class PsxGpuMmio {
public:
	static const uint32_t GP0 = 0x1F801810;
	static const uint32_t GP1 = 0x1F801814;
	static const uint32_t DMA2_MADR = 0x1F8010A0;
	static const uint32_t DMA2_BCR  = 0x1F8010A4;
	static const uint32_t DMA2_CHCR = 0x1F8010A8;
	static const uint32_t DMA6_MADR = 0x1F8010E0;
	static const uint32_t DMA6_BCR  = 0x1F8010E4;
	static const uint32_t DMA6_CHCR = 0x1F8010E8;
	static const uint32_t DPCR      = 0x1F8010F0;

	PsxGpuMmio(PsxGpu& gpu, PsxGpu::Read32 read32, PsxGpu::Write32 write32);

	uint32_t read32(uint32_t address);
	void write32(uint32_t address, uint32_t value);

private:
	void runDma2();
	void runDma6();

	PsxGpu& m_gpu;
	PsxGpu::Read32 m_read32;
	PsxGpu::Write32 m_write32;

	uint32_t m_dma2Madr = 0;
	uint32_t m_dma2Bcr = 0;
	uint32_t m_dma2Chcr = 0;
	uint32_t m_dma6Madr = 0;
	uint32_t m_dma6Bcr = 0;
	uint32_t m_dma6Chcr = 0;
	uint32_t m_dpcr = 0;
};

} // namespace psxgpu
