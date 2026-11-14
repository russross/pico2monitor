Preparing and running executables
=================================

This monitor runs one assembly or freestanding C program with up to two threads on a Pico 2's RISC-V cores. It supplies a small Linux-style userspace: static executables, an initial startup stack, console syscalls, and a shared brk heap. Programs link to physical SRAM addresses and use only the supported syscalls.

The monitor and hardware resources are always protected. Protections inside the guest sandbox are best effort: the monitor protects text, constants, stack boundaries, and other memory when PMP entries permit it. A bad access inside the sandbox may succeed. Use `map` to inspect the actual permissions for your image.


Build an assembly program
-------------------------

`tests/hello.S` is the complete assembly example. It starts at `_start`, writes a message with syscall 64, and exits with syscall 93. Build it using the toolchain's default linker script, changing only the text-segment base:

    ./deps/toolchain/bin/riscv32-pico-elf-gcc \
        -march=rv32imac_zicsr_zifencei -mabi=ilp32 \
        -nostdlib -static -fno-pie -no-pie \
        -Wl,-Ttext-segment=0x20000000,-e,_start \
        tests/hello.S -o hello.elf

`make build/assembly.elf` builds the same example with debug information. `make build/exec.elf` builds `tests/guest_exec.c` using the supplied startup and helper functions, also with the default linker script. `tests/guest.ld` remains a fixed-layout regression fixture for older demonstrations and protection probes; new programs do not need it.

Inspect a file before uploading:

    ./deps/toolchain/bin/riscv32-pico-elf-readelf -h -l hello.elf

Check ELF32, little endian, RISC-V, EXEC, and the entry address. The monitor loads PT_LOAD program headers, not individual sections. FileSiz is initialized content; MemSiz also includes zero-filled memory. Debug information and other non-loadable metadata occupy transfer bytes but no guest SRAM.

The stable recommended base is 0x20000000, the start of SRAM. Higher load addresses remain supported. Firmware executes from flash and reserves 0x2007c000..0x20080000 for monitor static storage; its output ring and shared stack occupy scratch SRAM above that range. Guest segments, heap, the 16 KiB initial stack, and optional protected snapshots share 496 KiB below the monitor boundary. Future monitor budgets may change the upper limit without changing the recommended link address. `map` reports the final layout. Link addresses are not relocated during loading.

Files must be static little-endian ELF32 RISC-V ET_EXEC using soft-float ILP32. Compressed instructions are supported. Floating-point ABI, RV32E, dynamic linking, ELF TLS program headers, and executable-stack requests are unsupported. Unsupported instructions fault during execution. There may be at most sixteen program headers, all within the first 4096 bytes. Files may be 52 bytes through 1 MiB; a valid executable normally needs more than the minimum header. Stripped files and loadable headers are supported.

Multiple load segments, arbitrary ordering, and RWX segments are allowed. Overlapping initialized destinations must name identical source file bytes. There is no requirement to split text, constants, and data into predetermined slots.


Upload and iterate
------------------

1. Run `./dist/pico hello.elf` in an interactive terminal. It connects, suspends any running guest, issues `load`, uploads the file, and leaves the console open.
2. Inspect the automatic memory map and type `run`, optionally followed by arguments.
3. Use Ctrl-C/Escape to suspend, `resume` to continue, or `reset` then `run` to restart a suspended guest. After exit, `run` automatically resets when supported.
4. Use Ctrl-D or `quit` at the monitor prompt, or Ctrl-] in the host terminal, to detach. Ctrl-C and Escape are sent to the board.

`./dist/pico` connects without uploading. To upload separately, type `load` (or `load --no-reset`) in an existing console, then run `./dist/pico --send hello.elf` in another terminal. The single script uses only Python's standard library and Unix serial interfaces; no Pico SDK is needed on the host.

Guest console output translates LF to CRLF. Input is echoed and buffered until Enter; Backspace and Delete edit the current line. Reads may return part of a completed line. The console and upload use separate USB CDC interfaces. The script selects stable device identities automatically; use `--console-port DEVICE` and `--upload-port DEVICE` when needed. When multiple boards are connected, specify both devices for the same board. Console uses the interface ending in -if00; upload uses -if02. Ordinary terminal software also works with local echo disabled.

Arming load or accepting BEGIN preserves the previous image. The first accepted nonempty DATA block invalidates it immediately. After that, cancellation, corruption, or disconnect can leave no runnable image; upload again. The board never starts a partially loaded file. Power loss and monitor reboot discard the guest.

The last thread's exit status, or an exit_group status, becomes the program status; zero is silent and nonzero is printed. A fault stops both threads and reports the faulting thread, cause, PC, and SP. The chip does not supply a reliable faulting address. For exited programs, run automatically resets before starting again when reset support is retained. Suspended or faulted programs require explicit reset before a fresh run. Ctrl-C/Escape suspends both threads, including infinite loops, before showing the monitor prompt. `ebreak` and `c.ebreak` also suspend both threads; resume continues after the breakpoint instruction. Resume preserves both contexts and pending calls; reset discards both and prepares only the initial thread.


Arguments
---------

    run one "two words" ""

The image's uploaded filename becomes argv[0]. Quoting supports single quotes, double quotes, and backslash escaping outside single quotes. There is no variable expansion, command substitution, or shell pipeline syntax. The environment is always empty; envp points to a single NULL pointer.

Limits are 32 arguments including argv[0] and 2048 bytes of argument strings including NUL terminators. A command line may contain 2304 characters. A rejected launch leaves the program ready.

At `_start`, sp is aligned to 16 bytes. Memory starting there contains:

    argc
    argv[0] ... argv[argc-1], NULL
    envp[0] ... envp[n-1], NULL
    auxiliary-vector type/value pairs, AT_NULL/0

Pointers refer to NUL-terminated strings in the guest stack. The auxiliary vector includes AT_PAGESZ=4096 and AT_ENTRY. AT_PHDR, AT_PHENT, and AT_PHNUM are supplied when the program-header table is itself loaded. The nominal page size does not imply an MMU. Do not depend on other registers at raw entry; the supplied startup also receives convenience copies of argc/argv/envp in a0/a1/a2.

`tests/guest_start.S` initializes gp with relaxation disabled, derives arguments from sp, calls `main(int argc, char **argv, char **envp)`, and exits with its result. Link this startup exactly once. Pure assembly may supply its own `_start` and use the initial stack directly. Maintain 16-byte stack alignment and normal RISC-V register conventions when calling functions. Do not return from `_start`; invoke exit.


Syscalls
--------

Put the syscall number in a7 and arguments in a0 through a5, then execute ecall. Results appear in a0. Ordinary failures return negative errno directly; there is no monitor-managed errno variable. Other integer registers are preserved across syscalls and interrupts.

| Number | Call | Supported arguments and behavior |
| ------ | ---- | -------------------------------- |
| 63 | read | a0=0, a1=buffer, a2=count; waits for console input |
| 64 | write | a0=1 or 2, a1=buffer, a2=count; console output |
| 93 | exit | a0=status; ends the calling thread; the last exit ends the program |
| 94 | exit_group | a0=status; ends both threads |
| 124 | sched_yield | No arguments |
| 214 | brk | a0=requested end; zero queries current break |
| 403 | clock_gettime64 | a0=1 (CLOCK_MONOTONIC), a1=timespec pointer |
| 407 | clock_nanosleep_time64 | a0=1, a1=0 (relative), a2=request timespec, a3 ignored |
| 1000 | thread_create | a0=start PC, a1=parameter; returns the new slot number, 0 or 1 |

Timespec contains two signed 64-bit little-endian integers: seconds and nanoseconds. Nanoseconds must be in 0..999999999. Monotonic time is board uptime; suspension preserves an outstanding sleep's deadline. Absolute sleeps and other clock IDs return -EINVAL. Relative sleeps are rounded upward to microseconds. The remaining-time argument is unused because this environment has no signal interruption.

Reads may return fewer bytes than requested, currently at most 256 per completion. Writes wait until all requested bytes have entered the monitor output queue; they do not wait for USB delivery. Zero-length calls do not dereference their pointer. Invalid descriptors return -EBADF (-9), invalid buffers return -EFAULT (-14), invalid sizes/arguments return -EINVAL (-22), and unsupported syscalls return -ENOSYS (-38). Ctrl-D at the start of a guest input line completes a read with zero bytes; after typed characters it submits those characters without a newline. Ctrl-C and Escape are monitor controls rather than guest input.

brk returns the resulting break, not a negative errno. An invalid request returns the unchanged break. The heap begins after the highest load segment; `brk(0)` discovers it without linker symbols. Growth zeroes newly allocated bytes, including bytes exposed again after shrinking. Growth cannot cross the reported heap limit. Syscall buffers may use heap bytes only below the current break; PMP may permit direct loads/stores beyond that logical limit. Reset returns the break to its initial value.

A freestanding C runtime can use `tests/guest.h` and `tests/guest.c`. A general static libc may need additional syscalls, initialization, or TLS; static linkage alone does not establish compatibility. Binaries using this project's former private syscall numbers must be rebuilt.


Threads
-------

Thread creation is a runtime choice and requires no load option, special ELF metadata, or additional monitor-reserved guest stack. The initial thread occupies slot 0 on core 0. Creation uses the other core's vacant slot; if both slots are occupied it returns -EAGAIN (-11). Either thread may exit first, and the survivor can repeatedly create replacements. There is no join operation or retained per-thread exit record.

The new thread starts at the supplied PC with the parameter in a0. Every other integer register is zero, including sp, gp, tp, and ra. An odd or inaccessible entry PC, or one without effective execute permission, returns -EFAULT (-14). The monitor does not manufacture a return address or check guest-designated stack and TLS layouts.

A guest entry shim normally obtains its stack pointer and other startup information through a0, establishes sp and any required gp/tp, calls its intended function, and invokes exit if the function returns. The guest can allocate a stack from shared memory or split the initial stack. Stack alignment, overlap avoidance, storage lifetime, and synchronization belong to the guest. `tests/guest_threads.S` demonstrates raw entry, explicit stack setup, and replacement after the initial thread exits.

Both threads share all guest memory, effective PMP permissions, heap state, and console descriptors. Stack guards are best-effort debugging aids rather than isolation between threads. Concurrent reads consume input in request order; writes may interleave at bounded service boundaries. Heap changes complete in request order. Pending console copies revalidate each chunk against the current break, so shrinking a buffer out from under a pending call can cause -EFAULT after some output has already been queued. Guest code must coordinate shared data and buffer lifetimes.

Either core can advance either thread's pending requests while it owns the monitor lock. A runnable thread resumes only on its own core. Each core checks progress through its local timer, with deadlines normally at most 1 ms ahead. No cross-core wakeup interrupt is required.


Reset and memory pressure
-------------------------

    load
    load --no-reset
    map

By default, loading retains original initialized writable bytes for reset when the verified memory/PMP layout leaves at least 32 KiB of heap capacity. Otherwise reset support is automatically disabled. `load --no-reset` deliberately omits that storage for the upload. Reset policy and the stack/PMP layout remain fixed until the next upload.

The map shows requested and effective policy, snapshot payload bytes, reserved bytes including alignment padding, and heap capacity. A stack-sized snapshot boundary is preferred because it produces better PMP mappings; a compact boundary is tried when needed to fit. There is no fixed text/data/rodata size limit independent of the available arena.

With reset support, reset restores initialized writable data and clears BSS, heap, stack, and transient padding. Immutable image contents remain intact. Without reset support, reset is rejected; upload again to restart the program.


Read the memory map
-------------------

A successful load prints one summary line with the image name, file size, heap capacity, stack size, and reset availability. It does not print the map automatically.

`map` prints an address-ordered table with exclusive end addresses. Each row identifies ELF memory, used heap, remaining heap capacity, the stack guard, stack, reset reservation, or monitor storage. Rows split at ELF, allocation, and PMP boundaries. Required permissions are the union of ELF requests or runtime needs; effective permissions come from the lowest-numbered matching PMP entry, identified in the last column. Effective `---` means denied access. Extra effective permissions show where internal protections were relaxed. Heap capacity is mapped even above the logical break; syscall buffer checks additionally enforce that break.

The following PMP report lists all eight configurable entries, including unused OFF entries and the system-wide denial at entry 7. It shows address ranges, access, NAPOT mode, and encoded pmpaddr values for the plan installed on guest entry. Entries can overlap: a broad RWX grant can contain earlier RX, RW, or denied exceptions. This is the guest plan rather than a readback of the temporarily disabled grants while the monitor handles the command. Reset accounting and monitor SRAM/stack measurements accompany the report.

Protections prioritize a useful stack guard, text write protection, constant-data write protection, non-executable writable memory, then denial of unused space. Boundaries may grant more access than ELF requests. A large stack adjustment can skip a guard. Do not make program correctness depend on an internal guard or permission fault.

`info` shows saved execution state and I/O statistics. Monitor memory reporting shows static SRAM extent, reserved normal-stack bytes, and observed stack high-water use since main began. That observation excludes earlier SDK startup and is not a proof of worst-case usage.


Troubleshooting
---------------

*   Load rejected: inspect architecture, static linkage, physical destinations, program-header limits, conflicting initialization, and space for the stack.
*   Unexpected fault: inspect PC/SP and effective PMP entries. An unsupported instruction, privileged CSR access, or device access also faults.
*   Syscall returned an error: check the number, descriptor, pointer permissions, allocation bounds, and negative errno.
*   Reset changed behavior: inspect the effective reset policy and arguments. Reset requires retained reset support.
*   Upload failed: read whether the previous image was preserved. Repeat the upload if replacement had started.
