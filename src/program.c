#include "monitor.h"
#include "syscall.h"

#include <string.h>

Program program;

/* Running and waiting describe the live threads, not separate lifecycle states. */
const char *program_state_name(void) {
    static const char *const names[] = {"empty",  "ready",   "active",  "stopped",
                                        "exited", "faulted", "stopping"};
    if (program.state == ACTIVE)
        return program.threads[0].state == THREAD_RUNNABLE ||
                       program.threads[1].state == THREAD_RUNNABLE
                   ? "running"
                   : "waiting";
    return names[program.state];
}

bool program_quiescent(void) {
    return !program.threads[0].executing && !program.threads[1].executing;
}

void program_forget(void) {
    if (!program_quiescent())
        monitor_panic("forgetting an executing guest");
    pmp_disable();
    memset(&program, 0, sizeof(program));
    guest_input_clear();
}

/* Only the initial thread receives the exec stack. New threads establish their
 * own stack and runtime registers in guest code. */
static void initial_frame(void) {
    memset(program.threads, 0, sizeof(program.threads));
    program.threads[0].frame.pc = program.image.entry;
    program.threads[0].frame.x[2] = program.image.plan.stack_top;
    program.current_break = program.image.plan.heap_base;
    program.exit_status = 0;
    program.stop_reason = NULL;
    program.next_order = 0;
    program.state = READY;
    guest_input_clear();
}

void program_install(const Image *image) {
    program.image = *image;
    program.valid = true;
    initial_frame();
}

void program_reset(void) {
    if (!program.valid || program.reset_busy || !program.image.plan.snapshot ||
        !program_quiescent())
        return;
    pmp_disable();
    program.reset_busy = true;
    program.reset_cursor = 0;
    for (unsigned i = 0; i < 2; ++i)
        program.threads[i].pending = (PendingCall){0};
    guest_input_clear();
}

static bool guest_access(uint32_t address, uint32_t size, uint8_t required) {
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
    uint32_t end = address + size;
    for (uint32_t a = address & ~31u; a < end; a += 32)
        if (!(memory_permissions(p, a) & required))
            return false;
    return true;
}

bool guest_range(uint32_t address, uint32_t size, bool writable) {
    return guest_access(address, size, writable ? PERM_W : PERM_R);
}

/* STOPPING closes both guest return paths before exposing stopped memory to
 * shell commands. Each core acknowledges by entering with its saved frame. */
static void finish_stop(void) {
    if (program.state != STOPPING || !program_quiescent())
        return;
    program.state = program.stop_target;
    if (program.state == STOPPED)
        shell_notice(program.stop_reason);
    else {
        if (program.state == FAULTED) {
            const GuestFrame *f = &program.threads[program.fault_thread].frame;
            console_printf("\r\nguest fault: cause=%lu pc=%08lx sp=%08lx thread=%u "
                           "(fault address unavailable)\r\n",
                           (unsigned long)f->cause, (unsigned long)f->pc, (unsigned long)f->x[2],
                           program.fault_thread);
        } else if (program.exit_status)
            console_printf("\r\nexit: %ld\r\n", (long)program.exit_status);
        shell_prompt();
    }
}

void program_suspend(const char *reason) {
    if (program.state != ACTIVE)
        return;
    program.state = STOPPING;
    program.stop_target = STOPPED;
    program.stop_reason = reason;
    guest_input_clear();
    finish_stop();
}

void program_resume(void) {
    if (program.state == STOPPED) {
        program.state = ACTIVE;
        guest_input_clear();
    }
}

static void complete_call(GuestThread *thread, int32_t result) {
    thread->frame.x[10] = (uint32_t)result;
    thread->pending = (PendingCall){0};
    thread->state = THREAD_RUNNABLE;
}

static bool first_call(const GuestThread *thread) {
    for (unsigned i = 0; i < 2; ++i) {
        const GuestThread *other = &program.threads[i];
        if (other->state == THREAD_BLOCKED && other->pending.kind == thread->pending.kind &&
            other->pending.order < thread->pending.order)
            return false;
    }
    return true;
}

/* Requests are serviced by either core. Rechecking each bounded copy also
 * accounts for a concurrent thread shrinking the shared logical heap. */
static void poll_call(GuestThread *thread) {
    PendingCall *call = &thread->pending;
    if (call->kind == CALL_READ || call->kind == CALL_WRITE) {
        uint32_t count = call->io.remaining < IO_CHUNK ? call->io.remaining : IO_CHUNK;
        if (call->kind == CALL_READ && !first_call(thread))
            return;
        if (!guest_range(call->io.address, count, call->kind == CALL_READ)) {
            complete_call(thread, -14);
            return;
        }
        if (call->kind == CALL_READ) {
            int32_t got = guest_input_read(physical(call->io.address), count);
            if (got >= 0)
                complete_call(thread, got);
        } else {
            size_t sent = guest_output_write(physical(call->io.address), count);
            call->io.address += (uint32_t)sent;
            call->io.remaining -= (uint32_t)sent;
            call->io.written += (uint32_t)sent;
            if (!call->io.remaining)
                complete_call(thread, (int32_t)call->io.written);
        }
    } else if (call->kind == CALL_SLEEP && monitor_time_us() >= call->deadline)
        complete_call(thread, 0);
    else if (call->kind == CALL_BRK && first_call(thread)) {
        uint32_t target = call->heap.target;
        if (!call->heap.started)
            call->heap.cursor = program.current_break;
        if (target <= call->heap.cursor) {
            program.current_break = target;
            complete_call(thread, (int32_t)target);
            return;
        }
        uint32_t bytes = target - call->heap.cursor;
        if (bytes > 1024)
            bytes = 1024;
        memset(physical(call->heap.cursor), 0, bytes);
        call->heap.cursor += bytes;
        call->heap.started = true;
        if (call->heap.cursor == target) {
            program.current_break = target;
            complete_call(thread, (int32_t)target);
        }
    }
}

static void block_call(GuestThread *thread, PendingCall call) {
    call.order = ++program.next_order;
    thread->pending = call;
    thread->state = THREAD_BLOCKED;
}

static void end_program(ProgramState target) {
    program.state = STOPPING;
    program.stop_target = target;
    for (unsigned i = 0; i < 2; ++i)
        program.threads[i].pending = (PendingCall){0};
    finish_stop();
}

/* Creation publishes a completely initialized vacant slot under the monitor
 * lock. No guest stack, TLS pointer, or return address is manufactured. */
static int32_t thread_create(unsigned core, uint32_t pc, uint32_t parameter) {
    unsigned slot = core ^ 1u;
    GuestThread *thread = &program.threads[slot];
    if (thread->state != THREAD_UNUSED || thread->executing)
        return -11;
    if ((pc & 1) || !guest_access(pc, 2, PERM_X))
        return -14;
    memset(thread, 0, sizeof(*thread));
    thread->frame.pc = pc;
    thread->frame.x[10] = parameter;
    thread->state = THREAD_RUNNABLE;
    return (int32_t)slot;
}

static void syscall_dispatch(unsigned core) {
    GuestThread *thread = &program.threads[core];
    GuestFrame *f = &thread->frame;
    uint32_t number = f->x[17];
    f->pc += 4;
    if (number == SYS_EXIT || number == SYS_EXIT_GROUP) {
        program.exit_status = (int32_t)f->x[10];
        thread->state = THREAD_UNUSED;
        thread->pending = (PendingCall){0};
        if (number == SYS_EXIT_GROUP || program.threads[core ^ 1u].state == THREAD_UNUSED)
            end_program(EXITED);
        return;
    }
    if (number == SYS_THREAD_CREATE) {
        complete_call(thread, thread_create(core, f->x[10], f->x[11]));
        return;
    }
    if (number == SYS_READ || number == SYS_WRITE) {
        uint32_t fd = f->x[10], address = f->x[11], count = f->x[12];
        if ((number == SYS_READ && fd != 0) || (number == SYS_WRITE && fd != 1 && fd != 2))
            complete_call(thread, -9);
        else if (count > INT32_MAX)
            complete_call(thread, -22);
        else if (!guest_range(address, count, number == SYS_READ))
            complete_call(thread, -14);
        else if (!count)
            complete_call(thread, 0);
        else {
            block_call(thread, (PendingCall){.kind = number == SYS_READ ? CALL_READ : CALL_WRITE,
                                             .io = {.address = address, .remaining = count}});
            poll_call(thread);
        }
        return;
    }

    /* Heap changes are queued in request order, so two growing requests cannot
     * clear or publish overlapping logical allocations independently. */
    if (number == SYS_YIELD)
        complete_call(thread, 0);
    else if (number == SYS_BRK) {
        const MemoryPlan *p = &program.image.plan;
        uint32_t target = f->x[10];
        if (target >= p->heap_base && target <= p->heap_limit)
            block_call(thread, (PendingCall){.kind = CALL_BRK, .heap = {.target = target}});
        else
            complete_call(thread, (int32_t)program.current_break);
    } else if (number == SYS_CLOCK_GETTIME64) {
        uint32_t clock_id = f->x[10], address = f->x[11];
        if (clock_id != 1)
            complete_call(thread, -22);
        else if (!guest_range(address, 16, true))
            complete_call(thread, -14);
        else {
            uint64_t us = monitor_time_us();
            int64_t times[2] = {(int64_t)(us / 1000000), (int64_t)(us % 1000000) * 1000};
            memcpy(physical(address), times, sizeof(times));
            complete_call(thread, 0);
        }
    } else if (number == SYS_CLOCK_NANOSLEEP64) {
        uint32_t clock_id = f->x[10], flags = f->x[11], request = f->x[12];
        if (clock_id != 1 || flags != 0)
            complete_call(thread, -22);
        else if (!guest_range(request, 16, false))
            complete_call(thread, -14);
        else {
            int64_t times[2];
            memcpy(times, physical(request), sizeof(times));
            uint64_t now = monitor_time_us();
            if (times[0] < 0 || times[1] < 0 || times[1] >= 1000000000 ||
                (uint64_t)times[0] > (UINT64_MAX - now - 1000000) / 1000000)
                complete_call(thread, -22);
            else
                block_call(thread, (PendingCall){.kind = CALL_SLEEP,
                                                 .deadline = now + (uint64_t)times[0] * 1000000 +
                                                             ((uint64_t)times[1] + 999) / 1000});
        }
    } else
        complete_call(thread, -38);
}

/* Trap frames become monitor-owned only when their core acquires the lock.
 * Syscalls arriving during suspension stay unconsumed until resume. */
void program_after_trap(unsigned core) {
    GuestThread *thread = &program.threads[core];
    thread->executing = false;
    thread->cpu_us += monitor_time_us() - thread->entered_at;
    thread->trap_pending = true;
    uint32_t cause = thread->frame.cause;
    if (cause == 3 && range_contains(GUEST_BASE, GUEST_END - GUEST_BASE, thread->frame.pc, 2)) {
        /* Breakpoints consume their instruction before closing both return
         * paths, including when the peer is already acknowledging a stop. */
        uint16_t instruction;
        memcpy(&instruction, physical(thread->frame.pc), sizeof(instruction));
        thread->frame.pc += (instruction & 3) == 3 ? 4 : 2;
        thread->trap_pending = false;
        program_suspend("ebreak");
    } else if (!(cause & UINT32_C(0x80000000)) && cause != 8
#ifdef MONITOR_EMULATOR
               && cause != 11
#endif
    ) {
        thread->trap_pending = false;
        program.fault_thread = core;
        end_program(FAULTED);
    }
    finish_stop();
}

static void reset_step(void) {
    const Image *image = &program.image;
    const MemoryPlan *p = &image->plan;
    uint32_t begin = GUEST_BASE + program.reset_cursor;
    uint32_t end = begin + 1024;
    if (end > GUEST_END)
        end = GUEST_END;
    /* Initialized overlaps win over zero-fill. Restarts restore writable
     * initializers while clearing BSS and transient SRAM. */
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
                    restore = p->snapshot_base + memory_snapshot_offset(image, i) + a - s->address;
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
        program.reset_busy = false;
        initial_frame();
        shell_prompt();
    }
}

void program_step(void) {
    finish_stop();
    if (program.reset_busy) {
        reset_step();
        return;
    }
    if (program.state != ACTIVE)
        return;
    for (unsigned i = 0; i < 2; ++i) {
        GuestThread *thread = &program.threads[i];
        bool dispatched = false;
        if (thread->trap_pending) {
            thread->trap_pending = false;
            if (!(thread->frame.cause & UINT32_C(0x80000000))) {
                syscall_dispatch(i);
                dispatched = true;
            }
        }
        if (program.state != ACTIVE)
            return;
        if (thread->state == THREAD_BLOCKED && !dispatched)
            poll_call(thread);
    }
}
