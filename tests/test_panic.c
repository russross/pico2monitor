#include "rp2350js-c.h"

#include <errno.h>

/* Symbols come from the linked ELF, so these checks execute the production
 * assembly and C paths without adding fault-injection hooks to firmware. */
typedef enum {
    OWNER,
    PARKED,
    STARTED,
    GATE_LOCK,
    DISPATCH,
    RETURN,
    GATE,
    IDLE,
    RECOVER,
    BOOTSEL,
    HALT,
    STACK_TOP,
    SYMBOL_COUNT
} Symbol;
typedef enum {
    GATE_CORE0,
    GATE_CORE1,
    SIMULTANEOUS_CORE0,
    SIMULTANEOUS_CORE1,
    PEER_IN_C,
    PEER_IN_GUEST,
    BEFORE_PEER_LAUNCH,
    UNRESPONSIVE_PEER,
    NESTED_OWNER,
    NESTED_PEER,
    SCENARIO_COUNT
} Scenario;
static RP2350 *chip;
static uint32_t symbols[SYMBOL_COUNT];

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "panic check failed at line %d: %s\n", __LINE__, #condition);          \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static CPU *core(unsigned id) {
    return chip->core[id].obj;
}
static uint32_t value(Symbol symbol) {
    return RP2350_readUint32(chip, symbols[symbol]);
}
static void set_value(Symbol symbol, uint32_t data) {
    RP2350_writeUint32(chip, symbols[symbol], data);
}
static void step(unsigned id) {
    chip->currentCore = (int32_t)id;
    CPU_executeInstruction(core(id));
}
static void run_to(unsigned id, Symbol symbol) {
    for (unsigned i = 0; i < 10000; ++i) {
        if ((uint32_t)core(id)->pc == symbols[symbol])
            return;
        step(id);
    }
    fprintf(stderr, "core %u did not reach symbol %u; pc=%08x\n", id, symbol,
            (uint32_t)core(id)->pc);
    exit(1);
}

/* Deliberately unusable interrupted SP/GP/TP prove that reaching recovery
 * depends only on trusted assembly state and the acknowledged guest stack. */
static uint32_t fault_pc(unsigned id) {
    return 0x20002000 + 4 * id;
}
static void fault(unsigned id) {
    CPU *cpu = core(id);
    cpu->waiting = false;
    cpu->pc = (int32_t)fault_pc(id);
    cpu->regs[2] = (int32_t)(0xdead0000 + id * 16);
    cpu->regs[3] = cpu->regs[4] = (int32_t)0xbad00000;
    cpu->csrs[0x340] = 0;
    chip->currentCore = (int32_t)id;
    CPU_trapEntry(cpu, 5, false);
}
static bool untouched(uint32_t offset, size_t count, uint8_t byte) {
    for (size_t i = 0; i < count; ++i)
        if (chip->sram[offset + i] != byte)
            return false;
    return true;
}
static void wait_without_peer(unsigned owner) {
    for (unsigned i = 0; i < 2000; ++i) {
        CHECK((uint32_t)core(owner)->pc != symbols[RECOVER]);
        CHECK((uint32_t)core(owner)->regs[2] == 0xdead0000 + owner * 16);
        step(owner);
    }
    CHECK(value(OWNER) == owner + 1);
    CHECK(value(PARKED) == 0);
    CHECK(untouched(0, 4096, 0xa5));
}

/* Peers can be spinning at the gate, finishing a C activation, or returning
 * from guest code through a timer trap. All must park before recovery starts. */
static void enter_peer(unsigned peer, Scenario scenario) {
    CPU *cpu = core(peer);
    cpu->waiting = false;
    cpu->csrs[0x300] &= ~8u;
    if (scenario == PEER_IN_C) {
        cpu->pc = (int32_t)symbols[DISPATCH];
        cpu->regs[1] = (int32_t)symbols[RETURN];
        cpu->regs[2] = (int32_t)symbols[STACK_TOP];
        cpu->regs[10] = (int32_t)peer;
        cpu->regs[11] = 0;
    } else if (scenario == PEER_IN_GUEST) {
        cpu->csrs[0x340] = 0x20003000;
        cpu->pc = 0x20002010;
        chip->currentCore = (int32_t)peer;
        CPU_trapEntry(cpu, (int32_t)0x80000007, false);
    } else if (scenario != GATE_CORE0 && scenario != GATE_CORE1) {
        cpu->pc = (int32_t)symbols[GATE];
        cpu->regs[2] = (int32_t)0xbad00000;
    }
    for (unsigned i = 0; i < 10000 && !(value(PARKED) & (1u << peer)); ++i)
        step(peer);
    CHECK(value(PARKED) == (1u << peer));
    run_to(peer, HALT);
    CHECK((cpu->csrs[0x300] & 8) == 0);
    CHECK(cpu->csrs[0x304] == 0);
    CHECK(cpu->csrs[0x340] == 0);
    CHECK(cpu->csrs[0x305] == symbols[HALT]);
}

static void recovered(unsigned owner) {
    uint8_t scratch[8192];
    memcpy(scratch, chip->sram + 0x80000, sizeof(scratch));
    run_to(owner, RECOVER);
    CHECK((uint32_t)core(owner)->regs[2] == 0x20001000);
    CHECK((uint32_t)core(owner)->regs[10] == fault_pc(owner));
    CHECK(core(owner)->regs[11] == 5);
    CHECK(core(owner)->regs[4] == 0);
    CHECK(value(OWNER) == owner + 1);
    CHECK(value(GATE_LOCK) == 1);
    CHECK(untouched(0, 4096, 0xa5));
    run_to(owner, BOOTSEL);
    CHECK(RP2350_readUint32(chip, 0x400d800c) == fault_pc(owner));
    CHECK(RP2350_readUint32(chip, 0x400d8010) == 5);
    CHECK(!memcmp(scratch, chip->sram + 0x80000, sizeof(scratch)));
}

static bool number(const char *text, uint32_t *result) {
    char *end;
    errno = 0;
    unsigned long parsed = strtoul(text, &end, 0);
    if (errno || !*text || *end || parsed > UINT32_MAX)
        return false;
    *result = (uint32_t)parsed;
    return true;
}

int main(int argc, char **argv) {
    uint32_t selected;
    if (argc != SYMBOL_COUNT + 3 || !number(argv[2], &selected) || selected >= SCENARIO_COUNT) {
        fprintf(stderr, "usage: %s firmware.hex scenario linked-symbol-addresses...\n", argv[0]);
        return 2;
    }
    for (unsigned i = 0; i < SYMBOL_COUNT; ++i) {
        if (!number(argv[i + 3], &symbols[i])) {
            fprintf(stderr, "invalid symbol address: %s\n", argv[i + 3]);
            return 2;
        }
    }
    Scenario scenario = (Scenario)selected;
    RP2350Options options = {.coreArch = "riscv"};
    chip = RP2350_new(&options);
    LoadFirmwareResult *firmware = RP2350_loadFirmware(chip, argv[1], NULL);
    CHECK(!firmware->useSram);
    free(firmware);

    /* Boot both cores normally before controlling their instruction ordering.
     * Firmware setup and ROM launch stay identical to ordinary emulator runs. */
    bool booted = false;
    for (unsigned i = 0; i < 5000000; ++i) {
        RP2350_step(chip);
        if (value(STARTED) == 3 && core(0)->waiting && core(1)->waiting &&
            (uint32_t)core(0)->pc >= symbols[IDLE] && (uint32_t)core(0)->pc <= symbols[IDLE] + 8 &&
            (uint32_t)core(1)->pc >= symbols[IDLE] && (uint32_t)core(1)->pc <= symbols[IDLE] + 8) {
            booted = true;
            break;
        }
    }
    CHECK(booted);
    memset(chip->sram, 0xa5, 4096);
    memset(chip->sram + 0x80000, 0x6b, 8192);
    set_value(GATE_LOCK, 1);
    unsigned owner = scenario == GATE_CORE1 || scenario == SIMULTANEOUS_CORE1;
    unsigned peer = owner ^ 1;
    fault(owner);

    /* Let an ordinary peer contend on the held gate before panic is published.
     * Its existing spin must notice the request without acquiring that gate. */
    if (scenario == GATE_CORE0 || scenario == GATE_CORE1) {
        core(peer)->waiting = false;
        core(peer)->csrs[0x300] &= ~8u;
        core(peer)->pc = (int32_t)symbols[GATE];
        core(peer)->regs[2] = (int32_t)0xbad00000;
        for (unsigned i = 0; i < 100; ++i)
            step(peer);
        CHECK(value(PARKED) == 0);
        CHECK(value(GATE_LOCK) == 1);
    }

    if (scenario == SIMULTANEOUS_CORE0 || scenario == SIMULTANEOUS_CORE1) {
        fault(peer);
        for (unsigned i = 0; i < 10000 && value(OWNER) == 0; ++i) {
            step(owner);
            step(peer);
        }
        CHECK(value(OWNER) == 1 || value(OWNER) == 2);
        owner = value(OWNER) - 1;
        peer = owner ^ 1;
        for (unsigned i = 0; i < 10000 && (uint32_t)core(owner)->pc != symbols[RECOVER]; ++i) {
            step(owner);
            step(peer);
        }
        CHECK(value(PARKED) == (1u << peer));
    } else if (scenario == BEFORE_PEER_LAUNCH) {
        set_value(STARTED, 1);
    } else {
        wait_without_peer(owner);
        if (scenario == UNRESPONSIVE_PEER)
            return 0;
        if (scenario == NESTED_OWNER) {
            CPU_trapEntry(core(owner), 3, false);
            run_to(owner, HALT);
            for (unsigned i = 0; i < 100; ++i)
                step(owner);
            CHECK(value(PARKED) == 0);
            CHECK(untouched(0, 4096, 0xa5));
            return 0;
        }
        enter_peer(peer, scenario);
        if (scenario == NESTED_PEER) {
            CPU_trapEntry(core(peer), 3, false);
            run_to(peer, HALT);
            CHECK(value(OWNER) == owner + 1);
        }
    }
    recovered(owner);
    puts("panic recovery checks passed");
    return 0;
}
