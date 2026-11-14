#include "monitor.h"
#include "transfer.h"

#include <string.h>

static char line[2305];
static size_t length;
static bool prompt_pending, swallow_lf;
static bool line_overflow;
static char *args[34];
static unsigned pending_argc;
static bool run_pending;
static bool command_pending, stopped_pending, redrawing;
static size_t redraw_cursor;
static const char *notice;
static uint64_t masked_irqs;

static void detach(void) {
    static const char sequence[] = "\033]pico-console;detach\007";
    console_write(sequence, sizeof(sequence) - 1);
}

void shell_prompt(void) {
    prompt_pending = true;
    length = 0;
    line_overflow = false;
    redrawing = false;
    redraw_cursor = 0;
}
void shell_notice(const char *message) {
    notice = message;
}
void shell_stopped(void) {
    if (!guest_attached())
        console_input_clear();
    stopped_pending = true;
}
void shell_irq_masked(uint32_t number) {
    masked_irqs |= UINT64_C(1) << number;
}
bool shell_idle(void) {
    return !command_pending && !run_pending && !memory_map_busy() && !transfer_active() &&
           !program.reset_busy && !stopped_pending && !notice && !redrawing && !masked_irqs &&
           length == 0;
}
bool shell_accepting(void) {
    return !command_pending && !run_pending && !prompt_pending && !memory_map_busy() &&
           !program.reset_busy && !stopped_pending && !notice && !redrawing &&
           !guest_handoff_pending() && !masked_irqs && program.state != STOPPING;
}
void shell_disconnected(void) {
    run_pending = false;
    command_pending = false;
    guest_disconnect("monitor connection lost");
    program_suspend("console disconnected");
    transfer_cancel("USB console disconnected");
    length = 0;
}
void shell_init(void) {
    console_printf("\r\nRV32 SRAM monitor 1\r\n");
#ifdef MONITOR_EMULATOR
    console_printf("EMULATOR BUILD: guests run in M-mode WITHOUT isolation\r\n");
#else
    console_printf(
        "two harts: serialized M-mode monitor, U-mode threads; Ctrl-C/Escape suspends\r\n");
#endif
    shell_prompt();
}

static void info(void) {
    console_printf("state: %s%s\r\n", program_state_name(),
                   program.reset_busy ? " (reset in progress)" : "");
    if (program.valid) {
        console_printf("image: %s size=%lu crc=%08lx entry=%08lx\r\n", program.image.name,
                       (unsigned long)program.image.file_size, (unsigned long)program.image.crc,
                       (unsigned long)program.image.entry);
        for (uint32_t i = 0; i < program.image.segment_count; ++i) {
            const Segment *s = &program.image.segments[i];
            console_printf("  %c%c%c %08lx mem=%lu file=%lu offset=%lu\r\n",
                           s->permissions & PERM_R ? 'r' : '-', s->permissions & PERM_W ? 'w' : '-',
                           s->permissions & PERM_X ? 'x' : '-', (unsigned long)s->address,
                           (unsigned long)s->memory_size, (unsigned long)s->file_size,
                           (unsigned long)s->offset);
        }
        console_printf("stack: %08lx..%08lx RW\r\n", (unsigned long)program.image.plan.stack_base,
                       (unsigned long)program.image.plan.stack_top);
        for (unsigned i = 0; i < 2; ++i) {
            const GuestThread *thread = &program.threads[i];
            static const char *const names[] = {"unused", "runnable", "blocked"};
            if (thread->executing) {
                console_printf("thread %u: executing\r\n", i);
                continue;
            }
            console_printf("thread %u: %s pc=%08lx sp=%08lx cpu-us=%llu\r\n", i,
                           names[thread->state], (unsigned long)thread->frame.pc,
                           (unsigned long)thread->frame.x[2], (unsigned long long)thread->cpu_us);
        }
        if (program.state == EXITED)
            console_printf("exit status: %ld\r\n", (long)program.exit_status);
        if (program.state == FAULTED)
            console_printf("trap cause: %lu\r\n",
                           (unsigned long)program.threads[program.fault_thread].frame.cause);
        if (program.state == STOPPED)
            console_printf("stop reason: %s\r\n", program.stop_reason);
    }
    io_status();
}

/* Tokenization supports literal quoting and backslash escaping without shell
 * expansion. Parsing completes before any command mutates program state. */
static int tokenize(char **tokens) {
    char *read = line, *write = line;
    int count = 0;
    while (*read) {
        while (*read == ' ' || *read == '\t')
            ++read;
        if (!*read)
            break;
        if (count == 34)
            return -1;
        tokens[count++] = write;
        char quote = 0;
        while (*read && (quote || (*read != ' ' && *read != '\t'))) {
            char c = *read++;
            if (c == '\\' && quote != '\'') {
                if (!*read)
                    return -1;
                *write++ = *read++;
            } else if (c == quote)
                quote = 0;
            else if (!quote && (c == '\'' || c == '"'))
                quote = c;
            else
                *write++ = c;
        }
        if (quote)
            return -1;
        while (*read == ' ' || *read == '\t')
            ++read;
        *write++ = 0;
    }
    return count;
}

typedef struct {
    const char *name, *usage, *summary, *detail;
} CommandHelp;

static const CommandHelp commands[] = {
    {"help", "help [COMMAND]", "Show command help", "Show all commands or details for COMMAND."},
    {"load", "load [--no-reset]", "Receive an ELF program",
     "Arm the upload port; --no-reset omits reset storage for this load."},
    {"info", "info", "Show program and I/O status",
     "Show the loaded image, execution state, and I/O counters."},
    {"map", "map", "Show the memory map", "Show the guest memory map and permissions."},
    {"run", "run [ARG ...]", "Start the loaded program",
     "Start with optional arguments; automatically reset exited programs when supported. "
     "Suspended or faulted programs require explicit reset."},
    {"resume", "resume", "Resume a suspended program",
     "Continue from the saved program state and pending system call."},
    {"suspend", "suspend", "Suspend both guest threads", "Preserve guest state for resume."},
    {"io", "io [merge]", "Show or merge guest I/O",
     "Merge a separate guest terminal while suspended; pico --attach splits I/O."},
    {"reset", "reset", "Reset the loaded program",
     "Restore the program for another run; requires reset support retained at load time."},
    {"bootsel", "bootsel", "Reboot into USB boot mode",
     "Restart the board in BOOTSEL mode for firmware upload."},
    {"quit", "quit", "Detach pico-console", "Close the pico-console connection to the board."},
};

static void help(const char *name) {
    if (!name) {
        console_printf("Commands:\r\n");
        for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i)
            console_printf("  %-24s %s\r\n", commands[i].usage, commands[i].summary);
        console_printf("Use help COMMAND for details. Ctrl-C/Escape suspends a guest.\r\n");
        return;
    }
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
        if (strcmp(name, commands[i].name) == 0) {
            console_printf("Usage: %s\r\n%s\r\n", commands[i].usage, commands[i].detail);
            return;
        }
    }
    console_printf("unknown command: %s\r\n", name);
}

static void execute(void) {
    int argc = tokenize(args);
    if (argc <= 0) {
        if (argc < 0)
            console_printf("invalid quoting or too many arguments\r\n");
        shell_prompt();
        return;
    }
    const char *error = NULL;
    if (strcmp(line, "run") != 0 && strcmp(line, "load") != 0 && strcmp(line, "help") != 0 &&
        strcmp(line, "io") != 0 && argc != 1) {
        console_printf("unexpected command arguments\r\n");
        shell_prompt();
        return;
    }
    if (program.state == ACTIVE && strcmp(line, "help") && strcmp(line, "info") &&
        strcmp(line, "map") && strcmp(line, "suspend") && strcmp(line, "io") &&
        strcmp(line, "quit")) {
        console_printf("suspend the guest before this command\r\n");
        shell_prompt();
        return;
    }
    if (guest_handoff_pending() && (!strcmp(line, "run") || !strcmp(line, "resume") ||
                                    !strcmp(line, "load") || !strcmp(line, "reset"))) {
        console_printf("I/O change pending\r\n");
        shell_prompt();
        return;
    }
    if (strcmp(line, "help") == 0) {
        if (argc > 2)
            error = "usage: help [COMMAND]";
        else
            help(argc == 2 ? args[1] : NULL);
    } else if (strcmp(line, "map") == 0)
        memory_map_start();
    else if (strcmp(line, "info") == 0)
        info();
    else if (strcmp(line, "io") == 0) {
        if (argc == 1)
            console_printf("io: %s\r\n", guest_attached() ? "split" : "combined");
        else if (argc == 2 && !strcmp(args[1], "merge"))
            error = guest_merge();
        else
            error = "usage: io [merge]";
    } else if (strcmp(line, "suspend") == 0) {
        program_suspend("monitor request");
        if (program.state == STOPPING)
            return;
    } else if (strcmp(line, "load") == 0) {
        if (argc > 2 || (argc == 2 && strcmp(args[1], "--no-reset")))
            error = "usage: load [--no-reset]";
        else if (guest_attached() && !guest_transport_idle())
            error = "guest output is draining; retry load";
        else {
            transfer_arm(argc == 1);
            return;
        }
    } else if (strcmp(line, "reset") == 0) {
        if (!program.valid)
            console_printf("no loaded program\r\n");
        else if (!program.image.plan.snapshot)
            error = "reset unavailable for this load; upload again";
        else {
            program_reset();
            return;
        }
    } else if (strcmp(line, "run") == 0) {
        /* The parsed command stays in monitor storage while reset advances.
         * Suspended contexts remain intact until an explicit reset command. */
        if (program.state == EXITED && program.image.plan.snapshot) {
            program_reset();
            if (program.reset_busy) {
                pending_argc = (unsigned)argc - 1;
                run_pending = true;
                return;
            }
        }
        error = program_launch((unsigned)argc - 1, (const char *const *)(args + 1));
        if (!error) {
            if (guest_attached())
                shell_prompt();
            return;
        }
    } else if (strcmp(line, "resume") == 0) {
        if (program.state != STOPPED)
            console_printf("resume requires a suspended program\r\n");
        else {
            program_resume();
            if (guest_attached())
                shell_prompt();
            return;
        }
    } else if (strcmp(line, "quit") == 0) {
        detach();
    } else if (strcmp(line, "bootsel") == 0)
        monitor_bootsel();
    else if (length)
        console_printf("unknown command: %s\r\n", line);
    if (error)
        console_printf("%s\r\n", error);
    shell_prompt();
}

void shell_input(uint8_t byte) {
    if (byte == 4) {
        detach();
        return;
    }
    if (transfer_active()) {
        if (byte == 3 || byte == 27)
            transfer_cancel("cancelled from console");
        return;
    }
    if (!shell_accepting())
        return;
    if (byte == '\n' && swallow_lf) {
        swallow_lf = false;
        return;
    }
    swallow_lf = byte == '\r';
    if (byte == 3 || byte == 27) {
        console_printf("\r\n");
        shell_prompt();
        return;
    }
    if (byte == 8 || byte == 127) {
        if (length) {
            --length;
            console_printf("\b \b");
        }
        return;
    }
    if (byte == '\r' || byte == '\n') {
        if (line_overflow) {
            console_printf("\r\ncommand line exceeds 2304 characters\r\n");
            shell_prompt();
            return;
        }
        line[length] = 0;
        console_printf("\r\n");
        command_pending = true;
    } else if (byte >= 32 && byte < 127) {
        if (length < sizeof(line) - 1) {
            line[length++] = (char)byte;
            console_write(&byte, 1);
        } else
            line_overflow = true;
    }
}

void shell_step(void) {
    /* Shell operations begin only after their command echo and prior output drain. */
    if (!console_connected() || !console_empty())
        return;
    if (command_pending) {
        command_pending = false;
        execute();
        length = 0;
        return;
    }
    /* Reset completion requests a prompt; a queued run consumes that request
     * before starting the guest with the retained command arguments. */
    if (run_pending && !program.reset_busy) {
        run_pending = false;
        const char *error = program_launch(pending_argc, (const char *const *)(args + 1));
        if (error) {
            console_printf("%s\r\n", error);
            shell_prompt();
        } else if (guest_attached())
            shell_prompt();
        else
            prompt_pending = false;
        return;
    }
    if (memory_map_busy()) {
        memory_map_step();
        return;
    }
    if (masked_irqs) {
        unsigned number = (unsigned)__builtin_ctzll(masked_irqs);
        masked_irqs &= ~(UINT64_C(1) << number);
        console_printf("\r\nmasked unexpected IRQ %u\r\n", number);
        redrawing = false;
        prompt_pending = true;
        return;
    }
    if (notice) {
        console_printf("\r\n%s\r\n", notice);
        notice = NULL;
        redrawing = false;
        prompt_pending = true;
        return;
    }
    if (transfer_report())
        return;
    /* Stop diagnostics and routing notices share the shell's output fence. A partial
     * command remains stored and is redrawn in bounded pieces after the notice. */
    if (stopped_pending) {
        stopped_pending = false;
        redrawing = false;
        if (program.state == STOPPED && !guest_handoff_pending())
            console_printf("\r\nsuspended: %s\r\n", program.stop_reason);
        else if (program.state == FAULTED) {
            const GuestFrame *f = &program.threads[program.fault_thread].frame;
            console_printf("\r\nguest fault: cause=%lu pc=%08lx sp=%08lx thread=%u "
                           "(fault address unavailable)\r\n",
                           (unsigned long)f->cause, (unsigned long)f->pc, (unsigned long)f->x[2],
                           program.fault_thread);
        } else if (program.state == EXITED && program.exit_status)
            console_printf("\r\nexit: %ld\r\n", (long)program.exit_status);
        prompt_pending = true;
        return;
    }
    if (!program.reset_busy && !transfer_active() && program.state != STOPPING) {
        GuestNotice change = guest_take_notice();
        if (change.kind != GUEST_NOTICE_NONE) {
            redrawing = false;
            if (change.kind == GUEST_NOTICE_ATTACHED)
                console_printf("\r\nio: split; guest terminal attached\r\n");
            else
                console_printf("\r\nio: combined; guest %s; %s\r\n",
                               program.state == STOPPED ? "suspended" : program_state_name(),
                               change.reason);
            prompt_pending = true;
            return;
        }
    }
    if (redrawing) {
        size_t count = length - redraw_cursor;
        if (count > 128)
            count = 128;
        console_write(line + redraw_cursor, count);
        redraw_cursor += count;
        redrawing = redraw_cursor < length;
        return;
    }
    if (prompt_pending && !program.reset_busy && (program.state != ACTIVE || guest_attached()) &&
        !transfer_active() && program.state != STOPPING &&
        (guest_attached() || program_quiescent())) {
        console_printf("pico> ");
        prompt_pending = false;
        redraw_cursor = 0;
        redrawing = length != 0;
    }
}
