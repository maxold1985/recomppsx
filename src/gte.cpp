#include "psxrecomp/gte.h"
#include "psxrecomp/compat.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace psxrecomp {

namespace {
const uint32_t F_MAC1_POS = 1u << 30;
const uint32_t F_MAC2_POS = 1u << 29;
const uint32_t F_MAC3_POS = 1u << 28;
const uint32_t F_MAC1_NEG = 1u << 27;
const uint32_t F_MAC2_NEG = 1u << 26;
const uint32_t F_MAC3_NEG = 1u << 25;
const uint32_t F_IR1_SAT  = 1u << 24;
const uint32_t F_IR2_SAT  = 1u << 23;
const uint32_t F_IR3_SAT  = 1u << 22;
const uint32_t F_R_SAT    = 1u << 21;
const uint32_t F_G_SAT    = 1u << 20;
const uint32_t F_B_SAT    = 1u << 19;
const uint32_t F_SZ_SAT   = 1u << 18;
const uint32_t F_DIV_OV   = 1u << 17;
const uint32_t F_MAC0_POS = 1u << 16;
const uint32_t F_MAC0_NEG = 1u << 15;
const uint32_t F_SX_SAT   = 1u << 14;
const uint32_t F_SY_SAT   = 1u << 13;
const uint32_t F_IR0_SAT  = 1u << 12;

static int64_t sar(int64_t v, unsigned s)
{
    return s ? (v >> s) : v;
}

static uint32_t packS16(int32_t lo, int32_t hi)
{
    return uint32_t(uint16_t(lo)) | (uint32_t(uint16_t(hi)) << 16);
}
}

void PsxGte::reset()
{
    m_data.fill(0);
    m_ctrl.fill(0);
    m_flags = 0;
    m_lastInstruction = 0;
}

int32_t PsxGte::clampS16(int64_t v, bool lm, uint32_t& flags, uint32_t bit)
{
    const int64_t lo = lm ? 0 : -32768;
    const int64_t hi = 32767;
    if (v < lo) { flags |= bit; return static_cast<int32_t>(lo); }
    if (v > hi) { flags |= bit; return static_cast<int32_t>(hi); }
    return static_cast<int32_t>(v);
}

uint16_t PsxGte::clampU16(int64_t v, uint32_t& flags, uint32_t bit)
{
    if (v < 0) { flags |= bit; return 0; }
    if (v > 65535) { flags |= bit; return 65535; }
    return static_cast<uint16_t>(v);
}

int32_t PsxGte::clampScreen(int64_t v, uint32_t& flags, uint32_t bit)
{
    if (v < -1024) { flags |= bit; return -1024; }
    if (v > 1023) { flags |= bit; return 1023; }
    return static_cast<int32_t>(v);
}

uint8_t PsxGte::clampColor(int64_t v, uint32_t& flags, uint32_t bit)
{
    if (v < 0) { flags |= bit; return 0; }
    if (v > 255) { flags |= bit; return 255; }
    return static_cast<uint8_t>(v);
}

int32_t PsxGte::clampIr0(int64_t v, uint32_t& flags)
{
    if (v < 0) { flags |= F_IR0_SAT; return 0; }
    if (v > 4096) { flags |= F_IR0_SAT; return 4096; }
    return static_cast<int32_t>(v);
}

PsxGte::Mat3 PsxGte::rotation() const
{
    Mat3 r{};
    r.m[0][0]=loS16(m_ctrl[0]); r.m[0][1]=hiS16(m_ctrl[0]); r.m[0][2]=loS16(m_ctrl[1]);
    r.m[1][0]=hiS16(m_ctrl[1]); r.m[1][1]=loS16(m_ctrl[2]); r.m[1][2]=hiS16(m_ctrl[2]);
    r.m[2][0]=loS16(m_ctrl[3]); r.m[2][1]=hiS16(m_ctrl[3]); r.m[2][2]=loS16(m_ctrl[4]);
    return r;
}

PsxGte::Mat3 PsxGte::light() const
{
    Mat3 r{};
    r.m[0][0]=loS16(m_ctrl[8]); r.m[0][1]=hiS16(m_ctrl[8]); r.m[0][2]=loS16(m_ctrl[9]);
    r.m[1][0]=hiS16(m_ctrl[9]); r.m[1][1]=loS16(m_ctrl[10]); r.m[1][2]=hiS16(m_ctrl[10]);
    r.m[2][0]=loS16(m_ctrl[11]); r.m[2][1]=hiS16(m_ctrl[11]); r.m[2][2]=loS16(m_ctrl[12]);
    return r;
}

PsxGte::Mat3 PsxGte::colorMatrix() const
{
    Mat3 r{};
    r.m[0][0]=loS16(m_ctrl[16]); r.m[0][1]=hiS16(m_ctrl[16]); r.m[0][2]=loS16(m_ctrl[17]);
    r.m[1][0]=hiS16(m_ctrl[17]); r.m[1][1]=loS16(m_ctrl[18]); r.m[1][2]=hiS16(m_ctrl[18]);
    r.m[2][0]=loS16(m_ctrl[19]); r.m[2][1]=hiS16(m_ctrl[19]); r.m[2][2]=loS16(m_ctrl[20]);
    return r;
}

PsxGte::Vec3 PsxGte::vector(unsigned index) const
{
    index = std::min(index, 2u);
    const unsigned b = index * 2u;
    return { loS16(m_data[b]), hiS16(m_data[b]), loS16(m_data[b+1]) };
}

PsxGte::Vec3 PsxGte::irVector() const
{
    return { loS16(m_data[9]), loS16(m_data[10]), loS16(m_data[11]) };
}

PsxGte::Vec3 PsxGte::translation(unsigned cv) const
{
    switch (cv & 3u) {
        case 0: return { static_cast<int32_t>(m_ctrl[5]), static_cast<int32_t>(m_ctrl[6]), static_cast<int32_t>(m_ctrl[7]) };
        case 1: return { static_cast<int32_t>(m_ctrl[13]), static_cast<int32_t>(m_ctrl[14]), static_cast<int32_t>(m_ctrl[15]) };
        case 2: return { static_cast<int32_t>(m_ctrl[21]), static_cast<int32_t>(m_ctrl[22]), static_cast<int32_t>(m_ctrl[23]) };
        default:return {};
    }
}

void PsxGte::clearFlags()
{
    m_flags = 0;
    m_ctrl[31] = 0;
}

void PsxGte::finalizeFlags()
{
    // Hardware summary bit is set by the error/saturation flags relevant to commands.
    if (m_flags & 0x7F87E000u) m_flags |= 0x80000000u;
    m_ctrl[31] = m_flags;
}

void PsxGte::setMacIr(const int64_t raw[3], bool sf, bool lm)
{
    const unsigned shift = sf ? 12u : 0u;
    const uint32_t posBits[3] = {F_MAC1_POS,F_MAC2_POS,F_MAC3_POS};
    const uint32_t negBits[3] = {F_MAC1_NEG,F_MAC2_NEG,F_MAC3_NEG};
    const uint32_t irBits[3] = {F_IR1_SAT,F_IR2_SAT,F_IR3_SAT};
    for (unsigned i=0;i<3;++i) {
        if (raw[i] > ((int64_t(1)<<43)-1)) m_flags |= posBits[i];
        if (raw[i] < -(int64_t(1)<<43)) m_flags |= negBits[i];
        const int64_t mac = sar(raw[i], shift);
        m_data[25+i] = static_cast<uint32_t>(mac);
        const int32_t ir = clampS16(mac, lm, m_flags, irBits[i]);
        m_data[9+i] = static_cast<uint32_t>(ir);
    }
}

void PsxGte::mvmva(const Mat3& m, const Vec3& v, const Vec3& t, bool sf, bool lm)
{
    int64_t raw[3]{};
    const int32_t vv[3]={v.x,v.y,v.z};
    const int32_t tt[3]={t.x,t.y,t.z};
    for (unsigned r=0;r<3;++r) {
        raw[r] = int64_t(tt[r]) * 4096;
        for (unsigned c=0;c<3;++c) raw[r] += int64_t(m.m[r][c]) * vv[c];
    }
    setMacIr(raw,sf,lm);
}

void PsxGte::pushSxy(int32_t sx, int32_t sy)
{
    m_data[12]=m_data[13];
    m_data[13]=m_data[14];
    m_data[14]=packS16(sx,sy);
    m_data[15]=m_data[14];
}

void PsxGte::pushSz(uint16_t sz)
{
    m_data[16]=m_data[17];
    m_data[17]=m_data[18];
    m_data[18]=m_data[19];
    m_data[19]=sz;
}

void PsxGte::transformPerspective(unsigned vecIndex, bool sf, bool lm, bool updateIr0)
{
    const Mat3 r=rotation();
    const Vec3 v=vector(vecIndex);
    const Vec3 t=translation(0);
    int64_t raw[3]{};
    const int32_t vv[3]={v.x,v.y,v.z};
    const int32_t tt[3]={t.x,t.y,t.z};
    for(unsigned row=0;row<3;++row){
        raw[row]=int64_t(tt[row])*4096;
        for(unsigned c=0;c<3;++c) raw[row]+=int64_t(r.m[row][c])*vv[c];
    }
    setMacIr(raw,sf,lm);

    const uint16_t sz=clampU16(raw[2]>>12,m_flags,F_SZ_SAT);
    pushSz(sz);

    const uint32_t h=loU16(m_ctrl[26]);
    uint32_t n=0x1FFFFu;
    if (sz != 0 && uint32_t(h) < uint32_t(sz)*2u) {
        const uint64_t q=(uint64_t(h)*0x10000ull + (sz/2u))/sz;
        n=static_cast<uint32_t>(std::min<uint64_t>(q,0x1FFFFu));
    } else {
        m_flags|=F_DIV_OV;
    }

    const int32_t ir1=loS16(m_data[9]);
    const int32_t ir2=loS16(m_data[10]);
    int64_t mac0=int64_t(n)*ir1 + static_cast<int32_t>(m_ctrl[24]);
    m_data[24]=static_cast<uint32_t>(mac0);
    int32_t sx=clampScreen(mac0>>16,m_flags,F_SX_SAT);
    mac0=int64_t(n)*ir2 + static_cast<int32_t>(m_ctrl[25]);
    m_data[24]=static_cast<uint32_t>(mac0);
    int32_t sy=clampScreen(mac0>>16,m_flags,F_SY_SAT);
    pushSxy(sx,sy);

    if(updateIr0){
        const int32_t dqa=loS16(m_ctrl[27]);
        const int32_t dqb=static_cast<int32_t>(m_ctrl[28]);
        mac0=int64_t(n)*dqa+dqb;
        if(mac0>std::numeric_limits<int32_t>::max()) m_flags|=F_MAC0_POS;
        if(mac0<std::numeric_limits<int32_t>::min()) m_flags|=F_MAC0_NEG;
        m_data[24]=static_cast<uint32_t>(mac0);
        m_data[8]=static_cast<uint32_t>(clampIr0(mac0>>12,m_flags));
    }
}

void PsxGte::pushColorFromMac(uint8_t code)
{
    const uint8_t r=clampColor(static_cast<int32_t>(m_data[25])/16,m_flags,F_R_SAT);
    const uint8_t g=clampColor(static_cast<int32_t>(m_data[26])/16,m_flags,F_G_SAT);
    const uint8_t b=clampColor(static_cast<int32_t>(m_data[27])/16,m_flags,F_B_SAT);
    m_data[20]=m_data[21];
    m_data[21]=m_data[22];
    m_data[22]=uint32_t(r)|(uint32_t(g)<<8)|(uint32_t(b)<<16)|(uint32_t(code)<<24);
}

void PsxGte::colorLightStage(const Vec3& normal,bool sf,bool lm)
{
    mvmva(light(),normal,{},sf,lm);
}

void PsxGte::colorMatrixStage(bool sf,bool lm)
{
    mvmva(colorMatrix(),irVector(),translation(1),sf,lm);
}

void PsxGte::multiplyPrimaryColor(bool sf,bool lm)
{
    const uint32_t rgbc=m_data[6];
    const int32_t c[3]={int32_t(rgbc&0xFFu),int32_t((rgbc>>8)&0xFFu),int32_t((rgbc>>16)&0xFFu)};
    const int32_t ir[3]={loS16(m_data[9]),loS16(m_data[10]),loS16(m_data[11])};
    int64_t raw[3]{};
    for(unsigned i=0;i<3;++i) raw[i]=int64_t(c[i])*ir[i]*16;
    setMacIr(raw,sf,lm);
}

void PsxGte::depthCue(bool sf,bool lm,bool pushColor)
{
    const int32_t fc[3]={static_cast<int32_t>(m_ctrl[21]),static_cast<int32_t>(m_ctrl[22]),static_cast<int32_t>(m_ctrl[23])};
    const int32_t ir0=loS16(m_data[8]);
    const unsigned shift=sf?12u:0u;
    int64_t raw[3]{};
    const uint32_t irBits[3]={F_IR1_SAT,F_IR2_SAT,F_IR3_SAT};
    for(unsigned i=0;i<3;++i){
        const int64_t mac=static_cast<int32_t>(m_data[25+i]);
        const int64_t diff=sar((int64_t(fc[i])<<12)-mac,shift);
        const int32_t diffIr=clampS16(diff,false,m_flags,irBits[i]);
        raw[i]=int64_t(diffIr)*ir0+mac;
    }
    setMacIr(raw,sf,lm);
    if(pushColor) pushColorFromMac(uint8_t(m_data[6]>>24));
}

void PsxGte::executeColorSingle(unsigned vecIndex,bool sf,bool lm,bool multiplyColor,bool cue)
{
    colorLightStage(vector(vecIndex),sf,lm);
    colorMatrixStage(sf,lm);
    if(multiplyColor) multiplyPrimaryColor(sf,lm);
    if(cue) depthCue(sf,lm,false);
    pushColorFromMac(uint8_t(m_data[6]>>24));
}

uint32_t PsxGte::read_data(uint32_t reg)
{
    reg &= 31u;
    if(reg==15) return m_data[14];
    if(reg==28 || reg==29){
        auto q=[](int32_t v)->uint32_t { return uint32_t(compat::clamp_value(v>>7,0,31)); };
        return q(loS16(m_data[9])) | (q(loS16(m_data[10]))<<5) | (q(loS16(m_data[11]))<<10);
    }
    return m_data[reg];
}

uint32_t PsxGte::read_ctrl(uint32_t reg)
{
    reg &= 31u;
    // 16-bit control registers sign-extend on CFC2, H famously sign-extends too.
    switch(reg){
        case 4: case 12: case 20: case 26: case 27: case 29: case 30:
            return static_cast<uint32_t>(static_cast<int32_t>(loS16(m_ctrl[reg])));
        case 31:
            return m_flags;
        default:
            return m_ctrl[reg];
    }
}

void PsxGte::write_data(uint32_t reg,uint32_t value)
{
    reg&=31u;
    switch(reg){
        case 7: m_data[7]=value&0xFFFFu; break;
        case 8: case 9: case 10: case 11:
            m_data[reg]=static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(value)));
            break;
        case 15:
            m_data[12]=m_data[13]; m_data[13]=m_data[14]; m_data[14]=value; m_data[15]=value;
            break;
        case 16: case 17: case 18: case 19:
            m_data[reg]=value&0xFFFFu; break;
        case 28: {
            m_data[28]=value&0x7FFFu;
            m_data[9]=((value>>0)&31u)<<7;
            m_data[10]=((value>>5)&31u)<<7;
            m_data[11]=((value>>10)&31u)<<7;
            break;
        }
        case 30: {
            m_data[30]=value;
            uint32_t x=value;
            unsigned count=0;
            if(static_cast<int32_t>(x)<0) x=~x;
            if(x==0) count=32;
            else { while((x&0x80000000u)==0){ ++count; x<<=1; } }
            m_data[31]=count;
            break;
        }
        default: m_data[reg]=value; break;
    }
}

void PsxGte::write_ctrl(uint32_t reg,uint32_t value)
{
    reg&=31u;
    switch(reg){
        case 4: case 12: case 20: case 27: case 29: case 30:
            m_ctrl[reg]=static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(value)));
            break;
        case 26:
            m_ctrl[reg]=value&0xFFFFu; break;
        case 31:
            m_flags=value&0x7FFFF000u; finalizeFlags(); break;
        default:
            m_ctrl[reg]=value; break;
    }
}

void PsxGte::execute(uint32_t instruction)
{
    m_lastInstruction=instruction;
    clearFlags();
    const uint32_t op=instruction&0x3Fu;
    const bool sf=(instruction&(1u<<19))!=0;
    const bool lm=(instruction&(1u<<10))!=0;

    switch(op){
        case 0x01: transformPerspective(0,sf,lm,true); break; // RTPS
        case 0x30: // RTPT
            transformPerspective(0,sf,lm,false);
            transformPerspective(1,sf,lm,false);
            transformPerspective(2,sf,lm,true);
            break;
        case 0x06: { // NCLIP
            const int32_t sx0=loS16(m_data[12]), sy0=hiS16(m_data[12]);
            const int32_t sx1=loS16(m_data[13]), sy1=hiS16(m_data[13]);
            const int32_t sx2=loS16(m_data[14]), sy2=hiS16(m_data[14]);
            const int64_t mac=int64_t(sx0)*sy1+int64_t(sx1)*sy2+int64_t(sx2)*sy0-int64_t(sx0)*sy2-int64_t(sx1)*sy0-int64_t(sx2)*sy1;
            if(mac>std::numeric_limits<int32_t>::max()) m_flags|=F_MAC0_POS;
            if(mac<std::numeric_limits<int32_t>::min()) m_flags|=F_MAC0_NEG;
            m_data[24]=static_cast<uint32_t>(mac);
            break;
        }
        case 0x2D: { // AVSZ3
            const int64_t mac=int64_t(loS16(m_ctrl[29]))*(loU16(m_data[17])+loU16(m_data[18])+loU16(m_data[19]));
            m_data[24]=static_cast<uint32_t>(mac);
            m_data[7]=clampU16(mac>>12,m_flags,F_SZ_SAT);
            break;
        }
        case 0x2E: { // AVSZ4
            const int64_t mac=int64_t(loS16(m_ctrl[30]))*(loU16(m_data[16])+loU16(m_data[17])+loU16(m_data[18])+loU16(m_data[19]));
            m_data[24]=static_cast<uint32_t>(mac);
            m_data[7]=clampU16(mac>>12,m_flags,F_SZ_SAT);
            break;
        }
        case 0x12: { // MVMVA
            const unsigned mx=(instruction>>17)&3u, v=(instruction>>15)&3u, cv=(instruction>>13)&3u;
            Mat3 mat{};
            if(mx==0) mat=rotation(); else if(mx==1) mat=light(); else if(mx==2) mat=colorMatrix(); else mat=rotation();
            const Vec3 vec=(v<3)?vector(v):irVector();
            mvmva(mat,vec,translation(cv),sf,lm);
            break;
        }
        case 0x0C: { // OP
            const Vec3 ir=irVector(); const Mat3 r=rotation();
            const int64_t raw[3]={int64_t(ir.z)*r.m[1][1]-int64_t(ir.y)*r.m[2][2], int64_t(ir.x)*r.m[2][2]-int64_t(ir.z)*r.m[0][0], int64_t(ir.y)*r.m[0][0]-int64_t(ir.x)*r.m[1][1]};
            setMacIr(raw,sf,lm); break;
        }
        case 0x28: { // SQR
            const Vec3 ir=irVector(); const int64_t raw[3]={int64_t(ir.x)*ir.x,int64_t(ir.y)*ir.y,int64_t(ir.z)*ir.z};
            setMacIr(raw,sf,true); break;
        }
        case 0x1E: executeColorSingle(0,sf,lm,false,false); break; // NCS
        case 0x20: for(unsigned i=0;i<3;++i) executeColorSingle(i,sf,lm,false,false); break; // NCT
        case 0x1B: executeColorSingle(0,sf,lm,true,false); break; // NCCS
        case 0x3F: for(unsigned i=0;i<3;++i) executeColorSingle(i,sf,lm,true,false); break; // NCCT
        case 0x13: executeColorSingle(0,sf,lm,true,true); break; // NCDS
        case 0x16: for(unsigned i=0;i<3;++i) executeColorSingle(i,sf,lm,true,true); break; // NCDT
        case 0x1C: // CC
            colorMatrixStage(sf,lm); multiplyPrimaryColor(sf,lm); pushColorFromMac(uint8_t(m_data[6]>>24)); break;
        case 0x14: // CDP
            colorMatrixStage(sf,lm); multiplyPrimaryColor(sf,lm); depthCue(sf,lm,true); break;
        case 0x10: { // DPCS
            const uint32_t rgb=m_data[6]; const int32_t c[3]={int32_t(rgb&255u),int32_t((rgb>>8)&255u),int32_t((rgb>>16)&255u)};
            for(unsigned i=0;i<3;++i) m_data[25+i]=uint32_t(c[i]<<16);
            depthCue(sf,lm,true); break;
        }
        case 0x11: { // INTPL
            const Vec3 ir=irVector(); m_data[25]=uint32_t(ir.x<<12);m_data[26]=uint32_t(ir.y<<12);m_data[27]=uint32_t(ir.z<<12); depthCue(sf,lm,true); break;
        }
        case 0x29: { // DCPL
            const uint32_t rgb=m_data[6]; const int32_t c[3]={int32_t(rgb&255u),int32_t((rgb>>8)&255u),int32_t((rgb>>16)&255u)}; const Vec3 ir=irVector(); const int32_t vv[3]={ir.x,ir.y,ir.z};
            for(unsigned i=0;i<3;++i) {
                m_data[25+i]=uint32_t(int64_t(c[i])*vv[i]*16);
            }
            depthCue(sf,lm,true);
            break;
        }
        case 0x2A: // DPCT
            for(unsigned k=0;k<3;++k){ const uint32_t rgb=m_data[20]; const int32_t c[3]={int32_t(rgb&255u),int32_t((rgb>>8)&255u),int32_t((rgb>>16)&255u)}; for(unsigned i=0;i<3;++i)m_data[25+i]=uint32_t(c[i]<<16); depthCue(sf,lm,true);} break;
        case 0x3D: // GPF
        case 0x3E: { // GPL
            const Vec3 ir=irVector(); const int32_t ir0=loS16(m_data[8]); const int32_t vv[3]={ir.x,ir.y,ir.z}; int64_t raw[3]{};
            for(unsigned i=0;i<3;++i){ const int64_t base=(op==0x3E)?(int64_t(static_cast<int32_t>(m_data[25+i]))<<(sf?12:0)):0; raw[i]=base+int64_t(vv[i])*ir0; }
            setMacIr(raw,sf,lm); pushColorFromMac(uint8_t(m_data[6]>>24)); break;
        }
        default:
            // Unknown GTE commands are intentionally side-effect free except FLAG reset.
            break;
    }
    finalizeFlags();
}

} // namespace psxrecomp
