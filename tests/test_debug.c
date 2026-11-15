#include "debug.h"
#include "disasm.h"
#include "monitor.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

Program program;
static uint8_t ram[256];
static char output[8192];
static size_t length;

uint8_t *physical(uint32_t address) {
    assert(address >= GUEST_BASE && address < GUEST_BASE + sizeof(ram));
    return ram + address - GUEST_BASE;
}
void console_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    int count = vsnprintf(output + length, sizeof(output) - length, format, args);
    va_end(args);
    assert(count >= 0 && (size_t)count < sizeof(output) - length);
    length += (size_t)count;
}
static void command(const char *text) {
    char line[128];
    snprintf(line, sizeof(line), "%s", text);
    length = 0;
    output[0] = 0;
    debug_command(line);
    while (debug_busy())
        debug_step();
}
static void decoded(uint32_t word, unsigned size, const char *mnemonic, const char *operands,
                    const char *target) {
    char row[128];
    disassemble(row, sizeof(row), GUEST_BASE + 0x100, word, size);
    assert(!strncmp(row, "0x20000100: ", 12));
    assert(!strncmp(row + 32, mnemonic, strlen(mnemonic)));
    if (*operands)
        assert(!strncmp(row + 48, operands, strlen(operands)));
    if (target)
        assert(!strcmp(row + 80, target));
}

/* Check operand reconstruction, real mnemonics, signed values, and reserved
 * encodings independently of the formatting implementation. */
static void decoder_checks(void) {
    decoded(0x00450513, 4, "addi", "a0, a0, 4", NULL);
    decoded(0xff050513, 4, "addi", "a0, a0, -0x10", NULL);
    decoded(0x00000013, 4, "addi", "zero, zero, 0", NULL);
    decoded(0xfe051ee3, 4, "bne", "a0, zero, -4", "[0x200000fc]");
    decoded(0xffdff0ef, 4, "jal", "ra, -4", "[0x200000fc]");
    decoded(0x00001517, 4, "auipc", "a0, 1", "[0x20001100]");
    decoded(0xffc12503, 4, "lw", "a0, -4(sp)", NULL);
    decoded(0xfea12e23, 4, "sw", "a0, -4(sp)", NULL);
    decoded(0x02b50533, 4, "mul", "a0, a0, a1", NULL);
    decoded(0xc0002573, 4, "csrrs", "a0, 0xc00, zero", NULL);
    decoded(0x1400d573, 4, "csrrwi", "a0, 0x140, 1", NULL);
    decoded(0x140525af, 4, "lr.w.aq", "a1, (a0)", NULL);
    decoded(0x18c525af, 4, "sc.w", "a1, a2, (a0)", NULL);
    decoded(0x00100073, 4, "ebreak", "", NULL);
    decoded(0xffffffff, 4, "???", "", NULL);
    decoded(0x02051513, 4, "???", "", NULL);
    decoded(0xf330000f, 4, "fence", "rw, rw, 0xf", NULL);
    decoded(0x0001, 2, "c.addi", "zero, 0", NULL);
    decoded(0x1571, 2, "c.addi", "a0, -4", NULL);
    decoded(0x8082, 2, "c.jr", "ra", NULL);
    decoded(0x9002, 2, "c.ebreak", "", NULL);
    decoded(0xbffd, 2, "c.j", "-2", "[0x200000fe]");
    decoded(0x0000, 2, "???", "", NULL);
    decoded(0x4002, 2, "???", "", NULL);
    decoded(0x6005, 2, "c.lui", "zero, 1", NULL);
}

/* The tiny arena makes every boundary observable. Reads beyond it fail at
 * physical(), including the second half of a truncated four-byte instruction. */
static void cursor_checks(void) {
    assert(!debug_enter());
    program.valid = true;
    program.image.entry = GUEST_BASE;
    program.image.plan.snapshot_base = GUEST_BASE + sizeof(ram);
    program.image.plan.heap_base = GUEST_BASE + 0x80;
    program.image.plan.stack_base = GUEST_BASE + 0xc0;
    program.image.plan.stack_top = GUEST_BASE + sizeof(ram);
    program.current_break = GUEST_BASE + 0x90;
    program.threads[0].present = true;
    program.threads[0].frame.pc = GUEST_BASE;
    program.threads[0].frame.x[10] = GUEST_BASE + 0x37;
    length = 0;
    assert(debug_enter());
    assert(strstr(output, ".=20000000 reset\r\n"));
    command("hart1");
    assert(strstr(output, "has no saved state"));
    command(".a0");
    assert(strstr(output, ".=20000037"));
    command("list");
    assert(strstr(output, "0x20000037: not aligned"));
    command(".zero");
    assert(strstr(output, ".=20000000"));
    command(". 0");
    command("list");
    assert(strstr(output, "0x20000026: 0000"));
    command("");
    assert(strstr(output, "0x20000028: 0000"));
    command(".");
    assert(strstr(output, ".=20000050"));
    command(". 200000fe");
    ram[254] = 3;
    command("list");
    assert(!strcmp(output, "0x200000fe: address out of range\r\n"));
    command(". 20000100");
    assert(strstr(output, ". is out of range"));
    command("dump");
    assert(!strcmp(output, "0x20000100: address out of range\r\n"));
    for (unsigned i = 0; i < sizeof(ram); ++i)
        ram[i] = (uint8_t)i;
    command(". 37");
    command("dump");
    assert(
        strstr(output,
               "0x20000037:                      37  38 39 3a 3b 3c 3d 3e 3f |        7 89:;<=>?"));
    assert(
        strstr(output,
               "0x20000040: 40 41 42 43 44 45 46 47  48 49 4a 4b 4c 4d 4e 4f | @ABCDEFG HIJKLMNO"));
    command(".");
    assert(strstr(output, ".=20000100"));
    command(". xyz");
    assert(strstr(output, "invalid address"));
    command(". ffffffff");
    assert(strstr(output, ".=ffffffff"));
    command("list");
    assert(strstr(output, "not aligned"));
    debug_leave();
    program.threads[1].present = true;
    program.threads[1].state = THREAD_RUNNABLE;
    program.threads[1].debug_status = HART_STOPPED;
    program.threads[1].frame.pc = GUEST_BASE + 4;
    program.threads[0].debug_status = HART_EXITED;
    length = 0;
    assert(debug_enter());
    assert(strstr(output, "** hart1: pc=20000004"));
    assert(strstr(output, "(hart0 exited)"));
    command("hart0");
    assert(strstr(output, "** hart0:"));
    debug_leave();
}

/* These words come from the toolchain assembler with the chip's complete ISA
 * enabled, including Zcb/Zcmp patterns that share the compressed opcode space. */
static void extension_checks(void) {
    static const struct {
        uint32_t word;
        const char *name, *args;
    } vectors[] = {
        {0x20c5a533, "sh1add", "a0, a1, a2"},
        {0x40c5f533, "andn", "a0, a1, a2"},
        {0x0ac5c533, "min", "a0, a1, a2"},
        {0x60c59533, "rol", "a0, a1, a2"},
        {0x61f5d513, "rori", "a0, a1, 0x1f"},
        {0x28c59533, "bset", "a0, a1, a2"},
        {0x49f59513, "bclri", "a0, a1, 0x1f"},
        {0x08c5c533, "pack", "a0, a1, a2"},
        {0x08c5f533, "packh", "a0, a1, a2"},
        {0x60059513, "clz", "a0, a1"},
        {0x60159513, "ctz", "a0, a1"},
        {0x60259513, "cpop", "a0, a1"},
        {0x60459513, "sext.b", "a0, a1"},
        {0x60559513, "sext.h", "a0, a1"},
        {0x0805c533, "zext.h", "a0, a1"},
        {0x2875d513, "orc.b", "a0, a1"},
        {0x6985d513, "rev8", "a0, a1"},
        {0x6875d513, "brev8", "a0, a1"},
        {0x08f59513, "zip", "a0, a1"},
        {0x08f5d513, "unzip", "a0, a1"},
        {0x81e8, "c.lbu", "a0, 3(a1)"},
        {0x85a8, "c.lhu", "a0, 2(a1)"},
        {0x85e8, "c.lh", "a0, 2(a1)"},
        {0x89e8, "c.sb", "a0, 3(a1)"},
        {0x8da8, "c.sh", "a0, 2(a1)"},
        {0x9d61, "c.zext.b", "a0"},
        {0x9d65, "c.sext.b", "a0"},
        {0x9d69, "c.zext.h", "a0"},
        {0x9d6d, "c.sext.h", "a0"},
        {0x9d75, "c.not", "a0"},
        {0x9d4d, "c.mul", "a0, a1"},
        {0xb8fe, "cm.push", "{ra, s0-s11}, -0x70"},
        {0xbafe, "cm.pop", "{ra, s0-s11}, 0x70"},
        {0xbefe, "cm.popret", "{ra, s0-s11}, 0x70"},
        {0xbcfe, "cm.popretz", "{ra, s0-s11}, 0x70"},
        {0xac3e, "cm.mvsa01", "s0, s7"},
        {0xac7e, "cm.mva01s", "s0, s7"},
        {0x1cc5850b, "h3.bextm", "a0, a1, a2, 8"},
        {0x1df5c50b, "h3.bextmi", "a0, a1, 0x1f, 8"},
        {0x00002033, "h3.block", ""},
        {0x00102033, "h3.unblock", ""},
    };
    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i)
        decoded(vectors[i].word, (vectors[i].word & 3) == 3 ? 4 : 2, vectors[i].name,
                vectors[i].args, NULL);
    decoded(0xb802, 2, "???", "", NULL);
    decoded(0xac22, 2, "???", "", NULL);
    decoded(0x8de8, 2, "???", "", NULL);
    decoded(0x2000000b, 4, "???", "", NULL);
}
int main(void) {
    decoder_checks();
    extension_checks();
    cursor_checks();
    puts("debug checks passed");
    return 0;
}
