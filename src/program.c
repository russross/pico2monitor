#include "monitor.h"
#include "syscall.h"

#include <string.h>

Program program;

const char *program_state_name(ProgramState state) {
    static const char *const names[] = {"empty",   "ready",  "running", "waiting",
                                        "stopped", "exited", "faulted"};
    return names[state];
}

void program_forget(void) {
    pmp_disable();
    memset(&program, 0, sizeof(program));
    guest_input_clear();
}

static void initial_frame(void) {
    monitor_fence_i();
    memset(&program.frame, 0, sizeof(program.frame));
    program.frame.pc = program.image.entry;
    program.frame.x[2] = program.image.plan.stack_top;
    program.current_break = program.image.plan.heap_base;
    program.pending = (PendingCall){0};
    program.exit_status = 0;
    program.stop_reason = NULL;
    program.cpu_us = 0;
    program.state = READY;
    guest_input_clear();
}

void program_install(const Image *image) {
    program.image = *image;
    program.valid = true;
    initial_frame();
}

void program_reset(void) {
    if (!program.valid || program.reset_busy)
        return;
    pmp_disable();
    program.reset_busy = true;
    program.reset_cursor = 0;
    program.pending = (PendingCall){0};
    guest_input_clear();
}

bool guest_range(uint32_t address, uint32_t size, bool writable) {
    if (!program.valid)
        return false;
    if (size == 0)
        return true;
    const MemoryPlan *p = &program.image.plan;
    if (!range_contains(GUEST_BASE, p->snapshot_base - GUEST_BASE, address, size))
        return false;
    if (address < p->heap_limit && address + size > program.current_break &&
        address + size > p->heap_base)
        return false;
    uint8_t required = writable ? PERM_W : PERM_R;
    uint32_t end = address + size;
    for (uint32_t a = address & ~31u; a < end; a += 32)
        if (!(memory_permissions(p, a) & required))
            return false;
    return true;
}

void program_suspend(const char *reason) {
    if (program.state != RUNNING && program.state != WAITING)
        return;
    program.stopped_state = program.state;
    program.state = STOPPED;
    program.stop_reason = reason;
    guest_input_clear();
    shell_notice(reason);
}

static void complete_call(int32_t result) {
    program.frame.x[10] = (uint32_t)result;
    program.pending = (PendingCall){0};
    program.state = RUNNING;
}

static void poll_call(void) {
    PendingCall *call = &program.pending;
    uint32_t count = call->count < IO_CHUNK ? call->count : IO_CHUNK;
    if (call->kind == CALL_READ) {
        int32_t got = guest_input_read(physical(call->address), count);
        if (got >= 0)
            complete_call(got);
    } else if (call->kind == CALL_WRITE) {
        size_t sent = guest_output_write(physical(call->address), count);
        call->address += (uint32_t)sent;
        call->count -= (uint32_t)sent;
        call->written += (uint32_t)sent;
        if (!call->count)
            complete_call((int32_t)call->written);
    } else if (call->kind == CALL_SLEEP && monitor_time_us() >= call->deadline) {
        complete_call(0);
    }
}

static void syscall_dispatch(void) {
    GuestFrame *f = &program.frame;
    uint32_t number = f->x[17], fd = f->x[10], address = f->x[11], count = f->x[12];
    f->pc += 4;
    if (number == SYS_EXIT || number == SYS_EXIT_GROUP) {
        program.exit_status = (int32_t)fd;
        program.state = EXITED;
        if (program.exit_status != 0)
            console_printf("\r\nexit: %ld\r\n", (long)program.exit_status);
        shell_prompt();
        return;
    }
    if (number == SYS_READ || number == SYS_WRITE) {
        if ((number == SYS_READ && fd != 0) || (number == SYS_WRITE && fd != 1 && fd != 2)) {
            complete_call(-9);
        } else if (count > INT32_MAX) {
            complete_call(-22);
        } else if (!guest_range(address, count, number == SYS_READ)) {
            complete_call(-14);
        } else if (!count) {
            complete_call(0);
        } else {
            program.pending = (PendingCall){.kind = number == SYS_READ ? CALL_READ : CALL_WRITE,
                                            .address = address,
                                            .count = count};
            program.state = WAITING;
            poll_call();
        }
        return;
    }

    /* Waiting operations retain arguments as data, never as kernel stack frames. */
    if (number == SYS_YIELD)
        complete_call(0);
    else if (number == SYS_BRK) {
        const MemoryPlan *p = &program.image.plan;
        if (fd >= p->heap_base && fd <= p->heap_limit) {
            if (fd > program.current_break) {
                program.pending = (PendingCall){.kind = CALL_BRK,
                                                .address = program.current_break,
                                                .count = fd - program.current_break};
                program.state = WAITING;
                return;
            }
            program.current_break = fd;
        }
        complete_call((int32_t)program.current_break);
    } else if (number == SYS_CLOCK_GETTIME64) {
        if (fd != 1)
            complete_call(-22);
        else if (!guest_range(address, 16, true))
            complete_call(-14);
        else {
            uint64_t us = monitor_time_us();
            int64_t times[2] = {(int64_t)(us / 1000000), (int64_t)(us % 1000000) * 1000};
            memcpy(physical(address), times, sizeof(times));
            complete_call(0);
        }
    } else if (number == SYS_CLOCK_NANOSLEEP64) {
        if (fd != 1 || address != 0)
            complete_call(-22);
        else if (!guest_range(count, 16, false))
            complete_call(-14);
        else {
            int64_t times[2];
            memcpy(times, physical(count), sizeof(times));
            uint64_t now = monitor_time_us();
            if (times[0] < 0 || times[1] < 0 || times[1] >= 1000000000 ||
                (uint64_t)times[0] > (UINT64_MAX - now - 1000000) / 1000000)
                complete_call(-22);
            else {
                program.pending = (PendingCall){.kind = CALL_SLEEP,
                                                .deadline = now + (uint64_t)times[0] * 1000000 +
                                                            ((uint64_t)times[1] + 999) / 1000};
                program.state = WAITING;
            }
        }
    } else
        complete_call(-38);
}

void program_after_trap(void) {
    uint32_t cause = program.frame.cause;
    if (cause & UINT32_C(0x80000000))
        return;
    bool syscall = cause == 8;
#ifdef MONITOR_EMULATOR
    /* rp2350js has no U-mode: only this explicitly labeled test build accepts M ecalls. */
    syscall = syscall || cause == 11;
#endif
    if (syscall) {
        syscall_dispatch();
        return;
    }
    program.state = FAULTED;
    program.pending = (PendingCall){0};
    console_printf("\r\nguest fault: cause=%lu pc=%08lx sp=%08lx (fault address unavailable)\r\n",
                   (unsigned long)cause, (unsigned long)program.frame.pc,
                   (unsigned long)program.frame.x[2]);
    shell_prompt();
}

void program_step(void) {
    if (program.reset_busy) {
        const Image *image = &program.image;
        const MemoryPlan *p = &image->plan;
        if (program.reset_cursor >= GUEST_END - GUEST_BASE) {
            uint32_t cleared = program.reset_cursor - (GUEST_END - GUEST_BASE);
            uint32_t count = GUEST_END - p->snapshot_base - cleared;
            if (count > 1024)
                count = 1024;
            if (count)
                memset(physical(p->snapshot_base + cleared), 0, count);
            program.reset_cursor += count;
            if (cleared + count < GUEST_END - p->snapshot_base)
                return;
            const char *error = memory_plan_build(&program.image, SNAP_OFF);
            if (error)
                monitor_panic(error);
            program.release_snapshot = false;
            program.reset_busy = false;
            initial_frame();
            memory_map_start();
            shell_prompt();
            return;
        }
        uint32_t begin = GUEST_BASE + program.reset_cursor;
        uint32_t end = begin + 1024;
        if (end > GUEST_END)
            end = GUEST_END;
        /* Initialized overlaps win over zero-fill. An absent snapshot preserves
         * initialized bytes, while every restart clears BSS and transient SRAM. */
        for (uint32_t a = begin; a < end; ++a) {
            if (a >= p->snapshot_base)
                continue;
            bool initialized = false;
            uint32_t restore = 0;
            for (uint32_t i = 0; i < image->segment_count; ++i) {
                const Segment *s = &image->segments[i];
                if (range_contains(s->address, s->file_size, a, 1)) {
                    initialized = true;
                    if (p->snapshot && (s->permissions & PERM_W))
                        restore =
                            p->snapshot_base + memory_snapshot_offset(image, i) + a - s->address;
                }
            }
            if (restore)
                *physical(a) = *physical(restore);
            else if (!initialized) {
                bool image_memory = false, writable = false;
                for (uint32_t i = 0; i < image->segment_count; ++i) {
                    const Segment *s = &image->segments[i];
                    if (range_contains(s->address, s->memory_size, a, 1)) {
                        image_memory = true;
                        writable = writable || (s->permissions & PERM_W);
                    }
                }
                if (!image_memory || writable)
                    *physical(a) = 0;
            }
        }
        program.reset_cursor += end - begin;
        if (end == GUEST_END) {
            if (program.release_snapshot)
                return;
            program.reset_busy = false;
            initial_frame();
            shell_prompt();
        }
        return;
    }
    if (program.state == WAITING && program.pending.kind == CALL_BRK) {
        PendingCall *call = &program.pending;
        uint32_t count = call->count < 1024 ? call->count : 1024;
        memset(physical(call->address), 0, count);
        call->address += count;
        call->count -= count;
        if (!call->count) {
            program.current_break = call->address;
            complete_call((int32_t)program.current_break);
        }
    } else if (program.state == WAITING)
        poll_call();
    if (program.state != RUNNING)
        return;

    /* Every return services the host before allowing another guest instruction slice. */
    pmp_enable(&program.image.plan);
    uint64_t start = monitor_time_us();
    guest_enter(&program.frame);
    program.cpu_us += monitor_time_us() - start;
    pmp_disable();
    program_after_trap();
}
