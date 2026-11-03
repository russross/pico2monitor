Pico 2 SRAM monitor functional specification
============================================

Purpose and design policy
-------------------------

The project gives students a short edit/build/upload/run cycle for assembly programs on real hardware. It approximates a minimal Linux userspace where that makes experiments familiar: static ELF loading, an exec-style stack, Linux syscall numbers, console descriptors, and a brk heap. Programs must link to physical SRAM and accept the documented syscall subset. Running arbitrary Linux applications, hosting a filesystem, or providing process isolation between multiple guests is outside the current scope.

The guest is strictly isolated from the monitor and the rest of the system. Inside its sandbox, protection is opportunistic debugging assistance. Accept a layout requiring RWX access when it can be isolated. Never remove a permission required by an ELF segment to make another segment safer. The primary internal risks are student pointer, indexing, stack, and allocation mistakes. A fault is useful evidence; lack of a fault is not evidence that an access was correct.

`EXECUTABLES.md` is the executable and shell contract. `DESIGN.md` records decision rationale and validation milestones. All source and build rules are in this directory; dependency trees are subdirectories. Git remains read-only unless explicitly authorized otherwise.


Hardware and trusted memory
---------------------------

RP2350 runs hart 0 in M-mode and one guest in U-mode, without an MMU. Hart 1 remains in the ROM wait-for-launch path. Pico 2 has 520 KiB SRAM: 512 KiB main SRAM followed by two 4 KiB banks. The board used for validation is revision A2, chip ID 081212E1C341B500, with 4 MiB flash.

The flash-executing monitor reserves 96 KiB at 0x20000000..0x20018000, including an 8 KiB normal stack at 0x20016000..0x20018000. Its static allocation extent and measured normal-stack use are reported by `map`. Linker assertions prevent static/heap allocations from reaching the reserved stack. There is no monitor application heap allocation after initialization; SDK/library use must remain included in accounting.

The SRAM-executing build reserves 128 KiB at 0x20000000..0x20020000 because its code also occupies SRAM. Its 8 KiB normal stack ends at 0x20020000. Build-time PICO_NO_FLASH selects the corresponding guest floor. The recommended executable base remains 0x20020000 so the same examples work on both builds.

Guest allocation ends at 0x20080000. The final two 4 KiB banks remain trusted; the emergency machine-fault stack ends at linker symbol __emergency_stack_top=0x20082000. Trusted guest frames, trap anchor, kernel continuation, and IRQ vector table are ordinary monitor BSS allocations. There is no separate 4 KiB trap-anchor allocation in the first scratch bank.

The original fixed layout reserved 128 KiB below the guest and another 128 KiB above its fixed snapshot slot, plus scratch banks. The new layout returns upper main SRAM to runtime allocation and reduces the flash monitor's lower reservation by 32 KiB. Normal-stack reservation remains 8 KiB rather than shrinking it to an observed minimum. Measurements are evidence for budgeting, not proof of all-path bounds.

RP2350 has eight configurable PMP entries supporting OFF and NAPOT, minimum region 32 bytes. Lower entry numbers win. Hardwired entries after them grant access to ROM/peripherals, so configurable entry 7 always denies the entire address space using address pattern 0x3fffffff. RP2350-E6 transposes R and X relative to standard RISC-V; hardware writes translate permissions through SDK register definitions. PMP locks remain clear and PMPCFGM0=0 so M-mode can initialize guest memory.

ACCESSCTRL additionally removes unprivileged access to ROM/XIP and peripherals. Writes require the 0xacce0000 key, including read/modify/write operations. Shared SRAM-bank NSU access remains enabled; PMP selects guest addresses. No DMA, device, ROM service, or peripheral register is delegated to the guest. mtval is hardwired to zero and cannot identify fault addresses.

References: [RP2350 datasheet](https://datasheets.raspberrypi.com/rp2350/rp2350-datasheet.pdf), Pico SDK 2.3.1, and TinyUSB commit 86ad6e56c1700e85f1c5678607a762cfe3aa2f47. Dependency setup pins local toolchain and build dependencies. SDK upgrades require auditing vector layout and register behavior.


Boot, service, and trap transaction
----------------------------------

SDK startup initializes clocks, flash/XIP, static storage, and the normal stack. main disables global interrupts, paints unused stack below its live frame for measurement, checks watchdog reboot state, enables watchdog recovery, installs trusted trap vectors, disables inherited peripheral IRQs, initializes deny-all PMP and ACCESSCTRL, forgets guest state, configures the 1 MHz timer, initializes USB, and enables USB plus MTIP interrupt sources.

The M-mode event loop is non-preemptive: global MIE remains clear. Each pass services pending IRQs and timer, pumps TinyUSB/input/output, services framed uploads and bounded loader work, emits shell/map output, updates the watchdog, and advances guest/reset/pending-syscall work. With no runnable guest or memory work, wfi waits for an individually enabled pending interrupt. Hazard3 wfi does not require global MIE.

Before guest entry, install the verified PMP plan, set MPP=U and MPIE=1, retain kernel-selected TW, and keep counter access disabled. guest_enter saves the kernel's continuation, sp/gp/tp, and callee-saved registers in trusted storage, sets mscratch to a trusted trap anchor, restores the guest frame, and executes mret. Guest register values never become kernel pointers or the kernel stack.

On a guest trap, assembly obtains trusted storage through mscratch before memory access, saves x1..x31 and PC/cause/status without using guest sp, clears the kernel-mode mscratch sentinel, restores the kernel continuation including sp/gp/tp, and returns to the event loop. PMP grants are revoked before syscall dispatch. Interrupts leave PC unchanged; an ecall advances PC four bytes exactly once. Other synchronous traps produce FAULTED and discard pending calls. Saved state permits arbitrary-instruction suspension even with a corrupt guest sp.

An M-mode exception selects the linker-defined emergency stack, records PC/cause in watchdog scratch registers, and returns to BOOTSEL. Watchdog expiry also returns to BOOTSEL. A monitor failure is never classified as a guest fault. Software bootsel uses the ROM reboot API.

The vector table preserves the SDK handler-table layout at VTABLE_FIRST_IRQ. Hardware slots are uncompressed four-byte jumps; monitor_vectors contains the copied SDK table. Xh3irq dispatch uses MEINEXT and MEICONTEXT, with bounded attempts and no nested M-mode interrupts. Unexpected persistent IRQs are masked and reported. The ordinary protected build uses MTIP, not the external timer alias as a second source.

U-mode is preempted on USB/timer events, with timer deadlines rearmed at most 1 ms ahead. File clearing, snapshot copies, reset clearing, snapshot erasure, and brk growth are chunked to at most 1024 bytes. Console completion moves at most 256 bytes. ELF planning is a synchronous bounded operation over at most sixteen headers and the finite SRAM NAPOT tree; it does not promise the normal guest stop latency while a load is being planned. No guest executes during that interval.


USB, console, and transport transaction
---------------------------------------

Two CDC ACM interfaces share one USB cable. Console CDC carries shell and guest input/output; upload CDC carries framed ELF transfer. TinyUSB is called directly without pico_stdio_usb, its background worker, or its reset interface. USB IRQ 14 queues work; foreground tud_task_ext(0,false) performs bounded work while global IRQ delivery remains masked.

Buffers are fixed: 2 KiB per CDC RX/TX FIFO, a 2 KiB guest input ring, an 8 KiB ordered output ring with 2 KiB held back for diagnostics, bounded encoded frames, 1024-byte payloads, and a 4096-byte ELF prefix. No guest pointer reaches TinyUSB. Short endpoint writes preserve order. Disconnection suspends guest execution, cancels uploads, and discards undeliverable console output with counters.

Shell input uses a 2304-character command buffer. Single/double quoting and backslash escaping are literal syntax; no expansion or command execution exists. help lists command usage, and help COMMAND describes an individual command. Ctrl-C and Escape suspend active guests before ordinary input buffering. Resume restores the stopped RUNNING/WAITING state, pending syscall, PC, and registers. run requires READY; reset prepares READY but does not run automatically. The shell quit command emits a detach sequence consumed by pico-console; Ctrl-D at the monitor requests detachment; Ctrl-] detaches in the host client. Console reconnection reprints the banner and prompt. Map output emits at most one bounded row per service pass and delays the prompt until complete.

load arms a timed upload session. The host receives READY and sends BEGIN containing length, expected whole-file CRC, and bounded name. DATA carries session/sequence/offset/payload and frame integrity. The monitor accepts only expected stream positions; identical retransmitted DATA is acknowledged without copying or updating CRC twice. END verifies total length and whole-file CRC, waits for loader jobs, then publishes a final RESULT. Duplicate END replays that result. Invalid framing, timeouts, disconnect, cancellation, and capacity failures have explicit outcomes. Host framing uses COBS and CRC; limits remain 1 MiB file and 1024-byte payload.


ELF planning and replacement transaction
---------------------------------------

Before modifying SRAM, buffer min(file size,4096) bytes and validate little-endian System V ELF32, RISC-V ET_EXEC, soft-float ILP32 flags, standard header sizes, entry alignment, bounded ranges, and segment alignment. Headers must fit in that prefix and number at most sixteen. Accept file sizes from 52 bytes through 1 MiB. Section headers, symbols, and debug information do not affect placement.

PT_LOAD destinations use p_vaddr. p_paddr does not request another copy. Multiple R/RX/RW/RWX segments, arbitrary ordering, shared file sources, and BSS-only segments are supported. Write-only ELF requests receive RW access because the hardware requires R with W. GNU_STACK is optional; when present it must request non-executable RW and no more than the default 16 KiB stack. Dynamic/interpreter/TLS program headers are unsupported. Known passive attributes, notes, PHDR, GNU_RELRO, and GNU_PROPERTY metadata need no runtime operation here.

Initialized destination overlaps are accepted only when they reference identical source offsets. Required permissions are unioned across all memory overlaps and across 32-byte hardware granules. A valid entry lies within initialized executable content. Every load destination must fit the guest arena.

memory_plan_build computes image end, heap start aligned to 32 bytes, stack and snapshot placement, and optional guard space. Stack is 16 KiB. A 1 KiB logical gap below it is reserved when enough space remains; hardware denial of that gap is optional. Heap capacity spans from image end to that gap. Unused holes below image end do not become separately allocatable heaps.

Snapshot policy is evaluated before replacement. Prefer a stack-sized (16 KiB) aligned snapshot boundary to reduce PMP cost, then try a compact 32-byte boundary if fitting/headroom requires it. AUTO requires at least 32 KiB of heap capacity with snapshots; ON accepts tight layouts but cannot override capacity or isolation; OFF avoids snapshot storage. Snapshot payload is the sum of initialized writable segment sizes, packed in segment order; duplicate writable overlaps can occupy duplicate snapshot bytes. Reserved storage includes alignment padding.

The solver builds a NAPOT tree rooted at main SRAM. Each 32-byte cell has required permissions, trusted ownership, and debugging roles. Dynamic programming considers inherited permissions, optional region overrides, and left/right budget splits. Uniform subtrees terminate directly. Child exceptions are emitted before enclosing fallback grants. Permission candidates are NONE, R, RX, RW, RWX; granting extra access inside the sandbox is allowed.

The lexicographic score counts, in order: denied cells in the nearest 1 KiB below the stack; text bytes without W; read-only-data bytes without W; writable bytes without X; unused bytes denied. Equal scores prefer fewer entries. This is a deterministic best effort within the supported tree and placement candidates, not a claim of globally optimal placement over every SRAM address.

A PMP boundary cannot split adjacent executable granules: a four-byte instruction can begin two bytes before that boundary, and Hazard3 rejects a fetch spanning different first-match entries even if both grant X. Candidate region boundaries inside continuous executable requirements are therefore forbidden. This can sacrifice partial text protection to preserve execution. The hardware RWX fixture exercises such a two-byte-aligned instruction.

An independent verifier checks entry geometry/configurations, runtime range ordering, required permission coverage, executable-entry continuity, and trusted denial throughout main SRAM. It does not trust DP scores. Only a verified complete plan can cross the replacement boundary. Early failure preserves the current Program byte-for-byte, including a suspended register context.

At replacement, revoke PMP, invalidate the old program, and clear the entire guest arena in bounded chunks. Replay prefix bytes through segment/file intersections, then consume the saved remainder of the boundary chunk and later stream chunks through the same function. Since zeroing precedes initialization, BSS overlap cannot erase file-backed bytes. Verify all segment byte counts and file integrity. Copy initialized writable bytes into protected snapshot storage when enabled, fence instruction fetch, install the committed image, and mark READY. Failure after invalidation leaves EMPTY. Automatic map output follows success.


Launch and syscall transaction
------------------------------

Monitor-owned environment storage starts empty. Environment edits are transactional and bounded to 32 entries and 2048 bytes. run accepts at most 31 additional arguments (32 including image name). Combined argv/environment strings including NULs must fit 2048 bytes. Invalid launch requests retain READY and do not modify guest memory.

At launch, calculate storage first, then place strings and tables at the top of the guest stack. Initial sp is aligned to 16 bytes. Tables contain argc, argv including uploaded name as argv[0], NULL, envp, NULL, and auxiliary-vector pairs ending AT_NULL/0. Include AT_PAGESZ=4096 and AT_ENTRY; include PHDR/PHENT/PHNUM only when those headers are mapped. Set convenience a0/a1/a2 values, mark RUNNING, and clear stale input. Supplied startup derives parameters from sp, initializes gp without relaxation, calls main, and exits.

Linux generic syscall numbers replace the old private ABI; all old guests must be rebuilt. read(63) accepts fd0; write(64) accepts fd1/2. Counts above INT32_MAX return -EINVAL, invalid descriptors -EBADF, and invalid pointers -EFAULT. Zero length does not dereference its pointer. Guest writes expand LF to CRLF. Guest input echoes and edits the current line with Backspace or Delete; CR completes a line as LF, and reads return up to one completed line, including its newline. Ctrl-D submits unfinished input without a newline, or completes a read with zero bytes at the start of a line. Input beyond the bounded ring is dropped. Reads may be short; writes remain pending until every requested byte is queued, advancing in bounded chunks under output backpressure. Completion does not wait for USB delivery. Blocking calls retain arguments as data in PendingCall, never as suspended C frames. Polling resumes the saved guest with the completion in a0.

exit(93) and exit_group(94) mark EXITED; nonzero status is printed. sched_yield(124) returns through scheduling. Unsupported numbers return -ENOSYS.

brk(214) returns the resulting break, with zero as a query and failure returning the unchanged value. Valid changes remain between heap base and planned limit. Shrink is immediate; growth clears newly exposed bytes in bounded chunks before publishing the break and returning. A stopped pending growth resumes from its saved cursor; reset discards it. PMP maps heap capacity, not each allocation, so direct guest access can exceed the logical break. Kernel copies independently enforce logical allocation bounds.

clock_gettime64(403) supports CLOCK_MONOTONIC=1 and writes two signed 64-bit seconds/nanoseconds fields. clock_nanosleep_time64(407) supports that clock with relative flags=0; it validates and copies a 16-byte request before retaining an absolute microsecond deadline. Seconds and nanoseconds must be nonnegative, nanoseconds less than one billion, and arithmetic must not overflow. Nanoseconds round upward to microseconds. Remaining-time storage is unused because there is no signal interruption. The guest helper library exposes millisecond sleep/uptime using these calls.

Every kernel guest copy validates effective PMP read/write permissions and trusted/heap bounds first. Requests can cross adjacent accessible regions. Checked range arithmetic rejects wraparound. All physical accesses remain within validated intervals; M-mode's bypass of unlocked PMP is never used as pointer validation.


Reset and snapshot reclamation transaction
------------------------------------------

reset revokes PMP, discards pending syscalls/input, and scans guest SRAM in 1024-byte jobs. Initialized bytes are restored from snapshots when the initialized segment is writable and a snapshot exists. Without snapshots, initialized bytes retain their current values. Immutable image memory remains intact. Other writable zero-fill and transient storage is cleared; initialized overlaps always win over zero-fill. Protected snapshot bytes are not touched during restoration.

After ordinary reset, clear registers/pending state/status/accounting, reset PC and initial sp, reset the break to heap base, mark READY, and prompt. Argument/environment tables are rebuilt at the next run from current settings. Immutable memory that had writable fallback permissions can have been corrupted by guest code; reset is not a complete immutable-image reload.

reset-data changes the next-load default. With a current snapshot, OFF schedules release at the next reset, ON cancels release, and AUTO releases only when current heap headroom is below the automatic threshold. Without an original copy, enabling preservation requires reload; saving current mutated bytes would misrepresent the original executable.

For release, restore initialized writable bytes one final time, clear transient storage, then erase snapshot payload and padding in bounded chunks. Only after erasure rebuild/verify the no-snapshot plan, move the stack upward, increase heap capacity, reset initial state, and publish READY plus the new map. Snapshot reclamation cannot be reversed without reloading. A failed replanning invariant is a monitor failure, not permission to publish an unsafe image.


Build and validation
--------------------

make uses ecosystem CMake/SDK dependency tracking; no clean rebuild is needed for normal development. Firmware and host source live in src/, repeatable fixtures and checks in tests/, and dependency setup in scripts/. CMake tracks firmware source and headers; the root Makefile drives builds, tests, packaging into dist/, and dependency downloads into disposable deps/. -fstack-usage emits compiler reports for firmware and SDK C paths. Stack painting begins in main below live frames and does not measure prior startup. The 8 KiB reservation retains margin above measured paths and compiler-assisted call-chain accounting.

make check orders C formatting, Ruff formatting/lint, and uv run ty check before native sanitizer tests, host framing tests, and emulator integration. The native suite includes a four-cell exhaustive ordered-region oracle against DP scores, malformed ELF/range cases, prefix-backed tiny RWX loading, brk logical bounds and zeroing, launch pointers/alignment, snapshot policy overrides, reclamation, framing/retries, and replacement failures.

The emulator uses rp2350js's transpiled C release and real USB controller model with two PTYs. It lacks PMP/U-mode/MTIP behavior, so monitor_emu runs guests in M-mode, accepts M ecalls, and uses the external timer alias. Its explicit banner prevents confusion with isolation tests. Existing upstream generated-header warnings remain visible. rp2350js-c.patch fixes an ORC.B mask defect, checked by dependency setup and startup vectors. Reduced compiler features avoid incomplete emulator Zcmp decoding. Emulator results establish workflows, not sandbox safety.

Protected hardware tests exercise default-linker assembly, quoted/empty arguments, environment, heap growth/shrink, preserved/released snapshots, all saved registers, blocking calls, uncooperative suspension, and twelve fault probes. Fixed-layout probes intentionally reserve simple representable regions so their internal protections are predictable. Tests also verify that faults cannot resume and the monitor remains responsive. A tiny RWX image in native tests establishes acceptance of permissive layouts; hardware boundary and fallback tests extend this evidence rather than relying on the emulator.

Connected-device console identity is /dev/serial/by-id/usb-Pico_monitor_RV32_SRAM_monitor_081212E1C341B500-if00; upload ends -if02. The existing device-specific udev rule grants CDC access and excludes ModemManager. BOOTSEL access currently uses sudo -n picotool. After software bootsel, wait for USB enumeration before invoking picotool; an immediate invocation can race disconnect/reconnect. No physical reset is normally necessary.

Historical validation and bring-up logs from the previous layout may be kept in artifacts/build/; they are not required for repeatable checks. Update DESIGN.md with final measurements and deployment state when validation finishes.
