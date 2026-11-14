Pico 2 monitor quickstart
========================

Run RISC-V assembly or freestanding C programs on a Raspberry Pi Pico 2 over USB. Extract the release tarball to get `monitor.uf2` (firmware), `pico` (upload and console script), and this README. The three files are also available as individual downloads.

The [pico2monitor repository](https://github.com/russross/pico2monitor) has full documentation, examples, source, and build instructions.


Install the firmware
--------------------

Install [picotool](https://github.com/raspberrypi/picotool): on Debian/Ubuntu, run `sudo apt install picotool`; on macOS, run `brew install picotool`.

1. Hold BOOTSEL while connecting the Pico 2 to USB, then release the button.
2. From the extracted release directory, run:

        picotool load -v monitor.uf2
        picotool reboot

On Linux, prefix these commands with `sudo` if USB permissions require it. Firmware remains in flash; uploaded guest programs disappear when the board reboots or loses power.


Prepare a program
-----------------

Install [risclet](https://github.com/russross/risclet), then run this in a directory containing your `*.s` assembly sources:

    risclet assemble -t 0x20000000

This produces a suitable `a.out`. Use RV32 instructions and the syscall interface below. End the program with `exit` rather than returning from its entry point.

Other assemblers and compilers must produce a statically linked, little-endian RISC-V ELF32 executable (ET_EXEC, soft-float ILP32), normally linked at SRAM address `0x20000000`. RV32IMAC instructions are supported; dynamic linking, floating-point ABI, privileged instructions, and direct device access are unsupported. Segments, heap, the initial 16 KiB stack, and optional reset storage share 496 KiB. The monitor supplies the initial stack, arguments, and an empty environment. General Linux programs usually need services this monitor does not provide. See [Preparing and running executables](https://github.com/russross/pico2monitor/blob/main/EXECUTABLES.md) for linker commands and the full contract.


Upload and run
--------------

The `pico` script needs Python 3.13 or later on Linux or macOS, with no third-party Python packages. If downloaded individually, first run `chmod +x pico`.

    ./pico a.out

On Linux, the script selects the USB serial interfaces automatically. It suspends any running guest, uploads, and leaves the console open. Uploading does not start execution. At the monitor prompt, type `run`, optionally with arguments:

    run one "two words"

`./pico` connects without uploading. Use `./pico --console-port DEVICE --upload-port DEVICE a.out` for explicit device selection, required on macOS and useful with multiple boards. On macOS, select the board's two `/dev/cu.usbmodem*` devices. On Linux, stable names end in `-if00` (console) and `-if02` (upload). Both must belong to the same board and be accessible to your user; the repository includes a [udev rule](https://github.com/russross/pico2monitor/blob/main/60-pico-monitor.rules). `./pico --help` lists options.

For a separate upload, type `load` in an open console and run `./pico --send a.out` in another terminal. A failed transfer after replacement begins requires another upload.


Guest lifecycle and shell
-------------------------

*   Ctrl-C or Escape suspends both guest threads, including infinite loops, and returns to the prompt. `resume` continues saved execution. Guest `ebreak` instructions also suspend; resuming continues after the breakpoint.
*   `reset` restores retained writable data, clears transient memory, and prepares a fresh run; then type `run`. Suspended or faulted programs require an explicit reset before restarting. A fault cannot be resumed.
*   After normal exit, `run` resets and restarts automatically when reset support is available. The load summary reports availability. If reset is unavailable, upload again to restart. `load --no-reset` deliberately disables retention for the next upload.
*   `info` shows saved state; `map` shows memory layout and effective protection; `help` or `help COMMAND` explains commands. Internal guest memory protection is best effort; monitor and device resources remain isolated.
*   Ctrl-] detaches the host console. At the monitor prompt, Ctrl-D or `quit` also detaches. Detaching preserves board state. During guest input, Enter submits a line; Ctrl-D submits pending characters or returns EOF on an empty line. `bootsel` reboots into firmware installation mode and discards the guest.


Syscall primer
--------------

Put the syscall number in `a7`, arguments in `a0` through `a5`, and execute `ecall`. Results appear in `a0`; ordinary errors are negative errno values. Other integer registers are preserved.

| Number | Call                   | Arguments / behavior                                            |
| ------ | ---------------------- | --------------------------------------------------------------- |
| 63     | read                   | `a0=0`, `a1=buffer`, `a2=count`; line-buffered console input       |
| 64     | write                  | `a0=1` or `2`, `a1=buffer`, `a2=count`; console output             |
| 93     | exit                   | `a0=status`; ends this thread; last thread ends the program       |
| 94     | exit_group             | `a0=status`; ends both threads                                   |
| 124    | sched_yield            | No arguments                                                    |
| 214    | brk                    | `a0=requested end`, zero queries; returns resulting break        |
| 403    | clock_gettime64        | `a0=1` (monotonic), `a1=timespec`                                 |
| 407    | clock_nanosleep_time64 | `a0=1`, `a1=0`, `a2=request timespec`, `a3` ignored; relative sleep |
| 1000   | thread_create          | `a0=entry PC`, `a1=parameter`; returns the other thread slot       |

Timespec contains signed 64-bit seconds and nanoseconds. `brk` failure returns the unchanged break. Unsupported calls return `-ENOSYS`. A new thread receives its parameter in `a0` and all other integer registers zero: guest code must establish its stack and any global/thread pointers. At most two threads run, sharing memory and console. See the [syscall and thread guide](https://github.com/russross/pico2monitor/blob/main/EXECUTABLES.md#syscalls) for details.
