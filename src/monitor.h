#ifndef MONITOR_H
#define MONITOR_H

#include "memory.h"

typedef struct {
    uint32_t address, memory_size, offset, file_size, received;
    uint8_t permissions;
} Segment;
typedef struct {
    uint32_t entry, file_size, crc;
    Segment segments[MAX_SEGMENTS];
    uint32_t segment_count;
    uint32_t phoff, phnum;
    MemoryPlan plan;
    char name[64];
} Image;

/* Assembly indexes this frame by architectural register number. x0 stays zero. */
typedef struct {
    uint32_t x[32];
    uint32_t pc, cause, status;
} GuestFrame;
typedef enum { EMPTY, READY, RUNNING, WAITING, STOPPED, EXITED, FAULTED } ProgramState;
typedef enum { CALL_NONE, CALL_READ, CALL_WRITE, CALL_SLEEP, CALL_BRK } PendingKind;
typedef struct {
    PendingKind kind;
    uint32_t address, count;
    uint64_t deadline;
    uint32_t written;
} PendingCall;
typedef struct {
    Image image;
    ProgramState state;
    ProgramState stopped_state;
    GuestFrame frame;
    PendingCall pending;
    bool valid, reset_busy;
    uint32_t reset_cursor;
    uint32_t current_break;
    bool release_snapshot;
    int32_t exit_status;
    const char *stop_reason;
    uint64_t cpu_us;
} Program;
extern Program program;

/* Platform services keep parsers and program state independent of USB internals. */
uint64_t monitor_time_us(void);
void monitor_fence_i(void);
void monitor_panic(const char *message);
void monitor_bootsel(void);
void monitor_service_irqs(void);
void monitor_memory_report(void);
void monitor_trap_init(void);
void guest_enter(GuestFrame *frame);
void pmp_disable(void);
void pmp_enable(const MemoryPlan *plan);
void pmp_init(void);

void program_forget(void);
void program_install(const Image *image);
void program_reset(void);
void program_step(void);
void program_suspend(const char *reason);
void program_after_trap(void);
bool guest_range(uint32_t address, uint32_t size, bool writable);
extern SnapshotPolicy snapshot_policy;
const char *memory_plan_build(Image *image, SnapshotPolicy policy);
bool memory_plan_verify(const Image *image);
uint8_t memory_permissions(const MemoryPlan *plan, uint32_t address);
uint32_t memory_snapshot_offset(const Image *image, uint32_t index);
void memory_map_start(void);
void memory_map_step(void);
bool memory_map_busy(void);
const char *program_launch(unsigned argc, const char *const *argv);
const char *environment_set(const char *name, const char *value);
void environment_print(void);
const char *program_state_name(ProgramState state);

void io_init(void);
void io_service(void);
bool console_connected(void);
bool upload_connected(void);
size_t console_write(const void *bytes, size_t count);
size_t guest_output_write(const void *bytes, size_t count);
size_t console_space(void);
bool console_empty(void);
void console_printf(const char *format, ...) __attribute__((format(printf, 1, 2)));
int32_t guest_input_read(void *bytes, size_t count);
void guest_input_clear(void);
bool guest_input_put(uint8_t byte);
void io_status(void);
size_t upload_read(void *bytes, size_t count);
size_t upload_write(const void *bytes, size_t count);
void upload_flush_input(void);

void shell_init(void);
void shell_step(void);
void shell_input(uint8_t byte);
void shell_notice(const char *message);
void shell_prompt(void);
void shell_disconnected(void);

#endif
