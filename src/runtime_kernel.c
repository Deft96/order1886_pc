/* libkernel/libScePosix process services: clocks, sleeping, errno mapping,
 * pthread once/keys, signal bookkeeping and a narrow sysctl. Guest values
 * use FreeBSD numbering; host errno values never reach the guest directly. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#ifndef _WIN32
#include <sys/random.h>
#include <sys/resource.h>
#endif
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define PAGE 16384

typedef struct { int64_t sec, nsec; } GuestTimespec;
typedef struct { int64_t sec, usec; } GuestTimeval;

int32_t runtime_guest_errno(int e) {
    switch (e) {
    case 0: return 0;
    case EPERM: return 1; case ENOENT: return 2; case ESRCH: return 3; case EINTR: return 4;
    case EIO: return 5; case ENXIO: return 6; case E2BIG: return 7; case ENOEXEC: return 8;
    case EBADF: return 9; case ECHILD: return 10; case EDEADLK: return 11; case ENOMEM: return 12;
    case EACCES: return 13; case EFAULT: return 14; case EBUSY: return 16; case EEXIST: return 17;
    case EXDEV: return 18; case ENODEV: return 19; case ENOTDIR: return 20; case EISDIR: return 21;
    case EINVAL: return 22; case ENFILE: return 23; case EMFILE: return 24; case ENOTTY: return 25;
    case EFBIG: return 27; case ENOSPC: return 28; case ESPIPE: return 29; case EROFS: return 30;
    case EMLINK: return 31; case EPIPE: return 32; case ERANGE: return 34; case EAGAIN: return 35;
    case ENAMETOOLONG: return 63; case ENOTEMPTY: return 66; case ETIMEDOUT: return 60;
    case ELOOP: return 62; case ENOSYS: return 78; case EOVERFLOW: return 84; case ECANCELED: return 85;
    default: return 5; /* EIO: unmapped host error */
    }
}
static int32_t fail_posix(int e) { *runtime_errno()=runtime_guest_errno(e); return -1; }

/* ---- clocks and sleeping ---- */
/* ORDER1886 diag (ORDER1886_TIMER_TRACE=1): loga as leituras de relógio com a thread e o
 * chamador guest, para achar o relógio do sistema de partículas (as bolhas ~10x lentas). */
static unsigned long timer_trace_total;
static int timer_trace_on(void) {
    static int checked = 0, on = 0;
    if (!checked) {
        checked = 1;
        const char* e = getenv("ORDER1886_TIMER_TRACE");
        on = e && e[0] == '1';
    }
    return on;
}
static void timer_trace(const char* name, uint64_t value, const void* caller) {
    if (!timer_trace_on()) return;
    // Throttle per caller: at most one line per 50 ms of the clock it reads (the value is the
    // clock itself). A 0.2 ms poller would exhaust the log before the interesting rhythms.
    struct Entry {
        uintptr_t key;
        uint64_t last;
    };
    static struct Entry table[32];
    const uintptr_t key = (uintptr_t)caller ^ (uintptr_t)(const void*)name;
    struct Entry* e = &table[key % 32];
    if (e->key != key) {
        e->key = key;
        e->last = 0;
    }
    if (value - e->last < 50000000ull) {
        return;
    }
    e->last = value;
    // Cap the LOGGED lines (after the throttle), not the calls: the poller would otherwise
    // exhaust the budget in the first seconds of the boot.
    if (++timer_trace_total > 60000) {
        return;
    }
#ifdef _WIN32
    fprintf(stderr, "TIMER %-18s tid=%p caller=%p value=%llu\n", name,
            (void*)pthread_self(), caller, (unsigned long long)value);
#else
    fprintf(stderr, "TIMER %-18s tid=%d caller=%p value=%llu\n", name, gettid(), caller,
            (unsigned long long)value);
#endif
}
static struct timespec process_start;
__attribute__((constructor)) static void remember_start(void) { clock_gettime(CLOCK_MONOTONIC,&process_start); }
/* ORDER1886: subtração com borrow correto. A forma antiga ((tv_nsec-start)/1000
 * em u64 sem borrow) estourava +584 anos a cada virada de segundo — deltas
 * gigantes que matavam sistemas transientes a cada segundo (bolhas/splashes/
 * muzzle congelando e teleportando; lifetimes com clamp viravam pose estática). */
static uint64_t ns_since_start(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    int64_t sec = (int64_t)t.tv_sec - (int64_t)process_start.tv_sec;
    int64_t nsec = (int64_t)t.tv_nsec - (int64_t)process_start.tv_nsec;
    if (nsec < 0) { sec -= 1; nsec += 1000000000; }
    if (sec < 0) { sec = 0; nsec = 0; }
    return (uint64_t)sec * 1000000000u + (uint64_t)nsec;
}
static int host_clock(uint32_t id,clockid_t *out) {
    switch (id) {
    case 0: case 9: case 10: *out=CLOCK_REALTIME; return 1;            /* REALTIME(_PRECISE/_FAST) */
    case 4: case 11: case 12: case 5: case 7: case 8: *out=CLOCK_MONOTONIC; return 1; /* MONOTONIC/UPTIME */
    case 13: *out=CLOCK_REALTIME_COARSE; return 1;                    /* SECOND */
    case 14: *out=CLOCK_THREAD_CPUTIME_ID; return 1;
    case 2: case 15: *out=CLOCK_PROCESS_CPUTIME_ID; return 1;         /* PROF/PROCTIME */
    case 16: case 17: case 18: case 19: *out=CLOCK_MONOTONIC; return 1; /* PS4 network clocks */
    default: return 0;
    }
}
static int clock_read(uint32_t id,GuestTimespec *ts) {
    clockid_t host;
    if (!ts || !host_clock(id,&host)) return EINVAL;
    struct timespec t;
    if (clock_gettime(host,&t)) return errno;
    if (id==13) t.tv_nsec=0;
    timer_trace("clock_gettime", (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec,
                __builtin_return_address(0));
    ts->sec=t.tv_sec; ts->nsec=t.tv_nsec; return 0;
}
static ABI int32_t kernel_clock_gettime(uint32_t id,GuestTimespec *ts) {
    int e=clock_read(id,ts); return e ? ERR(runtime_guest_errno(e)) : 0;
}
static ABI int32_t posix_clock_gettime(uint32_t id,GuestTimespec *ts) {
    int e=clock_read(id,ts); return e ? fail_posix(e) : 0;
}
static ABI int32_t posix_clock_getres(uint32_t id,GuestTimespec *ts) {
    clockid_t host; struct timespec t;
    if (!host_clock(id,&host)) return fail_posix(EINVAL);
    if (clock_getres(host,&t)) return fail_posix(errno);
    if (ts) { ts->sec=t.tv_sec; ts->nsec=t.tv_nsec; }
    return 0;
}
static ABI uint64_t process_time(void) {
    uint64_t v = ns_since_start() / 1000;
    timer_trace("process_time_us", v, __builtin_return_address(0));
    return v;
}
static ABI uint64_t process_time_counter(void) {
    return ns_since_start();
}
static ABI uint64_t process_time_frequency(void) { return 1000000000; }
/* The guest's TSC is the process clock in nanoseconds. The raw rdtsc differs per core on some
 * hosts (3.9 s apart on a Xeon E5-2680 v4, #42), so a thread moving cores made the game jump. */
static ABI uint64_t read_tsc(void) { uint64_t v = process_time_counter(); timer_trace("read_tsc", v, __builtin_return_address(0)); return v; }
static ABI uint64_t tsc_frequency(void) { return 1000000000; }
/* Shared with the GPU library so flip/label timestamps use the guest's clock. */
uint64_t runtime_process_time_us(void) { return process_time(); }
uint64_t runtime_process_time_counter(void) { return process_time_counter(); }
uint64_t runtime_tsc_frequency(void) { return tsc_frequency(); }
uint64_t runtime_read_tsc(void) { return read_tsc(); }
static int sleep_ns(uint64_t ns) {
#ifdef _WIN32
    compat_sleep_ns(ns);
#else
    struct timespec t={.tv_sec=(time_t)(ns/1000000000),.tv_nsec=(long)(ns%1000000000)};
    while (nanosleep(&t,&t)) if (errno!=EINTR) return errno;
#endif
    return 0;
}
static ABI int32_t kernel_usleep(uint32_t usec) { sleep_ns((uint64_t)usec*1000); return 0; }
static ABI int32_t posix_usleep(uint32_t usec) { sleep_ns((uint64_t)usec*1000); return 0; }
static ABI uint32_t posix_sleep(uint32_t seconds) { sleep_ns((uint64_t)seconds*1000000000); return 0; }
static ABI int32_t posix_nanosleep(const GuestTimespec *rq,GuestTimespec *rem) {
    if (!rq || rq->nsec<0 || rq->nsec>=1000000000 || rq->sec<0) return fail_posix(EINVAL);
    sleep_ns((uint64_t)rq->sec*1000000000+(uint64_t)rq->nsec);
    if (rem) { rem->sec=0; rem->nsec=0; }
    return 0;
}
static ABI int32_t kernel_nanosleep(const GuestTimespec *rq,GuestTimespec *rem) {
    return posix_nanosleep(rq,rem) ? ERR(*runtime_errno()) : 0;
}
typedef struct { int32_t minuteswest, dsttime; } GuestTimezone;
static ABI int32_t kernel_gettimezone(GuestTimezone *tz) {
    if (!tz) return ERR(22);
    time_t now=time(NULL);
#ifdef _WIN32
    tz->minuteswest=(int32_t)(-compat_utc_offset(now)/60); tz->dsttime=0;
#else
    struct tm local; localtime_r(&now,&local);
    tz->minuteswest=(int32_t)(-local.tm_gmtoff/60); tz->dsttime=0;
#endif
    return 0;
}
static ABI int32_t posix_gettimeofday(GuestTimeval *tv,GuestTimezone *tz) {
    struct timeval t;
    gettimeofday(&t,NULL);
    if (tv) { tv->sec=t.tv_sec; tv->usec=t.tv_usec; }
    if (tz) kernel_gettimezone(tz);
    return 0;
}
static ABI int32_t kernel_gettimeofday(GuestTimeval *tv) {
    if (!tv) return ERR(22);
    return posix_gettimeofday(tv,NULL);
}
static ABI int64_t posix_time(int64_t *out) { int64_t t=(int64_t)time(NULL); if (out) *out=t; return t; }

/* ---- process ---- */
static ABI int32_t get_pagesize(void) { return PAGE; }
static ABI int32_t get_pid(void) { return 1000; }
static ABI int32_t yield(void) { sched_yield(); return 0; }
static ABI __attribute__((noreturn)) void hard_exit(int status) {
    printf("Runtime: guest requested _exit(%d)\n",status);
    runtime_report();
    fflush(stdout);
    _exit(status);
}
static ABI __attribute__((noreturn)) void raise_exception(uint32_t code,uint64_t argument) {
    fprintf(stderr,"STOP: guest raised debug exception code=0x%x argument=0x%llx\n",code,(unsigned long long)argument);
    runtime_report();
    exit(23);
}
/* The process was started without arguments other than its own name. */
static const char *guest_argv[]={"eboot.bin",NULL};
static ABI int32_t get_argc(void) { return 1; }
static ABI const char **get_argv(void) { return guest_argv; }
/* Only the game and its bundled modules exist; nothing was loaded at run time. */
static ABI int32_t stop_unload_module(int32_t handle,uint64_t args,const void *argp,uint32_t flags,const void *opt,int32_t *result) {
    (void)args; (void)argp; (void)flags; (void)opt; (void)result;
    printf("Runtime: sceKernelStopUnloadModule(%d): no such module\n",handle);
    return ERR(3); /* ESRCH */
}
static ABI int32_t module_info_from_addr(uint64_t address,int32_t flags,void *info) {
    (void)address;
    if (flags>=3) return ERR(22);
    if (!info) return ERR(14);
    return ERR(3); /* ESRCH: the runtime keeps no module records */
}
static ABI int32_t kernel_mlock(const void *address,uint64_t size) { (void)address; (void)size; return 0; }
/* libc's C++ exception unwinder. The game does not throw (no __cxa_throw import): unwinding
 * finds no frame data, and resuming a context is a stop, never a silent success. */
static ABI int32_t elf_phdr_match_addr(void *info,uint64_t address) { (void)info; (void)address; return 0; }
static ABI int32_t is_signal_return(uint64_t pc) { (void)pc; return 0; }
static ABI void pthread_cxa_finalize(void *dso) { (void)dso; }
static ABI __attribute__((noreturn)) void unwind_resume(void) {
    fputs("STOP: guest C++ exception unwinding (sigreturn/__Ux86_64_setcontext) is not supported\n",stderr);
    runtime_report();
    exit(23);
}
static ABI int32_t print_backtrace(void) {
    fputs("Runtime: guest requested a backtrace (not available)\n",stderr);
    return 0;
}
typedef struct { uint32_t bits[4]; } GuestSigset;
static _Thread_local GuestSigset signal_mask;
static uintptr_t handlers[128];
static ABI uintptr_t guest_signal(int sig,uintptr_t handler) {
    if (sig<=0 || sig>=128) { *runtime_errno()=22; return (uintptr_t)-1; } /* SIG_ERR */
    uintptr_t old=handlers[sig]; handlers[sig]=handler;
    printf("Runtime: guest signal(%d) handler recorded (host signals are not forwarded)\n",sig);
    return old;
}
static ABI int32_t guest_sigprocmask(int how,const GuestSigset *set,GuestSigset *old) {
    if (old) *old=signal_mask;
    if (!set) return 0;
    for (int i=0;i<4;++i) {
        if (how==1) signal_mask.bits[i]|=set->bits[i];        /* SIG_BLOCK */
        else if (how==2) signal_mask.bits[i]&=~set->bits[i];  /* SIG_UNBLOCK */
        else if (how==3) signal_mask.bits[i]=set->bits[i];    /* SIG_SETMASK */
        else return fail_posix(EINVAL);
    }
    return 0;
}
static ABI int32_t guest_sigfillset(GuestSigset *set) { if (!set) return fail_posix(EINVAL); memset(set,0xff,sizeof(*set)); return 0; }
static ABI int32_t guest_sigemptyset(GuestSigset *set) { if (!set) return fail_posix(EINVAL); memset(set,0,sizeof(*set)); return 0; }
typedef struct { GuestTimeval utime, stime; int64_t rest[14]; } GuestRusage;
static ABI int32_t guest_getrusage(int who,GuestRusage *out) {
    if (!out || (who!=0 && who!=1)) return fail_posix(EINVAL);
    memset(out,0,sizeof(*out));
#ifdef _WIN32
    int64_t user=0,kernel=0;
    compat_cpu_times(who,&user,&kernel);
    out->utime=(GuestTimeval){user/1000000,user%1000000};
    out->stime=(GuestTimeval){kernel/1000000,kernel%1000000};
#else
    struct rusage r;
    getrusage(who==0 ? RUSAGE_SELF : RUSAGE_THREAD,&r);
    out->utime=(GuestTimeval){r.ru_utime.tv_sec,r.ru_utime.tv_usec};
    out->stime=(GuestTimeval){r.ru_stime.tv_sec,r.ru_stime.tv_usec};
#endif
    return 0;
}
static ABI int32_t guest_sysctl(const int32_t *name,uint32_t namelen,void *old,uint64_t *oldlen,const void *new_value,uint64_t newlen) {
    (void)newlen;
    if (!name || namelen<2 || new_value) return fail_posix(EINVAL);
    if (name[0]==1 && name[1]==37) { /* kern.arandom */
        if (!old || !oldlen) return fail_posix(EINVAL);
#ifdef _WIN32
        if (compat_random(old,(size_t)*oldlen)) return fail_posix(EIO);
#else
        if (getrandom(old,(size_t)*oldlen,0)<0) return fail_posix(errno);
#endif
        return 0;
    }
    if (name[0]==6 && (name[1]==7 || name[1]==3)) { /* hw.pagesize / hw.ncpu */
        if (!oldlen) return fail_posix(EINVAL);
        int32_t value=name[1]==7 ? PAGE : 7;
        if (old) { if (*oldlen<4) return fail_posix(ENOMEM); memcpy(old,&value,4); }
        *oldlen=4; return 0;
    }
    fprintf(stderr,"STOP: unsupported sysctl mib");
    for (uint32_t i=0;i<namelen && i<8;++i) fprintf(stderr," %d",name[i]);
    fputc('\n',stderr);
    exit(21);
}

/* ---- pthread_once and thread-specific data ---- */
static ABI int32_t thread_once(int32_t *once,void (ABI *routine)(void)) {
    if (!once || !routine) return ERR(22);
    for (;;) {
        int32_t state=__atomic_load_n(once,__ATOMIC_ACQUIRE);
        if (state==1) return 0;
        if (state==0 && __atomic_compare_exchange_n(once,&state,2,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
            routine();
            __atomic_store_n(once,1,__ATOMIC_RELEASE);
            return 0;
        }
        sched_yield();
    }
}
static ABI int32_t posix_once(int32_t *once,void (ABI *routine)(void)) { return thread_once(once,routine) ? 22 : 0; }
#define KEYS 256
typedef void (ABI *KeyDestructor)(void *);
static struct { int used; KeyDestructor destructor; } keys[KEYS];
static pthread_mutex_t key_lock=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local void *key_values[KEYS];
static ABI int32_t key_create(uint32_t *key,KeyDestructor destructor) {
    if (!key) return ERR(22);
    pthread_mutex_lock(&key_lock);
    for (uint32_t i=1;i<KEYS;++i) if (!keys[i].used) {
        keys[i].used=1; keys[i].destructor=destructor;
        pthread_mutex_unlock(&key_lock);
        *key=i; return 0;
    }
    pthread_mutex_unlock(&key_lock);
    return ERR(35);
}
static ABI int32_t key_delete(uint32_t key) {
    if (!key || key>=KEYS) return ERR(22);
    pthread_mutex_lock(&key_lock);
    int32_t r=keys[key].used ? 0 : ERR(22);
    keys[key].used=0; keys[key].destructor=NULL;
    pthread_mutex_unlock(&key_lock);
    return r;
}
static ABI int32_t key_set(uint32_t key,void *value) {
    if (!key || key>=KEYS || !keys[key].used) return ERR(22);
    key_values[key]=value;
#ifdef _WIN32
    if (getenv("ORDER1886_TRACE_IMPORTS")) fprintf(stderr,"Runtime: key_set(%u)=%p\n",key,value);
#endif
    return 0;
}
static ABI void *key_get(uint32_t key) {
    void *v=key && key<KEYS ? key_values[key] : NULL;
#ifdef _WIN32
    if (getenv("ORDER1886_TRACE_IMPORTS")) fprintf(stderr,"Runtime: key_get(%u)=%p\n",key,v);
#endif
    return v;
}
void runtime_thread_keys_cleanup(void) {
    for (int round=0;round<4;++round) {
        int any=0;
        for (uint32_t i=1;i<KEYS;++i) {
            void *value=key_values[i];
            if (!value || !keys[i].used || !keys[i].destructor) continue;
            key_values[i]=NULL; any=1;
            keys[i].destructor(value);
        }
        if (!any) break;
    }
}
static ABI int32_t posix_key_create(uint32_t *k,KeyDestructor d) { int32_t r=key_create(k,d); return r ? r&0xffff : 0; }
static ABI int32_t posix_key_delete(uint32_t k) { int32_t r=key_delete(k); return r ? r&0xffff : 0; }
static ABI int32_t posix_key_set(uint32_t k,void *v) { int32_t r=key_set(k,v); return r ? r&0xffff : 0; }

/* ---- pthread scheduling/IO multiplexing (Order 1886 posix imports) ---- */
static ABI int32_t sched_getparam(int32_t thread,int32_t *policy,int32_t *param) {
    (void)thread;
    if (!policy || !param) return ERR(22);
    *policy=2; *param=0; return 0; /* SCHED_OTHER, priority 0: no host scheduler */
}
static ABI int32_t sched_setparam(int32_t thread,int32_t policy,const int32_t *param) {
    (void)thread; (void)policy; (void)param; return 0;
}
/* FreeBSD fd_set: 1024 bits. Regular files are always ready; sockets never
 * connect offline, so ready bits are counted without blocking on NULL timeout. */
static ABI int32_t posix_select(int nfds,uint64_t *r, uint64_t *w, uint64_t *e,GuestTimeval *timeout) {
    if (nfds<0 || nfds>1024) return ERR(22);
    int ready=0;
    for (int fd=0;fd<nfds;++fd) {
        int bit=(r && (r[fd>>6]>>(fd&63)&1)) || (w && (w[fd>>6]>>(fd&63)&1)) ||
                (e && (e[fd>>6]>>(fd&63)&1));
        if (bit) ++ready;
    }
    if (timeout && (timeout->sec>0 || timeout->usec>0)) {
        struct timespec t={.tv_sec=(time_t)timeout->sec,.tv_nsec=(long)(timeout->usec*1000)};
        nanosleep(&t,NULL);
    }
    return ready;
}

/* ---- EventFlag: pattern-bit thread sync (Order 1886) ---- */
#define EVF_SINGLE 0x10
#define EVF_WAIT_AND 0x01
#define EVF_WAIT_OR 0x02
#define EVF_WAIT_CLEAR_ALL 0x10
#define EVF_WAIT_CLEAR_PAT 0x20
typedef struct EvWaiter {
    pthread_cond_t event; uint64_t bits; uint32_t mode;
    int32_t result; int done; struct EvWaiter *next;
} EvWaiter;
typedef struct { int used, deleted; uint64_t bits; uint32_t attr; EvWaiter *first; } EventFlag;
#define EVFLAGS 64
static EventFlag evflags[EVFLAGS];
static pthread_mutex_t evf_lock=PTHREAD_MUTEX_INITIALIZER;
static void evf_check(pthread_condattr_t *a) { int e=pthread_condattr_init(a); if (e) { fprintf(stderr,"pthread: %d\n",e); exit(1); } }
static uint64_t evf_now_ns(void) {
    struct timespec t; clock_gettime(CLOCK_REALTIME,&t);
    return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec;
}
static int evf_met(const EventFlag *f,const EvWaiter *w) {
    if (w->mode&EVF_WAIT_AND) return (f->bits&w->bits)==w->bits;
    return (f->bits&w->bits)!=0;
}
static void evf_apply_clear(EventFlag *f,const EvWaiter *w) {
    if (w->mode&EVF_WAIT_CLEAR_ALL) f->bits=0;
    else if (w->mode&EVF_WAIT_CLEAR_PAT) f->bits&=~w->bits;
}
static void evf_wake_ready(EventFlag *f,int single) {
    EvWaiter **p=&f->first;
    while (*p) {
        EvWaiter *w=*p;
        if (w->done || !evf_met(f,w)) { p=&w->next; continue; }
        evf_apply_clear(f,w);
        w->result=0; w->done=1;
        *p=w->next;
        pthread_cond_signal(&w->event);
        if (single) return;
    }
}
static ABI int32_t evf_create(uint32_t *id,const char *name,uint32_t attr,uint64_t bits,const void *opt) {
    (void)opt;
    if (getenv("ORDER1886_TRACE_IMPORTS")) fprintf(stderr,"Runtime: evf_create(name=%s,attr=0x%x,bits=0x%llx)\n",
        name ? name : "(null)",attr,(unsigned long long)bits);
    if (!id) return ERR(22);
    if (attr&~(uint32_t)(0x01|0x02|EVF_SINGLE|0x20)) return ERR(22);
    pthread_mutex_lock(&evf_lock);
    for (uint32_t i=1;i<EVFLAGS;++i) if (!evflags[i].used) {
        evflags[i].used=1; evflags[i].deleted=0;
        evflags[i].bits=bits; evflags[i].attr=attr; evflags[i].first=NULL;
        pthread_mutex_unlock(&evf_lock);
        *id=i;
        fprintf(stderr,"Runtime: sceKernelCreateEventFlag -> id=%u\n",i);
        return 0;
    }
    pthread_mutex_unlock(&evf_lock);
    return ERR(35);
}
static ABI int32_t evf_delete(uint32_t id) {
    pthread_mutex_lock(&evf_lock);
    if (!id || id>=EVFLAGS || !evflags[id].used) { pthread_mutex_unlock(&evf_lock); return ERR(3); }
    EventFlag *f=&evflags[id];
    f->deleted=1;
    for (EvWaiter *w=f->first;w;w=w->next) { w->result=ERR(13); w->done=1; pthread_cond_signal(&w->event); }
    pthread_mutex_unlock(&evf_lock);
    return 0; /* slots are never freed: woken waiters may still hold the lock */
}
static int32_t evf_wait_on(uint32_t id,uint64_t bits,uint32_t mode,uint64_t *result,uint32_t *timeout,int block) {
    pthread_mutex_lock(&evf_lock);
    if (!id || id>=EVFLAGS || !evflags[id].used || evflags[id].deleted) {
        pthread_mutex_unlock(&evf_lock); return ERR(3);
    }
    EventFlag *f=&evflags[id];
    int single=(f->attr&EVF_SINGLE)!=0;
    EvWaiter w;
    memset(&w,0,sizeof(w)); w.bits=bits; w.mode=mode;
    if (evf_met(f,&w)) {
        evf_apply_clear(f,&w);
        if (result) *result=f->bits;
        pthread_mutex_unlock(&evf_lock); return 0;
    }
    if (!block) { pthread_mutex_unlock(&evf_lock); return ERR(16); }
    if (single && f->first) { pthread_mutex_unlock(&evf_lock); return ERR(1); }
    if (timeout && !*timeout) { pthread_mutex_unlock(&evf_lock); return ERR(60); }
    pthread_condattr_t attr; evf_check(&attr);
    int e=pthread_condattr_setclock(&attr,CLOCK_REALTIME); if (e) { fprintf(stderr,"pthread: %d\n",e); exit(1); }
    e=pthread_cond_init(&w.event,&attr); if (e) { fprintf(stderr,"pthread: %d\n",e); exit(1); }
    pthread_condattr_destroy(&attr);
    EvWaiter **tail=&f->first;
    while (*tail) tail=&(*tail)->next;
    *tail=&w;
    uint64_t deadline=timeout ? evf_now_ns()+(uint64_t)*timeout*1000 : 0;
    struct timespec end={.tv_sec=(time_t)(deadline/1000000000),.tv_nsec=(long)(deadline%1000000000)};
    while (!w.done) {
        e=timeout ? pthread_cond_timedwait(&w.event,&evf_lock,&end) : pthread_cond_wait(&w.event,&evf_lock);
        if (e==ETIMEDOUT && !w.done) {
            EvWaiter **p=&f->first;
            while (*p!=&w) p=&(*p)->next;
            *p=w.next; w.result=ERR(60); w.done=1;
        } else if (e!=ETIMEDOUT && e!=0) { fprintf(stderr,"pthread: %d\n",e); exit(1); }
    }
    if (result && !w.result) *result=f->bits;
    int32_t r=w.result;
    if (timeout) {
        uint64_t now=evf_now_ns();
        *timeout=r ? 0 : (uint32_t)(now>=deadline ? 0 : (deadline-now)/1000);
    }
    pthread_cond_destroy(&w.event);
    pthread_mutex_unlock(&evf_lock);
    return r;
}
static ABI int32_t evf_wait(uint32_t id,uint64_t bits,uint32_t mode,uint64_t *result,uint32_t *timeout) {
    if (!id) {
        /* Order1886 diag: who waits on an uninitialized flag? Rate-limited. */
        static int logged;
        if (logged < 20) {
            ++logged;
            fprintf(stderr,"Runtime: evf_wait(id=0) caller=%p bits=0x%llx mode=0x%x #%d\n",
                    __builtin_return_address(0),(unsigned long long)bits,mode,logged);
        }
    }
    if (getenv("ORDER1886_TRACE_IMPORTS")) fprintf(stderr,"Runtime: evf_wait(id=%u)\n",id);
    return evf_wait_on(id,bits,mode,result,timeout,1);
}
static ABI int32_t evf_poll(uint32_t id,uint64_t bits,uint32_t mode,uint64_t *result) {
    return evf_wait_on(id,bits,mode,result,NULL,0);
}
static ABI int32_t evf_set(uint32_t id,uint64_t pattern) {
    { /* Order1886 diag (temporário): quem sinaliza? */
        static int logged;
        if (logged < 20) {
            ++logged;
            fprintf(stderr,"Runtime: evf_set(id=%u,pat=0x%llx) caller=%p #%d\n",
                    id,(unsigned long long)pattern,__builtin_return_address(0),logged);
        }
    }
    pthread_mutex_lock(&evf_lock);
    if (!id || id>=EVFLAGS || !evflags[id].used || evflags[id].deleted) {
        pthread_mutex_unlock(&evf_lock); return ERR(3);
    }
    EventFlag *f=&evflags[id];
    f->bits|=pattern;
    evf_wake_ready(f,(f->attr&EVF_SINGLE)!=0);
    pthread_mutex_unlock(&evf_lock);
    return 0;
}
static ABI int32_t evf_clear(uint32_t id,uint64_t pattern) {
    pthread_mutex_lock(&evf_lock);
    if (!id || id>=EVFLAGS || !evflags[id].used || evflags[id].deleted) {
        pthread_mutex_unlock(&evf_lock); return ERR(3);
    }
    evflags[id].bits&=~pattern;
    pthread_mutex_unlock(&evf_lock);
    return 0;
}

/* ---- file mode/time/listing passthroughs (Order 1886) ---- */
static ABI int32_t kern_chmod(const char *path,int32_t mode) { (void)path; (void)mode; return 0; }
static ABI int32_t kern_utimes(const char *path,const GuestTimeval *times) {
    (void)path; (void)times; return 0;
}
static ABI int32_t kern_futimes(int fd,const GuestTimeval *times) { (void)fd; (void)times; return 0; }
static ABI int64_t kern_getdents(int fd,char *buffer,uint64_t size) {
    int64_t base=0,r=runtime_file_getdents(fd,buffer,size,&base);
    fprintf(stderr,"Runtime: sceKernelGetdents(fd=%d,size=%llu) -> %lld\n",fd,(unsigned long long)size,(long long)r);
    if (r<0) { *runtime_errno()=runtime_guest_errno((int)-r); return -1; }
    return r;
}

static const RuntimeExport exports[]={
    {"sceKernelClockGettime",kernel_clock_gettime}, {"clock_gettime",posix_clock_gettime},
    {"clock_getres",posix_clock_getres},
    {"sceKernelGetProcessTime",process_time}, {"sceKernelGetProcessTimeCounter",process_time_counter},
    {"sceKernelGetProcessTimeCounterFrequency",process_time_frequency},
    {"sceKernelReadTsc",read_tsc}, {"sceKernelGetTscFrequency",tsc_frequency},
    {"sceKernelUsleep",kernel_usleep}, {"usleep",posix_usleep}, {"sleep",posix_sleep},
    {"nanosleep",posix_nanosleep}, {"sceKernelNanosleep",kernel_nanosleep},
    {"sceKernelGettimezone",kernel_gettimezone}, {"gettimeofday",posix_gettimeofday},
    {"sceKernelGettimeofday",kernel_gettimeofday},
    {"time",posix_time},
    {"getpagesize",get_pagesize}, {"getpid",get_pid}, {"sched_yield",yield},
    {"_exit",hard_exit},
    {"sceKernelDebugRaiseException",raise_exception},
    {"sceKernelDebugRaiseExceptionOnReleaseMode",raise_exception},
    {"sceKernelPrintBacktraceWithModuleInfo",print_backtrace},
    {"getargc",get_argc}, {"getargv",get_argv}, {"sceKernelStopUnloadModule",stop_unload_module},
    {"sceKernelGetModuleInfoFromAddr",module_info_from_addr}, {"sceKernelMlock",kernel_mlock},
    {"__elf_phdr_match_addr",elf_phdr_match_addr}, {"_is_signal_return",is_signal_return},
    {"__pthread_cxa_finalize",pthread_cxa_finalize}, {"sigreturn",unwind_resume},
    {"__Ux86_64_setcontext",unwind_resume},
    {"signal",guest_signal}, {"sigprocmask",guest_sigprocmask}, {"_sigprocmask",guest_sigprocmask},
    {"sigfillset",guest_sigfillset}, {"sigemptyset",guest_sigemptyset},
    {"getrusage",guest_getrusage}, {"sysctl",guest_sysctl},
    {"scePthreadOnce",thread_once}, {"pthread_once",posix_once},
    {"scePthreadKeyCreate",key_create}, {"scePthreadKeyDelete",key_delete},
    {"scePthreadSetspecific",key_set}, {"scePthreadGetspecific",key_get},
    {"pthread_key_create",posix_key_create}, {"pthread_key_delete",posix_key_delete},
    {"pthread_setspecific",posix_key_set}, {"pthread_getspecific",key_get},
    {"pthread_getschedparam",sched_getparam}, {"pthread_setschedparam",sched_setparam},
    {"select",posix_select},
    {"sceKernelCreateEventFlag",evf_create}, {"sceKernelDeleteEventFlag",evf_delete},
    {"sceKernelWaitEventFlag",evf_wait}, {"sceKernelPollEventFlag",evf_poll},
    {"sceKernelSetEventFlag",evf_set}, {"sceKernelClearEventFlag",evf_clear},
    {"sceKernelChmod",kern_chmod}, {"sceKernelUtimes",kern_utimes}, {"sceKernelFutimes",kern_futimes},
    {"sceKernelGetdents",kern_getdents},
};
uintptr_t runtime_kernel_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
