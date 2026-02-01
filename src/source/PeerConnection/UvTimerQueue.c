//
// Created by zhukov on 2/17/23.
//

#include "../Include_i.h"
#include "UvTimerQueue.h"

struct UvTimerQueue;  // Forward declaration

struct UvTimerContext {
    uv_timer_t timer;
    TimerCallbackFunc timerCallbackFn;
    UINT64 customData;
    UINT32 timerid;
    struct UvTimerQueue* pTimerQueue;  // Back-pointer for close callback
};

struct UvTimerQueue {
    uv_loop_t* loop;
    struct UvTimerContext timers[DEFAULT_TIMER_QUEUE_TIMER_COUNT];
    volatile ATOMIC_BOOL shutdown;
    volatile ATOMIC_BOOL freeRequested;  // Set when uvTimerQueueFree is called
    UINT32 maxTimerCount;
    volatile SIZE_T index;
    volatile SIZE_T pendingCloses;  // Track timers waiting for close callback
};

STATUS uvTimerQueueCreate(TIMER_QUEUE_HANDLE *pInt, uv_loop_t *loop) {
    STATUS retStatus = STATUS_SUCCESS;
    CHK(pInt != NULL && loop != NULL, STATUS_INVALID_ARG);
    struct UvTimerQueue *tq = MEMCALLOC(1, SIZEOF(struct UvTimerQueue));
    tq->loop = loop;
    tq->maxTimerCount = DEFAULT_TIMER_QUEUE_TIMER_COUNT;
    *pInt = (TIMER_QUEUE_HANDLE) (PVOID) tq;
CleanUp:
    return retStatus;
}

// Close callback to decrement pending close counter and free queue when all done
// NOTE: ATOMIC_DECREMENT returns the OLD value (before decrement), so remaining==1 means "was 1, now 0"
static void uvTimerCloseCallback(uv_handle_t* handle) {
    struct UvTimerContext *ctx = (struct UvTimerContext *) handle->data;
    if (ctx != NULL && ctx->pTimerQueue != NULL) {
        struct UvTimerQueue *pTimerQueue = ctx->pTimerQueue;
        SIZE_T remaining = ATOMIC_DECREMENT(&pTimerQueue->pendingCloses);
        BOOL freeRequested = ATOMIC_LOAD_BOOL(&pTimerQueue->freeRequested);
        // When all timers are closed (oldValue was 1, now 0) AND free was requested, free the queue memory
        if (remaining == 1 && freeRequested) {
            DLOGD("uvTimerCloseCallback: queue=%p all timers closed, freeing queue", pTimerQueue);
            MEMFREE(pTimerQueue);
        }
    }
}

void uvTimerCallback(uv_timer_t *timer) {
    struct UvTimerContext *ctx = (struct UvTimerContext *) timer->data;
    if (ctx->timerCallbackFn != NULL) {
        UINT64 currentTime = GETTIME();
        STATUS status = ctx->timerCallbackFn(ctx->timerid, currentTime, ctx->customData);
        if (status == STATUS_TIMER_QUEUE_STOP_SCHEDULING) {
            UV_LOG_ERR(uv_timer_stop(timer), "uv_timer_stop");
        } else {
            CHK_LOG(status, "in uvTimerCallback");
        }
    }
}

STATUS uvTimerQueueAddTimer(TIMER_QUEUE_HANDLE handle, UINT64 start, UINT64 period, TimerCallbackFunc timerCallbackFn,
                            UINT64 customData, PUINT32 pIndex) {
    STATUS retStatus = STATUS_SUCCESS;
    struct UvTimerQueue *pTimerQueue = (PVOID) handle;
    CHK(pTimerQueue != NULL, STATUS_NULL_ARG);
    CHK(period == TIMER_QUEUE_SINGLE_INVOCATION_PERIOD || period >= MIN_TIMER_QUEUE_PERIOD_DURATION,
        STATUS_INVALID_TIMER_PERIOD_VALUE);
    CHK(!ATOMIC_LOAD_BOOL(&pTimerQueue->shutdown), STATUS_TIMER_QUEUE_SHUTDOWN);

    UINT64 startMsec = KVS_CONVERT_TIMESCALE(start, HUNDREDS_OF_NANOS_IN_A_SECOND, 1000);
    UINT64 periodMsec = KVS_CONVERT_TIMESCALE(period, HUNDREDS_OF_NANOS_IN_A_SECOND, 1000);
    SIZE_T idx = *pIndex;
    if (idx == INVALID_TIMER_ID) {
        idx = pTimerQueue->index;
        CHK(idx < pTimerQueue->maxTimerCount, STATUS_MAX_TIMER_COUNT_REACHED);
        ATOMIC_INCREMENT(&pTimerQueue->index);
        *pIndex = idx;

    }
    struct UvTimerContext *ctx = &pTimerQueue->timers[idx];
    ctx->timer.data = ctx;
    ctx->pTimerQueue = pTimerQueue;  // Set back-pointer for close callback
    UV_CHK_ERR(uv_timer_init(pTimerQueue->loop, &ctx->timer), STATUS_MAX_TIMER_COUNT_REACHED, "uv_timer_init");
    ctx->timerCallbackFn = timerCallbackFn;
    ctx->customData = customData;
    ctx->timerid = idx;
    UV_CHK_ERR(uv_timer_start(&ctx->timer, uvTimerCallback, startMsec, periodMsec), STATUS_MAX_TIMER_COUNT_REACHED, "uv_timer_start");
CleanUp:

    return retStatus;
}

STATUS uvTimerQueueCancelTimer(TIMER_QUEUE_HANDLE handle, UINT32 timerId, UINT64 customData) {
    STATUS retStatus = STATUS_SUCCESS;
    struct UvTimerQueue *pTimerQueue = (PVOID) handle;
    CHK(pTimerQueue != NULL, STATUS_NULL_ARG);

    CHK(pTimerQueue != NULL, STATUS_NULL_ARG);
    CHK(timerId < pTimerQueue->maxTimerCount, STATUS_INVALID_ARG);
    CHK(pTimerQueue->timers[timerId].timerCallbackFn != NULL && pTimerQueue->timers[timerId].customData == customData,
        retStatus);
    pTimerQueue->timers[timerId].timerCallbackFn = NULL;
    UV_CHK_ERR(uv_timer_stop(&pTimerQueue->timers[timerId].timer), STATUS_INVALID_OPERATION, "uv_timer_stop");
    if (!uv_is_closing((uv_handle_t *) &pTimerQueue->timers[timerId].timer)) {
        // Increment pending closes BEFORE calling uv_close
        ATOMIC_INCREMENT(&pTimerQueue->pendingCloses);
        uv_close((uv_handle_t *) &pTimerQueue->timers[timerId].timer, uvTimerCloseCallback);
    }

CleanUp:
    CHK_LOG_ERR(retStatus);

    return retStatus;
}

STATUS uvTimerQueueUpdateTimerPeriod(TIMER_QUEUE_HANDLE handle, UINT64 customData, UINT32 timerId, UINT64 period) {
    STATUS retStatus = STATUS_SUCCESS;
    struct UvTimerQueue *pTimerQueue = (PVOID) handle;
    CHK(pTimerQueue != NULL, STATUS_NULL_ARG);
    CHK(timerId < pTimerQueue->maxTimerCount, STATUS_INVALID_ARG);
    CHK(pTimerQueue->timers[timerId].timerCallbackFn != NULL &&
        customData == pTimerQueue->timers[timerId].customData,
        retStatus);
    CHK(period == TIMER_QUEUE_SINGLE_INVOCATION_PERIOD || period >= MIN_TIMER_QUEUE_PERIOD_DURATION,
        STATUS_INVALID_TIMER_PERIOD_VALUE);

    UINT64 periodMsec = KVS_CONVERT_TIMESCALE(period, HUNDREDS_OF_NANOS_IN_A_SECOND, 1000);
    uv_timer_t *timer = &pTimerQueue->timers[timerId].timer;
    UINT64 due = uv_timer_get_due_in(timer);
    UV_CHK_ERR(uv_timer_stop(timer), STATUS_INVALID_OPERATION, "uv_timer_stop");;
    UV_CHK_ERR(uv_timer_start(timer, uvTimerCallback, due, periodMsec), STATUS_INVALID_OPERATION, "uv_timer_start");;
CleanUp:
    return retStatus;
}

STATUS uvTimerQueueFree(PTIMER_QUEUE_HANDLE pHandle) {
    STATUS retStatus = STATUS_SUCCESS;

    CHK(pHandle != NULL, STATUS_NULL_ARG);

    struct UvTimerQueue *pTimerQueue = (PVOID) *pHandle;
    CHK(pTimerQueue != NULL, retStatus);

    // Mark that free has been requested - close callbacks can now free the memory
    ATOMIC_STORE_BOOL(&pTimerQueue->freeRequested, TRUE);

    CHK_STATUS(uvTimerQueueShutdown(*pHandle));

    // If no timers were created (or all already closed), free immediately
    // Otherwise, the last close callback will free the memory
    if (ATOMIC_LOAD(&pTimerQueue->pendingCloses) == 0) {
        MEMFREE(pTimerQueue);
    }

    // Set the handle pointer to invalid
    *pHandle = INVALID_TIMER_QUEUE_HANDLE_VALUE;

CleanUp:
    return retStatus;
}

STATUS uvTimerQueueShutdown(TIMER_QUEUE_HANDLE handle) {
    STATUS retStatus = STATUS_SUCCESS;
    struct UvTimerQueue *pTimerQueue = (PVOID) handle;
    CHK(pTimerQueue != NULL, STATUS_NULL_ARG);
    BOOL shutdown = ATOMIC_EXCHANGE_BOOL(&pTimerQueue->shutdown, TRUE);
    CHK(!shutdown, retStatus);
    SIZE_T idx = pTimerQueue->index;
    for (SIZE_T i = 0; i < idx; i++) {
        pTimerQueue->timers[i].timerCallbackFn = NULL;
        UV_CHK_ERR(uv_timer_stop(&pTimerQueue->timers[i].timer), STATUS_INVALID_OPERATION, "uv_timer_stop");
        if (!uv_is_closing((uv_handle_t *) &pTimerQueue->timers[i].timer)) {
            // Increment pending closes BEFORE calling uv_close
            ATOMIC_INCREMENT(&pTimerQueue->pendingCloses);
            uv_close((uv_handle_t *) &pTimerQueue->timers[i].timer, uvTimerCloseCallback);
        }
    }
CleanUp:
    CHK_LOG_ERR(retStatus);
    return retStatus;
}

