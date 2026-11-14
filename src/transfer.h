#ifndef MONITOR_TRANSFER_H
#define MONITOR_TRANSFER_H
#include <stdbool.h>
#include <stdint.h>
typedef enum { GUEST_NOTICE_NONE, GUEST_NOTICE_ATTACHED, GUEST_NOTICE_MERGED } GuestNoticeKind;
typedef struct {
    GuestNoticeKind kind;
    const char *reason;
} GuestNotice;
bool transfer_active(void);
void transfer_arm(bool reset);
void transfer_cancel(const char *reason);
void transfer_step(void);
bool transfer_report(void);
bool guest_attached(void);
bool guest_transport_idle(void);
bool guest_handoff_pending(void);
const char *guest_merge(void);
void guest_disconnect(const char *reason);
GuestNotice guest_take_notice(void);
#endif
