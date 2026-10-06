#include "recompiler.h"
#include "decoder.h"
#include <sstream>
#include <iomanip>
#include <unordered_set>
#include <queue>
#include <cstring>

static uint32_t rd32le(const std::vector<uint8_t>& p, size_t off) {
    if (off + 4 > p.size()) return 0;
    return uint32_t(p[off]) | (uint32_t(p[off+1]) << 8) | (uint32_t(p[off+2]) << 16) | (uint32_t(p[off+3]) << 24);
}

static std::string hex8(uint32_t v) {
    std::ostringstream o; o << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << v; return o.str();
}

static bool in_image(const PsxExeImage& e, uint32_t pc) {
    return pc >= e.load_address && uint64_t(pc - e.load_address) + 4 <= e.payload.size();
}

static uint32_t ins_at(const PsxExeImage& e, uint32_t pc) {
    return rd32le(e.payload, pc - e.load_address);
}

static void emit_exec(std::ostringstream& o, uint32_t pc, const Decoded& d) {
    auto G=[&](int r){ return std::string("s.gpr[")+std::to_string(r)+"]"; };
    if (d.raw == 0) return;
    if (d.op == 0) {
        switch (d.funct) {
            case 0x00: if (d.rd) o << G(d.rd) << " = " << G(d.rt) << " << " << int(d.sa) << ";\n"; return;
            case 0x02: if (d.rd) o << G(d.rd) << " = " << G(d.rt) << " >> " << int(d.sa) << ";\n"; return;
            case 0x03: if (d.rd) o << G(d.rd) << " = uint32_t(int32_t("<<G(d.rt)<<") >> " << int(d.sa) << ");\n"; return;
            case 0x04: if (d.rd) o << G(d.rd) << " = " << G(d.rt) << " << (" << G(d.rs) << " & 31);\n"; return;
            case 0x06: if (d.rd) o << G(d.rd) << " = " << G(d.rt) << " >> (" << G(d.rs) << " & 31);\n"; return;
            case 0x07: if (d.rd) o << G(d.rd) << " = uint32_t(int32_t("<<G(d.rt)<<") >> ("<<G(d.rs)<<" & 31));\n"; return;
            case 0x10: if (d.rd) o << G(d.rd) << " = s.hi;\n"; return;
            case 0x11: o << "s.hi = " << G(d.rs) << ";\n"; return;
            case 0x12: if (d.rd) o << G(d.rd) << " = s.lo;\n"; return;
            case 0x13: o << "s.lo = " << G(d.rs) << ";\n"; return;
            case 0x18: o << "{ int64_t q=int64_t(int32_t("<<G(d.rs)<<"))*int64_t(int32_t("<<G(d.rt)<<")); s.lo=uint32_t(q); s.hi=uint32_t(uint64_t(q)>>32); }\n"; return;
            case 0x19: o << "{ uint64_t q=uint64_t("<<G(d.rs)<<")*uint64_t("<<G(d.rt)<<"); s.lo=uint32_t(q); s.hi=uint32_t(q>>32); }\n"; return;
            case 0x1A: o << "r3k_div_signed(s, "<<G(d.rs)<<", "<<G(d.rt)<<");\n"; return;
            case 0x1B: o << "r3k_div_unsigned(s, "<<G(d.rs)<<", "<<G(d.rt)<<");\n"; return;
            case 0x20: case 0x21: if (d.rd) o << G(d.rd) << " = "<<G(d.rs)<<" + "<<G(d.rt)<<";\n"; return;
            case 0x22: case 0x23: if (d.rd) o << G(d.rd) << " = "<<G(d.rs)<<" - "<<G(d.rt)<<";\n"; return;
            case 0x24: if (d.rd) o << G(d.rd) << " = "<<G(d.rs)<<" & "<<G(d.rt)<<";\n"; return;
            case 0x25: if (d.rd) o << G(d.rd) << " = "<<G(d.rs)<<" | "<<G(d.rt)<<";\n"; return;
            case 0x26: if (d.rd) o << G(d.rd) << " = "<<G(d.rs)<<" ^ "<<G(d.rt)<<";\n"; return;
            case 0x27: if (d.rd) o << G(d.rd) << " = ~("<<G(d.rs)<<" | "<<G(d.rt)<<");\n"; return;
            case 0x2A: if (d.rd) o << G(d.rd) << " = int32_t("<<G(d.rs)<<") < int32_t("<<G(d.rt)<<");\n"; return;
            case 0x2B: if (d.rd) o << G(d.rd) << " = "<<G(d.rs)<<" < "<<G(d.rt)<<";\n"; return;
            case 0x0C: o << "r3k_syscall(s);\n"; return;
            case 0x0D: o << "r3k_break(s);\n"; return;
            default: o << "r3k_unknown("<<hex8(d.raw)<<", "<<hex8(pc)<<");\n"; return;
        }
    }
    switch (d.op) {
        case 0x08: case 0x09: if (d.rt) o << G(d.rt) << " = "<<G(d.rs)<<" + uint32_t(int32_t(int16_t("<<d.imm<<")));\n"; return;
        case 0x0A: if (d.rt) o << G(d.rt) << " = int32_t("<<G(d.rs)<<") < int32_t(int16_t("<<d.imm<<"));\n"; return;
        case 0x0B: if (d.rt) o << G(d.rt) << " = "<<G(d.rs)<<" < uint32_t(int32_t(int16_t("<<d.imm<<")));\n"; return;
        case 0x0C: if (d.rt) o << G(d.rt) << " = "<<G(d.rs)<<" & "<<d.imm<<"u;\n"; return;
        case 0x0D: if (d.rt) o << G(d.rt) << " = "<<G(d.rs)<<" | "<<d.imm<<"u;\n"; return;
        case 0x0E: if (d.rt) o << G(d.rt) << " = "<<G(d.rs)<<" ^ "<<d.imm<<"u;\n"; return;
        case 0x0F: if (d.rt) o << G(d.rt) << " = "<<(uint32_t(d.imm)<<16)<<"u;\n"; return;
        case 0x20: if (d.rt) o << G(d.rt) << " = uint32_t(int32_t(int8_t(mem.read8("<<G(d.rs)<<" + uint32_t(int32_t(int16_t("<<d.imm<<")))))));\n"; return;
        case 0x21: if (d.rt) o << G(d.rt) << " = uint32_t(int32_t(int16_t(mem.read16("<<G(d.rs)<<" + uint32_t(int32_t(int16_t("<<d.imm<<")))))));\n"; return;
        case 0x22: if (d.rt) o << G(d.rt) << " = r3k_lwl(mem, "<<G(d.rs)<<" + uint32_t(int32_t(int16_t("<<d.imm<<"))), "<<G(d.rt)<<");\n"; return;
        case 0x23: if (d.rt) o << G(d.rt) << " = mem.read32("<<G(d.rs)<<" + uint32_t(int32_t(int16_t("<<d.imm<<"))));\n"; return;
        case 0x24: if (d.rt) o << G(d.rt) << " = mem.read8("<<G(d.rs)<<" + uint32_t(int32_t(int16_t("<<d.imm<<"))));\n"; return;
        case 0x25: if (d.rt) o << G(d.rt) << " = mem.read16("<<G(d.rs)<<" + uint32_t(int32_t(int16_t("<<d.imm<<"))));\n"; return;
        case 0x26: if (d.rt) o << G(d.rt) << " = r3k_lwr(mem, "<<G(d.rs)<<" + uint32_t(int32_t(int16_t("<<d.imm<<"))), "<<G(d.rt)<<");\n"; return;
        case 0x28: o << "mem.write8("<<G(d.rs)<<" + uint32_t(int32_t(int16_t("<<d.imm<<"))), uint8_t("<<G(d.rt)<<"));\n"; return;
        case 0x29: o << "mem.write16("<<G(d.rs)<<" + uint32_t(int32_t(int16_t("<<d.imm<<"))), uint16_t("<<G(d.rt)<<"));\n"; return;
        case 0x2A: o << "r3k_swl(mem, "<<G(d.rs)<<" + uint32_t(int32_t(int16_t("<<d.imm<<"))), "<<G(d.rt)<<");\n"; return;
        case 0x2B: o << "mem.write32("<<G(d.rs)<<" + uint32_t(int32_t(int16_t("<<d.imm<<"))), "<<G(d.rt)<<");\n"; return;
        case 0x2E: o << "r3k_swr(mem, "<<G(d.rs)<<" + uint32_t(int32_t(int16_t("<<d.imm<<"))), "<<G(d.rt)<<");\n"; return;
        case 0x10: {
            const uint8_t coprs=d.rs;
            if (coprs==0x00) { if(d.rt) o << G(d.rt) << " = s.cop0["<<int(d.rd)<<"];\n"; return; }
            if (coprs==0x04) { o << "s.cop0["<<int(d.rd)<<"] = "<<G(d.rt)<<";\n"; return; }
            if ((d.raw & 0x01FFFFFFu)==0x10u) { o << "r3k_rfe(s);\n"; return; }
            o << "r3k_cop0_unknown("<<hex8(d.raw)<<");\n"; return;
        }
        case 0x12: {
            if (d.rs==0x00) { if(d.rt) o << G(d.rt) << " = gte.read_data("<<int(d.rd)<<");\n"; return; }
            if (d.rs==0x02) { if(d.rt) o << G(d.rt) << " = gte.read_ctrl("<<int(d.rd)<<");\n"; return; }
            if (d.rs==0x04) { o << "gte.write_data("<<int(d.rd)<<", "<<G(d.rt)<<");\n"; return; }
            if (d.rs==0x06) { o << "gte.write_ctrl("<<int(d.rd)<<", "<<G(d.rt)<<");\n"; return; }
            if (d.rs & 0x10) { o << "gte.execute("<<hex8(d.raw)<<");\n"; return; }
            o << "r3k_cop2_unknown("<<hex8(d.raw)<<");\n"; return;
        }
        case 0x32: if(d.rt) o << "gte.write_data("<<int(d.rt)<<", mem.read32("<<G(d.rs)<<" + uint32_t(int32_t(int16_t("<<d.imm<<")))));\n"; return;
        case 0x3A: o << "mem.write32("<<G(d.rs)<<" + uint32_t(int32_t(int16_t("<<d.imm<<"))), gte.read_data("<<int(d.rt)<<"));\n"; return;
        default: o << "r3k_unknown("<<hex8(d.raw)<<", "<<hex8(pc)<<");\n"; return;
    }
}

static bool is_control(const Decoded& d) {
    if (d.op==0 && (d.funct==0x08 || d.funct==0x09)) return true;
    if (d.op==0x01 || d.op==0x02 || d.op==0x03 || (d.op>=0x04 && d.op<=0x07)) return true;
    if (d.op==0x12 && d.rs==0x08) return true; // BC2F/BC2T (GTE condition is always false on PS1)
    return false;
}

std::string recompile_psx_exe_to_cpp(const PsxExeImage& e, const RecompileOptions& opt) {
    std::ostringstream o;
    o << "#include <cstdint>\n#include <limits>\n#include \"r3000a.h\"\n#include \"psxrecomp/trace.h\"\nusing namespace r3k;\n\n";
    o << "static void r3k_unknown(uint32_t,uint32_t){}\n";
    o << "static void r3k_syscall(CpuState& s){ uint32_t fn=s.gpr[4]; if(fn==1){ uint32_t old=s.cop0[12]; s.gpr[2]=((old&(1u<<10))&&(old&1u))?1u:0u; s.cop0[12]&=~((1u<<10)|1u); } else if(fn==2){ s.cop0[12]|=((1u<<10)|1u); s.gpr[2]=1; } }\n";
    o << "static void r3k_break(CpuState&){} static void r3k_cop0_unknown(uint32_t){} static void r3k_cop2_unknown(uint32_t){}\n";
    o << "static void r3k_rfe(CpuState& s){ uint32_t m=s.cop0[12]&0x3Fu; s.cop0[12]=(s.cop0[12]&~0x3Fu)|((m>>2)&0x0Fu); }\n";
    o << "static void r3k_div_signed(CpuState& s,uint32_t a,uint32_t b){ int32_t x=int32_t(a),y=int32_t(b); if(y==0){s.lo=x>=0?0xFFFFFFFFu:1u;s.hi=a;} else if(x==std::numeric_limits<int32_t>::min()&&y==-1){s.lo=uint32_t(x);s.hi=0;} else {s.lo=uint32_t(x/y);s.hi=uint32_t(x%y);} }\n";
    o << "static void r3k_div_unsigned(CpuState& s,uint32_t a,uint32_t b){ if(!b){s.lo=0xFFFFFFFFu;s.hi=a;} else {s.lo=a/b;s.hi=a%b;} }\n";
    o << "static uint32_t r3k_lwl(Memory& m,uint32_t a,uint32_t rt){ uint32_t w=m.read32(a&~3u); switch(a&3u){case 0:return (rt&0x00FFFFFFu)|(w<<24);case 1:return (rt&0x0000FFFFu)|(w<<16);case 2:return (rt&0x000000FFu)|(w<<8);default:return w;} }\n";
    o << "static uint32_t r3k_lwr(Memory& m,uint32_t a,uint32_t rt){ uint32_t w=m.read32(a&~3u); switch(a&3u){case 0:return w;case 1:return (rt&0xFF000000u)|(w>>8);case 2:return (rt&0xFFFF0000u)|(w>>16);default:return (rt&0xFFFFFF00u)|(w>>24);} }\n";
    o << "static void r3k_swl(Memory& m,uint32_t a,uint32_t rt){ uint32_t q=a&~3u,w=m.read32(q); switch(a&3u){case 0:w=(w&0xFFFFFF00u)|(rt>>24);break;case 1:w=(w&0xFFFF0000u)|(rt>>16);break;case 2:w=(w&0xFF000000u)|(rt>>8);break;default:w=rt;} m.write32(q,w);}\n";
    o << "static void r3k_swr(Memory& m,uint32_t a,uint32_t rt){ uint32_t q=a&~3u,w=m.read32(q); switch(a&3u){case 0:w=rt;break;case 1:w=(w&0x000000FFu)|(rt<<8);break;case 2:w=(w&0x0000FFFFu)|(rt<<16);break;default:w=(w&0x00FFFFFFu)|(rt<<24);} m.write32(q,w);}\n\n";

    // Generate one dispatcher with labeled basic blocks for all aligned words in payload.
    o << "void run_recompiled(CpuState& s, Memory& mem, Gte& gte) {\n";
    o << "  for(;;){ s.gpr[0]=0; switch(s.pc){\n";

    for (uint32_t pc=e.load_address; in_image(e,pc); pc+=4) {
        Decoded d=decode(ins_at(e,pc));
        o << "case "<<hex8(pc)<<"u: {\n";
        o << "++s.cycles;\n";
        if ((pc >= 0x8005C200u && pc <= 0x8005C400u) ||
            (pc >= 0x80058840u && pc <= 0x800588A0u)) {
            o << "psxrecomp::tracePrintf(\"" << ((pc >= 0x80058840u && pc <= 0x800588A0u) ? "[CD WAIT PC]" : "[CD PC]") << " pc=%08X raw=%08X cyc=%llu "
                 "v0=%08X v1=%08X a0=%08X a1=%08X a2=%08X a3=%08X "
                 "t0=%08X t1=%08X t2=%08X t3=%08X s0=%08X s1=%08X "
                 "sp=%08X ra=%08X\\n\","
                 "s.pc," << hex8(d.raw) << "u,"
                 "(unsigned long long)s.cycles,"
                 "s.gpr[2],s.gpr[3],s.gpr[4],s.gpr[5],s.gpr[6],s.gpr[7],"
                 "s.gpr[8],s.gpr[9],s.gpr[10],s.gpr[11],"
                 "s.gpr[16],s.gpr[17],s.gpr[29],s.gpr[31]);\n";
            if (pc >= 0x80058840u && pc <= 0x800588A0u) {
                o << "psxrecomp::tracePrintf(\"[CD WAIT MEM] pc=%08X "
                     "m734D4=%02X mA00A0=%08X mA00A4=%08X mA00A8=%08X\\n\","
                     "s.pc,"
                     "mem.read8(0x800734D4u),"
                     "mem.read32(0x800A00A0u),"
                     "mem.read32(0x800A00A4u),"
                     "mem.read32(0x800A00A8u));\n";
            }
        }
        if (opt.emit_comments) o << "// "<<hex8(pc)<<": "<<disasm(pc,d)<<"\n";
        if (is_control(d)) {
            // execute delay slot first, but preserve branch decision source values when needed
            if (d.op==0x04 || d.op==0x05) o << "uint32_t _brs=s.gpr["<<int(d.rs)<<"], _brt=s.gpr["<<int(d.rt)<<"];\n";
            else if (d.op==0x06 || d.op==0x07 || d.op==0x01) o << "uint32_t _brs=s.gpr["<<int(d.rs)<<"];\n";
            else if (d.op==0 && (d.funct==0x08 || d.funct==0x09)) o << "uint32_t _jtarget=s.gpr["<<int(d.rs)<<"];\n";
            if (in_image(e,pc+4)) {
                auto ds=decode(ins_at(e,pc+4));
                if (opt.emit_comments) o << "// delay: "<<disasm(pc+4,ds)<<"\n";
                o << "++s.cycles;\n";
                emit_exec(o,pc+4,ds);
            }
            // custom tail using preserved vars
            if ((pc >= 0x8005C200u && pc <= 0x8005C400u) ||
            (pc >= 0x80058840u && pc <= 0x800588A0u)) {
                if (d.op==0x04 || d.op==0x05) {
                    o << "psxrecomp::tracePrintf(\"[CD BR] pc=%08X rs=%08X rt=%08X\\n\","
                         << hex8(pc) << "u,_brs,_brt);\n";
                } else if (d.op==0x06 || d.op==0x07 || d.op==0x01) {
                    o << "psxrecomp::tracePrintf(\"[CD BR] pc=%08X rs=%08X\\n\","
                         << hex8(pc) << "u,_brs);\n";
                } else if (d.op==0 && (d.funct==0x08 || d.funct==0x09)) {
                    o << "psxrecomp::tracePrintf(\"[CD JMP] pc=%08X target=%08X\\n\","
                         << hex8(pc) << "u,_jtarget);\n";
                } else if (d.op==0x02 || d.op==0x03) {
                    o << "psxrecomp::tracePrintf(\"[CD JMP] pc=%08X target=%08X\\n\","
                         << hex8(pc) << "u," << hex8((((pc+4)&0xF0000000u)|(d.target<<2))) << "u);\n";
                }
            }
            const int32_t simm=static_cast<int16_t>(d.imm);
            const uint32_t bt=pc+4+(uint32_t(simm)<<2);
            const uint32_t jt=((pc+4)&0xF0000000u)|(d.target<<2);
            if (d.op==0 && d.funct==0x08) o << "s.pc=_jtarget; return;\n";
            else if (d.op==0 && d.funct==0x09) { if(d.rd)o<<"s.gpr["<<int(d.rd)<<"]="<<hex8(pc+8)<<"u;\n"; o<<"s.pc=_jtarget; return;\n"; }
            else if (d.op==0x02) o << "s.pc="<<hex8(jt)<<"u; return;\n";
            else if (d.op==0x03) o << "s.gpr[31]="<<hex8(pc+8)<<"u; s.pc="<<hex8(jt)<<"u; return;\n";
            else if (d.op==0x04) o << "s.pc=(_brs==_brt)?"<<hex8(bt)<<"u:"<<hex8(pc+8)<<"u; return;\n";
            else if (d.op==0x05) o << "s.pc=(_brs!=_brt)?"<<hex8(bt)<<"u:"<<hex8(pc+8)<<"u; return;\n";
            else if (d.op==0x06) o << "s.pc=(int32_t(_brs)<=0)?"<<hex8(bt)<<"u:"<<hex8(pc+8)<<"u; return;\n";
            else if (d.op==0x07) o << "s.pc=(int32_t(_brs)>0)?"<<hex8(bt)<<"u:"<<hex8(pc+8)<<"u; return;\n";
            else if (d.op==0x01) {
                if (d.rt==0x10 || d.rt==0x11) o << "s.gpr[31]="<<hex8(pc+8)<<"u;\n";
                if (d.rt==0x00 || d.rt==0x10) o << "s.pc=(int32_t(_brs)<0)?"<<hex8(bt)<<"u:"<<hex8(pc+8)<<"u; return;\n";
                else if (d.rt==0x01 || d.rt==0x11) o << "s.pc=(int32_t(_brs)>=0)?"<<hex8(bt)<<"u:"<<hex8(pc+8)<<"u; return;\n";
                else o << "r3k_unknown("<<hex8(d.raw)<<", "<<hex8(pc)<<"); s.pc="<<hex8(pc+8)<<"u; return;\n";
            }
            else if (d.op==0x12 && d.rs==0x08) {
                // PS1 GTE condition flag is always false: BC2F takes, BC2T falls through.
                if ((d.rt & 1u)==0) o << "s.pc="<<hex8(bt)<<"u; return;\n";
                else o << "s.pc="<<hex8(pc+8)<<"u; return;\n";
            }
            else o << "s.pc="<<hex8(pc+8)<<"u; return;\n";
        } else {
            emit_exec(o,pc,d);
            o << "s.pc="<<hex8(pc+4)<<"u; break;\n";
        }
        o << "}\n";
    }
    o << "default: return; } } }\n";
    return o.str();
}

#include <fstream>
#include "psxrecomp/compat.h"
#include <vector>
#include <cstdio>

bool recompile_psx_exe_to_split_files(const PsxExeImage& exe, const std::string& outputDir,
                                      std::size_t wordsPerPart, const RecompileOptions& opt)
{
    if (wordsPerPart == 0) wordsPerPart = 4096;
    if (!psxrecomp::compat::create_directories(outputDir)) return false;

    const std::string mono = recompile_psx_exe_to_cpp(exe, opt);
    const std::string fnMarker = "void run_recompiled(CpuState& s, Memory& mem, Gte& gte) {\n  for(;;){ s.gpr[0]=0; switch(s.pc){\n";
    const std::size_t fn = mono.find(fnMarker);
    if (fn == std::string::npos) return false;

    std::string preamble = mono.substr(0, fn);
    // Convert the common prefix into an includable header.
    {
        std::ofstream h(psxrecomp::compat::join_path(outputDir, "game_helpers.h").c_str(), std::ios::binary);
        h << "#pragma once\n" << preamble;
    }

    const std::size_t casesBegin = fn + fnMarker.size();
    const std::size_t defaultPos = mono.find("default: return; } } }", casesBegin);
    if (defaultPos == std::string::npos) return false;
    const std::string caseRegion = mono.substr(casesBegin, defaultPos - casesBegin);

    std::vector<std::string> cases;
    std::vector<uint32_t> pcs;
    std::size_t pos = 0;
    while (true) {
        const std::size_t c = caseRegion.find("case 0x", pos);
        if (c == std::string::npos) break;
        const std::size_t next = caseRegion.find("case 0x", c + 1);
        std::string block = caseRegion.substr(c, next == std::string::npos ? std::string::npos : next - c);
        const std::size_t hexStart = c + 5;
        const std::string hex = caseRegion.substr(hexStart, 10);
        uint32_t pc = static_cast<uint32_t>(std::stoul(hex, nullptr, 16));

        // The monolithic body returns from run_recompiled on a control-flow
        // boundary and breaks to continue on sequential instructions.
        std::size_t q = 0;
        while ((q = block.find(" return;", q)) != std::string::npos) {
            block.replace(q, 8, " return 2;"); q += 10;
        }
        q = 0;
        while ((q = block.find(" break;", q)) != std::string::npos) {
            block.replace(q, 7, " return 1;"); q += 10;
        }
        cases.push_back(std::move(block));
        pcs.push_back(pc);
        if (next == std::string::npos) break;
        pos = next;
    }
    if (cases.empty()) return false;

    const std::size_t parts = (cases.size() + wordsPerPart - 1) / wordsPerPart;
    std::ofstream decl(psxrecomp::compat::join_path(outputDir, "game_parts.h").c_str(), std::ios::binary);
    decl << "#pragma once\n#include \"r3000a.h\"\n";

    struct Range { uint32_t first, last; };
    std::vector<Range> ranges;
    for (std::size_t pi=0; pi<parts; ++pi) {
        const std::size_t begin=pi*wordsPerPart;
        const std::size_t end=std::min(cases.size(), begin+wordsPerPart);
        const uint32_t first=pcs[begin], last=pcs[end-1];
        ranges.push_back({first,last});
        char name[64];
#ifdef _MSC_VER
        sprintf_s(name,sizeof(name),"run_recompiled_part_%03u",static_cast<unsigned>(pi));
#else
        std::snprintf(name,sizeof(name),"run_recompiled_part_%03zu",pi);
#endif
        decl << "int " << name << "(r3k::CpuState&,r3k::Memory&,r3k::Gte&);\n";

        char file[64];
#ifdef _MSC_VER
        sprintf_s(file,sizeof(file),"game_part_%03u.cpp",static_cast<unsigned>(pi));
#else
        std::snprintf(file,sizeof(file),"game_part_%03zu.cpp",pi);
#endif
        std::ofstream out(psxrecomp::compat::join_path(outputDir, file).c_str(), std::ios::binary);
        out << "#include \"game_helpers.h\"\n#include \"game_parts.h\"\n";
        out << "int " << name << "(CpuState& s, Memory& mem, Gte& gte){ s.gpr[0]=0; switch(s.pc){\n";
        for(std::size_t i=begin;i<end;++i) out << cases[i];
        out << "default:return 0;} }\n";
    }
    decl.close();

    std::ofstream disp(psxrecomp::compat::join_path(outputDir, "game_dispatch.cpp").c_str(), std::ios::binary);
    disp << "#include \"psxrecomp/generated_game.h\"\n#include \"game_parts.h\"\n";
    disp << "void run_recompiled(r3k::CpuState& s,r3k::Memory& mem,r3k::Gte& gte){ for(;;){ int r=0;\n";
    for(std::size_t pi=0; pi<ranges.size(); ++pi){
        char name[64];
#ifdef _MSC_VER
        sprintf_s(name,sizeof(name),"run_recompiled_part_%03u",static_cast<unsigned>(pi));
#else
        std::snprintf(name,sizeof(name),"run_recompiled_part_%03zu",pi);
#endif
        disp << (pi?"else ":"") << "if(s.pc>=0x" << std::hex << std::uppercase << ranges[pi].first
             << "u && s.pc<=0x" << ranges[pi].last << "u) r=" << name << "(s,mem,gte);\n";
    }
    disp << "else return; if(r!=1)return; } }\n";
    return true;
}
