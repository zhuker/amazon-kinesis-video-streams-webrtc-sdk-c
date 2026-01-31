#ifndef KINESISVIDEOWEBRTCCLIENT_UVPLATFORM_H
#define KINESISVIDEOWEBRTCCLIENT_UVPLATFORM_H

#ifdef USE_LIBUV

// Single-threaded UV mode - mutexes are no-ops
#define MUTEX_CREATE(x) 42
#define MUTEX_LOCK(x) (void)(0)
#define MUTEX_UNLOCK(x) (void)(0)
#define MUTEX_TRYLOCK(x) TRUE
#define MUTEX_FREE(x) (void)(0)

// Crashing thread functions to catch legacy code trying to spawn threads
// In UV mode, all SDK operations must happen on the UV loop thread
static inline STATUS crashingThreadCreate(PTID pTid, PVOID (*fn)(PVOID), PVOID arg) {
    (void)pTid;
    (void)fn;
    (void)arg;
    DLOGE("THREAD_CREATE called in UV mode - this is a bug!");
    exit(44);
    return STATUS_SUCCESS;
}

static inline STATUS crashingThreadJoin(TID tid, PVOID* retval) {
    (void)tid;
    (void)retval;
    DLOGE("THREAD_JOIN called in UV mode - this is a bug!");
    exit(45);
    return STATUS_SUCCESS;
}

#define globalCreateThread crashingThreadCreate
#define globalJoinThread crashingThreadJoin

#endif // USE_LIBUV

#endif //KINESISVIDEOWEBRTCCLIENT_UVPLATFORM_H
