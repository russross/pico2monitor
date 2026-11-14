#ifndef MONITOR_SYSCALL_H
#define MONITOR_SYSCALL_H

/* Supported Linux generic syscall numbers; time uses the RV32 time64 ABI. */
#define SYS_READ 63
#define SYS_WRITE 64
#define SYS_EXIT 93
#define SYS_EXIT_GROUP 94
#define SYS_YIELD 124
#define SYS_BRK 214
#define SYS_CLOCK_GETTIME64 403
#define SYS_CLOCK_NANOSLEEP64 407
/* Private teaching ABI: start PC in a0, passthrough parameter in a1. */
#define SYS_THREAD_CREATE 1000

#endif
