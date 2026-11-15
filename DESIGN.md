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
*   The recommended text base is 0x20000000. The flash-executing monitor occupies the top 16 KiB of main SRAM; capacity may change without moving the guest base. Scratch SRAM holds the 4 KiB output ring and one shared 4 KiB normal stack. Panic coordination runs stack-free until the peer parks, then reuses guest RAM for recovery.
*   Four-byte instructions may start two bytes before a NAPOT boundary. Hardware testing established that a fetch spanning different first-match entries faults even when both grant X. The solver and independent verifier therefore preserve one effective entry across continuous executable granules. This deliberately sacrifices some partial text protection to keep valid instructions executable.
*   Static ELF32 ET_EXEC uses p_vaddr as destination. No dynamic linking, TLS, relocation, or physical/virtual translation is implemented. Program headers must fit in the first 4096 bytes and there may be at most sixteen.
*   Initialization overlaps are allowed only when overlapping initialized destinations refer to identical file offsets. Final clearing skips the union of initialized destinations, so initialized bytes win over BSS overlaps.
*   Reset support is fixed at load time. Automatic retention requires 32 KiB of heap capacity and a verified PMP layout; `load --no-reset` omits snapshots. Reset never releases storage or replans PMP. The first accepted DATA payload invalidates the old guest; loader buffers and planner workspace then use guest SRAM, with live buffers moved into the planned stack before segment initialization.
*   The heap is planned to its capacity. brk changes logical allocation and zeroes growth without changing PMP entries during ordinary execution.
*   Linux generic syscall numbers replace the original private numbers. Existing guest binaries must be rebuilt. RV32 time uses time64 clock_gettime and clock_nanosleep with CLOCK_MONOTONIC; unsupported calls return -ENOSYS.
*   Arguments live on the guest stack; envp points to an empty table. No environment configuration is retained. run supplies the image name as argv[0]. Quoting has no expansion or command execution.
*   One atomic SRAM word gates monitor execution on either core. Trap entry saves guest registers before acquiring the gate; assembly establishes a fresh shared C stack and abandons it before release. Pending operations retain all continuation state as data. Device interrupts stay on their receiving core, while either invocation can advance either thread's pending syscall.
*   Two reusable thread slots are pinned to their corresponding cores. Private syscall 1000 supplies only a PC and passthrough parameter; the new thread's other integer registers are zero. Guests own stacks, TLS, shims, and explicit exit conventions. No load flag, ELF extension, second-stack reservation, migration, or join machinery is required.
*   Timer interrupts provide correctness for blocked-thread wakeup and whole-program suspension. STOPPING closes both return paths; shell access begins only after both cores acknowledge their saved frames. Fault and group exit use the same barrier. A courtesy interrupt is unnecessary for correctness and is not implemented.


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


Two-core monitor and upper SRAM placement (2026-11-13)
-----------------------------------------------------

The monitor uses a stack-free atomic gate, one fresh C stack per invocation, per-thread frames and pending calls, and timer-driven return to runnable guest threads. Guest slot 0 can exit before slot 1, and either survivor can repeatedly create a replacement. Pending reads and heap changes follow request order; buffered writes revalidate each copy against the current shared break. Fault, group exit, and suspension wait for both cores before exposing stopped guest state.

The shared normal stack is 4096 bytes at 0x2007f000..0x20080000. With floating printf disabled, the deepest audited service chain is dispatch -> transfer_step -> reply -> wire_encode, totaling 2304 bytes in both builds. Direct emulator SP observation confirmed that depth; stack painting reports 2296 bytes because eight reserved bytes remain unwritten. Both firmware variants build with linker assertions enforcing the stack bounds. All ten flash emulator scenarios and SRAM executable/thread scenarios passed with the 4 KiB stack; the flash suite reported 2296 bytes of painted-stack use with 4096 reserved. Logs are build/stack4k-build.log, build/stack4k-ram-build.log, build/stack4k-emulator.log, and build/stack4k-ram-emulator.log. Total monitor reservations and guest ceilings remain unchanged; reducing the stack adds 4 KiB of static/heap headroom. No hardware validation was performed for this stack size.

Loader planning is incremental, including explicit solver traversal, reconstruction, and independent verification across all main SRAM. The transport withholds the boundary DATA acknowledgment while planning and prefix replay complete. Guest build rules track Makefile flag changes so switching the default base rebuilds assembly fixtures as well as C fixtures. Protected fault probes now target the upper trusted reservation rather than the newly available low SRAM. SRAM emulator boot exposed an upstream fixed 256 KiB ROM search window that extended beyond physical SRAM; the host adapter now bounds that window by physical memory before resetting the chip.

Formatting, Ruff, ty, native ASan/UBSan checks, three host transport tests, and all ten emulator scenarios for both flash and SRAM layouts passed. The SRAM suite ran the thread scenario separately from the other nine cases after correcting its boot-search window; flash executable/thread checks also passed after that host-adapter change. Coverage includes the zero-register entry contract, slot exhaustion and replacement, simultaneous busy-thread suspension/resume, reset, and a secondary-thread breakpoint fault. The emulator aborts on some illegal instruction encodings, so the shared fault scenario uses architectural ebreak; native tests also exercise an illegal-instruction cause. Both protected firmware variants link successfully. Current linked static extents are 40880 bytes for flash and 84592 bytes for SRAM execution; each reserves one 8192-byte normal stack. These are link measurements, not hardware stack measurements.

No physical board was visible during this work, so the revised protection layout, real timer/trap behavior, and two-core execution have not been validated on hardware or flashed. The hardware suite includes the new shared thread scenarios. The protected flash firmware is packaged at dist/monitor.uf2. Logs are build/thread-lint.log, build/thread-native.log, build/thread-host.log, build/thread-emulator.log, build/thread-emu-final.log, build/thread-ram-smoke.log, build/thread-ram-emulator.log, and build/thread-ram-build.log. Historical hardware results above describe earlier firmware layouts.


Flash-only firmware and stack-free panic recovery (2026-11-13)
-------------------------------------------------------------

Both supported firmware targets execute from flash: monitor is protected deployment firmware, and monitor_emu is the emulator variant. SRAM execution, its linker script, and its optional startup diagnostic have been removed. CMake rejects PICO_NO_FLASH; the host emulator rejects SRAM firmware. Historical SRAM-build measurements above remain records of earlier configurations. The shared 4 KiB stack and guest ceiling remain unchanged.

A synchronous monitor fault elects one panic owner without touching SP or the normal gate. The peer observes panic at guest trap entry, while waiting for the gate, before entering C, or after completing its C activation. It disables interrupts, installs a stack-free nested-fault halt vector, publishes its parked bit with release ordering, and never resumes. The owner acquires that acknowledgment before assigning sp=0x20001000 and entering the existing C/ROM recovery path. Only the owner records PC/cause and requests BOOTSEL. Before core 1 launch only core 0 participates; the launch handshake marks core 1 expected before it can run. Missing acknowledgment or nested recovery faults leave watchdog recovery in control. No permanent emergency stacks remain; 0x20080000..0x20082000 is unassigned.

Both supported images build successfully. Formatting, Ruff lint, ty, ten ordinary emulator scenarios, and ten deterministically scheduled panic scenarios passed. Panic checks cover either owner, simultaneous faults, a peer already spinning on a held gate, C completion, guest timer entry, pre-launch participation, an unresponsive peer, and nested faults. They validate unusable interrupted stack/global/thread pointers, guest RAM protection until acknowledgment, coherent owner diagnostics, and untouched scratch banks. The driver stops at monitor_bootsel entry; actual ROM BOOTSEL reboot and watchdog timeout remain unvalidated on hardware. Current protected static SRAM extent is 35192 bytes. Logs are build/flash-only-build.log, build/flash-only-test-build.log, build/flash-only-panic.log, build/flash-only-emulator.log, build/flash-only-rejection.log, and build/flash-only-emulator-rejection.log. Updated firmware is packaged at dist/monitor.uf2; no hardware flashing was performed.


16 KiB main-SRAM monitor reservation (2026-11-13)
-----------------------------------------------

The main reservation is 0x2007c000..0x20080000, leaving 496 KiB for the guest arena, 80 KiB more than the preceding layout. Scratch bank X holds a 4096-byte NOLOAD console output payload; its empty indices remain in main BSS. Scratch bank Y holds the shared 4096-byte stack at 0x20081000..0x20082000. Linker assertions enforce the assignments. The protected image uses 3852 initialized main-SRAM bytes, 11112 BSS bytes, and 52 bytes of alignment: 15016 static bytes. The SDK minimum heap allowance is 256 bytes, with no allocator linked, leaving 1112 bytes of main-reservation headroom.

All four TinyUSB CDC FIFOs are 512 bytes, guest keyboard input is 512 bytes, and the monitor output ring is 4 KiB with its existing 2 KiB diagnostic reserve. The shell retains its 2304-character line and 2048-byte launch-string budget. DATA payloads remain 1024 bytes. COBS receive decoding compacts the 1056-byte monitor buffer in place and exposes a validated borrowed payload; the loader copies accepted data before that buffer can be reused. Replies fit a 192-byte buffer, which retains the encoded final RESULT for retransmission. Header, payload, and CRC are encoded directly without a full raw-packet stack array. Transfer state falls from 4384 to 1360 bytes, excluding the four-byte generation counter. SDK linker include overrides keep ordinary compiler helpers in flash and allocate output storage without adding zero-filled bytes to the flash image; SDK time-critical routines remain in SRAM.

Formatting, Ruff, ty, native ASan/UBSan checks, three host tests, eleven emulator scenarios, and ten panic scenarios passed. The initial new pressure test incorrectly expected keyboard input to survive suspension; source tracing confirmed the existing suspend/resume paths clear it. Its corrected isolated rerun verifies Escape under a full input ring and fresh input after resume. Other integration scenarios passed in the combined run. Native codec coverage includes every payload length with zero, nonzero, and mixed data, COBS block boundaries, corruption, destructive decoding, and rejection of undersized output storage before writes. Updated panic checks obtain the normal stack top from the ELF and verify scratch contents remain unchanged by recovery after the peer finishes its activation. Conservative compiler-assisted normal-stack accounting is 1536 bytes; encode/decode frames are 32/0 bytes. No new hardware validation or throughput measurement was performed.

Artifacts are build/16k-build.log, build/16k-test-build.log, build/16k-guests.log, build/16k-native.log, build/16k-emulator.log, build/16k-input-pressure.log, and build/16k-stack-analysis.txt. The initial pressure-test failure remains in the combined log; the corrected test passes in the dedicated log. dist/monitor.uf2 contains the protected firmware. Historical measurements above describe earlier memory layouts.


Separate monitor and guest terminals (2026-11-14)
-----------------------------------------------

CDC 0 owns the monitor shell. CDC 1 uses the existing framed transport for upload and guest interaction in separate phases, with one shared 1056-byte receive buffer and one shared 192-byte encoded transmit buffer. A guest claim after RESULT acknowledges upload completion before retiring upload replay. Repeated claims and releases acknowledge the existing transition without emitting another status notice. An upload-phase control error cannot replace the retained upload outcome. Guest ownership survives stop, exit, fault, reset, and replacement; a new host process must first reclaim the occupied interface through a merge.

Scratch X is partitioned into independent 2 KiB monitor and guest rings. Shell commands wait for echoed input and preceding output to drain before execution, and diagnostics wait at the same output fence. Guest service and USB continue during fences. Split-mode guest backpressure does not block monitor commands. Asynchronous routing notices preserve partially typed commands and redraw them incrementally. Unexpected IRQ and loader-replacement messages also defer output, so they cannot consume the space reserved for a bounded command response.

Guest release, USB connection loss, monitor loss, and five-second heartbeat expiry suspend both harts before merging. Two typed notice slots retain attachment followed by recovery even if the monitor has not read the first notice; recovery itself does not wait for that reader. A stopped explicit merge requires drained guest output. Neither upload, attach, merge, nor recovery automatically runs or resumes a guest.

Protected static storage is 15216 bytes, 72 bytes above the preceding build, leaving 912 bytes after the SDK's 256-byte heap allowance. The 496 KiB guest arena and both 4 KiB scratch reservations remain unchanged. Byte-at-a-time shell admission reduces io_service's local stack frame to 64 bytes, and deferred formatting removes the previous combined cancellation/formatting chain. These compiler reports do not establish an all-path stack bound.

Validation covers native ASan/UBSan ownership conflicts, retry replay, retained-owner uploads, both-hart recovery, DTR loss before notice delivery, lease expiry, and rejection without a monitor; four host framing/serial tests; twenty emulator workflows including the interactive send/attach CLI, terminal restoration, load-option forwarding, separate echo, partial-command redraw, queued output fences, and reupload on a retained guest connection; and the ten scheduled panic scenarios. Firmware and host artifacts are built under dist/. No hardware flashing or new hardware validation was performed.


Guest disassembly and memory inspection (2026-11-14)
--------------------------------------------------

The shell's `*` mode inspects a loaded guest after suspension or termination. A switch/table decoder covers the RP2350 instruction set, including bit manipulation, Zcb, Zcmp, and custom Hazard3 power/bit-extract encodings. The debugger uses the shell's existing command storage and streams one display row per output fence; it allocates no persistent listing or dump buffer. Hart records retain presence and termination status, and syscall exits retain their instruction PC for postmortem inspection.

Protected main-SRAM static use is 15252 bytes, 36 bytes above the preceding build: 16 bytes of hart metadata plus 25 bytes of debugger state, offset by 5 fewer alignment bytes. Initialized RAM remains 3852 bytes. The existing 256-byte heap allowance leaves 876 bytes inside the 16 KiB reservation. The 496 KiB guest arena, scratch output rings, and shared 4 KiB stack remain unchanged. Decoder tables and strings occupy flash. Compiler reports give debug_step a 176-byte frame and disassemble a 272-byte frame; formatting callees also consume stack, so these are not all-path bounds.

A temporary baseline built from read-only Git source exposed a 604-byte Newlib SRAM dependency from strtoul. A bounded hexadecimal parser removes that dependency. Validation covers native sanitizers, assembler-derived opcode vectors, shell emulator workflows, and all ten scheduled panic scenarios. Updated firmware builds in build/; no hardware flashing or new hardware validation was performed.
