#include "decoder.h"
#include <sstream>
#include <iomanip>

Decoded decode(uint32_t raw) {
    Decoded d;
    d.raw = raw;
    d.op = (raw >> 26) & 0x3F;
    d.rs = (raw >> 21) & 0x1F;
    d.rt = (raw >> 16) & 0x1F;
    d.rd = (raw >> 11) & 0x1F;
    d.sa = (raw >> 6) & 0x1F;
    d.funct = raw & 0x3F;
    d.imm = raw & 0xFFFF;
    d.target = raw & 0x03FFFFFF;
    return d;
}

static std::string r(int n) { return "r" + std::to_string(n); }
static std::string hx(uint32_t v) { std::ostringstream o; o << "0x" << std::hex << std::uppercase << v; return o.str(); }

std::string disasm(uint32_t pc, const Decoded& d) {
    std::ostringstream o;
    if (d.raw == 0) return "nop";
    if (d.op == 0) {
        switch (d.funct) {
            case 0x00: o << "sll " << r(d.rd) << "," << r(d.rt) << "," << int(d.sa); break;
            case 0x02: o << "srl " << r(d.rd) << "," << r(d.rt) << "," << int(d.sa); break;
            case 0x03: o << "sra " << r(d.rd) << "," << r(d.rt) << "," << int(d.sa); break;
            case 0x08: o << "jr " << r(d.rs); break;
            case 0x09: o << "jalr " << r(d.rd) << "," << r(d.rs); break;
            case 0x10: o << "mfhi " << r(d.rd); break;
            case 0x11: o << "mthi " << r(d.rs); break;
            case 0x12: o << "mflo " << r(d.rd); break;
            case 0x13: o << "mtlo " << r(d.rs); break;
            case 0x18: o << "mult " << r(d.rs) << "," << r(d.rt); break;
            case 0x19: o << "multu " << r(d.rs) << "," << r(d.rt); break;
            case 0x1A: o << "div " << r(d.rs) << "," << r(d.rt); break;
            case 0x1B: o << "divu " << r(d.rs) << "," << r(d.rt); break;
            case 0x20: o << "add " << r(d.rd) << "," << r(d.rs) << "," << r(d.rt); break;
            case 0x21: o << "addu " << r(d.rd) << "," << r(d.rs) << "," << r(d.rt); break;
            case 0x22: o << "sub " << r(d.rd) << "," << r(d.rs) << "," << r(d.rt); break;
            case 0x23: o << "subu " << r(d.rd) << "," << r(d.rs) << "," << r(d.rt); break;
            case 0x24: o << "and " << r(d.rd) << "," << r(d.rs) << "," << r(d.rt); break;
            case 0x25: o << "or " << r(d.rd) << "," << r(d.rs) << "," << r(d.rt); break;
            case 0x26: o << "xor " << r(d.rd) << "," << r(d.rs) << "," << r(d.rt); break;
            case 0x27: o << "nor " << r(d.rd) << "," << r(d.rs) << "," << r(d.rt); break;
            case 0x2A: o << "slt " << r(d.rd) << "," << r(d.rs) << "," << r(d.rt); break;
            case 0x2B: o << "sltu " << r(d.rd) << "," << r(d.rs) << "," << r(d.rt); break;
            case 0x0C: o << "syscall"; break;
            case 0x0D: o << "break"; break;
            default: o << "special(" << hx(d.raw) << ")"; break;
        }
        return o.str();
    }
    switch (d.op) {
        case 0x01: o << "regimm " << hx(d.raw); break;
        case 0x02: o << "j " << hx(((pc + 4) & 0xF0000000u) | (d.target << 2)); break;
        case 0x03: o << "jal " << hx(((pc + 4) & 0xF0000000u) | (d.target << 2)); break;
        case 0x04: o << "beq " << r(d.rs) << "," << r(d.rt); break;
        case 0x05: o << "bne " << r(d.rs) << "," << r(d.rt); break;
        case 0x06: o << "blez " << r(d.rs); break;
        case 0x07: o << "bgtz " << r(d.rs); break;
        case 0x08: o << "addi " << r(d.rt) << "," << r(d.rs); break;
        case 0x09: o << "addiu " << r(d.rt) << "," << r(d.rs); break;
        case 0x0A: o << "slti " << r(d.rt) << "," << r(d.rs); break;
        case 0x0B: o << "sltiu " << r(d.rt) << "," << r(d.rs); break;
        case 0x0C: o << "andi " << r(d.rt) << "," << r(d.rs); break;
        case 0x0D: o << "ori " << r(d.rt) << "," << r(d.rs); break;
        case 0x0E: o << "xori " << r(d.rt) << "," << r(d.rs); break;
        case 0x0F: o << "lui " << r(d.rt); break;
        case 0x10: o << "cop0 " << hx(d.raw); break;
        case 0x12: o << "cop2 " << hx(d.raw); break;
        case 0x20: o << "lb " << r(d.rt); break;
        case 0x21: o << "lh " << r(d.rt); break;
        case 0x22: o << "lwl " << r(d.rt); break;
        case 0x23: o << "lw " << r(d.rt); break;
        case 0x24: o << "lbu " << r(d.rt); break;
        case 0x25: o << "lhu " << r(d.rt); break;
        case 0x26: o << "lwr " << r(d.rt); break;
        case 0x28: o << "sb " << r(d.rt); break;
        case 0x29: o << "sh " << r(d.rt); break;
        case 0x2A: o << "swl " << r(d.rt); break;
        case 0x2B: o << "sw " << r(d.rt); break;
        case 0x2E: o << "swr " << r(d.rt); break;
        case 0x32: o << "lwc2 " << r(d.rt); break;
        case 0x3A: o << "swc2 " << r(d.rt); break;
        default: o << "op(" << hx(d.raw) << ")"; break;
    }
    return o.str();
}
