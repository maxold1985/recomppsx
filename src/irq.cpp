#include "psxrecomp/irq.h"
#include "psxrecomp/trace.h"

namespace psxrecomp {

void IrqController::reset(){m_stat=0;m_mask=0;}
void IrqController::request(Source source){const uint16_t before=m_stat;m_stat|=static_cast<uint16_t>(source);tracePrintf("[IRQ REQUEST] source=%04X stat=%04X->%04X mask=%04X pending=%d\n",(unsigned)source,before,m_stat,m_mask,pending()?1:0);}
void IrqController::requestMask(uint16_t bits){m_stat|=(bits&0x07FFu);}
void IrqController::acknowledge(uint16_t value){const uint16_t before=m_stat;m_stat&=(value&0x07FFu);tracePrintf("[IRQ ACK] value=%04X stat=%04X->%04X mask=%04X\n",value,before,m_stat,m_mask);}
void IrqController::updateCop0(r3k::CpuState& cpu) const{const uint32_t kCauseIp2=1u<<10;if(pending())cpu.cop0[13]|=kCauseIp2;else cpu.cop0[13]&=~kCauseIp2;}
bool IrqController::takeInterrupt(r3k::CpuState& cpu) const{
 const uint32_t kStatusIEc=1u<<0,kStatusIM2=1u<<10;
 if(!pending())return false;
 if((cpu.cop0[12]&(kStatusIEc|kStatusIM2))!=(kStatusIEc|kStatusIM2))return false;
 const uint32_t low=cpu.cop0[12]&0x3Fu;
 cpu.cop0[12]=(cpu.cop0[12]&~0x3Fu)|((low<<2)&0x3Fu);
 cpu.cop0[13]=(cpu.cop0[13]&~0x7Cu);
 cpu.cop0[14]=cpu.pc;
 const bool bev=(cpu.cop0[12]&(1u<<22))!=0;
 tracePrintf("[IRQ/EXCEPTION] oldPC=%08X Cause=%08X Status=%08X EPC=%08X cycles=%llu BEV=%d\n",cpu.pc,cpu.cop0[13],cpu.cop0[12],cpu.cop0[14],(unsigned long long)cpu.cycles,bev?1:0);
 std::fflush(stdout);
 cpu.pc=bev?0xBFC00180u:0x80000080u;
 return true;
}
} // namespace psxrecomp
