#include "psxgpu/psx_gpu_mmio.h"

#include <utility>
#include <cstdio>
namespace psxgpu {

PsxGpuMmio::PsxGpuMmio(PsxGpu& gpu, PsxGpu::Read32 read32, PsxGpu::Write32 write32)
	: m_gpu(gpu), m_read32(std::move(read32)), m_write32(std::move(write32))
{
}

uint32_t PsxGpuMmio::read32(uint32_t address)
{
	static unsigned gp0Reads = 0;
	static unsigned gp1Reads = 0;

	switch(address) {
		case GP0: {
			const uint32_t value = m_gpu.readData();

			if(gp0Reads < 100) {
				std::printf(
					"[GPU READ GP0 #%u] value=%08X\n",
					gp0Reads,
					static_cast<unsigned>(value)
				);
				std::fflush(stdout);
			}

			++gp0Reads;
			return value;
		}

		case GP1: {
			const uint32_t value = m_gpu.readStatus();

			if(gp1Reads < 200) {
				std::printf(
					"[GPU READ STAT #%u] value=%08X\n",
					gp1Reads,
					static_cast<unsigned>(value)
				);
				std::fflush(stdout);
			}

			++gp1Reads;
			return value;
		}

		case DMA2_MADR:
			return m_dma2Madr;

		case DMA2_BCR:
			return m_dma2Bcr;

		case DMA2_CHCR:
			return m_dma2Chcr;

		case DMA6_MADR:
			return m_dma6Madr;

		case DMA6_BCR:
			return m_dma6Bcr;

		case DMA6_CHCR:
			return m_dma6Chcr;

		case DPCR:
			return m_dpcr;

		default:
			return 0;
	}
}
void PsxGpuMmio::write32(
	uint32_t address,
	uint32_t value
)
{
	static unsigned mmioLogCount = 0;

	if(mmioLogCount < 200)
	{
		std::printf(
			"[GPU MMIO #%u] addr=%08X value=%08X\n",
			mmioLogCount,
			static_cast<unsigned>(address),
			static_cast<unsigned>(value)
		);

		std::fflush(stdout);

		++mmioLogCount;
	}

	switch(address)
	{
		case GP0:
		{
			m_gpu.writeGp0(value);
			break;
		}

		case GP1:
		{
			m_gpu.writeGp1(value);
			break;
		}

		case DMA2_MADR:
		{
			m_dma2Madr =
				value & 0x00FFFFFFu;

			std::printf(
				"[DMA2 MADR] value=%08X\n",
				static_cast<unsigned>(m_dma2Madr)
			);

			break;
		}

		case DMA2_BCR:
		{
			m_dma2Bcr = value;

			std::printf(
				"[DMA2 BCR] value=%08X\n",
				static_cast<unsigned>(value)
			);

			break;
		}

		case DMA2_CHCR:
		{
			m_dma2Chcr = value;

			const unsigned start =
				(value >> 24) & 1u;

			const unsigned direction =
				value & 1u;

			const unsigned syncMode =
				(value >> 9) & 3u;

			std::printf(
				"[DMA2 CHCR] "
				"value=%08X start=%u dir=%u sync=%u\n",
				static_cast<unsigned>(value),
				start,
				direction,
				syncMode
			);

			std::fflush(stdout);

			if(value & (1u << 24))
			{
				runDma2();
			}

			break;
		}

		case DMA6_MADR:
		{
			m_dma6Madr =
				value & 0x00FFFFFFu;

			break;
		}

		case DMA6_BCR:
		{
			m_dma6Bcr = value;

			break;
		}

		case DMA6_CHCR:
		{
			m_dma6Chcr = value;

			if(value & (1u << 24))
			{
				runDma6();
			}

			break;
		}

		case DPCR:
		{
			m_dpcr = value;

			const unsigned dma2Priority =
				(value >> 8) & 7u;

			const unsigned dma2Enable =
				(value >> 11) & 1u;

			std::printf(
				"[DMA DPCR] "
				"value=%08X "
				"DMA2enable=%u "
				"DMA2priority=%u\n",
				static_cast<unsigned>(value),
				dma2Enable,
				dma2Priority
			);

			std::fflush(stdout);

			break;
		}

		default:
		{
			break;
		}
	}
}

/*void PsxGpuMmio::runDma2()
{
	const bool fromRam = (m_dma2Chcr & 1u) != 0;
	const uint32_t syncMode = (m_dma2Chcr >> 9) & 3u;
	if (syncMode == 2 && fromRam) {
		m_gpu.dmaLinkedList(m_dma2Madr, m_read32);
	} else {
		uint32_t words = m_dma2Bcr & 0xFFFFu;
		const uint32_t blocks = (m_dma2Bcr >> 16) & 0xFFFFu;
		if (syncMode == 1)
			words *= blocks;
		if (fromRam)
			m_gpu.dmaBlockToGpu(m_dma2Madr, words, m_read32);
		else
			m_gpu.dmaGpuToRam(m_dma2Madr, words, m_write32);
	}
	m_dma2Chcr &= ~(1u << 24);
}
*/
void PsxGpuMmio::runDma2()
{
	const bool fromRam = (m_dma2Chcr & 1u) != 0;
	const uint32_t syncMode = (m_dma2Chcr >> 9) & 3u;

	std::printf(
		"[DMA2] MADR=%06X BCR=%08X CHCR=%08X "
		"fromRam=%d sync=%u read32=%d\n",
		static_cast<unsigned>(m_dma2Madr),
		static_cast<unsigned>(m_dma2Bcr),
		static_cast<unsigned>(m_dma2Chcr),
		fromRam ? 1 : 0,
		static_cast<unsigned>(syncMode),
		m_read32 ? 1 : 0
	);
	std::fflush(stdout);

	if (syncMode == 2 && fromRam) {
		std::printf(
			"[DMA2] linked-list start=%06X\n",
			static_cast<unsigned>(m_dma2Madr)
		);
		std::fflush(stdout);

		m_gpu.dmaLinkedList(
			m_dma2Madr,
			m_read32
		);
	} else {
		uint32_t words = m_dma2Bcr & 0xFFFFu;
		const uint32_t blocks =
			(m_dma2Bcr >> 16) & 0xFFFFu;

		if (syncMode == 1)
			words *= blocks;

		std::printf(
			"[DMA2] block words=%u blocks=%u\n",
			static_cast<unsigned>(words),
			static_cast<unsigned>(blocks)
		);
		std::fflush(stdout);

		if (fromRam) {
			m_gpu.dmaBlockToGpu(
				m_dma2Madr,
				words,
				m_read32
			);
		} else {
			m_gpu.dmaGpuToRam(
				m_dma2Madr,
				words,
				m_write32
			);
		}
	}

	m_dma2Chcr &= ~(1u << 24);
}
void PsxGpuMmio::runDma6()
{
	const uint32_t words = m_dma6Bcr & 0xFFFFu;
	m_gpu.dmaOtc(m_dma6Madr, words, m_write32);
	m_dma6Chcr &= ~(1u << 24);
}

} // namespace psxgpu
