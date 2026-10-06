#pragma once

#include <cstdint>

namespace psxgpu { namespace mml2_slps02711 {

// Rockman DASH 2 / Mega Man Legends 2 (SLPS-02711)
// Addresses identified in the supplied PS-X EXE.
const uint32_t ResetGraph        = 0x80060A70;
const uint32_t SetGraphDebug     = 0x80060BE8;
const uint32_t SetGrapQue        = 0x80060C44;
const uint32_t DrawSyncCallback  = 0x80060CF8;
const uint32_t SetDispMask       = 0x80060D58;
const uint32_t DrawSync          = 0x80060DF0;
const uint32_t ClearImage        = 0x80060F74;
const uint32_t ClearImage2       = 0x80061004;
const uint32_t LoadImage         = 0x8006109C;
const uint32_t StoreImage        = 0x800610FC;
const uint32_t MoveImage         = 0x8006115C;
const uint32_t ClearOTag         = 0x80061214;
const uint32_t ClearOTagR        = 0x800612DC;
const uint32_t DrawOTag          = 0x800613E4;
const uint32_t PutDrawEnv        = 0x80061454;
const uint32_t DrawOTagEnv       = 0x80061514;
const uint32_t PutDispEnv        = 0x80061620;

const uint32_t GpuStatusRead     = 0x80062290;
const uint32_t ClearOTagRLow     = 0x800622A8;
const uint32_t LoadImageLow      = 0x800625B8;
const uint32_t StoreImageLow     = 0x800627F4;
const uint32_t Gp1WriteLow       = 0x80062A74;
const uint32_t Gp0WriteLow       = 0x80062A90;
const uint32_t DmaLinkedListLow  = 0x80062AD0;
const uint32_t GpuGetInfoLow     = 0x80062B18;
const uint32_t GpuQueueLow       = 0x80062B6C;

const uint32_t Gp0Register       = 0x1F801810;
const uint32_t Gp1Register       = 0x1F801814;
const uint32_t Dma2Madr          = 0x1F8010A0;
const uint32_t Dma2Bcr           = 0x1F8010A4;
const uint32_t Dma2Chcr          = 0x1F8010A8;
const uint32_t Dma6Madr          = 0x1F8010E0;
const uint32_t Dma6Bcr           = 0x1F8010E4;
const uint32_t Dma6Chcr          = 0x1F8010E8;
const uint32_t Dpcr              = 0x1F8010F0;

} // namespace mml2_slps02711
} // namespace psxgpu
