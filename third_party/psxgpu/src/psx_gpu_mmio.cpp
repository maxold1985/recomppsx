#include "psxgpu/psx_gpu_mmio.h"
#include <utility>
#include <cstdio>
namespace psxgpu {
PsxGpuMmio::PsxGpuMmio(PsxGpu& gpu, PsxGpu::Read32 read32, PsxGpu::Write32 write32):m_gpu(gpu),m_read32(std::move(read32)),m_write32(std::move(write32)){}
uint32_t PsxGpuMmio::read32(uint32_t address){switch(address){case GP0:return m_gpu.readData();case GP1:return m_gpu.readStatus();case DMA2_MADR:return m_dma2Madr;case DMA2_BCR:return m_dma2Bcr;case DMA2_CHCR:return m_dma2Chcr;case DMA6_MADR:return m_dma6Madr;case DMA6_BCR:return m_dma6Bcr;case DMA6_CHCR:return m_dma6Chcr;case DPCR:return m_dpcr;default:return 0;}}
void PsxGpuMmio::write32(uint32_t address,uint32_t value){switch(address){case GP0:m_gpu.writeGp0(value);break;case GP1:m_gpu.writeGp1(value);break;case DMA2_MADR:m_dma2Madr=value&0x00FFFFFFu;break;case DMA2_BCR:m_dma2Bcr=value;break;case DMA2_CHCR:m_dma2Chcr=value;if(value&(1u<<24))runDma2();break;case DMA6_MADR:m_dma6Madr=value&0x00FFFFFFu;break;case DMA6_BCR:m_dma6Bcr=value;break;case DMA6_CHCR:m_dma6Chcr=value;if(value&(1u<<24))runDma6();break;case DPCR:m_dpcr=value;break;default:break;}}
void PsxGpuMmio::runDma2(){const bool fromRam=(m_dma2Chcr&1u)!=0;const uint32_t syncMode=(m_dma2Chcr>>9)&3u;if(syncMode==2&&fromRam)m_gpu.dmaLinkedList(m_dma2Madr,m_read32);else{uint32_t words=m_dma2Bcr&0xFFFFu;const uint32_t blocks=(m_dma2Bcr>>16)&0xFFFFu;if(syncMode==1)words*=blocks;if(fromRam)m_gpu.dmaBlockToGpu(m_dma2Madr,words,m_read32);else m_gpu.dmaGpuToRam(m_dma2Madr,words,m_write32);}m_dma2Chcr&=~(1u<<24);}
void PsxGpuMmio::runDma6(){const uint32_t words=m_dma6Bcr&0xFFFFu;m_gpu.dmaOtc(m_dma6Madr,words,m_write32);m_dma6Chcr&=~(1u<<24);}
}
