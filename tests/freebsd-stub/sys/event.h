/* Minimal stand-in for FreeBSD's <sys/event.h>, matching the real
   declarations, so the kqueue backend can be compile-checked off FreeBSD. */
#ifndef _SYS_EVENT_H_STUB_
#define _SYS_EVENT_H_STUB_
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

struct kevent {
    uintptr_t ident;
    short     filter;
    unsigned short flags;
    unsigned int   fflags;
    int64_t   data;
    void     *udata;
    uint64_t  ext[4];
};

#define EVFILT_READ  (-1)
#define EVFILT_WRITE (-2)

#define EV_ADD    0x0001
#define EV_DELETE 0x0002
#define EV_ENABLE 0x0004
#define EV_DISABLE 0x0008
#define EV_EOF    0x8000
#define EV_ERROR  0x4000

#define EV_SET(kevp, a, b, c, d, e, f) do {  \
    struct kevent *__kevp = (kevp);          \
    __kevp->ident  = (a);                    \
    __kevp->filter = (b);                    \
    __kevp->flags  = (c);                    \
    __kevp->fflags = (d);                    \
    __kevp->data   = (e);                    \
    __kevp->udata  = (f);                    \
    __kevp->ext[0] = 0; __kevp->ext[1] = 0;  \
    __kevp->ext[2] = 0; __kevp->ext[3] = 0;  \
} while (0)

#ifdef __cplusplus
extern "C" {
#endif
int kqueue(void);
int kevent(int kq, const struct kevent *changelist, int nchanges,
           struct kevent *eventlist, int nevents,
           const struct timespec *timeout);
#ifdef __cplusplus
}
#endif
#endif
