#ifndef DEBUG_H
#define DEBUG_H

#include <stdbool.h>

bool debug_enter(void);
void debug_leave(void);
bool debug_active(void);
bool debug_busy(void);
void debug_command(char *command);
void debug_step(void);

#endif
