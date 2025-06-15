//
// Created by zhukov on 2/17/23.
//

#include "../Include_i.h"
#include "UvTimerQueue.h"

struct UvTimerContext {
    uv_timer_t *timer;
    TimerCallbackFunc timerCallbackFn;
    UINT64 customData;
    UINT32 timerid;
};

struct UvTimerQueue {
    uv_loop_t* loop;
    struct UvTimerContext timers[DEFAULT_TIMER_QUEUE_TIMER_COUNT];
    volatile ATOMIC_BOOL shutdown;
    UINT32 maxTimerCount;
    volatile SIZE_T index;
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
    DLOGD("uvTimerQueueAddTimer %llu", handle);
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
    ctx->timer = MEMALLOC(sizeof(uv_timer_t));
    ctx->timer->data = ctx;
    UV_CHK_ERR(uv_timer_init(pTimerQueue->loop, ctx->timer), STATUS_MAX_TIMER_COUNT_REACHED, "uv_timer_init");
    ctx->timerCallbackFn = timerCallbackFn;
    ctx->customData = customData;
    ctx->timerid = idx;
    UV_CHK_ERR(uv_timer_start(ctx->timer, uvTimerCallback, startMsec, periodMsec), STATUS_MAX_TIMER_COUNT_REACHED, "uv_timer_start");
CleanUp:

    return retStatus;
}
static void on_timer_closed(uv_handle_t* handle)
{
    DLOGD("timer closed");
    MEMFREE(handle);
}

static STATUS stopUvTimer(struct UvTimerQueue *pTimerQueue, UINT32 timerId)
{
    STATUS retStatus = STATUS_SUCCESS;
    if (pTimerQueue->timers[timerId].timerCallbackFn != NULL) {
        pTimerQueue->timers[timerId].timerCallbackFn = NULL;
        if (!uv_is_closing((uv_handle_t *) pTimerQueue->timers[timerId].timer)) {
            uv_close((uv_handle_t *) pTimerQueue->timers[timerId].timer, on_timer_closed);
        } else {
            DLOGD("timer already closing");    
        }
    } else {
        DLOGD("timer already closing 2");
    }
CleanUp:
    return retStatus;
}

STATUS uvTimerQueueCancelTimer(TIMER_QUEUE_HANDLE handle, UINT32 timerId, UINT64 customData) {
    DLOGD("uvTimerQueueCancelTimer %llu %lu", handle, timerId);

    STATUS retStatus = STATUS_SUCCESS;
    struct UvTimerQueue *pTimerQueue = (PVOID) handle;
    CHK(pTimerQueue != NULL, STATUS_NULL_ARG);

    CHK(pTimerQueue != NULL, STATUS_NULL_ARG);
    CHK(timerId < pTimerQueue->maxTimerCount, STATUS_INVALID_ARG);
    CHK(pTimerQueue->timers[timerId].timerCallbackFn != NULL && pTimerQueue->timers[timerId].customData == customData,
        retStatus);
    CHK_STATUS(stopUvTimer(pTimerQueue, timerId));

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
    uv_timer_t *timer = pTimerQueue->timers[timerId].timer;
    UINT64 due = uv_timer_get_due_in(timer);
    UV_CHK_ERR(uv_timer_stop(timer), STATUS_INVALID_OPERATION, "uv_timer_stop");;
    UV_CHK_ERR(uv_timer_start(timer, uvTimerCallback, due, periodMsec), STATUS_INVALID_OPERATION, "uv_timer_start");;
CleanUp:
    return retStatus;
}

STATUS uvTimerQueueFree(PTIMER_QUEUE_HANDLE pHandle) {
    DLOGD("uvTimerQueueFree %llu", *pHandle);
    STATUS retStatus = STATUS_SUCCESS;

    CHK(pHandle != NULL, STATUS_NULL_ARG);

    CHK_STATUS(uvTimerQueueShutdown(*pHandle));

    struct UvTimerQueue *pTimerQueue = (PVOID) *pHandle;
    SAFE_MEMFREE(pTimerQueue);

    // Set the handle pointer to invalid
    *pHandle = INVALID_TIMER_QUEUE_HANDLE_VALUE;

    CleanUp:
    return retStatus;
}

STATUS uvTimerQueueShutdown(TIMER_QUEUE_HANDLE handle) {
    DLOGD("uvTimerQueueShutdown %llu", handle);
    STATUS retStatus = STATUS_SUCCESS;
    struct UvTimerQueue *pTimerQueue = (PVOID) handle;
    CHK(pTimerQueue != NULL, STATUS_NULL_ARG);
    BOOL shutdown = ATOMIC_EXCHANGE_BOOL(&pTimerQueue->shutdown, TRUE);
    CHK(!shutdown, retStatus);
    SIZE_T idx = pTimerQueue->index;
    for (SIZE_T i = 0; i < idx; i++) {
        stopUvTimer(pTimerQueue, i);
    }
CleanUp:
    CHK_LOG_ERR(retStatus);
    return retStatus;
}

