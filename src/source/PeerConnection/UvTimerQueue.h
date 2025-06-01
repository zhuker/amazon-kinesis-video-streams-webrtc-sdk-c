#ifndef UVTIMERQUEUE_H
#define UVTIMERQUEUE_H

#include "../Include_i.h"
#include <uv.h>

//extern TID defaultTid;
#define UV_ASYNC_LOG_ERR(uv_err, errorMessage, ...) \
        do {                                      \
        if (uv_err != 0) {                                                                          \
            DLOGE(errorMessage" %s %s",  ##__VA_ARGS__, uv_err_name(uv_err), uv_strerror(uv_err));  \
        } \
} while (FALSE)

#define UV_LOG_ERR(uv_err, errorMessage, ...) \
        do {                                      \
        if (uv_err != 0) {                                                                          \
            DLOGE(errorMessage" %s %s",  ##__VA_ARGS__, uv_err_name(uv_err), uv_strerror(uv_err));  \
        } \
} while (FALSE)

#define UV_ASYNC_CHK_ERR(uv_err, errRet, errorMessage, ...)                                               \
    do {                                                                                            \
        if (uv_err != 0) {                                                                          \
            retStatus = (errRet);                                                                   \
            DLOGE(errorMessage" %s %s",  ##__VA_ARGS__, uv_err_name(uv_err), uv_strerror(uv_err));  \
            goto CleanUp;                                                                           \
        }                                                                                           \
    } while (FALSE)

#define UV_CHK_ERR(uv_err, errRet, errorMessage, ...)                                               \
    do {                                                                                            \
        if (uv_err != 0) {                                                                          \
            retStatus = (errRet);                                                                   \
            DLOGE(errorMessage" %s %s",  ##__VA_ARGS__, uv_err_name(uv_err), uv_strerror(uv_err));  \
            goto CleanUp;                                                                           \
        }                                                                                           \
    } while (FALSE)

STATUS uvTimerQueueCreate(TIMER_QUEUE_HANDLE *pInt, uv_loop_t *loop);


STATUS uvTimerQueueAddTimer(TIMER_QUEUE_HANDLE handle, UINT64 start, UINT64 period, TimerCallbackFunc timerCallbackFn,
                            UINT64 customData,
                            PUINT32 pIndex);

STATUS uvTimerQueueCancelTimer(TIMER_QUEUE_HANDLE handle, UINT32 timerid, UINT64 customData);
/*
 * update timer id's period. Do nothing if timer not found.
 *
 * @param - TIMER_QUEUE_HANDLE - IN - Timer queue handle
 * @param - UINT64 - IN - custom data to match
 * @param - UINT32 - IN - Timer id to update
 * @param - UINT32 - IN - new period
 *
 * @return - STATUS code of the execution
 */
STATUS uvTimerQueueUpdateTimerPeriod(TIMER_QUEUE_HANDLE handle, UINT64 customData, UINT32 timerId, UINT64 period);

/*
 * Frees the Timer queue object
 *
 * NOTE: The call is idempotent.
 *
 * @param - PTIMER_QUEUE_HANDLE - IN/OUT/OPT - Timer queue handle to free
 *
 * @return - STATUS code of the execution
 */
STATUS uvTimerQueueFree(PTIMER_QUEUE_HANDLE);

/*
 * stop the timer. Once stopped timer can't be restarted. There will be no more timer callback invocation after
 * timerQueueShutdown returns.
 *
 * @param - TIMER_QUEUE_HANDLE - IN - Timer queue handle
 *
 * @return - STATUS code of the execution
 */
STATUS uvTimerQueueShutdown(TIMER_QUEUE_HANDLE);

#endif
