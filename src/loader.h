#ifndef MONITOR_LOADER_H
#define MONITOR_LOADER_H
#include "monitor.h"

typedef enum {
    LOAD_IDLE,
    LOAD_HEADERS,
    LOAD_STREAM,
    LOAD_DISCARD,
    LOAD_CLEAR,
    LOAD_SNAPSHOT,
    LOAD_DONE
} LoadState;
typedef struct {
    LoadState state;
    Image image;
    uint32_t cursor, work_cursor;
    uint8_t *prefix, *upload;
    const char *error;
    bool destructive;
} Loader;
extern Loader loader;
void loader_begin(uint32_t size, uint32_t crc, const char *name);
void loader_feed(uint32_t offset, const uint8_t *bytes, uint32_t count);
void loader_step(void);
void loader_end(void);
void loader_abort(const char *error);
bool loader_busy(void);
#endif
