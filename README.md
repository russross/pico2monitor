Pico 2 SRAM monitor
===================

Run student RISC-V assembly programs on a Pico 2 with a short build/upload/run cycle. The monitor runs in machine mode and executes one statically linked guest in user mode. The sandbox always protects the monitor and system resources; protections against mistakes inside guest memory are best effort.

Programs use a small Linux-style userspace contract: physical-address static ELF32 linkage, argc/argv/envp on the initial stack, console descriptors, Linux syscall numbers, and a brk heap. A special guest linker script is not required. See [Preparing and running executables](EXECUTABLES.md) for the complete contract and build commands.


Build and use
-------------

1. On Linux x86-64, install GNU Make, a C compiler, Python 3.13 or later, `patch`, `uv`, and Debian/Ubuntu `apt-get`, `dpkg-deb`, and the `libclang-cpp19` and `libllvm19` runtime packages for formatting checks. Run `make`; `make deps` downloads and verifies the pinned SDK, compiler, CMake, picotool, and emulator header into `deps/`. The first run needs network access.
2. Flash explicitly with `make flash` (which uses sudo for picotool).
3. Connect with `./dist/pico-console` and type `load`.
4. In another terminal, run `./dist/pico-send build/assembly.elf`. The monitor prints the loaded image's memory map.
5. Type `run`. Ctrl-C/Escape suspends; `resume` continues; `reset` prepares another run. Ctrl-D at the monitor, Ctrl-], or `quit` detaches the host console; Ctrl-D in a guest sends terminal EOF.

Both channels use the same USB cable. Utilities prefer stable serial identities; `--port DEVICE` selects a particular board. Console is -if00 and upload is -if02. Install the existing desktop access/ModemManager rule with `make install-udev`, then reconnect. No UART adapter is required.

`build/exec.elf` demonstrates quoted arguments, empty envp, brk allocation, and reset state. Both it and `build/assembly.elf` use the toolchain's default linker script with `-Ttext-segment=0x20020000`. Older hello/echo/loop/register examples and protection probes retain the fixed-layout linker fixture for regression coverage; that layout is not a monitor requirement.


Memory and shell
----------------

Flash firmware reserves 96 KiB of low SRAM for monitor storage and an 8 KiB normal stack. SRAM-executing firmware reserves 128 KiB. Guest load segments, heap, stack, and optional protected reset data share the remainder of main SRAM through 0x20080000. Final scratch banks remain trusted. Linker assertions, measured stack high-water output, and compiler stack-usage reports support the budget.

The shell provides `load [--no-reset]`, `run [args]`, `reset`, `resume`, `info`, `map`, `help [COMMAND]`, `quit`, and `bootsel`.

Loading retains initialized writable data for reset when the verified layout leaves at least 32 KiB of heap capacity. Otherwise reset support is automatically disabled. `load --no-reset` omits reset storage for that upload. Reset requires retained support; its memory/PMP layout stays fixed until reload. Arguments remain supported; the environment is always empty.

PMP entries adapt to each program. Required permissions take precedence over optional guards and write/execute restrictions. `map` reports the effective first-match entries, heap/break, stack, and snapshot cost. Guest code cannot access peripherals, ROM, monitor memory, flash, or protected snapshot storage. This is a syscall environment, not Pico SDK firmware hosting.

Arming load and accepting BEGIN preserve the previous program. The first accepted nonempty DATA block invalidates it; subsequent failure leaves no runnable image. Power loss or monitor reboot loses the SRAM guest. `bootsel` returns to the ROM programmer; watchdog expiry supplies monitor recovery.


Checks and project notes
------------------------

`make check` runs formatting, lint/type checks, native sanitizers, host serial tests, and emulator integration. `make emulator` exposes two PTYs usable with the same host utilities. The emulator banner explicitly states that it runs without U-mode/PMP isolation; real-hardware tests establish that boundary.

`PYTHONPATH=src:tests python3 tests/test_hardware.py --console DEVICE --upload DEVICE` checks the already running protected firmware, including default-linker executables, launch parameters, heap allocation, snapshot lifecycle, preemption, and protection probes. It leaves hello.elf READY. `make flash` always selects the protected image rather than monitor_emu.

The emulator's generated upstream header retains existing warnings. Dependency setup applies the checked ORC.B fix; reduced compiler features avoid incomplete Zcmp emulation. No protection claims rely on emulator behavior.

[NOTES.md](NOTES.md) specifies complete operation and transaction flows. [DESIGN.md](DESIGN.md) records project aims, decision rationale, and implementation milestones. [EXECUTABLES.md](EXECUTABLES.md) is the user's guide. Incremental builds are the default; clean targets do not remove dependencies or host configuration.


Project layout
--------------

`src/` contains monitor firmware and host utilities; `tests/` contains repeatable tests, emulator, and guest fixtures. `scripts/` fetches dependencies. `build/` holds intermediate and test outputs, while `dist/` contains the firmware and host commands needed for deployment. `deps/`, `build/`, and `dist/` are disposable and ignored by Git. `make clean` removes build and distribution outputs; remove `deps/` separately to download dependencies again. Local one-off experiments and old build logs may be kept in ignored `artifacts/`; they are not required for a fresh checkout.
