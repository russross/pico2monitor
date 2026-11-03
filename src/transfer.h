#ifndef MONITOR_TRANSFER_H
#define MONITOR_TRANSFER_H
#include <stdbool.h>
bool transfer_active(void);
void transfer_arm(void);
void transfer_cancel(const char *reason);
void transfer_step(void);
#endif
