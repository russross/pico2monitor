Pico 2 SRAM monitor
===================

Run student RISC-V assembly programs on a Pico 2 with a short build/upload/run cycle. The monitor runs in machine mode and executes one statically linked guest with up to two user-mode threads. The sandbox always protects the monitor and system resources; protections against mistakes inside guest memory are best effort.

Programs use a small Linux-style userspace contract: physical-address static ELF32 linkage, argc/argv/envp on the initial stack, console descriptors, Linux syscall numbers, and a brk heap. A special guest linker script is not required. See [Preparing and running executables](EXECUTABLES.md) for the complete contract and build commands.


Build and use
-------------

1. On Linux x86-64, install GNU Make, a C compiler, Python 3.13 or later, `patch`, `uv`, and Debian/Ubuntu `apt-get`, `dpkg-deb`, and the `libclang-cpp19` and `libllvm19` runtime packages for formatting checks. Run `make`; `make deps` downloads and verifies the pinned SDK, compiler, CMake, picotool, and emulator header into `deps/`. The first run needs network access.
2. Flash explicitly with `make flash` (which uses sudo for picotool).
3. Run `./dist/pico build/assembly.elf` to connect, upload, and keep the console open. The monitor prints a one-line load summary; type `map` to inspect memory and protections.
4. Type `run`. Ctrl-C/Escape suspends; `resume` continues; `reset` prepares another run. Ctrl-D at the monitor, Ctrl-], or `quit` detaches the host console; Ctrl-D in a guest sends terminal EOF.

Both channels use the same USB cable. `pico` connects without uploading. To separate guest I/O, type `load` in that monitor and run `pico --send FILE` in another terminal; the sender remains attached as the guest terminal. `pico --send FILE --detach` uploads and exits. `pico --attach` attaches guest I/O without uploading. Uploading and attaching never run the guest automatically. Load options pass through in combined mode: `pico FILE --no-reset` issues `load --no-reset`.

In split mode the monitor remains a shell while the guest runs. `suspend` stops both threads; `io` shows routing and `io merge` merges it while stopped. Ctrl-] in the guest terminal suspends and merges automatically. A lost guest connection or expired five-second heartbeat lease also suspends and merges, reporting one status line to the monitor. Guest EOF remains Ctrl-D. Each serial interface has one cooperating `pico` owner; merge before using another process to upload on the guest interface.

The script uses only Python's standard library and Unix serial interfaces; it needs no SDK or third-party Python packages. Stable serial identities are selected automatically; `--console-port DEVICE` and `--upload-port DEVICE` override them. Console is -if00 and upload/guest is -if02. Install the existing desktop access/ModemManager rule with `make install-udev`, then reconnect. No UART adapter is required.

`build/exec.elf` demonstrates quoted arguments, empty envp, brk allocation, and reset state. Both it and `build/assembly.elf` use the toolchain's default linker script with `-Ttext-segment=0x20000000`. `build/threads.elf` demonstrates explicit stack setup and reusable thread slots. Older hello/echo/loop/register examples and protection probes retain the higher-address fixed-layout linker fixture for regression coverage; that layout is not a monitor requirement.


Memory and shell
----------------

Guest images can link at the stable SRAM base, 0x20000000. The monitor executes from flash and reserves the top 16 KiB of main SRAM for static storage. The 496 KiB below that boundary holds guest segments, heap, initial stack, and protected reset data. Scratch SRAM holds separate 2 KiB monitor and guest output rings and one shared 4 KiB monitor stack. Monitor operations wait for output to drain before executing the next command; USB and guest service continue during these fences. Monitor faults stop the peer before reusing guest RAM as a recovery stack. Linker assertions, measured stack high-water output, and compiler stack-usage reports support the budget.

Either core can enter the monitor through a stack-free atomic gate. Monitor calls advance bounded state-machine work and release the lock before resuming a guest or waiting for an interrupt. Custom syscall 1000 starts a second thread at a supplied PC with a parameter in a0 and all other registers zero. Guests own additional stack and TLS arrangements; no special load mode is needed. Ctrl-C/Escape suspends both threads before presenting the prompt.

The shell provides `load [--no-reset]`, `run [args]`, `reset`, `resume`, `suspend`, `io [merge]`, `info`, `map`, `help [COMMAND]`, `quit`, and `bootsel`.

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
