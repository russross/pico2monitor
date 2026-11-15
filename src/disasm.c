#include "disasm.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

const char *const abi_registers[32] = {"zero", "ra", "sp",  "gp",  "tp", "t0", "t1", "t2",
                                       "s0",   "s1", "a0",  "a1",  "a2", "a3", "a4", "a5",
                                       "a6",   "a7", "s2",  "s3",  "s4", "s5", "s6", "s7",
                                       "s8",   "s9", "s10", "s11", "t3", "t4", "t5", "t6"};

typedef enum {
    NONE,
    R,
    RR,
    RR1,
    RRR,
    RI,
    RRI,
    LOAD,
    STORE,
    BRANCH,
    JUMP,
    UPPER,
    CSR,
    CSRI,
    FENCE,
    BEXTM,
    BEXTMI,
    REGLIST
} Form;
typedef struct {
    const char *name;
    Form form;
    unsigned rd, rs1, rs2;
    int32_t immediate;
    bool relative;
} Instruction;

/* Unsigned arithmetic keeps sign extension defined for every encoded value. */
static int32_t signed_bits(uint32_t value, unsigned bits) {
    uint32_t sign = UINT32_C(1) << (bits - 1);
    return (int32_t)(value ^ sign) - (int32_t)sign;
}
static void immediate_text(char *text, size_t size, int32_t value) {
    uint32_t magnitude = value < 0 ? 0u - (uint32_t)value : (uint32_t)value;
    if (magnitude <= 9)
        snprintf(text, size, "%s%lu", value < 0 ? "-" : "", (unsigned long)magnitude);
    else
        snprintf(text, size, "%s0x%lx", value < 0 ? "-" : "", (unsigned long)magnitude);
}

/* RP2350 enables Zba, Zbb, Zbs and Zbkb. Fixed unary encodings are checked
 * separately from the register and shift formats so reserved bits stay checked. */
static void bitmanip(uint32_t w, Instruction *d) {
    d->form = RR1;
    switch (w & 0xfff0707f) {
    case 0x60001013:
        d->name = "clz";
        return;
    case 0x60101013:
        d->name = "ctz";
        return;
    case 0x60201013:
        d->name = "cpop";
        return;
    case 0x60401013:
        d->name = "sext.b";
        return;
    case 0x60501013:
        d->name = "sext.h";
        return;
    case 0x28705013:
        d->name = "orc.b";
        return;
    case 0x69805013:
        d->name = "rev8";
        return;
    case 0x68705013:
        d->name = "brev8";
        return;
    case 0x08f01013:
        d->name = "zip";
        return;
    case 0x08f05013:
        d->name = "unzip";
        return;
    case 0x08004033:
        d->name = "zext.h";
        return;
    }
    d->form = RRR;
    switch (w & 0xfe00707f) {
    case 0x20002033:
        d->name = "sh1add";
        break;
    case 0x20004033:
        d->name = "sh2add";
        break;
    case 0x20006033:
        d->name = "sh3add";
        break;
    case 0x40007033:
        d->name = "andn";
        break;
    case 0x40006033:
        d->name = "orn";
        break;
    case 0x40004033:
        d->name = "xnor";
        break;
    case 0x0a004033:
        d->name = "min";
        break;
    case 0x0a005033:
        d->name = "minu";
        break;
    case 0x0a006033:
        d->name = "max";
        break;
    case 0x0a007033:
        d->name = "maxu";
        break;
    case 0x60001033:
        d->name = "rol";
        break;
    case 0x60005033:
        d->name = "ror";
        break;
    case 0x48001033:
        d->name = "bclr";
        break;
    case 0x48005033:
        d->name = "bext";
        break;
    case 0x68001033:
        d->name = "binv";
        break;
    case 0x28001033:
        d->name = "bset";
        break;
    case 0x08004033:
        d->name = "pack";
        break;
    case 0x08007033:
        d->name = "packh";
        break;
    case 0x60005013:
        d->name = "rori";
        d->form = RRI;
        break;
    case 0x48001013:
        d->name = "bclri";
        d->form = RRI;
        break;
    case 0x48005013:
        d->name = "bexti";
        d->form = RRI;
        break;
    case 0x68001013:
        d->name = "binvi";
        d->form = RRI;
        break;
    case 0x28001013:
        d->name = "bseti";
        d->form = RRI;
        break;
    }
    if (d->form == RRI)
        d->immediate = (int32_t)d->rs2;
}

static Instruction decode32(uint32_t w) {
    unsigned op = w & 127, f3 = (w >> 12) & 7, f7 = w >> 25;
    Instruction d = {.rd = (w >> 7) & 31,
                     .rs1 = (w >> 15) & 31,
                     .rs2 = (w >> 20) & 31,
                     .immediate = signed_bits(w >> 20, 12)};
    switch (op) {
    case 0x0b:
        if (!(w & 0xe2000000) && (f3 == 0 || f3 == 4)) {
            d.name = f3 ? "h3.bextmi" : "h3.bextm";
            d.form = f3 ? BEXTMI : BEXTM;
            d.immediate = (int32_t)(((w >> 26) & 7) + 1);
        }
        break;
    case 0x37:
    case 0x17:
        d.name = op == 0x37 ? "lui" : "auipc";
        d.form = UPPER;
        d.immediate = (int32_t)(w >> 12);
        d.relative = op == 0x17;
        break;
    case 0x6f:
        d.name = "jal";
        d.form = JUMP;
        d.relative = true;
        d.immediate = signed_bits(((w >> 31) << 20) | (((w >> 12) & 255) << 12) |
                                      (((w >> 20) & 1) << 11) | (((w >> 21) & 1023) << 1),
                                  21);
        break;
    case 0x67:
        if (!f3) {
            d.name = "jalr";
            d.form = LOAD;
        }
        break;
    case 0x63: {
        static const char *const names[8] = {"beq", "bne", NULL,   NULL,
                                             "blt", "bge", "bltu", "bgeu"};
        d.name = names[f3];
        d.form = BRANCH;
        d.relative = true;
        d.immediate = signed_bits(((w >> 31) << 12) | (((w >> 7) & 1) << 11) |
                                      (((w >> 25) & 63) << 5) | (((w >> 8) & 15) << 1),
                                  13);
        break;
    }
    case 3: {
        static const char *const names[8] = {"lb", "lh", "lw", NULL, "lbu", "lhu", NULL, NULL};
        d.name = names[f3];
        d.form = LOAD;
        break;
    }
    case 0x23: {
        static const char *const names[8] = {"sb", "sh", "sw", NULL, NULL, NULL, NULL, NULL};
        d.name = names[f3];
        d.form = STORE;
        d.immediate = signed_bits(((w >> 25) << 5) | ((w >> 7) & 31), 12);
        break;
    }
    case 0x13: {
        static const char *const names[8] = {"addi", NULL, "slti", "sltiu",
                                             "xori", NULL, "ori",  "andi"};
        d.name = names[f3];
        d.form = RRI;
        if (f3 == 1 || f3 == 5) {
            d.immediate = (int32_t)d.rs2;
            if (!f7)
                d.name = f3 == 1 ? "slli" : "srli";
            else if (f3 == 5 && f7 == 0x20)
                d.name = "srai";
        }
        break;
    }
    case 0x33: {
        static const char *const base[8] = {"add", "sll", "slt", "sltu", "xor", "srl", "or", "and"};
        static const char *const mul[8] = {"mul", "mulh", "mulhsu", "mulhu",
                                           "div", "divu", "rem",    "remu"};
        d.form = RRR;
        if (!f7)
            d.name = base[f3];
        else if (f7 == 1)
            d.name = mul[f3];
        else if (f7 == 0x20 && (f3 == 0 || f3 == 5))
            d.name = f3 ? "sra" : "sub";
        break;
    }
    /* Atomic ordering bits are retained as mnemonic suffixes by the formatter. */
    case 0x2f:
        if (f3 == 2) {
            static const char *const names[32] = {
                [0] = "amoadd.w",  [1] = "amoswap.w",  [2] = "lr.w",      [3] = "sc.w",
                [4] = "amoxor.w",  [8] = "amoor.w",    [12] = "amoand.w", [16] = "amomin.w",
                [20] = "amomax.w", [24] = "amominu.w", [28] = "amomaxu.w"};
            d.name = names[w >> 27];
            if ((w >> 27) == 2 && d.rs2)
                d.name = NULL;
            d.form = RRR;
        }
        break;
    case 0x73:
        if (f3 && f3 != 4) {
            static const char *const names[8] = {NULL, "csrrw",  "csrrs",  "csrrc",
                                                 NULL, "csrrwi", "csrrsi", "csrrci"};
            d.name = names[f3];
            d.form = f3 < 4 ? CSR : CSRI;
            d.immediate = (int32_t)(w >> 20);
        } else if (w == 0x73)
            d.name = "ecall";
        else if (w == 0x100073)
            d.name = "ebreak";
        else if (w == 0x30200073)
            d.name = "mret";
        else if (w == 0x10500073)
            d.name = "wfi";
        break;
    case 0x0f:
        if (!f3 && !d.rd && !d.rs1) {
            d.name = "fence";
            d.form = FENCE;
        } else if (w == 0x100f)
            d.name = "fence.i";
        break;
    }
    if (!d.name)
        bitmanip(w, &d);
    if (w == 0x00002033 || w == 0x00102033) {
        d.name = w == 0x00002033 ? "h3.block" : "h3.unblock";
        d.form = NONE;
    }
    return d;
}

/* Compressed formats reconstruct scattered immediates before sign extension.
 * Reserved encodings stay unknown; architectural hints retain their operands. */
static Instruction decode16(uint32_t w) {
    unsigned q = w & 3, f = w >> 13;
    Instruction d = {.rd = (w >> 7) & 31, .rs1 = (w >> 7) & 31, .rs2 = (w >> 2) & 31};
    int32_t imm = signed_bits(((w >> 2) & 31) | (((w >> 12) & 1) << 5), 6);
    if (q == 0) {
        d.rd = 8 + ((w >> 2) & 7);
        d.rs2 = d.rd;
        d.rs1 = 8 + ((w >> 7) & 7);
        if (f == 0) {
            d.immediate = (int32_t)(((w >> 7) & 15) << 6 | ((w >> 11) & 3) << 4 |
                                    ((w >> 5) & 1) << 3 | ((w >> 6) & 1) << 2);
            if (d.immediate) {
                d.name = "c.addi4spn";
                d.form = RRI;
                d.rs1 = 2;
            }
        } else if (f == 2 || f == 6) {
            d.name = f == 2 ? "c.lw" : "c.sw";
            d.form = f == 2 ? LOAD : STORE;
            d.immediate =
                (int32_t)(((w >> 5) & 1) << 6 | ((w >> 10) & 7) << 3 | ((w >> 6) & 1) << 2);
        }
    } else if (q == 1) {
        d.immediate = imm;
        d.form = RI;
        switch (f) {
        case 0:
            d.name = "c.addi";
            break;
        case 2:
            d.name = "c.li";
            break;
        case 3:
            if (d.rd == 2) {
                d.immediate =
                    signed_bits(((w >> 12) & 1) << 9 | ((w >> 3) & 3) << 7 | ((w >> 5) & 1) << 6 |
                                    ((w >> 2) & 1) << 5 | ((w >> 6) & 1) << 4,
                                10);
                if (d.immediate)
                    d.name = "c.addi16sp";
            } else if (imm) {
                d.name = "c.lui";
                d.immediate = (int32_t)((uint32_t)imm & 0xfffff);
            }
            break;
        case 1:
        case 5:
            d.name = f == 1 ? "c.jal" : "c.j";
            d.form = NONE;
            d.relative = true;
            d.immediate =
                signed_bits(((w >> 12) & 1) << 11 | ((w >> 11) & 1) << 4 | ((w >> 9) & 3) << 8 |
                                ((w >> 8) & 1) << 10 | ((w >> 7) & 1) << 6 | ((w >> 6) & 1) << 7 |
                                ((w >> 3) & 7) << 1 | ((w >> 2) & 1) << 5,
                            12);
            break;
        case 6:
        case 7:
            d.name = f == 6 ? "c.beqz" : "c.bnez";
            d.form = RI;
            d.relative = true;
            d.rd = 8 + ((w >> 7) & 7);
            d.immediate =
                signed_bits(((w >> 12) & 1) << 8 | ((w >> 10) & 3) << 3 | ((w >> 5) & 3) << 6 |
                                ((w >> 3) & 3) << 1 | ((w >> 2) & 1) << 5,
                            9);
            break;
        case 4: {
            unsigned sub = (w >> 10) & 3;
            d.rd = 8 + ((w >> 7) & 7);
            if (sub == 2)
                d.name = "c.andi";
            else if (sub < 2 && !(w & 0x1000)) {
                d.name = sub ? "c.srai" : "c.srli";
                d.immediate = (int32_t)((w >> 2) & 31);
            } else if (sub == 3 && !(w & 0x1000)) {
                static const char *const names[4] = {"c.sub", "c.xor", "c.or", "c.and"};
                d.name = names[(w >> 5) & 3];
                d.form = RR;
                d.rs2 = 8 + ((w >> 2) & 7);
            }
            break;
        }
        }
    } else if (q == 2) {
        switch (f) {
        case 0:
            if (!(w & 0x1000)) {
                d.name = "c.slli";
                d.form = RI;
                d.immediate = (int32_t)((w >> 2) & 31);
            }
            break;
        case 2:
            if (d.rd) {
                d.name = "c.lwsp";
                d.form = LOAD;
                d.rs1 = 2;
                d.immediate =
                    (int32_t)(((w >> 2) & 3) << 6 | ((w >> 12) & 1) << 5 | ((w >> 4) & 7) << 2);
            }
            break;
        case 6:
            d.name = "c.swsp";
            d.form = STORE;
            d.rs1 = 2;
            d.immediate = (int32_t)(((w >> 7) & 3) << 6 | ((w >> 9) & 15) << 2);
            break;
        case 4:
            if (d.rs2) {
                d.name = w & 0x1000 ? "c.add" : "c.mv";
                d.form = RR;
            } else if (d.rd) {
                d.name = w & 0x1000 ? "c.jalr" : "c.jr";
                d.form = R;
            } else if (w & 0x1000)
                d.name = "c.ebreak";
            break;
        }
    }
    /* Zcb occupies otherwise unused compressed load/store and ALU slots. */
    if (!d.name) {
        d.rd = 8 + ((w >> 7) & 7);
        d.rs1 = d.rd;
        d.rs2 = 8 + ((w >> 2) & 7);
        d.form = R;
        switch (w & 0xfc7f) {
        case 0x9c61:
            d.name = "c.zext.b";
            break;
        case 0x9c65:
            d.name = "c.sext.b";
            break;
        case 0x9c69:
            d.name = "c.zext.h";
            break;
        case 0x9c6d:
            d.name = "c.sext.h";
            break;
        case 0x9c75:
            d.name = "c.not";
            break;
        }
        if ((w & 0xfc63) == 0x9c41) {
            d.name = "c.mul";
            d.form = RR;
        }
        if (q == 0 && (w & 0xf000) == 0x8000) {
            unsigned sub = (w >> 10) & 3;
            static const char *const names[4] = {"c.lbu", "c.lhu", "c.sb", "c.sh"};
            d.name = names[sub];
            d.form = sub < 2 ? LOAD : STORE;
            d.rd = d.rs2;
            d.immediate = (int32_t)(((w >> 5) & 1) << 1);
            if (!(sub & 1))
                d.immediate |= (int32_t)((w >> 6) & 1);
            else if (sub == 1 && (w & 0x40))
                d.name = "c.lh";
            else if (sub == 3 && (w & 0x40))
                d.name = NULL;
        }
    }
    /* Zcmp encodes a register list and stack adjustment, or two saved registers. */
    if (q == 2 && (w & 0xf900) == 0xb800 && ((w >> 4) & 15) >= 4) {
        static const char *const names[4] = {"cm.push", "cm.pop", "cm.popretz", "cm.popret"};
        unsigned list = (w >> 4) & 15;
        d.name = names[(w >> 9) & 3];
        d.form = REGLIST;
        d.rd = list;
        d.immediate = (int32_t)((list == 15   ? 64
                                 : list >= 12 ? 48
                                 : list >= 8  ? 32
                                              : 16) +
                                ((w >> 2) & 3) * 16);
        if (((w >> 9) & 3) == 0)
            d.immediate = -d.immediate;
    } else if (q == 2 && (w & 0xfc23) == 0xac22) {
        unsigned a = (w >> 7) & 7, b = (w >> 2) & 7;
        if ((w & 0x40) || a != b) {
            d.name = w & 0x40 ? "cm.mva01s" : "cm.mvsa01";
            d.form = RR;
            d.rd = a < 2 ? 8 + a : 16 + a;
            d.rs2 = b < 2 ? 8 + b : 16 + b;
        }
    }
    return d;
}

void disassemble(char *line, size_t capacity, uint32_t pc, uint32_t word, unsigned size) {
    Instruction d = size == 2 ? decode16(word & 0xffff) : decode32(word);
    char imm[16], operands[80] = "", mnemonic[24];
    immediate_text(imm, sizeof(imm), d.immediate);
    snprintf(mnemonic, sizeof(mnemonic), "%s", d.name ? d.name : "???");
    const char *rd = abi_registers[d.rd], *r1 = abi_registers[d.rs1], *r2 = abi_registers[d.rs2];
    if (d.name) {
        switch (d.form) {
        case R:
            snprintf(operands, sizeof(operands), "%s", rd);
            break;
        case RR:
            snprintf(operands, sizeof(operands), "%s, %s", rd, r2);
            break;
        case RR1:
            snprintf(operands, sizeof(operands), "%s, %s", rd, r1);
            break;
        case BEXTM:
        case BEXTMI: {
            char shift[16];
            immediate_text(shift, sizeof(shift), (int32_t)d.rs2);
            snprintf(operands, sizeof(operands), "%s, %s, %s, %s", rd, r1,
                     d.form == BEXTM ? r2 : shift, imm);
            break;
        }
        case REGLIST:
            if (d.rd == 4)
                snprintf(operands, sizeof(operands), "{ra}, %s", imm);
            else if (d.rd == 5)
                snprintf(operands, sizeof(operands), "{ra, s0}, %s", imm);
            else
                snprintf(operands, sizeof(operands), "{ra, s0-s%u}, %s", d.rd == 15 ? 11 : d.rd - 5,
                         imm);
            break;
        case RRR:
            snprintf(operands, sizeof(operands), "%s, %s, %s", rd, r1, r2);
            break;
        case RI:
        case JUMP:
        case UPPER:
            snprintf(operands, sizeof(operands), "%s, %s", rd, imm);
            break;
        case RRI:
            snprintf(operands, sizeof(operands), "%s, %s, %s", rd, r1, imm);
            break;
        case LOAD:
        case STORE:
            snprintf(operands, sizeof(operands), "%s, %s(%s)", d.form == LOAD ? rd : r2, imm, r1);
            break;
        case BRANCH:
            snprintf(operands, sizeof(operands), "%s, %s, %s", r1, r2, imm);
            break;
        case CSR:
        case CSRI: {
            char source[16];
            immediate_text(source, sizeof(source), (int32_t)d.rs1);
            snprintf(operands, sizeof(operands), "%s, %s, %s", rd, imm,
                     d.form == CSR ? r1 : source);
            break;
        }
        case FENCE: {
            char pred[5] = "", succ[5] = "";
            char mode[16];
            immediate_text(mode, sizeof(mode), (int32_t)(word >> 28));
            unsigned p = 0, s = 0;
            for (unsigned bit = 0; bit < 4; ++bit) {
                if (word & (1u << (27 - bit)))
                    pred[p++] = "iorw"[bit];
                if (word & (1u << (23 - bit)))
                    succ[s++] = "iorw"[bit];
            }
            snprintf(operands, sizeof(operands), "%s, %s, %s", p ? pred : "0", s ? succ : "0",
                     mode);
            break;
        }
        case NONE:
            if (d.relative)
                snprintf(operands, sizeof(operands), "%s", imm);
            break;
        }
        if (size == 4 && (word & 127) == 0x2f) {
            unsigned order = (word >> 25) & 3;
            snprintf(mnemonic, sizeof(mnemonic), "%s%s", d.name,
                     order == 3   ? ".aqrl"
                     : order == 2 ? ".aq"
                     : order == 1 ? ".rl"
                                  : "");
            if ((word >> 27) == 2)
                snprintf(operands, sizeof(operands), "%s, (%s)", rd, r1);
            else
                snprintf(operands, sizeof(operands), "%s, %s, (%s)", rd, r2, r1);
        }
    }
    /* Mnemonics start at column 32 and operands at 48 (zero-based). Targets
     * share column 80, leaving room for every supported operand format. */
    snprintf(line, capacity, "0x%08lx: %0*lx%*s%-16s%s", (unsigned long)pc, (int)(size * 2),
             (unsigned long)(size == 2 ? word & 0xffff : word), size == 2 ? 16 : 12, "", mnemonic,
             operands);
    if (d.name && d.relative) {
        size_t used = strlen(line);
        uint32_t offset = d.form == UPPER ? (uint32_t)d.immediate << 12 : (uint32_t)d.immediate;
        if (used < capacity)
            snprintf(line + used, capacity - used, "%*s[0x%08lx]", used < 80 ? (int)(80 - used) : 4,
                     "", (unsigned long)(pc + offset));
    }
}
