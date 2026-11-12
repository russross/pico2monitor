Flexible guest memory and execution contract
============================================

Aims and philosophy
-------------------

This is a teaching environment for running assembly programs on actual hardware, with short edit/build/upload/run cycles. A minimal Linux-style userspace provides familiar startup and syscall conventions. Students supply static RV32 executables linked to physical SRAM and accept a small syscall set. General Linux application compatibility is not a goal.

The sandbox boundary is mandatory: monitor state, protected reset images, ROM, flash, peripherals, and other system resources are inaccessible to guests. Protection within guest memory is a debugging aid. Accept a working guest layout even when it requires RWX access. Never remove required access to improve a protection score. Faults help students find ordinary memory mistakes; their absence does not prove a program is correct.


Decisions and rationale
-----------------------

*   ELF permissions, runtime allocations, and effective PMP permissions are distinct data. This prevents padding and best-effort protections from being mistaken for exact ELF mappings.
*   The NAPOT tree solver spends seven configurable entries on guards, text write protection, read-only-data write protection, writable-memory execute protection, then unused-space denial. The eighth entry always denies the entire address space, overriding the hardware's hardwired peripheral grants.
*   The solver uses bounded dynamic-programming workspace in disposable guest SRAM and terminates uniform subtrees. An independent effective-map verifier checks trusted boundaries and required permissions before writing segment destinations.
*   The recommended text base remains 0x20020000 for compatibility between flash and SRAM monitor builds. Measured allocations and compiler stack reports support reducing the flash reservation to 96 KiB; the SRAM build keeps 128 KiB because it also holds executable code. Both retain an 8 KiB normal stack.
*   Four-byte instructions may start two bytes before a NAPOT boundary. Hardware testing established that a fetch spanning different first-match entries faults even when both grant X. The solver and independent verifier therefore preserve one effective entry across continuous executable granules. This deliberately sacrifices some partial text protection to keep valid instructions executable.
*   Static ELF32 ET_EXEC uses p_vaddr as destination. No dynamic linking, TLS, relocation, or physical/virtual translation is implemented. Program headers must fit in the first 4096 bytes and there may be at most sixteen.
*   Initialization overlaps are allowed only when overlapping initialized destinations refer to identical file offsets. Final clearing skips the union of initialized destinations, so initialized bytes win over BSS overlaps.
*   Reset support is fixed at load time. Automatic retention requires 32 KiB of heap capacity and a verified PMP layout; `load --no-reset` omits snapshots. Reset never releases storage or replans PMP. The first accepted DATA payload invalidates the old guest; loader buffers and planner workspace then use guest SRAM, with live buffers moved into the planned stack before segment initialization.
*   The heap is planned to its capacity. brk changes logical allocation and zeroes growth without changing PMP entries during ordinary execution.
*   Linux generic syscall numbers replace the original private numbers. Existing guest binaries must be rebuilt. RV32 time uses time64 clock_gettime and clock_nanosleep with CLOCK_MONOTONIC; unsupported calls return -ENOSYS.
*   Arguments live on the guest stack; envp points to an empty table. No environment configuration is retained. run supplies the image name as argv[0]. Quoting has no expansion or command execution.


Milestones and validation
-------------------------

Implementation and validation completed on 2026-11-02:

1. Flexible ELF loading and the PMP planner passed native sanitizer checks, including prefix-backed tiny files, RWX acceptance, malformed inputs, and replacement failure semantics.
2. Launch tables, environment, Linux-numbered syscalls, brk zeroing/bounds, snapshot policy overrides, and irreversible reclamation passed native and end-to-end checks. The four-cell solver oracle exhaustively compared all 256 permission patterns against every ordered pair of candidate regions, including executable-entry continuity.
3. All seven emulator scenarios passed. The shared executable scenario also passed against the final RWX fixture after correcting its data alignment. Upstream generated emulator warnings remain visible; the deliberately RWX linker fixture emits its expected RWX warning.
4. The protected flash hardware suite passed default-linker assembly/RWX programs, arguments/environment, heap behavior, snapshot retention/release, preemption, saved registers, and twelve protection/trap probes. The reduced flash reservation also ran an executable linked at the newly available 0x20018000 base.
5. The protected SRAM build passed the same hardware suite after reconnecting once. Its first attempt encountered malformed console startup input; the exact cause is unestablished. Both attempt logs are retained. No source workaround or warning suppression was added for that observation.

Final observed normal-stack high-water use was 2716 bytes with 8192 reserved. Flash static SRAM extent was 68456 bytes; SRAM-executing extent was 108648 bytes. Compiler reports show 160-byte solver frames, 80-byte reconstruction frames, and 1088-byte framing encode/decode frames. These reports support retaining margin rather than reducing the normal stack to the observed minimum. The flash layout leaves 21656 bytes between static allocations and the stack; linker assertions and region limits enforce the boundary.

Primary artifacts are build/test-flex-check.log, build/test-flex-hardware.log, build/test-flex-hardware-ram-retry.log, build/test-flex-ram-build.log, build/test-flex-flash.log, and build/flashed-image.sha256. Host framing tests also passed. An accidentally unscoped Ruff invocation inspected the pre-existing .venv and reported dependency-file lint; the repository's scoped checks passed and no dependency files changed.

BOOTSEL programming used sudo -n picotool without changing host rules. Initial immediate programming attempts raced USB re-enumeration; waiting for picotool to discover the device resolved that timing. The final protected image was written and flash-verified. The board returns to that flash image after SRAM testing; no Git checkout, reset, commit, or other Git mutation was performed.


Load-time reset and guest scratch validation (2026-11-11)
-------------------------------------------------------

Formatting, Ruff lint, ty, native ASan/UBSan checks, three host transport tests, and all nine emulator scenarios passed. Native coverage includes destinations overwriting initial buffers and planner workspace, shared file sources, BSS overlapping initialized content, and a prefix boundary crossed inside a packet. Startup checks verify arguments and empty envp. Reset-disabled loads reject restart; retained snapshots restore data without changing the memory/PMP plan.

The protected flash image was programmed and flash-verified on the attached Pico 2. The complete hardware suite passed, including twelve fault probes, reset policy, register preservation, heap behavior, and suspension. Additional hardware checks confirmed cancellation before BEGIN and after BEGIN preserves the ready guest, while cancellation after the first accepted DATA block leaves EMPTY. The board was restored to hello.elf in READY state. Hardware suite output is in build/test-load-hardware.log.

Observed flash static SRAM extent is 40040 bytes, down from 68456 bytes (28416 bytes removed). Observed normal-stack high-water use in this suite was 2456 bytes, with 8192 reserved. The flash and SRAM monitor reservations remain unchanged; the newly unused trusted SRAM is not yet added to the guest arena. Transport framing buffers remain monitor-owned to validate the first payload before guest invalidation.
