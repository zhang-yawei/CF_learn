/*
 * Copyright (c) 2014 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */

/*	CFRunLoop.c
	Copyright (c) 1998-2014, Apple Inc. All rights reserved.
	Responsibility: Tony Parker
*/

#include <CoreFoundation/CFRunLoop.h>
#include <CoreFoundation/CFSet.h>
#include <CoreFoundation/CFBag.h>
#include <CoreFoundation/CFNumber.h>
#include <CoreFoundation/CFPreferences.h>
#include "CFInternal.h"
#include <math.h>
#include <stdio.h>
#include <limits.h>
#include <pthread.h>
#include <dispatch/dispatch.h>

#if DEPLOYMENT_TARGET_WINDOWS
#include <typeinfo.h>
#endif
#include <checkint.h>

#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_EMBEDDED_MINI
#include <sys/param.h>
#include <dispatch/private.h>
#include <CoreFoundation/CFUserNotification.h>
#include <mach/mach.h>
#include <mach/clock_types.h>
#include <mach/clock.h>
#include <unistd.h>
#include <dlfcn.h>
#include <pthread/private.h>
#include <os/voucher_private.h>
extern mach_port_t _dispatch_get_main_queue_port_4CF(void);
extern void _dispatch_main_queue_callback_4CF(mach_msg_header_t *msg);
#elif DEPLOYMENT_TARGET_WINDOWS
#include <process.h>
DISPATCH_EXPORT HANDLE _dispatch_get_main_queue_handle_4CF(void);
DISPATCH_EXPORT void _dispatch_main_queue_callback_4CF(void);

#define MACH_PORT_NULL 0
#define mach_port_name_t HANDLE
#define mach_port_t HANDLE
#define _dispatch_get_main_queue_port_4CF _dispatch_get_main_queue_handle_4CF
#define _dispatch_main_queue_callback_4CF(x) _dispatch_main_queue_callback_4CF()

#define AbsoluteTime LARGE_INTEGER

#endif

#if DEPLOYMENT_TARGET_WINDOWS || DEPLOYMENT_TARGET_IPHONESIMULATOR
CF_EXPORT pthread_t _CF_pthread_main_thread_np(void);
#define pthread_main_thread_np() _CF_pthread_main_thread_np()
#endif

#include <Block.h>
#include <Block_private.h>

#if DEPLOYMENT_TARGET_MACOSX
#define USE_DISPATCH_SOURCE_FOR_TIMERS 1
#define USE_MK_TIMER_TOO 1
#else
#define USE_DISPATCH_SOURCE_FOR_TIMERS 0
#define USE_MK_TIMER_TOO 1
#endif

static int _LogCFRunLoop = 0;
static void _runLoopTimerWithBlockContext(CFRunLoopTimerRef timer, void *opaqueBlock);

// for conservative arithmetic safety, such that (TIMER_DATE_LIMIT + TIMER_INTERVAL_LIMIT + kCFAbsoluteTimeIntervalSince1970) * 10^9 < 2^63
#define TIMER_DATE_LIMIT 4039289856.0
#define TIMER_INTERVAL_LIMIT 504911232.0

#define HANDLE_DISPATCH_ON_BASE_INVOCATION_ONLY 0

#define CRASH(string, errcode)               \
    do                                       \
    {                                        \
        char msg[256];                       \
        snprintf(msg, 256, string, errcode); \
        CRSetCrashLogMessage(msg);           \
        HALT;                                \
    } while (0)

#if DEPLOYMENT_TARGET_WINDOWS

static pthread_t kNilPthreadT = {nil, nil};
#define pthreadPointer(a) a.p
typedef int kern_return_t;
#define KERN_SUCCESS 0

#else

static pthread_t kNilPthreadT = (pthread_t)0;
#define pthreadPointer(a) a
#define lockCount(a) a
#endif

#pragma mark -

#define CF_RUN_LOOP_PROBES 0

#if CF_RUN_LOOP_PROBES
#include "CFRunLoopProbes.h"
#else
#define CFRUNLOOP_NEXT_TIMER_ARMED(arg0) \
    do                                   \
    {                                    \
    } while (0)
#define CFRUNLOOP_NEXT_TIMER_ARMED_ENABLED() (0)
#define CFRUNLOOP_POLL() \
    do                   \
    {                    \
    } while (0)
#define CFRUNLOOP_POLL_ENABLED() (0)
#define CFRUNLOOP_SLEEP() \
    do                    \
    {                     \
    } while (0)
#define CFRUNLOOP_SLEEP_ENABLED() (0)
#define CFRUNLOOP_SOURCE_FIRED(arg0, arg1, arg2) \
    do                                           \
    {                                            \
    } while (0)
#define CFRUNLOOP_SOURCE_FIRED_ENABLED() (0)
#define CFRUNLOOP_TIMER_CREATED(arg0, arg1, arg2, arg3, arg4, arg5, arg6) \
    do                                                                    \
    {                                                                     \
    } while (0)
#define CFRUNLOOP_TIMER_CREATED_ENABLED() (0)
#define CFRUNLOOP_TIMER_FIRED(arg0, arg1, arg2, arg3, arg4) \
    do                                                      \
    {                                                       \
    } while (0)
#define CFRUNLOOP_TIMER_FIRED_ENABLED() (0)
#define CFRUNLOOP_TIMER_RESCHEDULED(arg0, arg1, arg2, arg3, arg4, arg5) \
    do                                                                  \
    {                                                                   \
    } while (0)
#define CFRUNLOOP_TIMER_RESCHEDULED_ENABLED() (0)
#define CFRUNLOOP_WAKEUP(arg0) \
    do                         \
    {                          \
    } while (0)
#define CFRUNLOOP_WAKEUP_ENABLED() (0)
#define CFRUNLOOP_WAKEUP_FOR_DISPATCH() \
    do                                  \
    {                                   \
    } while (0)
#define CFRUNLOOP_WAKEUP_FOR_DISPATCH_ENABLED() (0)
#define CFRUNLOOP_WAKEUP_FOR_NOTHING() \
    do                                 \
    {                                  \
    } while (0)
#define CFRUNLOOP_WAKEUP_FOR_NOTHING_ENABLED() (0)
#define CFRUNLOOP_WAKEUP_FOR_SOURCE() \
    do                                \
    {                                 \
    } while (0)
#define CFRUNLOOP_WAKEUP_FOR_SOURCE_ENABLED() (0)
#define CFRUNLOOP_WAKEUP_FOR_TIMEOUT() \
    do                                 \
    {                                  \
    } while (0)
#define CFRUNLOOP_WAKEUP_FOR_TIMEOUT_ENABLED() (0)
#define CFRUNLOOP_WAKEUP_FOR_TIMER() \
    do                               \
    {                                \
    } while (0)
#define CFRUNLOOP_WAKEUP_FOR_TIMER_ENABLED() (0)
#define CFRUNLOOP_WAKEUP_FOR_WAKEUP() \
    do                                \
    {                                 \
    } while (0)
#define CFRUNLOOP_WAKEUP_FOR_WAKEUP_ENABLED() (0)
#endif

// In order to reuse most of the code across Mach and Windows v1 RunLoopSources, we define a
// simple abstraction layer spanning Mach ports and Windows HANDLES
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_EMBEDDED_MINI

// 获取进程端口数
CF_PRIVATE uint32_t __CFGetProcessPortCount(void)
{
    ipc_info_space_t info;
    ipc_info_name_array_t table = 0;
    mach_msg_type_number_t tableCount = 0;
    ipc_info_tree_name_array_t tree = 0;
    mach_msg_type_number_t treeCount = 0;

    kern_return_t ret = mach_port_space_info(mach_task_self(), &info, &table, &tableCount, &tree, &treeCount);
    if (ret != KERN_SUCCESS)
    {
        return (uint32_t)0;
    }
    if (table != NULL)
    {
        ret = vm_deallocate(mach_task_self(), (vm_address_t)table, tableCount * sizeof(*table));
    }
    if (tree != NULL)
    {
        ret = vm_deallocate(mach_task_self(), (vm_address_t)tree, treeCount * sizeof(*tree));
    }
    return (uint32_t)tableCount;
}

CF_PRIVATE CFArrayRef __CFStopAllThreads(void)
{
    CFMutableArrayRef suspended_list = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, NULL);
    mach_port_t my_task = mach_task_self();
    mach_port_t my_thread = mach_thread_self();
    thread_act_array_t thr_list = 0;
    mach_msg_type_number_t thr_cnt = 0;

    // really, should loop doing the stopping until no more threads get added to the list N times in a row
    kern_return_t ret = task_threads(my_task, &thr_list, &thr_cnt);
    if (ret == KERN_SUCCESS)
    {
        for (CFIndex idx = 0; idx < thr_cnt; idx++)
        {
            thread_act_t thread = thr_list[idx];
            if (thread == my_thread)
                continue;
            if (CFArrayContainsValue(suspended_list, CFRangeMake(0, CFArrayGetCount(suspended_list)), (const void *)(uintptr_t)thread))
                continue;
            ret = thread_suspend(thread);
            if (ret == KERN_SUCCESS)
            {
                CFArrayAppendValue(suspended_list, (const void *)(uintptr_t)thread);
            }
            else
            {
                mach_port_deallocate(my_task, thread);
            }
        }
        vm_deallocate(my_task, (vm_address_t)thr_list, sizeof(thread_t) * thr_cnt);
    }
    mach_port_deallocate(my_task, my_thread);
    return suspended_list;
}

CF_PRIVATE void __CFRestartAllThreads(CFArrayRef threads)
{
    for (CFIndex idx = 0; idx < CFArrayGetCount(threads); idx++)
    {
        thread_act_t thread = (thread_act_t)(uintptr_t)CFArrayGetValueAtIndex(threads, idx);
        kern_return_t ret = thread_resume(thread);
        if (ret != KERN_SUCCESS)
            CRASH("*** Failure from thread_resume (%d) ***", ret);
        mach_port_deallocate(mach_task_self(), thread);
    }
}

static uint32_t __CF_last_warned_port_count = 0;

static void foo() __attribute__((unused));
static void foo()
{
    uint32_t pcnt = __CFGetProcessPortCount();
    if (__CF_last_warned_port_count + 1000 < pcnt)
    {
        CFArrayRef threads = __CFStopAllThreads();

        // do stuff here
        CFOptionFlags responseFlags = 0;
        SInt32 result = CFUserNotificationDisplayAlert(0.0, kCFUserNotificationCautionAlertLevel, NULL, NULL, NULL, CFSTR("High Mach Port Usage"), CFSTR("This application is using a lot of Mach ports."), CFSTR("Default"), CFSTR("Altern"), CFSTR("Other b"), &responseFlags);
        if (0 != result)
        {
            CFLog(3, CFSTR("ERROR"));
        }
        else
        {
            switch (responseFlags)
            {
            case kCFUserNotificationDefaultResponse:
                CFLog(3, CFSTR("DefaultR"));
                break;
            case kCFUserNotificationAlternateResponse:
                CFLog(3, CFSTR("AltR"));
                break;
            case kCFUserNotificationOtherResponse:
                CFLog(3, CFSTR("OtherR"));
                break;
            case kCFUserNotificationCancelResponse:
                CFLog(3, CFSTR("CancelR"));
                break;
            }
        }

        __CFRestartAllThreads(threads);
        CFRelease(threads);
        __CF_last_warned_port_count = pcnt;
    }
}

typedef mach_port_t __CFPort;
#define CFPORT_NULL MACH_PORT_NULL
typedef mach_port_t __CFPortSet;

static void __THE_SYSTEM_HAS_NO_PORTS_AVAILABLE__(kern_return_t ret) __attribute__((noinline));
static void __THE_SYSTEM_HAS_NO_PORTS_AVAILABLE__(kern_return_t ret) { HALT; };

static __CFPort __CFPortAllocate(void)
{
    __CFPort result = CFPORT_NULL;
    kern_return_t ret = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &result);
    if (KERN_SUCCESS != ret)
    {
        char msg[256];
        snprintf(msg, 256, "*** The system has no mach ports available. You may be able to diagnose which application(s) are using ports by using 'top' or Activity Monitor. (%d) ***", ret);
        CRSetCrashLogMessage(msg);
        __THE_SYSTEM_HAS_NO_PORTS_AVAILABLE__(ret);
        return CFPORT_NULL;
    }

    ret = mach_port_insert_right(mach_task_self(), result, result, MACH_MSG_TYPE_MAKE_SEND);
    if (KERN_SUCCESS != ret)
        CRASH("*** Unable to set send right on mach port. (%d) ***", ret);

    mach_port_limits_t limits;
    limits.mpl_qlimit = 1;
    ret = mach_port_set_attributes(mach_task_self(), result, MACH_PORT_LIMITS_INFO, (mach_port_info_t)&limits, MACH_PORT_LIMITS_INFO_COUNT);
    if (KERN_SUCCESS != ret)
        CRASH("*** Unable to set attributes on mach port. (%d) ***", ret);

    return result;
}

CF_INLINE void __CFPortFree(__CFPort port)
{
    mach_port_destroy(mach_task_self(), port);
}

static void __THE_SYSTEM_HAS_NO_PORT_SETS_AVAILABLE__(kern_return_t ret) __attribute__((noinline));
static void __THE_SYSTEM_HAS_NO_PORT_SETS_AVAILABLE__(kern_return_t ret) { HALT; };

CF_INLINE __CFPortSet __CFPortSetAllocate(void)
{
    __CFPortSet result;
    kern_return_t ret = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_PORT_SET, &result);
    if (KERN_SUCCESS != ret)
    {
        __THE_SYSTEM_HAS_NO_PORT_SETS_AVAILABLE__(ret);
    }
    return (KERN_SUCCESS == ret) ? result : CFPORT_NULL;
}

CF_INLINE kern_return_t __CFPortSetInsert(__CFPort port, __CFPortSet portSet)
{
    if (MACH_PORT_NULL == port)
    {
        return -1;
    }
    return mach_port_insert_member(mach_task_self(), port, portSet);
}

CF_INLINE kern_return_t __CFPortSetRemove(__CFPort port, __CFPortSet portSet)
{
    if (MACH_PORT_NULL == port)
    {
        return -1;
    }
    return mach_port_extract_member(mach_task_self(), port, portSet);
}

CF_INLINE void __CFPortSetFree(__CFPortSet portSet)
{
    kern_return_t ret;
    mach_port_name_array_t array;
    mach_msg_type_number_t idx, number;

    ret = mach_port_get_set_status(mach_task_self(), portSet, &array, &number);
    if (KERN_SUCCESS == ret)
    {
        for (idx = 0; idx < number; idx++)
        {
            mach_port_extract_member(mach_task_self(), array[idx], portSet);
        }
        vm_deallocate(mach_task_self(), (vm_address_t)array, number * sizeof(mach_port_name_t));
    }
    mach_port_destroy(mach_task_self(), portSet);
}

#elif DEPLOYMENT_TARGET_WINDOWS

typedef HANDLE __CFPort;
#define CFPORT_NULL NULL

// A simple dynamic array of HANDLEs, which grows to a high-water mark
typedef struct ___CFPortSet
{
    uint16_t used;
    uint16_t size;
    HANDLE *handles;
    CFLock_t lock; // insert and remove must be thread safe, like the Mach calls
} * __CFPortSet;

CF_INLINE __CFPort __CFPortAllocate(void)
{
    return CreateEventA(NULL, true, false, NULL);
}

CF_INLINE void __CFPortFree(__CFPort port)
{
    CloseHandle(port);
}

static __CFPortSet __CFPortSetAllocate(void)
{
    __CFPortSet result = (__CFPortSet)CFAllocatorAllocate(kCFAllocatorSystemDefault, sizeof(struct ___CFPortSet), 0);
    result->used = 0;
    result->size = 4;
    result->handles = (HANDLE *)CFAllocatorAllocate(kCFAllocatorSystemDefault, result->size * sizeof(HANDLE), 0);
    CF_SPINLOCK_INIT_FOR_STRUCTS(result->lock);
    return result;
}

static void __CFPortSetFree(__CFPortSet portSet)
{
    CFAllocatorDeallocate(kCFAllocatorSystemDefault, portSet->handles);
    CFAllocatorDeallocate(kCFAllocatorSystemDefault, portSet);
}

// Returns portBuf if ports fit in that space, else returns another ptr that must be freed
static __CFPort *__CFPortSetGetPorts(__CFPortSet portSet, __CFPort *portBuf, uint32_t bufSize, uint32_t *portsUsed)
{
    __CFLock(&(portSet->lock));
    __CFPort *result = portBuf;
    if (bufSize < portSet->used)
        result = (__CFPort *)CFAllocatorAllocate(kCFAllocatorSystemDefault, portSet->used * sizeof(HANDLE), 0);
    if (portSet->used > 1)
    {
        // rotate the ports to vaguely simulate round-robin behaviour
        uint16_t lastPort = portSet->used - 1;
        HANDLE swapHandle = portSet->handles[0];
        memmove(portSet->handles, &portSet->handles[1], lastPort * sizeof(HANDLE));
        portSet->handles[lastPort] = swapHandle;
    }
    memmove(result, portSet->handles, portSet->used * sizeof(HANDLE));
    *portsUsed = portSet->used;
    __CFUnlock(&(portSet->lock));
    return result;
}

static kern_return_t __CFPortSetInsert(__CFPort port, __CFPortSet portSet)
{
    if (NULL == port)
    {
        return -1;
    }
    __CFLock(&(portSet->lock));
    if (portSet->used >= portSet->size)
    {
        portSet->size += 4;
        portSet->handles = (HANDLE *)CFAllocatorReallocate(kCFAllocatorSystemDefault, portSet->handles, portSet->size * sizeof(HANDLE), 0);
    }
    if (portSet->used >= MAXIMUM_WAIT_OBJECTS)
    {
        CFLog(kCFLogLevelWarning, CFSTR("*** More than MAXIMUM_WAIT_OBJECTS (%d) ports add to a port set.  The last ones will be ignored."), MAXIMUM_WAIT_OBJECTS);
    }
    portSet->handles[portSet->used++] = port;
    __CFUnlock(&(portSet->lock));
    return KERN_SUCCESS;
}

static kern_return_t __CFPortSetRemove(__CFPort port, __CFPortSet portSet)
{
    int i, j;
    if (NULL == port)
    {
        return -1;
    }
    __CFLock(&(portSet->lock));
    for (i = 0; i < portSet->used; i++)
    {
        if (portSet->handles[i] == port)
        {
            for (j = i + 1; j < portSet->used; j++)
            {
                portSet->handles[j - 1] = portSet->handles[j];
            }
            portSet->used--;
            __CFUnlock(&(portSet->lock));
            return true;
        }
    }
    __CFUnlock(&(portSet->lock));
    return KERN_SUCCESS;
}

#endif

#if !defined(__MACTYPES__) && !defined(_OS_OSTYPES_H)
#if defined(__BIG_ENDIAN__)
typedef struct UnsignedWide
{
    UInt32 hi;
    UInt32 lo;
} UnsignedWide;
#elif defined(__LITTLE_ENDIAN__)
typedef struct UnsignedWide
{
    UInt32 lo;
    UInt32 hi;
} UnsignedWide;
#endif
typedef UnsignedWide AbsoluteTime;
#endif

#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_EMBEDDED_MINI

#if USE_DISPATCH_SOURCE_FOR_TIMERS
#endif
#if USE_MK_TIMER_TOO
extern mach_port_name_t mk_timer_create(void);
extern kern_return_t mk_timer_destroy(mach_port_name_t name);
extern kern_return_t mk_timer_arm(mach_port_name_t name, AbsoluteTime expire_time);
extern kern_return_t mk_timer_cancel(mach_port_name_t name, AbsoluteTime *result_time);

CF_INLINE AbsoluteTime __CFUInt64ToAbsoluteTime(uint64_t x)
{
    AbsoluteTime a;
    a.hi = x >> 32;
    a.lo = x & (uint64_t)0xFFFFFFFF;
    return a;
}
#endif

// 手动调用 mach_msg 向 rl->_wakeUpPort sendMsg 以唤醒runloop
static uint32_t __CFSendTrivialMachMessage(mach_port_t port, uint32_t msg_id, CFOptionFlags options, uint32_t timeout)
{
    kern_return_t result;
    mach_msg_header_t header;
    // 配置header...
    header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    header.msgh_size = sizeof(mach_msg_header_t);
    header.msgh_remote_port = port;
    header.msgh_local_port = MACH_PORT_NULL;
    header.msgh_id = msg_id;
    // 向内核发送消息唤醒runloop
    result = mach_msg(&header, MACH_SEND_MSG | options, header.msgh_size, 0, MACH_PORT_NULL, timeout, MACH_PORT_NULL);
    if (result == MACH_SEND_TIMED_OUT)
        mach_msg_destroy(&header);
    return result;
}
#elif DEPLOYMENT_TARGET_WINDOWS

static HANDLE mk_timer_create(void)
{
    return CreateWaitableTimer(NULL, FALSE, NULL);
}

static kern_return_t mk_timer_destroy(HANDLE name)
{
    BOOL res = CloseHandle(name);
    if (!res)
    {
        DWORD err = GetLastError();
        CFLog(kCFLogLevelError, CFSTR("CFRunLoop: Unable to destroy timer: %d"), err);
    }
    return (int)res;
}

static kern_return_t mk_timer_arm(HANDLE name, LARGE_INTEGER expire_time)
{
    BOOL res = SetWaitableTimer(name, &expire_time, 0, NULL, NULL, FALSE);
    if (!res)
    {
        DWORD err = GetLastError();
        CFLog(kCFLogLevelError, CFSTR("CFRunLoop: Unable to set timer: %d"), err);
    }
    return (int)res;
}

static kern_return_t mk_timer_cancel(HANDLE name, LARGE_INTEGER *result_time)
{
    BOOL res = CancelWaitableTimer(name);
    if (!res)
    {
        DWORD err = GetLastError();
        CFLog(kCFLogLevelError, CFSTR("CFRunLoop: Unable to cancel timer: %d"), err);
    }
    return (int)res;
}

// The name of this function is a lie on Windows. The return value matches the argument of the fake mk_timer functions above. Note that the Windows timers expect to be given "system time". We have to do some calculations to get the right value, which is a FILETIME-like value.
CF_INLINE LARGE_INTEGER __CFUInt64ToAbsoluteTime(uint64_t desiredFireTime)
{
    LARGE_INTEGER result;
    // There is a race we know about here, (timer fire time calculated -> thread suspended -> timer armed == late timer fire), but we don't have a way to avoid it at this time, since the only way to specify an absolute value to the timer is to calculate the relative time first. Fixing that would probably require not using the TSR for timers on Windows.
    uint64_t now = mach_absolute_time();
    if (now > desiredFireTime)
    {
        result.QuadPart = 0;
    }
    else
    {
        uint64_t timeDiff = desiredFireTime - now;
        CFTimeInterval amountOfTimeToWait = __CFTSRToTimeInterval(timeDiff);
        // Result is in 100 ns (10**-7 sec) units to be consistent with a FILETIME.
        // CFTimeInterval is in seconds.
        result.QuadPart = -(amountOfTimeToWait * 10000000);
    }
    return result;
}

#endif

#pragma mark - CFRunLoopModeRef
#pragma mark Modes

/* unlock a run loop and modes before doing callouts/sleeping */
/* never try to take the run loop lock with a mode locked */
/* be very careful of common subexpression elimination and compacting code, particular across locks and unlocks! */
/* run loop mode structures should never be deallocated, even if they become empty */

static CFTypeID __kCFRunLoopModeTypeID = _kCFRuntimeNotATypeID;
static CFTypeID __kCFRunLoopTypeID = _kCFRuntimeNotATypeID;
static CFTypeID __kCFRunLoopSourceTypeID = _kCFRuntimeNotATypeID;
static CFTypeID __kCFRunLoopObserverTypeID = _kCFRuntimeNotATypeID;
static CFTypeID __kCFRunLoopTimerTypeID = _kCFRuntimeNotATypeID;

//CFRunLoopModeRef 指向一个结构体 __CFRunLoopMode
typedef struct __CFRunLoopMode *CFRunLoopModeRef;

// 对于一个 RunLoop 来说，其内部的 mode 只能增加不能删除。
//结构体 __CFRunLoopMode
struct __CFRunLoopMode
{
    CFRuntimeBase _base;
    // pthread_mutex_t线程互斥锁  https://blog.csdn.net/zmxiangde_88/article/details/7998458
    pthread_mutex_t _lock; /* must have the run loop locked before locking this */// 访问mode集合时用到的锁
    CFStringRef _name;     // Mode Name, 例如 @"kCFRunLoopDefaultMode"
    Boolean _stopped;      //mode是否被终止
    char _padding[3];

    //几种事件，下面这四个字段，在苹果官方文档里面称为Item。runloop中有个commomitems字段，里面就是保存的下面这些内容。
    CFMutableSetRef _sources0;                 // set  sources0
    CFMutableSetRef _sources1;                 // set  sources1
    CFMutableArrayRef _observers;              // array 观察者
    CFMutableArrayRef _timers;                 // array 计时器

    CFMutableDictionaryRef _portToV1SourceMap; //字典  key是mach_port_t，value是CFRunLoopSourceRef
    __CFPortSet _portSet;                      //保存所有需要监听的port，比如_wakeUpPort，_timerPort都保存在这个数组中
    CFIndex _observerMask;
#if USE_DISPATCH_SOURCE_FOR_TIMERS
    dispatch_source_t _timerSource;
    dispatch_queue_t _queue;
    Boolean _timerFired; // set to true by the source when a timer has fired
    Boolean _dispatchTimerArmed;
#endif
#if USE_MK_TIMER_TOO
    mach_port_t _timerPort;
    Boolean _mkTimerArmed;
#endif
#if DEPLOYMENT_TARGET_WINDOWS
    DWORD _msgQMask;
    void (*_msgPump)(void);
#endif
    uint64_t _timerSoftDeadline; /* TSR */
    uint64_t _timerHardDeadline; /* TSR */
};

//锁住runloopModel内的线程互斥锁.
CF_INLINE void __CFRunLoopModeLock(CFRunLoopModeRef rlm)
{
    pthread_mutex_lock(&(rlm->_lock));
    //CFLog(6, CFSTR("__CFRunLoopModeLock locked %p"), rlm);
}

//打开runloopModel内的线程互斥锁.
CF_INLINE void __CFRunLoopModeUnlock(CFRunLoopModeRef rlm)
{
    //CFLog(6, CFSTR("__CFRunLoopModeLock unlocking %p"), rlm);
    pthread_mutex_unlock(&(rlm->_lock));
}

// 使用name  判断两个runloopModel是否相等
static Boolean __CFRunLoopModeEqual(CFTypeRef cf1, CFTypeRef cf2)
{
    CFRunLoopModeRef rlm1 = (CFRunLoopModeRef)cf1;
    CFRunLoopModeRef rlm2 = (CFRunLoopModeRef)cf2;
    return CFEqual(rlm1->_name, rlm2->_name);
}

static CFHashCode __CFRunLoopModeHash(CFTypeRef cf)
{
    CFRunLoopModeRef rlm = (CFRunLoopModeRef)cf;
    return CFHash(rlm->_name);
}

static CFStringRef __CFRunLoopModeCopyDescription(CFTypeRef cf)
{
    CFRunLoopModeRef rlm = (CFRunLoopModeRef)cf;
    CFMutableStringRef result;
    result = CFStringCreateMutable(kCFAllocatorSystemDefault, 0);
    CFStringAppendFormat(result, NULL, CFSTR("<CFRunLoopMode %p [%p]>{name = %@, "), rlm, CFGetAllocator(rlm), rlm->_name);
    CFStringAppendFormat(result, NULL, CFSTR("port set = 0x%x, "), rlm->_portSet);
#if USE_DISPATCH_SOURCE_FOR_TIMERS
    CFStringAppendFormat(result, NULL, CFSTR("queue = %p, "), rlm->_queue);
    CFStringAppendFormat(result, NULL, CFSTR("source = %p (%s), "), rlm->_timerSource, rlm->_timerFired ? "fired" : "not fired");
#endif
#if USE_MK_TIMER_TOO
    CFStringAppendFormat(result, NULL, CFSTR("timer port = 0x%x, "), rlm->_timerPort);
#endif
#if DEPLOYMENT_TARGET_WINDOWS
    CFStringAppendFormat(result, NULL, CFSTR("MSGQ mask = %p, "), rlm->_msgQMask);
#endif
    CFStringAppendFormat(result, NULL, CFSTR("\n\tsources0 = %@,\n\tsources1 = %@,\n\tobservers = %@,\n\ttimers = %@,\n\tcurrently %0.09g (%lld) / soft deadline in: %0.09g sec (@ %lld) / hard deadline in: %0.09g sec (@ %lld)\n},\n"), rlm->_sources0, rlm->_sources1, rlm->_observers, rlm->_timers, CFAbsoluteTimeGetCurrent(), mach_absolute_time(), __CFTSRToTimeInterval(rlm->_timerSoftDeadline - mach_absolute_time()), rlm->_timerSoftDeadline, __CFTSRToTimeInterval(rlm->_timerHardDeadline - mach_absolute_time()), rlm->_timerHardDeadline);
    return result;
}

// 释放model中的成员变量
static void __CFRunLoopModeDeallocate(CFTypeRef cf)
{
    CFRunLoopModeRef rlm = (CFRunLoopModeRef)cf;
    if (NULL != rlm->_sources0)
        CFRelease(rlm->_sources0);
    if (NULL != rlm->_sources1)
        CFRelease(rlm->_sources1);
    if (NULL != rlm->_observers)
        CFRelease(rlm->_observers);
    if (NULL != rlm->_timers)
        CFRelease(rlm->_timers);
    if (NULL != rlm->_portToV1SourceMap)
        CFRelease(rlm->_portToV1SourceMap);
    CFRelease(rlm->_name);
    __CFPortSetFree(rlm->_portSet);
#if USE_DISPATCH_SOURCE_FOR_TIMERS
    if (rlm->_timerSource)
    {
        dispatch_source_cancel(rlm->_timerSource);
        dispatch_release(rlm->_timerSource);
    }
    if (rlm->_queue)
    {
        dispatch_release(rlm->_queue);
    }
#endif
#if USE_MK_TIMER_TOO
    if (MACH_PORT_NULL != rlm->_timerPort)
        mk_timer_destroy(rlm->_timerPort);
#endif
    pthread_mutex_destroy(&rlm->_lock);
    memset((char *)cf + sizeof(CFRuntimeBase), 0x7C, sizeof(struct __CFRunLoopMode) - sizeof(CFRuntimeBase));
}

#pragma mark -
#pragma mark Run Loops

// CFRunLoop 中block的 item
struct _block_item
{
    struct _block_item *_next;
    CFTypeRef _mode; // CFString or CFSet 内容是,modeName
    void (^_block)(void);
};

typedef struct _per_run_data
{
    uint32_t a;
    uint32_t b;
    uint32_t stopped;
    uint32_t ignoreWakeUps;
} _per_run_data;

//如果在RunLoop中需要切换 Mode，只能退出 Loop，再重新指定一个 Mode 进入。这样做主要是为了分隔开不同组的 Source/Timer/Observer，让其互不影响。
struct __CFRunLoop
{
    CFRuntimeBase _base;
    pthread_mutex_t _lock; //线程互斥锁(https://blog.csdn.net/guotianqing/article/details/80559865)		/* locked for accessing mode list */
    __CFPort _wakeUpPort;  // used for CFRunLoopWakeUp
// 手动唤醒runloop的端口。初始化runloop时设置，仅用于CFRunLoopWakeUp，CFRunLoopWakeUp函数会向_wakeUpPort发送一条消息
    Boolean _unused;
    // volatile 关键字, 不对变量进行编译优化
    volatile _per_run_data *_perRunData; //            // reset for runs of the run loop
    pthread_t _pthread;                  // runloop 对应的线程
    uint32_t _winthread;
    CFMutableSetRef _commonModes;     // Set.//存储的是字符串，记录所有标记为common的mode
    CFMutableSetRef _commonModeItems; // Set所有commonModel的item <Source/Observer/Timer> // 存储所有commonMode的sources、timers、observers

    CFRunLoopModeRef _currentMode;    // Current Runloop Mode 当前运行的mode
    CFMutableSetRef _modes;           // Set 存储的是CFRunLoopModeRef，
    struct _block_item *_blocks_head; // 链表头指针，该链表保存了所有需要被runloop执行的block。外部通过调用CFRunLoopPerformBlock函数来向链表中添加一个block节点。runloop会在CFRunLoopDoBlock时遍历该链表，逐一执行block

    struct _block_item *_blocks_tail; // tail-尾巴  // 链表尾指针，之所以有尾指针，是为了降低增加block时的时间复杂度
    CFAbsoluteTime _runTime;          // CFTimeInterval, 绝对时间
    CFAbsoluteTime _sleepTime;
    CFTypeRef _counterpart;
};

/* Bit 0 of the base reserved bits is used for stopped state */
/* Bit 1 of the base reserved bits is used for sleeping state */
/* Bit 2 of the base reserved bits is used for deallocating state */
// 设置一些标记位,表示停止,休眠,析构的状态
CF_INLINE volatile _per_run_data *__CFRunLoopPushPerRunData(CFRunLoopRef rl)
{
    volatile _per_run_data *previous = rl->_perRunData;
    rl->_perRunData = (volatile _per_run_data *)CFAllocatorAllocate(kCFAllocatorSystemDefault, sizeof(_per_run_data), 0);
    rl->_perRunData->a = 0x4346524C;
    rl->_perRunData->b = 0x4346524C; // 'CFRL'
    rl->_perRunData->stopped = 0x00000000;
    rl->_perRunData->ignoreWakeUps = 0x00000000;
    return previous;
}

CF_INLINE void __CFRunLoopPopPerRunData(CFRunLoopRef rl, volatile _per_run_data *previous)
{
    if (rl->_perRunData)
        CFAllocatorDeallocate(kCFAllocatorSystemDefault, (void *)rl->_perRunData);
    rl->_perRunData = previous;
}

CF_INLINE Boolean __CFRunLoopIsStopped(CFRunLoopRef rl)
{
    return (rl->_perRunData->stopped) ? true : false;
}

CF_INLINE void __CFRunLoopSetStopped(CFRunLoopRef rl)
{
    rl->_perRunData->stopped = 0x53544F50; // 'STOP'
}

CF_INLINE void __CFRunLoopUnsetStopped(CFRunLoopRef rl)
{
    rl->_perRunData->stopped = 0x0;
}

CF_INLINE Boolean __CFRunLoopIsIgnoringWakeUps(CFRunLoopRef rl)
{
    return (rl->_perRunData->ignoreWakeUps) ? true : false;
}

CF_INLINE void __CFRunLoopSetIgnoreWakeUps(CFRunLoopRef rl)
{
    rl->_perRunData->ignoreWakeUps = 0x57414B45; // 'WAKE'
}

CF_INLINE void __CFRunLoopUnsetIgnoreWakeUps(CFRunLoopRef rl)
{
    rl->_perRunData->ignoreWakeUps = 0x0;
}

// runloop是否为休眠状态
CF_INLINE Boolean __CFRunLoopIsSleeping(CFRunLoopRef rl)
{
    return (Boolean)__CFBitfieldGetValue(((const CFRuntimeBase *)rl)->_cfinfo[CF_INFO_BITS], 1, 1);
}

CF_INLINE void __CFRunLoopSetSleeping(CFRunLoopRef rl)
{
    __CFBitfieldSetValue(((CFRuntimeBase *)rl)->_cfinfo[CF_INFO_BITS], 1, 1, 1);
}

CF_INLINE void __CFRunLoopUnsetSleeping(CFRunLoopRef rl)
{
    __CFBitfieldSetValue(((CFRuntimeBase *)rl)->_cfinfo[CF_INFO_BITS], 1, 1, 0);
}

CF_INLINE Boolean __CFRunLoopIsDeallocating(CFRunLoopRef rl)
{
    return (Boolean)__CFBitfieldGetValue(((const CFRuntimeBase *)rl)->_cfinfo[CF_INFO_BITS], 2, 2);
}

CF_INLINE void __CFRunLoopSetDeallocating(CFRunLoopRef rl)
{
    __CFBitfieldSetValue(((CFRuntimeBase *)rl)->_cfinfo[CF_INFO_BITS], 2, 2, 1);
}
//使用rl的lock，加锁
CF_INLINE void __CFRunLoopLock(CFRunLoopRef rl)
{
    pthread_mutex_lock(&(((CFRunLoopRef)rl)->_lock));
    //    CFLog(6, CFSTR("__CFRunLoopLock locked %p"), rl);
}

CF_INLINE void __CFRunLoopUnlock(CFRunLoopRef rl)
{
    //    CFLog(6, CFSTR("__CFRunLoopLock unlocking %p"), rl);
    pthread_mutex_unlock(&(((CFRunLoopRef)rl)->_lock));
}

static CFStringRef __CFRunLoopCopyDescription(CFTypeRef cf)
{
    CFRunLoopRef rl = (CFRunLoopRef)cf;
    CFMutableStringRef result;
    result = CFStringCreateMutable(kCFAllocatorSystemDefault, 0);
#if DEPLOYMENT_TARGET_WINDOWS
    CFStringAppendFormat(result, NULL, CFSTR("<CFRunLoop %p [%p]>{wakeup port = 0x%x, stopped = %s, ignoreWakeUps = %s, \ncurrent mode = %@,\n"), cf, CFGetAllocator(cf), rl->_wakeUpPort, __CFRunLoopIsStopped(rl) ? "true" : "false", __CFRunLoopIsIgnoringWakeUps(rl) ? "true" : "false", rl->_currentMode ? rl->_currentMode->_name : CFSTR("(none)"));
#else
    CFStringAppendFormat(result, NULL, CFSTR("<CFRunLoop %p [%p]>{wakeup port = 0x%x, stopped = %s, ignoreWakeUps = %s, \ncurrent mode = %@,\n"), cf, CFGetAllocator(cf), rl->_wakeUpPort, __CFRunLoopIsStopped(rl) ? "true" : "false", __CFRunLoopIsIgnoringWakeUps(rl) ? "true" : "false", rl->_currentMode ? rl->_currentMode->_name : CFSTR("(none)"));
#endif
    CFStringAppendFormat(result, NULL, CFSTR("common modes = %@,\ncommon mode items = %@,\nmodes = %@}\n"), rl->_commonModes, rl->_commonModeItems, rl->_modes);
    return result;
}

CF_PRIVATE void __CFRunLoopDump()
{ // __private_extern__ to keep the compiler from discarding it
    CFShow(CFCopyDescription(CFRunLoopGetCurrent()));
}

CF_INLINE void __CFRunLoopLockInit(pthread_mutex_t *lock)
{
    pthread_mutexattr_t mattr;
    pthread_mutexattr_init(&mattr);
    pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_RECURSIVE);
    int32_t mret = pthread_mutex_init(lock, &mattr);
    pthread_mutexattr_destroy(&mattr);
    if (0 != mret)
    {
    }
}

/* call with rl locked, returns mode locked */
// 根据modeName,查找runloop中的runloopMode,如果没有,根据create判断是否创建.
static CFRunLoopModeRef __CFRunLoopFindMode(CFRunLoopRef rl, CFStringRef modeName, Boolean create)
{
    CHECK_FOR_FORK();
    CFRunLoopModeRef rlm;
    struct __CFRunLoopMode srlm;
    memset(&srlm, 0, sizeof(srlm));
    _CFRuntimeSetInstanceTypeIDAndIsa(&srlm, __kCFRunLoopModeTypeID);
    srlm._name = modeName;
    rlm = (CFRunLoopModeRef)CFSetGetValue(rl->_modes, &srlm);
    if (NULL != rlm)
    {
        __CFRunLoopModeLock(rlm);
        return rlm;
    }
    if (!create)
    {
        return NULL;
    }
    rlm = (CFRunLoopModeRef)_CFRuntimeCreateInstance(kCFAllocatorSystemDefault, __kCFRunLoopModeTypeID, sizeof(struct __CFRunLoopMode) - sizeof(CFRuntimeBase), NULL);
    if (NULL == rlm)
    {
        return NULL;
    }
    __CFRunLoopLockInit(&rlm->_lock);
    rlm->_name = CFStringCreateCopy(kCFAllocatorSystemDefault, modeName);
    rlm->_stopped = false;
    rlm->_portToV1SourceMap = NULL;
    rlm->_sources0 = NULL;
    rlm->_sources1 = NULL;
    rlm->_observers = NULL;
    rlm->_timers = NULL;
    rlm->_observerMask = 0;
    rlm->_portSet = __CFPortSetAllocate();
    rlm->_timerSoftDeadline = UINT64_MAX;
    rlm->_timerHardDeadline = UINT64_MAX;

    kern_return_t ret = KERN_SUCCESS;
#if USE_DISPATCH_SOURCE_FOR_TIMERS
    rlm->_timerFired = false;
    rlm->_queue = _dispatch_runloop_root_queue_create_4CF("Run Loop Mode Queue", 0);
    mach_port_t queuePort = _dispatch_runloop_root_queue_get_port_4CF(rlm->_queue);
    if (queuePort == MACH_PORT_NULL)
        CRASH("*** Unable to create run loop mode queue port. (%d) ***", -1);
    rlm->_timerSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, rlm->_queue);

    __block Boolean *timerFiredPointer = &(rlm->_timerFired);

    // 设置计时器回调
    dispatch_source_set_event_handler(rlm->_timerSource, ^{
      *timerFiredPointer = true;
    });

    // Set timer to far out there. The unique leeway makes this timer easy to spot in debug output.
    _dispatch_source_set_runloop_timer_4CF(rlm->_timerSource, DISPATCH_TIME_FOREVER, DISPATCH_TIME_FOREVER, 321);
    dispatch_resume(rlm->_timerSource);

    ret = __CFPortSetInsert(queuePort, rlm->_portSet);
    if (KERN_SUCCESS != ret)
        CRASH("*** Unable to insert timer port into port set. (%d) ***", ret);

#endif
#if USE_MK_TIMER_TOO
    rlm->_timerPort = mk_timer_create();
    ret = __CFPortSetInsert(rlm->_timerPort, rlm->_portSet);
    if (KERN_SUCCESS != ret)
        CRASH("*** Unable to insert timer port into port set. (%d) ***", ret);
#endif

    ret = __CFPortSetInsert(rl->_wakeUpPort, rlm->_portSet);
    if (KERN_SUCCESS != ret)
        CRASH("*** Unable to insert wake up port into port set. (%d) ***", ret);

#if DEPLOYMENT_TARGET_WINDOWS
    rlm->_msgQMask = 0;
    rlm->_msgPump = NULL;
#endif
    CFSetAddValue(rl->_modes, rlm);
    CFRelease(rlm);
    __CFRunLoopModeLock(rlm); /* return mode locked */
    return rlm;
}

// expects rl and rlm locked
// 判断runloop的mode是否为空, commonModel存在,或者 _sources0,_sources1,_timers,block存在,那么mode不为空
static Boolean __CFRunLoopModeIsEmpty(CFRunLoopRef rl, CFRunLoopModeRef rlm, CFRunLoopModeRef previousMode)
{
    CHECK_FOR_FORK();
    if (NULL == rlm)
        return true;
#if DEPLOYMENT_TARGET_WINDOWS
    if (0 != rlm->_msgQMask)
        return false;
#endif
    Boolean libdispatchQSafe = pthread_main_np() && ((HANDLE_DISPATCH_ON_BASE_INVOCATION_ONLY && NULL == previousMode) || (!HANDLE_DISPATCH_ON_BASE_INVOCATION_ONLY && 0 == _CFGetTSD(__CFTSDKeyIsInGCDMainQ)));
    if (libdispatchQSafe && (CFRunLoopGetMain() == rl) && CFSetContainsValue(rl->_commonModes, rlm->_name))
        return false; // represents the libdispatch main queue
    if (NULL != rlm->_sources0 && 0 < CFSetGetCount(rlm->_sources0))
        return false;
    if (NULL != rlm->_sources1 && 0 < CFSetGetCount(rlm->_sources1))
        return false;
    if (NULL != rlm->_timers && 0 < CFArrayGetCount(rlm->_timers))
        return false;
    struct _block_item *item = rl->_blocks_head;
    while (item)
    {
        struct _block_item *curr = item;
        item = item->_next;
        Boolean doit = false;
        if (CFStringGetTypeID() == CFGetTypeID(curr->_mode))
        {
            doit = CFEqual(curr->_mode, rlm->_name) || (CFEqual(curr->_mode, kCFRunLoopCommonModes) && CFSetContainsValue(rl->_commonModes, rlm->_name));
        }
        else
        {
            doit = CFSetContainsValue((CFSetRef)curr->_mode, rlm->_name) || (CFSetContainsValue((CFSetRef)curr->_mode, kCFRunLoopCommonModes) && CFSetContainsValue(rl->_commonModes, rlm->_name));
        }
        if (doit)
            return false;
    }
    return true;
}

#if DEPLOYMENT_TARGET_WINDOWS

uint32_t _CFRunLoopGetWindowsMessageQueueMask(CFRunLoopRef rl, CFStringRef modeName)
{
    if (modeName == kCFRunLoopCommonModes)
    {
        CFLog(kCFLogLevelError, CFSTR("_CFRunLoopGetWindowsMessageQueueMask: kCFRunLoopCommonModes unsupported"));
        HALT;
    }
    DWORD result = 0;
    __CFRunLoopLock(rl);
    CFRunLoopModeRef rlm = __CFRunLoopFindMode(rl, modeName, false);
    if (rlm)
    {
        result = rlm->_msgQMask;
        __CFRunLoopModeUnlock(rlm);
    }
    __CFRunLoopUnlock(rl);
    return (uint32_t)result;
}

void _CFRunLoopSetWindowsMessageQueueMask(CFRunLoopRef rl, uint32_t mask, CFStringRef modeName)
{
    if (modeName == kCFRunLoopCommonModes)
    {
        CFLog(kCFLogLevelError, CFSTR("_CFRunLoopSetWindowsMessageQueueMask: kCFRunLoopCommonModes unsupported"));
        HALT;
    }
    __CFRunLoopLock(rl);
    CFRunLoopModeRef rlm = __CFRunLoopFindMode(rl, modeName, true);
    rlm->_msgQMask = (DWORD)mask;
    __CFRunLoopModeUnlock(rlm);
    __CFRunLoopUnlock(rl);
}

uint32_t _CFRunLoopGetWindowsThreadID(CFRunLoopRef rl)
{
    return rl->_winthread;
}

CFWindowsMessageQueueHandler _CFRunLoopGetWindowsMessageQueueHandler(CFRunLoopRef rl, CFStringRef modeName)
{
    if (modeName == kCFRunLoopCommonModes)
    {
        CFLog(kCFLogLevelError, CFSTR("_CFRunLoopGetWindowsMessageQueueMask: kCFRunLoopCommonModes unsupported"));
        HALT;
    }
    if (rl != CFRunLoopGetCurrent())
    {
        CFLog(kCFLogLevelError, CFSTR("_CFRunLoopGetWindowsMessageQueueHandler: run loop parameter must be the current run loop"));
        HALT;
    }
    void (*result)(void) = NULL;
    __CFRunLoopLock(rl);
    CFRunLoopModeRef rlm = __CFRunLoopFindMode(rl, modeName, false);
    if (rlm)
    {
        result = rlm->_msgPump;
        __CFRunLoopModeUnlock(rlm);
    }
    __CFRunLoopUnlock(rl);
    return result;
}

void _CFRunLoopSetWindowsMessageQueueHandler(CFRunLoopRef rl, CFStringRef modeName, CFWindowsMessageQueueHandler func)
{
    if (modeName == kCFRunLoopCommonModes)
    {
        CFLog(kCFLogLevelError, CFSTR("_CFRunLoopGetWindowsMessageQueueMask: kCFRunLoopCommonModes unsupported"));
        HALT;
    }
    if (rl != CFRunLoopGetCurrent())
    {
        CFLog(kCFLogLevelError, CFSTR("_CFRunLoopGetWindowsMessageQueueHandler: run loop parameter must be the current run loop"));
        HALT;
    }
    __CFRunLoopLock(rl);
    CFRunLoopModeRef rlm = __CFRunLoopFindMode(rl, modeName, true);
    rlm->_msgPump = func;
    __CFRunLoopModeUnlock(rlm);
    __CFRunLoopUnlock(rl);
}

#endif

#pragma mark - CFRunLoopSourceRef
#pragma mark Sources

/* Bit 3 in the base reserved bits is used for invalid state in run loop objects */

CF_INLINE Boolean __CFIsValid(const void *cf)
{
    return (Boolean)__CFBitfieldGetValue(((const CFRuntimeBase *)cf)->_cfinfo[CF_INFO_BITS], 3, 3);
}

CF_INLINE void __CFSetValid(void *cf)
{
    __CFBitfieldSetValue(((CFRuntimeBase *)cf)->_cfinfo[CF_INFO_BITS], 3, 3, 1);
}

CF_INLINE void __CFUnsetValid(void *cf)
{
    __CFBitfieldSetValue(((CFRuntimeBase *)cf)->_cfinfo[CF_INFO_BITS], 3, 3, 0);
}

/*
    source0 只包含了一个回调（函数指针），source0是需要手动触发的Source，它并不能主动触发事件，必须要先把它标记为signal状态。使用时，你需要先调用CFRunLoopSourceSignal(source)，将这个 Source 标记为待处理，也就是通过uint32_t _bits来实现的，只有_bits标记Signaled状态才会被处理。然后手动调用CFRunLoopWakeUp(runloop) 来唤醒 RunLoop，让其处理这个事件

    source1 包含了一个 mach_port 和一个回调（函数指针），被用于通过内核和其他线程相互发送消息。这种 Source 能主动唤醒 RunLoop 的线程。简单来说就是更加偏向于底层。


*/

struct __CFRunLoopSource
{
    CFRuntimeBase _base;
    uint32_t _bits; //用于标记Signaled状态，source0只有在被标记为Signaled状态，才会被处理
    pthread_mutex_t _lock;
    CFIndex _order; /* immutable */
    CFMutableBagRef _runLoops; //一个source对应多个runloop。之所以使用CFMutableBagRef这种集合结构保存runloop而非array或set。主要原因是bag是无序的且允许重复。更多信息详见：https://developer.apple.com/documentation/corefoundation/cfbag-s1l
    union {
        CFRunLoopSourceContext version0;  /* immutable, except invalidation */
        CFRunLoopSourceContext1 version1; /* immutable, except invalidation */
    } _context;

// Source0：source0是App内部事件，由App自己管理的，像UIEvent、CFSocket都是source0。source0并不能主动触发事件，当一个source0事件准备处理时，要先调用 CFRunLoopSourceSignal(source)，将这个 Source 标记为待处理。然后手动调用 CFRunLoopWakeUp(runloop) 来唤醒 RunLoop，让其处理这个事件。框架已经帮我们做好了这些调用，比如网络请求的回调、滑动触摸的回调，我们不需要自己处理。

// Source1：由RunLoop和内核管理，Mach port驱动，如CFMachPort、CFMessagePort。source1包含了一个 mach_port 和一个回调（函数指针），被用于通过内核和其他线程相互发送消息。这种 Source 能主动唤醒 RunLoop 的线程。

};

/* Bit 1 of the base reserved bits is used for signalled state */

CF_INLINE Boolean __CFRunLoopSourceIsSignaled(CFRunLoopSourceRef rls)
{
    return (Boolean)__CFBitfieldGetValue(rls->_bits, 1, 1);
}

CF_INLINE void __CFRunLoopSourceSetSignaled(CFRunLoopSourceRef rls)
{
    __CFBitfieldSetValue(rls->_bits, 1, 1, 1);
}

CF_INLINE void __CFRunLoopSourceUnsetSignaled(CFRunLoopSourceRef rls)
{
    __CFBitfieldSetValue(rls->_bits, 1, 1, 0);
}

CF_INLINE void __CFRunLoopSourceLock(CFRunLoopSourceRef rls)
{
    pthread_mutex_lock(&(rls->_lock));
    //    CFLog(6, CFSTR("__CFRunLoopSourceLock locked %p"), rls);
}

CF_INLINE void __CFRunLoopSourceUnlock(CFRunLoopSourceRef rls)
{
    //    CFLog(6, CFSTR("__CFRunLoopSourceLock unlocking %p"), rls);
    pthread_mutex_unlock(&(rls->_lock));
}
#pragma mark - CFRunLoopObserverRef
#pragma mark Observers

//CFRunLoopObserver是观察者，可以观察RunLoop的各种状态，每个 Observer 都包含了一个回调（也就是上面的CFRunLoopObserverCallBack函数指针），当 RunLoop 的状态发生变化时，观察者就能通过回调接受到这个变化。状态定义在_CF_OPTIONS
struct __CFRunLoopObserver
{
    CFRuntimeBase _base;
    pthread_mutex_t _lock;
    CFRunLoopRef _runLoop;  // observer所观察的runloop
    CFIndex _rlCount; // 每schedule一次,count++
    CFOptionFlags _activities;          /* immutable */ // CFOptionFlags是UInt类型的别名，_activities用来说明要观察runloop的哪些状态。一旦指定了就不可变。
    CFIndex _order;                     /* immutable */
    CFRunLoopObserverCallBack _callout; /* immutable */ // 观察到runloop状态变化后的回调(不可变)
    CFRunLoopObserverContext _context;  /* immutable, except invalidation */
};

/* Bit 0 of the base reserved bits is used for firing state */
/* Bit 1 of the base reserved bits is used for repeats state */

CF_INLINE Boolean __CFRunLoopObserverIsFiring(CFRunLoopObserverRef rlo)
{
    return (Boolean)__CFBitfieldGetValue(((const CFRuntimeBase *)rlo)->_cfinfo[CF_INFO_BITS], 0, 0);
}

CF_INLINE void __CFRunLoopObserverSetFiring(CFRunLoopObserverRef rlo)
{
    __CFBitfieldSetValue(((CFRuntimeBase *)rlo)->_cfinfo[CF_INFO_BITS], 0, 0, 1);
}

CF_INLINE void __CFRunLoopObserverUnsetFiring(CFRunLoopObserverRef rlo)
{
    __CFBitfieldSetValue(((CFRuntimeBase *)rlo)->_cfinfo[CF_INFO_BITS], 0, 0, 0);
}

CF_INLINE Boolean __CFRunLoopObserverRepeats(CFRunLoopObserverRef rlo)
{
    return (Boolean)__CFBitfieldGetValue(((const CFRuntimeBase *)rlo)->_cfinfo[CF_INFO_BITS], 1, 1);
}

CF_INLINE void __CFRunLoopObserverSetRepeats(CFRunLoopObserverRef rlo)
{
    __CFBitfieldSetValue(((CFRuntimeBase *)rlo)->_cfinfo[CF_INFO_BITS], 1, 1, 1);
}

CF_INLINE void __CFRunLoopObserverUnsetRepeats(CFRunLoopObserverRef rlo)
{
    __CFBitfieldSetValue(((CFRuntimeBase *)rlo)->_cfinfo[CF_INFO_BITS], 1, 1, 0);
}

CF_INLINE void __CFRunLoopObserverLock(CFRunLoopObserverRef rlo)
{
    pthread_mutex_lock(&(rlo->_lock));
    //    CFLog(6, CFSTR("__CFRunLoopObserverLock locked %p"), rlo);
}

CF_INLINE void __CFRunLoopObserverUnlock(CFRunLoopObserverRef rlo)
{
    //    CFLog(6, CFSTR("__CFRunLoopObserverLock unlocking %p"), rlo);
    pthread_mutex_unlock(&(rlo->_lock));
}

static void __CFRunLoopObserverSchedule(CFRunLoopObserverRef rlo, CFRunLoopRef rl, CFRunLoopModeRef rlm)
{
    __CFRunLoopObserverLock(rlo);
    if (0 == rlo->_rlCount)
    {
        rlo->_runLoop = rl;
    }
    rlo->_rlCount++;
    __CFRunLoopObserverUnlock(rlo);
}

static void __CFRunLoopObserverCancel(CFRunLoopObserverRef rlo, CFRunLoopRef rl, CFRunLoopModeRef rlm)
{
    __CFRunLoopObserverLock(rlo);
    rlo->_rlCount--;
    if (0 == rlo->_rlCount)
    {
        rlo->_runLoop = NULL;
    }
    __CFRunLoopObserverUnlock(rlo);
}

#pragma mark Timers

struct __CFRunLoopTimer
{
    CFRuntimeBase _base;
    uint16_t _bits;   // 标记fire状态
    pthread_mutex_t _lock;
    CFRunLoopRef _runLoop;    //添加该timer的runloop // timer所处的runloop 和source不同，timer对应的runloop是一个runloop指针，而非数组，所以此处说明一个timer只能添加到一个runloop。
    CFMutableSetRef _rlModes; //存放所有包含该timer的 mode的 modeName，意味着一个timer可能会在多个mode中存在
    CFAbsoluteTime _nextFireDate; // 下次触发时间
    CFTimeInterval _interval; /* immutable */ //理想时间间隔
    CFTimeInterval _tolerance; /* mutable */  // 允许的误差(可变)
    uint64_t _fireTSR;                        /* TSR units */
    CFIndex _order;                           /* immutable */
    CFRunLoopTimerCallBack _callout;          /* immutable */ // timer回调
    CFRunLoopTimerContext _context;           /* immutable, except invalidation */
};

/* Bit 0 of the base reserved bits is used for firing state */
/* Bit 1 of the base reserved bits is used for fired-during-callout state */
/* Bit 2 of the base reserved bits is used for waking state */

CF_INLINE Boolean __CFRunLoopTimerIsFiring(CFRunLoopTimerRef rlt)
{
    return (Boolean)__CFBitfieldGetValue(rlt->_bits, 0, 0);
}

CF_INLINE void __CFRunLoopTimerSetFiring(CFRunLoopTimerRef rlt)
{
    __CFBitfieldSetValue(rlt->_bits, 0, 0, 1);
}

CF_INLINE void __CFRunLoopTimerUnsetFiring(CFRunLoopTimerRef rlt)
{
    __CFBitfieldSetValue(rlt->_bits, 0, 0, 0);
}

CF_INLINE Boolean __CFRunLoopTimerIsDeallocating(CFRunLoopTimerRef rlt)
{
    return (Boolean)__CFBitfieldGetValue(rlt->_bits, 2, 2);
}

CF_INLINE void __CFRunLoopTimerSetDeallocating(CFRunLoopTimerRef rlt)
{
    __CFBitfieldSetValue(rlt->_bits, 2, 2, 1);
}

CF_INLINE void __CFRunLoopTimerLock(CFRunLoopTimerRef rlt)
{
    pthread_mutex_lock(&(rlt->_lock));
    //    CFLog(6, CFSTR("__CFRunLoopTimerLock locked %p"), rlt);
}

CF_INLINE void __CFRunLoopTimerUnlock(CFRunLoopTimerRef rlt)
{
    //    CFLog(6, CFSTR("__CFRunLoopTimerLock unlocking %p"), rlt);
    pthread_mutex_unlock(&(rlt->_lock));
}

static CFLock_t __CFRLTFireTSRLock = CFLockInit;

CF_INLINE void __CFRunLoopTimerFireTSRLock(void)
{
    __CFLock(&__CFRLTFireTSRLock);
}

CF_INLINE void __CFRunLoopTimerFireTSRUnlock(void)
{
    __CFUnlock(&__CFRLTFireTSRLock);
}

#pragma mark -


// 一个 RunLoop 包含若干个 Mode，每个 Mode 又可以包含若干个 Source/Timer/Observer。每次调用 RunLoop 的主函数时，只能指定其中一个 Mode，这个Mode就是runloop的 CurrentMode。如果需要切换 Mode，只能退出 Loop，再重新指定一个 Mode 进入。这样做主要是为了分隔开不同组的 Source/Timer/Observer，让其互不影响。

/* CFRunLoop */
// App的默认 Mode，通常主线程是在这个 Mode 下运行的。
CONST_STRING_DECL(kCFRunLoopDefaultMode, "kCFRunLoopDefaultMode")
// 
CONST_STRING_DECL(kCFRunLoopCommonModes, "kCFRunLoopCommonModes")

// kCFRunLoopDefaultMode: App的默认 Mode，通常主线程是在这个 Mode 下运行的。
// UITrackingRunLoopMode: 界面跟踪 Mode，用于 ScrollView 追踪触摸滑动，保证界面滑动时不受其他 Mode 影响。
// UIInitializationRunLoopMode: 在刚启动 App 时第进入的第一个 Mode，启动完成后就不再使用。
// GSEventReceiveRunLoopMode: 接受系统事件的内部 Mode，通常用不到。
// kCFRunLoopCommonModes: 这是一个占位的 Mode，没有实际作用。

// call with rl and rlm locked
// 查找对应machPort 下的runloopSource
static CFRunLoopSourceRef __CFRunLoopModeFindSourceForMachPort(CFRunLoopRef rl, CFRunLoopModeRef rlm, __CFPort port)
{ /* DOES CALLOUT */
    CHECK_FOR_FORK();
    CFRunLoopSourceRef found = rlm->_portToV1SourceMap ? (CFRunLoopSourceRef)CFDictionaryGetValue(rlm->_portToV1SourceMap, (const void *)(uintptr_t)port) : NULL;
    return found;
}

// Remove backreferences the mode's sources have to the rl (context);
// the primary purpose of rls->_runLoops is so that Invalidation can remove
// the source from the run loops it is in, but during deallocation of a
// run loop, we already know that the sources are going to be punted
// from it, so invalidation of sources does not need to remove from a
// deallocating run loop.

// 清空source0 source1
static void __CFRunLoopCleanseSources(const void *value, void *context)
{
    CFRunLoopModeRef rlm = (CFRunLoopModeRef)value;
    CFRunLoopRef rl = (CFRunLoopRef)context;
    CFIndex idx, cnt;
    const void **list, *buffer[256];
    if (NULL == rlm->_sources0 && NULL == rlm->_sources1)
        return;

    cnt = (rlm->_sources0 ? CFSetGetCount(rlm->_sources0) : 0) + (rlm->_sources1 ? CFSetGetCount(rlm->_sources1) : 0);
    list = (const void **)((cnt <= 256) ? buffer : CFAllocatorAllocate(kCFAllocatorSystemDefault, cnt * sizeof(void *), 0));
    if (rlm->_sources0)
        CFSetGetValues(rlm->_sources0, list);
    if (rlm->_sources1)
        CFSetGetValues(rlm->_sources1, list + (rlm->_sources0 ? CFSetGetCount(rlm->_sources0) : 0));
    for (idx = 0; idx < cnt; idx++)
    {
        CFRunLoopSourceRef rls = (CFRunLoopSourceRef)list[idx];
        __CFRunLoopSourceLock(rls);
        if (NULL != rls->_runLoops)
        {
        
            CFBagRemoveValue(rls->_runLoops, rl);
        }
        __CFRunLoopSourceUnlock(rls);
    }
    if (list != buffer)
        CFAllocatorDeallocate(kCFAllocatorSystemDefault, list);
}
//  释放各种source
static void __CFRunLoopDeallocateSources(const void *value, void *context)
{
    CFRunLoopModeRef rlm = (CFRunLoopModeRef)value;
    CFRunLoopRef rl = (CFRunLoopRef)context;
    CFIndex idx, cnt;
    const void **list, *buffer[256];
    if (NULL == rlm->_sources0 && NULL == rlm->_sources1)
        return;

    cnt = (rlm->_sources0 ? CFSetGetCount(rlm->_sources0) : 0) + (rlm->_sources1 ? CFSetGetCount(rlm->_sources1) : 0);
    list = (const void **)((cnt <= 256) ? buffer : CFAllocatorAllocate(kCFAllocatorSystemDefault, cnt * sizeof(void *), 0));
    if (rlm->_sources0)
        CFSetGetValues(rlm->_sources0, list);
    if (rlm->_sources1)
        CFSetGetValues(rlm->_sources1, list + (rlm->_sources0 ? CFSetGetCount(rlm->_sources0) : 0));
    for (idx = 0; idx < cnt; idx++)
    {
        CFRetain(list[idx]);
    }
    if (rlm->_sources0)
        CFSetRemoveAllValues(rlm->_sources0);
    if (rlm->_sources1)
        CFSetRemoveAllValues(rlm->_sources1);
    for (idx = 0; idx < cnt; idx++)
    {
        CFRunLoopSourceRef rls = (CFRunLoopSourceRef)list[idx];
        __CFRunLoopSourceLock(rls);
        if (NULL != rls->_runLoops)
        {
            CFBagRemoveValue(rls->_runLoops, rl);
        }
        __CFRunLoopSourceUnlock(rls);
        if (0 == rls->_context.version0.version)
        {
            if (NULL != rls->_context.version0.cancel)
            {
                rls->_context.version0.cancel(rls->_context.version0.info, rl, rlm->_name); /* CALLOUT */
            }
        }
        else if (1 == rls->_context.version0.version)
        {
            __CFPort port = rls->_context.version1.getPort(rls->_context.version1.info); /* CALLOUT */
            if (CFPORT_NULL != port)
            {
                __CFPortSetRemove(port, rlm->_portSet);
            }
        }
        CFRelease(rls);
    }
    if (list != buffer)
        CFAllocatorDeallocate(kCFAllocatorSystemDefault, list);
}

// 释放Observers
static void __CFRunLoopDeallocateObservers(const void *value, void *context)
{
    CFRunLoopModeRef rlm = (CFRunLoopModeRef)value;
    CFRunLoopRef rl = (CFRunLoopRef)context;
    CFIndex idx, cnt;
    const void **list, *buffer[256];
    if (NULL == rlm->_observers)
        return;
    cnt = CFArrayGetCount(rlm->_observers);
    list = (const void **)((cnt <= 256) ? buffer : CFAllocatorAllocate(kCFAllocatorSystemDefault, cnt * sizeof(void *), 0));
    CFArrayGetValues(rlm->_observers, CFRangeMake(0, cnt), list);
    for (idx = 0; idx < cnt; idx++)
    {
        CFRetain(list[idx]);
    }
    CFArrayRemoveAllValues(rlm->_observers);
    for (idx = 0; idx < cnt; idx++)
    {
        __CFRunLoopObserverCancel((CFRunLoopObserverRef)list[idx], rl, rlm);
        CFRelease(list[idx]);
    }
    if (list != buffer)
        CFAllocatorDeallocate(kCFAllocatorSystemDefault, list);
}
// 释放timers
static void __CFRunLoopDeallocateTimers(const void *value, void *context)
{
    CFRunLoopModeRef rlm = (CFRunLoopModeRef)value;
    if (NULL == rlm->_timers)
        return;
    void (^deallocateTimers)(CFMutableArrayRef timers) = ^(CFMutableArrayRef timers) {
      CFIndex idx, cnt;
      const void **list, *buffer[256];
      cnt = CFArrayGetCount(timers);
      list = (const void **)((cnt <= 256) ? buffer : CFAllocatorAllocate(kCFAllocatorSystemDefault, cnt * sizeof(void *), 0));
      CFArrayGetValues(timers, CFRangeMake(0, CFArrayGetCount(timers)), list);
      for (idx = 0; idx < cnt; idx++)
      {
          CFRetain(list[idx]);
      }
      CFArrayRemoveAllValues(timers);
      for (idx = 0; idx < cnt; idx++)
      {
          CFRunLoopTimerRef rlt = (CFRunLoopTimerRef)list[idx];
          __CFRunLoopTimerLock(rlt);
          // if the run loop is deallocating, and since a timer can only be in one
          // run loop, we're going to be removing the timer from all modes, so be
          // a little heavy-handed and direct
          CFSetRemoveAllValues(rlt->_rlModes);
          rlt->_runLoop = NULL;
          __CFRunLoopTimerUnlock(rlt);
          CFRelease(list[idx]);
      }
      if (list != buffer)
          CFAllocatorDeallocate(kCFAllocatorSystemDefault, list);
    };

    if (rlm->_timers && CFArrayGetCount(rlm->_timers))
        deallocateTimers(rlm->_timers);
}

CF_EXPORT CFRunLoopRef _CFRunLoopGet0b(pthread_t t);

// 销毁runloop, 释放其中的所有source,timer,block以及mm
static void __CFRunLoopDeallocate(CFTypeRef cf)
{
    CFRunLoopRef rl = (CFRunLoopRef)cf;

    if (_CFRunLoopGet0b(pthread_main_thread_np()) == cf)
        HALT;

    /* We try to keep the run loop in a valid state as long as possible,
       since sources may have non-retained references to the run loop.
       Another reason is that we don't want to lock the run loop for
       callback reasons, if we can get away without that.  We start by
       eliminating the sources, since they are the most likely to call
       back into the run loop during their "cancellation". Common mode
       items will be removed from the mode indirectly by the following
       three lines. */
    __CFRunLoopSetDeallocating(rl);
    if (NULL != rl->_modes)
    {
        CFSetApplyFunction(rl->_modes, (__CFRunLoopCleanseSources), rl); // remove references to rl
        CFSetApplyFunction(rl->_modes, (__CFRunLoopDeallocateSources), rl);
        CFSetApplyFunction(rl->_modes, (__CFRunLoopDeallocateObservers), rl);
        CFSetApplyFunction(rl->_modes, (__CFRunLoopDeallocateTimers), rl);
    }
    __CFRunLoopLock(rl);
    struct _block_item *item = rl->_blocks_head;
    while (item)
    {
        struct _block_item *curr = item;
        item = item->_next;
        CFRelease(curr->_mode);
        Block_release(curr->_block);
        free(curr);
    }
    if (NULL != rl->_commonModeItems)
    {
        CFRelease(rl->_commonModeItems);
    }
    if (NULL != rl->_commonModes)
    {
        CFRelease(rl->_commonModes);
    }
    if (NULL != rl->_modes)
    {
        CFRelease(rl->_modes);
    }
    __CFPortFree(rl->_wakeUpPort);
    rl->_wakeUpPort = CFPORT_NULL;
    __CFRunLoopPopPerRunData(rl, NULL);
    __CFRunLoopUnlock(rl);
    pthread_mutex_destroy(&rl->_lock);
    memset((char *)cf + sizeof(CFRuntimeBase), 0x8C, sizeof(struct __CFRunLoop) - sizeof(CFRuntimeBase));
}

static const CFRuntimeClass __CFRunLoopModeClass = {
    0,
    "CFRunLoopMode",
    NULL, // init
    NULL, // copy
    __CFRunLoopModeDeallocate,
    __CFRunLoopModeEqual,
    __CFRunLoopModeHash,
    NULL, //
    __CFRunLoopModeCopyDescription};

static const CFRuntimeClass __CFRunLoopClass = {
    0,
    "CFRunLoop",
    NULL, // init
    NULL, // copy
    __CFRunLoopDeallocate,
    NULL,
    NULL,
    NULL, //
    __CFRunLoopCopyDescription};

CF_PRIVATE void __CFFinalizeRunLoop(uintptr_t data);

// 注册runloop runloopMode类
CFTypeID CFRunLoopGetTypeID(void)
{
    static dispatch_once_t initOnce;
    dispatch_once(&initOnce, ^{
      __kCFRunLoopTypeID = _CFRuntimeRegisterClass(&__CFRunLoopClass);
      __kCFRunLoopModeTypeID = _CFRuntimeRegisterClass(&__CFRunLoopModeClass);
    });
    return __kCFRunLoopTypeID;
}

// 根据线程创建对应run loop
// loop 有thread的成员变量.   model
static CFRunLoopRef __CFRunLoopCreate(pthread_t t)
{
    CFRunLoopRef loop = NULL;
    CFRunLoopModeRef rlm;
    uint32_t size = sizeof(struct __CFRunLoop) - sizeof(CFRuntimeBase);
    loop = (CFRunLoopRef)_CFRuntimeCreateInstance(kCFAllocatorSystemDefault, CFRunLoopGetTypeID(), size, NULL);
    if (NULL == loop)
    {
        return NULL;
    }
    //设置标记位
    (void)__CFRunLoopPushPerRunData(loop);
    __CFRunLoopLockInit(&loop->_lock);
    // 初始化唤醒端口
    loop->_wakeUpPort = __CFPortAllocate();

    if (CFPORT_NULL == loop->_wakeUpPort)
        HALT;
    __CFRunLoopSetIgnoreWakeUps(loop);

    loop->_commonModes = CFSetCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeSetCallBacks);

    // 加入defaultMode,初始Mode
    CFSetAddValue(loop->_commonModes, kCFRunLoopDefaultMode);

    loop->_commonModeItems = NULL;
    loop->_currentMode = NULL;
    loop->_modes = CFSetCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeSetCallBacks);
    loop->_blocks_head = NULL;
    loop->_blocks_tail = NULL;
    loop->_counterpart = NULL;
    loop->_pthread = t;
#if DEPLOYMENT_TARGET_WINDOWS
    loop->_winthread = GetCurrentThreadId();
#else
    loop->_winthread = 0;
#endif
    rlm = __CFRunLoopFindMode(loop, kCFRunLoopDefaultMode, true);
    if (NULL != rlm)
        __CFRunLoopModeUnlock(rlm);
    return loop;
}
//全局的Dictionary，key 是 pthread_t， value 是 CFRunLoopRef
static CFMutableDictionaryRef __CFRunLoops = NULL;
// 访问 loopsDic 时的锁, 可以知道__CFRunLoops是线程不安全的
static CFLock_t loopsLock = CFLockInit;

// should only be called by Foundation
// t==0 is a synonym for "main thread" that always works

// 根据线程取得对应的runLoop,如果不存在,会创建
CF_EXPORT CFRunLoopRef _CFRunLoopGet0(pthread_t t)
{
    // 如果线程为nil,就使用主线程
    if (pthread_equal(t, kNilPthreadT))
    {
        t = pthread_main_thread_np();
    }
    __CFLock(&loopsLock);
    if (!__CFRunLoops)
    { //加锁访问字典
        __CFUnlock(&loopsLock);
        // 创建dict,存储线程和runloop
        // 第一次进入时，初始化全局__CFSpinUnlock字典，并先为主线程创建一个 RunLoop。
        CFMutableDictionaryRef dict = CFDictionaryCreateMutable(kCFAllocatorSystemDefault, 0, NULL, &kCFTypeDictionaryValueCallBacks);
        
        CFRunLoopRef mainLoop = __CFRunLoopCreate(pthread_main_thread_np());
        CFDictionarySetValue(dict, pthreadPointer(pthread_main_thread_np()), mainLoop);
        //释放临时创建的变量
        if (!OSAtomicCompareAndSwapPtrBarrier(NULL, dict, (void *volatile *)&__CFRunLoops))
        {
            // 释放dict
            CFRelease(dict);
        }
        // release main loop.
        CFRelease(mainLoop);
        __CFLock(&loopsLock);
    }
    //从字典里面取
    // 从创建的__CFRunLoops中取出RunLoop
    CFRunLoopRef loop = (CFRunLoopRef)CFDictionaryGetValue(__CFRunLoops, pthreadPointer(t));
    __CFUnlock(&loopsLock);
    if (!loop)
    {
        // 如果线程对应的loop不存在,就创建.  然后存储起来.
        CFRunLoopRef newLoop = __CFRunLoopCreate(t);
        __CFLock(&loopsLock);
       
        loop = (CFRunLoopRef)CFDictionaryGetValue(__CFRunLoops, pthreadPointer(t));
        if (!loop)
        {
            //保存到字典, // 存入Runloops
            CFDictionarySetValue(__CFRunLoops, pthreadPointer(t), newLoop);
            loop = newLoop;
        }
        // don't release run loops inside the loopsLock, because CFRunLoopDeallocate may end up taking it
        //释放资源
        __CFUnlock(&loopsLock);
        CFRelease(newLoop);
    }
    // pthread_self获取线程自身的id 如果传入的线程是当前线程
    if (pthread_equal(t, pthread_self()))
    {
        _CFSetTSD(__CFTSDKeyRunLoop, (void *)loop, NULL);
        //
        if (0 == _CFGetTSD(__CFTSDKeyRunLoopCntr))
        {
            // 注册一个回调，当线程销毁时，顺便也销毁其对应的 RunLoop。
            _CFSetTSD(__CFTSDKeyRunLoopCntr, (void *)(PTHREAD_DESTRUCTOR_ITERATIONS - 1), (void (*)(void *))__CFFinalizeRunLoop);
        }
    }
    return loop;
}

// should only be called by Foundation
// 取得线程对应的run loop ,如果不存在,并不会创建新的.
CFRunLoopRef _CFRunLoopGet0b(pthread_t t)
{
    if (pthread_equal(t, kNilPthreadT))
    {
        t = pthread_main_thread_np();
    }
    __CFLock(&loopsLock);
    CFRunLoopRef loop = NULL;
    if (__CFRunLoops)
    {
        loop = (CFRunLoopRef)CFDictionaryGetValue(__CFRunLoops, pthreadPointer(t));
    }
    __CFUnlock(&loopsLock);
    return loop;
}

static void __CFRunLoopRemoveAllSources(CFRunLoopRef rl, CFStringRef modeName);

// Called for each thread as it exits
// 线程退出时调用. 
CF_PRIVATE void __CFFinalizeRunLoop(uintptr_t data)
{
    CFRunLoopRef rl = NULL;
    if (data <= 1)
    {
        __CFLock(&loopsLock);
        if (__CFRunLoops)
        {
            rl = (CFRunLoopRef)CFDictionaryGetValue(__CFRunLoops, pthreadPointer(pthread_self()));
            if (rl)
                CFRetain(rl);
            CFDictionaryRemoveValue(__CFRunLoops, pthreadPointer(pthread_self()));
        }
        __CFUnlock(&loopsLock);
    }
    else
    {
        _CFSetTSD(__CFTSDKeyRunLoopCntr, (void *)(data - 1), (void (*)(void *))__CFFinalizeRunLoop);
    }
    if (rl && CFRunLoopGetMain() != rl)
    { // protect against cooperative threads
        if (NULL != rl->_counterpart)
        {
            CFRelease(rl->_counterpart);
            rl->_counterpart = NULL;
        }
        // purge all sources before deallocation
        CFArrayRef array = CFRunLoopCopyAllModes(rl);
        for (CFIndex idx = CFArrayGetCount(array); idx--;)
        {
            CFStringRef modeName = (CFStringRef)CFArrayGetValueAtIndex(array, idx);
            __CFRunLoopRemoveAllSources(rl, modeName);
        }
        __CFRunLoopRemoveAllSources(rl, kCFRunLoopCommonModes);
        CFRelease(array);
    }
    if (rl)
        CFRelease(rl);
}

// 获取runloop对应的线程
pthread_t _CFRunLoopGet1(CFRunLoopRef rl)
{
    return rl->_pthread;
}

// should only be called by Foundation
// CFTypeRef _counterpart,runloop 对应的counterpart
CF_EXPORT CFTypeRef _CFRunLoopGet2(CFRunLoopRef rl)
{
    CFTypeRef ret = NULL;
    __CFLock(&loopsLock);
    ret = rl->_counterpart;
    __CFUnlock(&loopsLock);
    return ret;
}

// should only be called by Foundation
CF_EXPORT CFTypeRef _CFRunLoopGet2b(CFRunLoopRef rl)
{
    return rl->_counterpart;
}

#if DEPLOYMENT_TARGET_MACOSX
// 设置当前线程的Runloop
void _CFRunLoopSetCurrent(CFRunLoopRef rl)
{
    if (pthread_main_np())
        return; // 主线程,return
    CFRunLoopRef currentLoop = CFRunLoopGetCurrent();
    if (rl != currentLoop)
    {
        CFRetain(currentLoop); // avoid a deallocation of the currentLoop inside the lock
        __CFLock(&loopsLock);
        if (rl)
        {
            CFDictionarySetValue(__CFRunLoops, pthreadPointer(pthread_self()), rl);
        }
        else
        {
            CFDictionaryRemoveValue(__CFRunLoops, pthreadPointer(pthread_self()));
        }
        __CFUnlock(&loopsLock);
        CFRelease(currentLoop);
        _CFSetTSD(__CFTSDKeyRunLoop, NULL, NULL);
        _CFSetTSD(__CFTSDKeyRunLoopCntr, 0, (void (*)(void *))__CFFinalizeRunLoop);
    }
}
#endif

// 获取主线程的runloop
CFRunLoopRef CFRunLoopGetMain(void)
{
    CHECK_FOR_FORK();
    // 创建 main run loop .如果不存在就创建一个
    static CFRunLoopRef __main = NULL; // no retain needed
    // pthread_main_thread_np 获取主线程.
    if (!__main)
        __main = _CFRunLoopGet0(pthread_main_thread_np()); // no CAS needed
    return __main;
}

// 获取当前线程的runloop
CFRunLoopRef CFRunLoopGetCurrent(void)
{
    CHECK_FOR_FORK(); // 什么操作都没有
    // TSD Thread-specific data 线程私有数据
    CFRunLoopRef rl = (CFRunLoopRef)_CFGetTSD(__CFTSDKeyRunLoop);
    if (rl)
        return rl;
        // 获取子线程的runloop,只能在线程内调用
    return _CFRunLoopGet0(pthread_self());
}
//返回当前运行的mode的name
CFStringRef CFRunLoopCopyCurrentMode(CFRunLoopRef rl)
{
    CHECK_FOR_FORK();
    CFStringRef result = NULL;
    __CFRunLoopLock(rl);
    if (NULL != rl->_currentMode)
    {
        result = (CFStringRef)CFRetain(rl->_currentMode->_name);
    }
    __CFRunLoopUnlock(rl);
    return result;
}

// 添加modeName
static void __CFRunLoopGetModeName(const void *value, void *context)
{
    CFRunLoopModeRef rlm = (CFRunLoopModeRef)value;
    CFMutableArrayRef array = (CFMutableArrayRef)context;
    CFArrayAppendValue(array, rlm->_name);
}
// 返回runloop rl的所有modes,字符串 modeName
CFArrayRef CFRunLoopCopyAllModes(CFRunLoopRef rl)
{
    CHECK_FOR_FORK();
    CFMutableArrayRef array;
    __CFRunLoopLock(rl);
    array = CFArrayCreateMutable(kCFAllocatorSystemDefault, CFSetGetCount(rl->_modes), &kCFTypeArrayCallBacks);
    CFSetApplyFunction(rl->_modes, (__CFRunLoopGetModeName), array);
    __CFRunLoopUnlock(rl);
    return array;
}

// 添加item和modelName到Runloop中,item是 source,observer,timer
static void __CFRunLoopAddItemsToCommonMode(const void *value, void *ctx)
{
    CFTypeRef item = (CFTypeRef)value;
    CFRunLoopRef rl = (CFRunLoopRef)(((CFTypeRef *)ctx)[0]);
    CFStringRef modeName = (CFStringRef)(((CFTypeRef *)ctx)[1]);
    if (CFGetTypeID(item) == CFRunLoopSourceGetTypeID())
    { 
        // 
         // item是source,就添加到source"集合"中
        CFRunLoopAddSource(rl, (CFRunLoopSourceRef)item, modeName);
    }
    else if (CFGetTypeID(item) == CFRunLoopObserverGetTypeID())
    {
 // item是observer就添加到observer"数组"中
        CFRunLoopAddObserver(rl, (CFRunLoopObserverRef)item, modeName);
    }
    else if (CFGetTypeID(item) == CFRunLoopTimerGetTypeID())
    {
        // item是timer就添加到timer"数组"中
        CFRunLoopAddTimer(rl, (CFRunLoopTimerRef)item, modeName);
    }
}

static void __CFRunLoopAddIte__CFRunLoopAddItemToCommonModesmToCommonModes(const void *value, void *ctx)
{
    CFStringRef modeName = (CFStringRef)value;
    CFRunLoopRef rl = (CFRunLoopRef)(((CFTypeRef *)ctx)[0]); //取出runloop
    CFTypeRef item = (CFTypeRef)(((CFTypeRef *)ctx)[1]);     //取出添加到item(可能是source、observer、timer)
    //根据类型添加到对应的model中。
    if (CFGetTypeID(item) == CFRunLoopSourceGetTypeID())
    {
        CFRunLoopAddSource(rl, (CFRunLoopSourceRef)item, modeName);
    }
    else if (CFGetTypeID(item) == CFRunLoopObserverGetTypeID())
    {
        CFRunLoopAddObserver(rl, (CFRunLoopObserverRef)item, modeName);
    }
    else if (CFGetTypeID(item) == CFRunLoopTimerGetTypeID())
    {
        CFRunLoopAddTimer(rl, (CFRunLoopTimerRef)item, modeName);
    }
}
// 从指定mode中移除item
static void __CFRunLoopRemoveItemFromCommonModes(const void *value, void *ctx)
{
    CFStringRef modeName = (CFStringRef)value;
    CFRunLoopRef rl = (CFRunLoopRef)(((CFTypeRef *)ctx)[0]);
    CFTypeRef item = (CFTypeRef)(((CFTypeRef *)ctx)[1]);
    if (CFGetTypeID(item) == CFRunLoopSourceGetTypeID())
    {
        CFRunLoopRemoveSource(rl, (CFRunLoopSourceRef)item, modeName);
    }
    else if (CFGetTypeID(item) == CFRunLoopObserverGetTypeID())
    {
        CFRunLoopRemoveObserver(rl, (CFRunLoopObserverRef)item, modeName);
    }
    else if (CFGetTypeID(item) == CFRunLoopTimerGetTypeID())
    {
        CFRunLoopRemoveTimer(rl, (CFRunLoopTimerRef)item, modeName);
    }
}
// runLoop中是否存在某一个mode
CF_EXPORT Boolean _CFRunLoop01(CFRunLoopRef rl, CFStringRef modeName)
{
    __CFRunLoopLock(rl);
    Boolean present = CFSetContainsValue(rl->_commonModes, modeName);
    __CFRunLoopUnlock(rl);
    return present;
}

//向当前RunLoop的common modes中添加一个mode。
void CFRunLoopAddCommonMode(CFRunLoopRef rl, CFStringRef modeName)
{
    CHECK_FOR_FORK();
    // 如果是当前正在析构的runloop，则return
    if (__CFRunLoopIsDeallocating(rl))
        return;
    __CFRunLoopLock(rl);
    // 如果rl 不包含当前mode，则加入
    //rl中是否已经有这个mode，如果有就什么都不做
    if (!CFSetContainsValue(rl->_commonModes, modeName))
    {
        CFSetRef set = rl->_commonModeItems ? CFSetCreateCopy(kCFAllocatorSystemDefault, rl->_commonModeItems) : NULL;
        
        // 把modeName添加到RunLoop的_commonModes![CFRunLoopRun调用链.png](https://upload-images.jianshu.io/upload_images/1055199-a612b6b9ae4c2a9e.png?imageMogr2/auto-orient/strip%7CimageView2/2/w/1240)

        CFSetAddValue(rl->_commonModes, modeName);

        if (NULL != set)
        {
            // 定义一个长度为2的数组context, 第一个元素是runloop，第二个元素是modeName
            CFTypeRef context[2] = {rl, modeName};
            /* add all common-modes items to new mode */
            //__CFRunLoopAddItemsToCommonMode是一个方法，里面会调用CFRunLoopAddSource/CFRunLoopAddObserver/CFRunLoopAddTimer
            //__CFRunLoopFindMode(rl, modeName, true)，CFRunLoopMode对象在这个时候被创建
             // 把commonModeItems数组中的所有Source/Observer/Timer同步到新添加的mode（CFRunLoopModeRef实例）
        
        // 遍历set集合中的每一个元素作为 __CFRunLoopAddItemsToCommonMode 的第一个参数，context 作为第二个参数，调用__CFRunLoopAddItemsToCommonMode
    
            CFSetApplyFunction(set, (__CFRunLoopAddItemsToCommonMode), (void *)context);
            CFRelease(set);
        }
    }
    else
    {
    }
    __CFRunLoopUnlock(rl);
}

static void __CFRUNLOOP_IS_SERVICING_THE_MAIN_DISPATCH_QUEUE__() __attribute__((noinline));
static void __CFRUNLOOP_IS_SERVICING_THE_MAIN_DISPATCH_QUEUE__(void *msg)
{
    _dispatch_main_queue_callback_4CF(msg);
    asm __volatile__(""); // thwart tail-call optimization
}

/// 2. 通知 Observers: 即将触发 xxx 回调。
static void __CFRUNLOOP_IS_CALLING_OUT_TO_AN_OBSERVER_CALLBACK_FUNCTION__() __attribute__((noinline));
//调用观察者的function
static void __CFRUNLOOP_IS_CALLING_OUT_TO_AN_OBSERVER_CALLBACK_FUNCTION__(CFRunLoopObserverCallBack func, CFRunLoopObserverRef observer, CFRunLoopActivity activity, void *info)
{
    if (func)
    {
        func(observer, activity, info);
    }
    asm __volatile__(""); // thwart tail-call optimization
}
//调用计时器的function
static void __CFRUNLOOP_IS_CALLING_OUT_TO_A_TIMER_CALLBACK_FUNCTION__() __attribute__((noinline));
static void __CFRUNLOOP_IS_CALLING_OUT_TO_A_TIMER_CALLBACK_FUNCTION__(CFRunLoopTimerCallBack func, CFRunLoopTimerRef timer, void *info)
{
    if (func)
    {
        func(timer, info);
    }
    asm __volatile__(""); // thwart tail-call optimization
}

// 调用block
static void __CFRUNLOOP_IS_CALLING_OUT_TO_A_BLOCK__() __attribute__((noinline));
static void __CFRUNLOOP_IS_CALLING_OUT_TO_A_BLOCK__(void (^block)(void))
{
    if (block)
    {
        block();
    }
    asm __volatile__(""); // thwart tail-call optimization
}

/// 执行 某一个mode中被加入的block.执行完成后移除和释放block
static Boolean __CFRunLoopDoBlocks(CFRunLoopRef rl, CFRunLoopModeRef rlm)
{ // Call with rl and rlm locked
    //如果头结点没有、或者model不存在则强制返回，什么也不做
    if (!rl->_blocks_head)
        return false;

    if (!rlm || !rlm->_name)
        return false;

    Boolean did = false; //记录其中一个block结点是否被执行过
    struct _block_item *head = rl->_blocks_head;
    struct _block_item *tail = rl->_blocks_tail;
    rl->_blocks_head = NULL;
    rl->_blocks_tail = NULL;
    //取出被标记为common的所有mode、及当前model的name
    CFSetRef commonModes = rl->_commonModes;
    CFStringRef curMode = rlm->_name;
    __CFRunLoopModeUnlock(rlm);
    __CFRunLoopUnlock(rl);
    //定义两个临时变量，用于对保存block链表的遍历
    struct _block_item *prev = NULL;
    struct _block_item *item = head; //记录头指针，从头部开始遍历
    while (item)
    {
        struct _block_item *curr = item;
        item = item->_next;
        Boolean doit = false;
        //从blockitem结构体就知道,其中的_mode只能是CFString 或者CFSet
        //如果block结点保存的model是CFString类型
        if (CFStringGetTypeID() == CFGetTypeID(curr->_mode))
        {
            // 如果 当前item指定的的modeName和传入modeName一致,或者item指定的的modeName是commonMode并且commonModes中包含了传入的mode
            doit = CFEqual(curr->_mode, curMode) || (CFEqual(curr->_mode, kCFRunLoopCommonModes) && CFSetContainsValue(commonModes, curMode));
        }
        else
        {
            //是否执行block只需要满足下面三个条件中的一个
            //1. blockitem 中保存的model是当前的model
            //2. blockitem 中保存的model是标记为kCFRunLoopCommonModes的model
            //3. 当前model保存在commonModes数组
            doit = CFSetContainsValue((CFSetRef)curr->_mode, curMode) || (CFSetContainsValue((CFSetRef)curr->_mode, kCFRunLoopCommonModes) && CFSetContainsValue(commonModes, curMode));
        }
        //如果不执行block,则直接移动当前结点，进行下一个blockitem的判断
        if (!doit)
            prev = curr;
        if (doit)
        {
            //如果执行block,则先移动结点。
            if (prev)
                prev->_next = item;
            if (curr == head)
                head = item;
            if (curr == tail)
                tail = prev;
            void (^block)(void) = curr->_block;
            CFRelease(curr->_mode); // 释放item的Mode
            free(curr); // 释放item
            if (doit)
            {
                //最终在这里执行block，__CFRUNLOOP_IS_CALLING_OUT_TO_A_BLOCK__的函数原型就是调用block
                __CFRUNLOOP_IS_CALLING_OUT_TO_A_BLOCK__(block);
                did = true;
            }
            // doblock执行完,释放block
            Block_release(block); // do this before relocking to prevent deadlocks where some yahoo wants to run the run loop reentrantly from their dealloc
        }
    }
    __CFRunLoopLock(rl);
    __CFRunLoopModeLock(rlm);
    if (head)
    {
        tail->_next = rl->_blocks_head;
        rl->_blocks_head = head;
        if (!rl->_blocks_tail)
            rl->_blocks_tail = tail;
    }
    return did;
}

/* rl is locked, rlm is locked on entrance and exit */
static void __CFRunLoopDoObservers() __attribute__((noinline));
/// 2. 通知 Observers: RunLoop 即将触发 xxx 回调。
static void __CFRunLoopDoObservers(CFRunLoopRef rl, CFRunLoopModeRef rlm, CFRunLoopActivity activity)
{ /* DOES CALLOUT */
    CHECK_FOR_FORK();

    CFIndex cnt = rlm->_observers ? CFArrayGetCount(rlm->_observers) : 0;
    if (cnt < 1)
        return;

    /* Fire the observers */
    STACK_BUFFER_DECL(CFRunLoopObserverRef, buffer, (cnt <= 1024) ? cnt : 1);
    
    CFRunLoopObserverRef *collectedObservers = (cnt <= 1024) ? buffer : (CFRunLoopObserverRef *)malloc(cnt * sizeof(CFRunLoopObserverRef));
    CFIndex obs_cnt = 0;
    for (CFIndex idx = 0; idx < cnt; idx++)
    {
        CFRunLoopObserverRef rlo = (CFRunLoopObserverRef)CFArrayGetValueAtIndex(rlm->_observers, idx);
        if (0 != (rlo->_activities & activity) && __CFIsValid(rlo) && !__CFRunLoopObserverIsFiring(rlo))
        {
            collectedObservers[obs_cnt++] = (CFRunLoopObserverRef)CFRetain(rlo);
        }
    }
    __CFRunLoopModeUnlock(rlm);
    __CFRunLoopUnlock(rl);
    for (CFIndex idx = 0; idx < obs_cnt; idx++)
    {
        CFRunLoopObserverRef rlo = collectedObservers[idx];
        __CFRunLoopObserverLock(rlo);
        if (__CFIsValid(rlo))
        {
            Boolean doInvalidate = !__CFRunLoopObserverRepeats(rlo);
            __CFRunLoopObserverSetFiring(rlo);
            __CFRunLoopObserverUnlock(rlo);
            __CFRUNLOOP_IS_CALLING_OUT_TO_AN_OBSERVER_CALLBACK_FUNCTION__(rlo->_callout, rlo, activity, rlo->_context.info);
            if (doInvalidate)
            {
                // 观察者不可用
                CFRunLoopObserverInvalidate(rlo);
            }
            // 
            __CFRunLoopObserverUnsetFiring(rlo);
        }
        else
        {
            __CFRunLoopObserverUnlock(rlo);
        }
        CFRelease(rlo);
    }
    __CFRunLoopLock(rl);
    __CFRunLoopModeLock(rlm);

    if (collectedObservers != buffer)
        free(collectedObservers);
}

static CFComparisonResult __CFRunLoopSourceComparator(const void *val1, const void *val2, void *context)
{
    CFRunLoopSourceRef o1 = (CFRunLoopSourceRef)val1;
    CFRunLoopSourceRef o2 = (CFRunLoopSourceRef)val2;
    if (o1->_order < o2->_order)
        return kCFCompareLessThan;
    if (o2->_order < o1->_order)
        return kCFCompareGreaterThan;
    return kCFCompareEqualTo;
}

// 获取source0的集合
static void __CFRunLoopCollectSources0(const void *value, void *context)
{
    CFRunLoopSourceRef rls = (CFRunLoopSourceRef)value;
    CFTypeRef *sources = (CFTypeRef *)context;
    if (0 == rls->_context.version0.version && __CFIsValid(rls) && __CFRunLoopSourceIsSignaled(rls))
    {
        if (NULL == *sources)
        {
            *sources = CFRetain(rls);
        }
        else if (CFGetTypeID(*sources) == CFRunLoopSourceGetTypeID())
        {
            CFTypeRef oldrls = *sources;
            *sources = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks);
            CFArrayAppendValue((CFMutableArrayRef)*sources, oldrls);
            CFArrayAppendValue((CFMutableArrayRef)*sources, rls);
            CFRelease(oldrls);
        }
        else
        {
            CFArrayAppendValue((CFMutableArrayRef)*sources, rls);
        }
    }
}

/// 4. 触发 Source  回调。
static void __CFRUNLOOP_IS_CALLING_OUT_TO_A_SOURCE0_PERFORM_FUNCTION__() __attribute__((noinline));
static void __CFRUNLOOP_IS_CALLING_OUT_TO_A_SOURCE0_PERFORM_FUNCTION__(void (*perform)(void *), void *info)
{
    if (perform)
    {
        perform(info);
    }
    asm __volatile__(""); // thwart tail-call optimization
}

static void __CFRUNLOOP_IS_CALLING_OUT_TO_A_SOURCE1_PERFORM_FUNCTION__() __attribute__((noinline));
static void __CFRUNLOOP_IS_CALLING_OUT_TO_A_SOURCE1_PERFORM_FUNCTION__(
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_EMBEDDED_MINI
    void *(*perform)(void *msg, CFIndex size, CFAllocatorRef allocator, void *info),
    mach_msg_header_t *msg, CFIndex size, mach_msg_header_t **reply,
#else
    void (*perform)(void *),
#endif
    void *info)
{
    if (perform)
    {
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_EMBEDDED_MINI
        *reply = perform(msg, size, kCFAllocatorSystemDefault, info);
#else
        perform(info);
#endif
    }
    asm __volatile__(""); // thwart tail-call optimization
}

/* rl is locked, rlm is locked on entrance and exit */
static Boolean __CFRunLoopDoSources0(CFRunLoopRef rl, CFRunLoopModeRef rlm, Boolean stopAfterHandle) __attribute__((noinline));
// 触发所有source0回调
static Boolean __CFRunLoopDoSources0(CFRunLoopRef rl, CFRunLoopModeRef rlm, Boolean stopAfterHandle)
{ /* DOES CALLOUT */
    CHECK_FOR_FORK();
    CFTypeRef sources = NULL;
    Boolean sourceHandled = false;

    /* Fire the version 0 sources */
    if (NULL != rlm->_sources0 && 0 < CFSetGetCount(rlm->_sources0))
    {
        CFSetApplyFunction(rlm->_sources0, (__CFRunLoopCollectSources0), &sources);
    }
    if (NULL != sources)
    {
        __CFRunLoopModeUnlock(rlm);
        __CFRunLoopUnlock(rl);
        // sources is either a single (retained) CFRunLoopSourceRef or an array of (retained) CFRunLoopSourceRef
        if (CFGetTypeID(sources) == CFRunLoopSourceGetTypeID())
        {
            CFRunLoopSourceRef rls = (CFRunLoopSourceRef)sources;
            __CFRunLoopSourceLock(rls);
            if (__CFRunLoopSourceIsSignaled(rls))
            {
                __CFRunLoopSourceUnsetSignaled(rls);
                if (__CFIsValid(rls))
                {
                    __CFRunLoopSourceUnlock(rls);
                    __CFRUNLOOP_IS_CALLING_OUT_TO_A_SOURCE0_PERFORM_FUNCTION__(rls->_context.version0.perform, rls->_context.version0.info);
                    CHECK_FOR_FORK();
                    sourceHandled = true;
                }
                else
                {
                    __CFRunLoopSourceUnlock(rls);
                }
            }
            else
            {
                __CFRunLoopSourceUnlock(rls);
            }
        }
        else
        {
            CFIndex cnt = CFArrayGetCount((CFArrayRef)sources);
            CFArraySortValues((CFMutableArrayRef)sources, CFRangeMake(0, cnt), (__CFRunLoopSourceComparator), NULL);
            for (CFIndex idx = 0; idx < cnt; idx++)
            {
                CFRunLoopSourceRef rls = (CFRunLoopSourceRef)CFArrayGetValueAtIndex((CFArrayRef)sources, idx);
                __CFRunLoopSourceLock(rls);
                if (__CFRunLoopSourceIsSignaled(rls))
                {
                    __CFRunLoopSourceUnsetSignaled(rls);
                    if (__CFIsValid(rls))
                    {
                        __CFRunLoopSourceUnlock(rls);
                        __CFRUNLOOP_IS_CALLING_OUT_TO_A_SOURCE0_PERFORM_FUNCTION__(rls->_context.version0.perform, rls->_context.version0.info);
                        CHECK_FOR_FORK();
                        sourceHandled = true;
                    }
                    else
                    {
                        __CFRunLoopSourceUnlock(rls);
                    }
                }
                else
                {
                    __CFRunLoopSourceUnlock(rls);
                }
                if (stopAfterHandle && sourceHandled)
                {
                    break;
                }
            }
        }
        CFRelease(sources);
        __CFRunLoopLock(rl);
        __CFRunLoopModeLock(rlm);
    }
    return sourceHandled;
}

CF_INLINE void __CFRunLoopDebugInfoForRunLoopSource(CFRunLoopSourceRef rls)
{
}

// msg, size and reply are unused on Windows
static Boolean __CFRunLoopDoSource1() __attribute__((noinline));
static Boolean __CFRunLoopDoSource1(CFRunLoopRef rl, CFRunLoopModeRef rlm, CFRunLoopSourceRef rls
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_EMBEDDED_MINI
                                    ,
                                    mach_msg_header_t *msg, CFIndex size, mach_msg_header_t **reply
#endif
)
{ /* DOES CALLOUT */
    CHECK_FOR_FORK();
    Boolean sourceHandled = false;

    /* Fire a version 1 source */
    CFRetain(rls);
    __CFRunLoopModeUnlock(rlm);
    __CFRunLoopUnlock(rl);
    __CFRunLoopSourceLock(rls);
    if (__CFIsValid(rls))
    {
        __CFRunLoopSourceUnsetSignaled(rls);
        __CFRunLoopSourceUnlock(rls);
        __CFRunLoopDebugInfoForRunLoopSource(rls);
        __CFRUNLOOP_IS_CALLING_OUT_TO_A_SOURCE1_PERFORM_FUNCTION__(rls->_context.version1.perform,
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_EMBEDDED_MINI
                                                                   msg, size, reply,
#endif
                                                                   rls->_context.version1.info);
        CHECK_FOR_FORK();
        sourceHandled = true;
    }
    else
    {
        if (_LogCFRunLoop)
        {
            CFLog(kCFLogLevelDebug, CFSTR("%p (%s) __CFRunLoopDoSource1 rls %p is invalid"), CFRunLoopGetCurrent(), *_CFGetProgname(), rls);
        }
        __CFRunLoopSourceUnlock(rls);
    }
    CFRelease(rls);
    __CFRunLoopLock(rl);
    __CFRunLoopModeLock(rlm);
    return sourceHandled;
}

static CFIndex __CFRunLoopInsertionIndexInTimerArray(CFArrayRef array, CFRunLoopTimerRef rlt) __attribute__((noinline));
//获取array中插入timer的index,但是并不插入
static CFIndex __CFRunLoopInsertionIndexInTimerArray(CFArrayRef array, CFRunLoopTimerRef rlt)
{
    CFIndex cnt = CFArrayGetCount(array);
    if (cnt <= 0)
    {
        return 0;
    }
    if (256 < cnt)
    {
        CFRunLoopTimerRef item = (CFRunLoopTimerRef)CFArrayGetValueAtIndex(array, cnt - 1);
        // 按照_fireTSR升序排列
        if (item->_fireTSR <= rlt->_fireTSR)
        {
            return cnt;
        }
        item = (CFRunLoopTimerRef)CFArrayGetValueAtIndex(array, 0);
        if (rlt->_fireTSR < item->_fireTSR)
        {
            return 0;
        }
    }

    CFIndex add = (1 << flsl(cnt)) * 2;
    CFIndex idx = 0;
    Boolean lastTestLEQ;
    // 二分查找
    do
    {
        add = add / 2;
        lastTestLEQ = false;
        CFIndex testIdx = idx + add;
        if (testIdx < cnt)
        {
            CFRunLoopTimerRef item = (CFRunLoopTimerRef)CFArrayGetValueAtIndex(array, testIdx);
            if (item->_fireTSR <= rlt->_fireTSR)
            {
                idx = testIdx;
                lastTestLEQ = true;
            }
        }
    } while (0 < add);

    return lastTestLEQ ? idx + 1 : idx;
}

// 看看计时器的列表。我们将计算两个TSR值：下一个软截止时间和下一个硬截止时间。
        // 下一个软截止日期是我们第一次可以启动任何定时器的时间。这是我们排序的定时器列表中第一个定时器的启动日期。
        // 下一个硬最后期限是我们可以在超出列表中计时器的允许容忍度之前启动计时器的最后时间。
static void __CFArmNextTimerInMode(CFRunLoopModeRef rlm, CFRunLoopRef rl)
{
    uint64_t nextHardDeadline = UINT64_MAX;
    uint64_t nextSoftDeadline = UINT64_MAX;

    if (rlm->_timers)
    {
        // Look at the list of timers. We will calculate two TSR values; the next soft and next hard deadline.
        // The next soft deadline is the first time we can fire any timer. This is the fire date of the first timer in our sorted list of timers.
        // The next hard deadline is the last time at which we can fire the timer before we've moved out of the allowable tolerance of the timers in our list.
        for (CFIndex idx = 0, cnt = CFArrayGetCount(rlm->_timers); idx < cnt; idx++)
        {
            CFRunLoopTimerRef t = (CFRunLoopTimerRef)CFArrayGetValueAtIndex(rlm->_timers, idx);
            // discount timers currently firing
            if (__CFRunLoopTimerIsFiring(t))
                continue;

            int32_t err = CHECKINT_NO_ERROR;
            uint64_t oneTimerSoftDeadline = t->_fireTSR;
            uint64_t oneTimerHardDeadline = check_uint64_add(t->_fireTSR, __CFTimeIntervalToTSR(t->_tolerance), &err);
            if (err != CHECKINT_NO_ERROR)
                oneTimerHardDeadline = UINT64_MAX;

            // We can stop searching if the soft deadline for this timer exceeds the current hard deadline. Otherwise, later timers with lower tolerance could still have earlier hard deadlines.
            if (oneTimerSoftDeadline > nextHardDeadline)
            {
                break;
            }

            if (oneTimerSoftDeadline < nextSoftDeadline)
            {
                nextSoftDeadline = oneTimerSoftDeadline;
            }

            if (oneTimerHardDeadline < nextHardDeadline)
            {
                nextHardDeadline = oneTimerHardDeadline;
            }
        }

        if (nextSoftDeadline < UINT64_MAX && (nextHardDeadline != rlm->_timerHardDeadline || nextSoftDeadline != rlm->_timerSoftDeadline))
        {
            if (CFRUNLOOP_NEXT_TIMER_ARMED_ENABLED())
            {
                CFRUNLOOP_NEXT_TIMER_ARMED((unsigned long)(nextSoftDeadline - mach_absolute_time()));
            }
#if USE_DISPATCH_SOURCE_FOR_TIMERS
            // We're going to hand off the range of allowable timer fire date to dispatch and let it fire when appropriate for the system.
            uint64_t leeway = __CFTSRToNanoseconds(nextHardDeadline - nextSoftDeadline);
            dispatch_time_t deadline = __CFTSRToDispatchTime(nextSoftDeadline);
#if USE_MK_TIMER_TOO
            if (leeway > 0)
            {
                // Only use the dispatch timer if we have any leeway
                // <rdar://problem/14447675>

                // Cancel the mk timer
                if (rlm->_mkTimerArmed && rlm->_timerPort)
                {
                    AbsoluteTime dummy;
                    mk_timer_cancel(rlm->_timerPort, &dummy);
                    rlm->_mkTimerArmed = false;
                }

                // Arm the dispatch timer
                _dispatch_source_set_runloop_timer_4CF(rlm->_timerSource, deadline, DISPATCH_TIME_FOREVER, leeway);
                rlm->_dispatchTimerArmed = true;
            }
            else
            {
                // Cancel the dispatch timer
                if (rlm->_dispatchTimerArmed)
                {
                    // Cancel the dispatch timer
                    _dispatch_source_set_runloop_timer_4CF(rlm->_timerSource, DISPATCH_TIME_FOREVER, DISPATCH_TIME_FOREVER, 888);
                    rlm->_dispatchTimerArmed = false;
                }

                // Arm the mk timer
                if (rlm->_timerPort)
                {
                    mk_timer_arm(rlm->_timerPort, __CFUInt64ToAbsoluteTime(nextSoftDeadline));
                    rlm->_mkTimerArmed = true;
                }
            }
#else
            _dispatch_source_set_runloop_timer_4CF(rlm->_timerSource, deadline, DISPATCH_TIME_FOREVER, leeway);
#endif
#else
            if (rlm->_timerPort)
            {
                mk_timer_arm(rlm->_timerPort, __CFUInt64ToAbsoluteTime(nextSoftDeadline));
            }
#endif
        }
        else if (nextSoftDeadline == UINT64_MAX)
        {
            // Disarm the timers - there is no timer scheduled

            if (rlm->_mkTimerArmed && rlm->_timerPort)
            {
                AbsoluteTime dummy;
                mk_timer_cancel(rlm->_timerPort, &dummy);
                rlm->_mkTimerArmed = false;
            }

#if USE_DISPATCH_SOURCE_FOR_TIMERS
            if (rlm->_dispatchTimerArmed)
            {
                _dispatch_source_set_runloop_timer_4CF(rlm->_timerSource, DISPATCH_TIME_FOREVER, DISPATCH_TIME_FOREVER, 333);
                rlm->_dispatchTimerArmed = false;
            }
#endif
        }
    }
    rlm->_timerHardDeadline = nextHardDeadline;
    rlm->_timerSoftDeadline = nextSoftDeadline;
}

// call with rlm and its run loop locked, and the TSRLock locked; rlt not locked; returns with same state
static void __CFRepositionTimerInMode(CFRunLoopModeRef rlm, CFRunLoopTimerRef rlt, Boolean isInArray) __attribute__((noinline));
// 复位timer,移除tiemr并且重新插入
// 调用时，RLM和它的运行循环被锁定，TSRLock被锁定；RLT没有被锁定；以相同的状态返回。
static void __CFRepositionTimerInMode(CFRunLoopModeRef rlm, CFRunLoopTimerRef rlt, Boolean isInArray)
{
    if (!rlt)
        return;

    CFMutableArrayRef timerArray = rlm->_timers;
    if (!timerArray)
        return;
    Boolean found = false;

    // If we know in advance that the timer is not in the array (just being added now) then we can skip this search
    if (isInArray)
    {
        CFIndex idx = CFArrayGetFirstIndexOfValue(timerArray, CFRangeMake(0, CFArrayGetCount(timerArray)), rlt);
        if (kCFNotFound != idx)
        {
            CFRetain(rlt);
            CFArrayRemoveValueAtIndex(timerArray, idx);
            found = true;
        }
    }
    if (!found && isInArray)
        return;
    CFIndex newIdx = __CFRunLoopInsertionIndexInTimerArray(timerArray, rlt);
    CFArrayInsertValueAtIndex(timerArray, newIdx, rlt);
    __CFArmNextTimerInMode(rlm, rlt->_runLoop);
    if (isInArray)
        CFRelease(rlt);
}

// 执行某一个timer item
// mode and rl are locked on entry and exit
static Boolean __CFRunLoopDoTimer(CFRunLoopRef rl, CFRunLoopModeRef rlm, CFRunLoopTimerRef rlt)
{ /* DOES CALLOUT */
    Boolean timerHandled = false;
    uint64_t oldFireTSR = 0;

    /* Fire a timer */
    CFRetain(rlt);
    __CFRunLoopTimerLock(rlt);

    if (__CFIsValid(rlt) && rlt->_fireTSR <= mach_absolute_time() && !__CFRunLoopTimerIsFiring(rlt) && rlt->_runLoop == rl)
    {
        void *context_info = NULL;
        void (*context_release)(const void *) = NULL;
        // 如果context 是retain的
        if (rlt->_context.retain)
        {
            context_info = (void *)rlt->_context.retain(rlt->_context.info);
            context_release = rlt->_context.release;
        }
        else
        {
            context_info = rlt->_context.info;
        }
        Boolean doInvalidate = (0.0 == rlt->_interval);
        __CFRunLoopTimerSetFiring(rlt);
        // 万一下一个定时器和这个定时器有完全相同的截止时间，我们重新设置这些值，这样arm下一个定时器代码就可以正确地找到列表中的下一个定时器，并arm下一个定时器。

        // Just in case the next timer has exactly the same deadlines as this one, we reset these values so that the arm next timer code can correctly find the next timer in the list and arm the underlying timer.
        rlm->_timerSoftDeadline = UINT64_MAX;
        rlm->_timerHardDeadline = UINT64_MAX;
        __CFRunLoopTimerUnlock(rlt);
        __CFRunLoopTimerFireTSRLock();
        oldFireTSR = rlt->_fireTSR;
        __CFRunLoopTimerFireTSRUnlock();

        __CFArmNextTimerInMode(rlm, rl);

        __CFRunLoopModeUnlock(rlm);
        __CFRunLoopUnlock(rl);
        __CFRUNLOOP_IS_CALLING_OUT_TO_A_TIMER_CALLBACK_FUNCTION__(rlt->_callout, rlt, context_info);
        CHECK_FOR_FORK();
        if (doInvalidate)
        {
            CFRunLoopTimerInvalidate(rlt); /* DOES CALLOUT */
        }
        if (context_release)
        {
            context_release(context_info);
        }
        __CFRunLoopLock(rl);
        __CFRunLoopModeLock(rlm);
        __CFRunLoopTimerLock(rlt);
        timerHandled = true;
        __CFRunLoopTimerUnsetFiring(rlt);
    }
    if (__CFIsValid(rlt) && timerHandled)
    {
        /* This is just a little bit tricky: we want to support calling
         * CFRunLoopTimerSetNextFireDate() from within the callout and
         * honor that new time here if it is a later date, otherwise
         * it is completely ignored. */
        if (oldFireTSR < rlt->_fireTSR)
        {
            /* Next fire TSR was set, and set to a date after the previous
            * fire date, so we honor it. */
            __CFRunLoopTimerUnlock(rlt);
            // The timer was adjusted and repositioned, during the
            // callout, but if it was still the min timer, it was
            // skipped because it was firing.  Need to redo the
            // min timer calculation in case rlt should now be that
            // timer instead of whatever was chosen.
            __CFArmNextTimerInMode(rlm, rl);
        }
        else
        {
            uint64_t nextFireTSR = 0LL;
            uint64_t intervalTSR = 0LL;
            if (rlt->_interval <= 0.0)
            {
            }
            else if (TIMER_INTERVAL_LIMIT < rlt->_interval)
            {
                intervalTSR = __CFTimeIntervalToTSR(TIMER_INTERVAL_LIMIT);
            }
            else
            {
                intervalTSR = __CFTimeIntervalToTSR(rlt->_interval);
            }
            if (LLONG_MAX - intervalTSR <= oldFireTSR)
            {
                nextFireTSR = LLONG_MAX;
            }
            else
            {
                if (intervalTSR == 0)
                {
                    // 15304159: Make sure we don't accidentally loop forever here
                    CRSetCrashLogMessage("A CFRunLoopTimer with an interval of 0 is set to repeat");
                    HALT;
                }
                uint64_t currentTSR = mach_absolute_time();
                nextFireTSR = oldFireTSR;
                while (nextFireTSR <= currentTSR)
                {
                    nextFireTSR += intervalTSR;
                }
            }
            CFRunLoopRef rlt_rl = rlt->_runLoop;
            if (rlt_rl)
            {
                CFRetain(rlt_rl);
                CFIndex cnt = CFSetGetCount(rlt->_rlModes);
                STACK_BUFFER_DECL(CFTypeRef, modes, cnt);
                CFSetGetValues(rlt->_rlModes, (const void **)modes);
                // To avoid A->B, B->A lock ordering issues when coming up
                // towards the run loop from a source, the timer has to be
                // unlocked, which means we have to protect from object
                // invalidation, although that's somewhat expensive.
                for (CFIndex idx = 0; idx < cnt; idx++)
                {
                    CFRetain(modes[idx]);
                }
                __CFRunLoopTimerUnlock(rlt);
                for (CFIndex idx = 0; idx < cnt; idx++)
                {
                    CFStringRef name = (CFStringRef)modes[idx];
                    modes[idx] = (CFTypeRef)__CFRunLoopFindMode(rlt_rl, name, false);
                    CFRelease(name);
                }
                __CFRunLoopTimerFireTSRLock();
                rlt->_fireTSR = nextFireTSR;
                rlt->_nextFireDate = CFAbsoluteTimeGetCurrent() + __CFTimeIntervalUntilTSR(nextFireTSR);
                for (CFIndex idx = 0; idx < cnt; idx++)
                {
                    CFRunLoopModeRef rlm = (CFRunLoopModeRef)modes[idx];
                    if (rlm)
                    {
                        __CFRepositionTimerInMode(rlm, rlt, true);
                    }
                }
                __CFRunLoopTimerFireTSRUnlock();
                for (CFIndex idx = 0; idx < cnt; idx++)
                {
                    __CFRunLoopModeUnlock((CFRunLoopModeRef)modes[idx]);
                }
                CFRelease(rlt_rl);
            }
            else
            {
                __CFRunLoopTimerUnlock(rlt);
                __CFRunLoopTimerFireTSRLock();
                rlt->_fireTSR = nextFireTSR;
                rlt->_nextFireDate = CFAbsoluteTimeGetCurrent() + __CFTimeIntervalUntilTSR(nextFireTSR);
                __CFRunLoopTimerFireTSRUnlock();
            }
        }
    }
    else
    {
        __CFRunLoopTimerUnlock(rlt);
    }
    CFRelease(rlt);
    return timerHandled;
}

// rl and rlm are locked on entry and exit
static Boolean __CFRunLoopDoTimers(CFRunLoopRef rl, CFRunLoopModeRef rlm, uint64_t limitTSR)
{ /* DOES CALLOUT */
    Boolean timerHandled = false;
    CFMutableArrayRef timers = NULL;
    for (CFIndex idx = 0, cnt = rlm->_timers ? CFArrayGetCount(rlm->_timers) : 0; idx < cnt; idx++)
    {
        CFRunLoopTimerRef rlt = (CFRunLoopTimerRef)CFArrayGetValueAtIndex(rlm->_timers, idx);

        if (__CFIsValid(rlt) && !__CFRunLoopTimerIsFiring(rlt))
        {
            if (rlt->_fireTSR <= limitTSR)
            {
                if (!timers)
                    timers = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks);
                CFArrayAppendValue(timers, rlt);
            }
        }
    }

    for (CFIndex idx = 0, cnt = timers ? CFArrayGetCount(timers) : 0; idx < cnt; idx++)
    {
        CFRunLoopTimerRef rlt = (CFRunLoopTimerRef)CFArrayGetValueAtIndex(timers, idx);
        Boolean did = __CFRunLoopDoTimer(rl, rlm, rlt);
        timerHandled = timerHandled || did;
    }
    if (timers)
        CFRelease(timers);
    return timerHandled;
}

// runloop中的某一个mode是否完成.根据是否runloopmode是否为空判断
CF_EXPORT Boolean _CFRunLoopFinished(CFRunLoopRef rl, CFStringRef modeName)
{
    CHECK_FOR_FORK();
    CFRunLoopModeRef rlm;
    Boolean result = false;
    __CFRunLoopLock(rl);
    rlm = __CFRunLoopFindMode(rl, modeName, false);
    if (NULL == rlm || __CFRunLoopModeIsEmpty(rl, rlm, NULL))
    {
        result = true;
    }
    if (rlm)
        __CFRunLoopModeUnlock(rlm);
    __CFRunLoopUnlock(rl);
    return result;
}

static int32_t __CFRunLoopRun(CFRunLoopRef rl, CFRunLoopModeRef rlm, CFTimeInterval seconds, Boolean stopAfterHandle, CFRunLoopModeRef previousMode) __attribute__((noinline));

#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_EMBEDDED_MINI

#define TIMEOUT_INFINITY (~(mach_msg_timeout_t)0)

static Boolean __CFRunLoopServiceMachPort(mach_port_name_t port, mach_msg_header_t **buffer, size_t buffer_size, mach_port_t *livePort, mach_msg_timeout_t timeout, voucher_mach_msg_state_t *voucherState, voucher_t *voucherCopy)
{
    Boolean originalBuffer = true;
    kern_return_t ret = KERN_SUCCESS;
    for (;;)
    { /* In that sleep of death what nightmares may come ... */
        mach_msg_header_t *msg = (mach_msg_header_t *)*buffer;
        msg->msgh_bits = 0;
        msg->msgh_local_port = port;
        msg->msgh_remote_port = MACH_PORT_NULL;
        msg->msgh_size = buffer_size;
        msg->msgh_id = 0;
        if (TIMEOUT_INFINITY == timeout)
        {
            CFRUNLOOP_SLEEP();
        }
        else
        {
            CFRUNLOOP_POLL();
        }
        ret = mach_msg(msg, MACH_RCV_MSG | (voucherState ? MACH_RCV_VOUCHER : 0) | MACH_RCV_LARGE | ((TIMEOUT_INFINITY != timeout) ? MACH_RCV_TIMEOUT : 0) | MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0) | MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_AV), 0, msg->msgh_size, port, timeout, MACH_PORT_NULL);

        // Take care of all voucher-related work right after mach_msg.
        // If we don't release the previous voucher we're going to leak it.
        voucher_mach_msg_revert(*voucherState);

        // Someone will be responsible for calling voucher_mach_msg_revert. This call makes the received voucher the current one.
        *voucherState = voucher_mach_msg_adopt(msg);

        if (voucherCopy)
        {
            if (*voucherState != VOUCHER_MACH_MSG_STATE_UNCHANGED)
            {
                // Caller requested a copy of the voucher at this point. By doing this right next to mach_msg we make sure that no voucher has been set in between the return of mach_msg and the use of the voucher copy.
                // CFMachPortBoost uses the voucher to drop importance explicitly. However, we want to make sure we only drop importance for a new voucher (not unchanged), so we only set the TSD when the voucher is not state_unchanged.
                *voucherCopy = voucher_copy();
            }
            else
            {
                *voucherCopy = NULL;
            }
        }

        CFRUNLOOP_WAKEUP(ret);
        if (MACH_MSG_SUCCESS == ret)
        {
            *livePort = msg ? msg->msgh_local_port : MACH_PORT_NULL;
            return true;
        }
        if (MACH_RCV_TIMED_OUT == ret)
        {
            if (!originalBuffer)
                free(msg);
            *buffer = NULL;
            *livePort = MACH_PORT_NULL;
            return false;
        }
        if (MACH_RCV_TOO_LARGE != ret)
            break;
        buffer_size = round_msg(msg->msgh_size + MAX_TRAILER_SIZE);
        if (originalBuffer)
            *buffer = NULL;
        originalBuffer = false;
        *buffer = realloc(*buffer, buffer_size);
    }
    HALT;
    return false;
}

#elif DEPLOYMENT_TARGET_WINDOWS

#define TIMEOUT_INFINITY INFINITE

// pass in either a portSet or onePort
static Boolean __CFRunLoopWaitForMultipleObjects(__CFPortSet portSet, HANDLE *onePort, DWORD timeout, DWORD mask, HANDLE *livePort, Boolean *msgReceived)
{
    DWORD waitResult = WAIT_TIMEOUT;
    HANDLE handleBuf[MAXIMUM_WAIT_OBJECTS];
    HANDLE *handles = NULL;
    uint32_t handleCount = 0;
    Boolean freeHandles = false;
    Boolean result = false;

    if (portSet)
    {
        // copy out the handles to be safe from other threads at work
        handles = __CFPortSetGetPorts(portSet, handleBuf, MAXIMUM_WAIT_OBJECTS, &handleCount);
        freeHandles = (handles != handleBuf);
    }
    else
    {
        handles = onePort;
        handleCount = 1;
        freeHandles = FALSE;
    }

    // The run loop mode and loop are already in proper unlocked state from caller
    waitResult = MsgWaitForMultipleObjectsEx(__CFMin(handleCount, MAXIMUM_WAIT_OBJECTS), handles, timeout, mask, MWMO_INPUTAVAILABLE);

    CFAssert2(waitResult != WAIT_FAILED, __kCFLogAssertion, "%s(): error %d from MsgWaitForMultipleObjects", __PRETTY_FUNCTION__, GetLastError());

    if (waitResult == WAIT_TIMEOUT)
    {
        // do nothing, just return to caller
        result = false;
    }
    else if (waitResult >= WAIT_OBJECT_0 && waitResult < WAIT_OBJECT_0 + handleCount)
    {
        // a handle was signaled
        if (livePort)
            *livePort = handles[waitResult - WAIT_OBJECT_0];
        result = true;
    }
    else if (waitResult == WAIT_OBJECT_0 + handleCount)
    {
        // windows message received
        if (msgReceived)
            *msgReceived = true;
        result = true;
    }
    else if (waitResult >= WAIT_ABANDONED_0 && waitResult < WAIT_ABANDONED_0 + handleCount)
    {
        // an "abandoned mutex object"
        if (livePort)
            *livePort = handles[waitResult - WAIT_ABANDONED_0];
        result = true;
    }
    else
    {
        CFAssert2(waitResult == WAIT_FAILED, __kCFLogAssertion, "%s(): unexpected result from MsgWaitForMultipleObjects: %d", __PRETTY_FUNCTION__, waitResult);
        result = false;
    }

    if (freeHandles)
    {
        CFAllocatorDeallocate(kCFAllocatorSystemDefault, handles);
    }

    return result;
}

#endif

struct __timeout_context
{
    dispatch_source_t ds;
    CFRunLoopRef rl;
    uint64_t termTSR;
};

static void __CFRunLoopTimeoutCancel(void *arg)
{
    struct __timeout_context *context = (struct __timeout_context *)arg;
    CFRelease(context->rl);
    dispatch_release(context->ds);
    free(context);
}

// runloop 超时,timeout
static void __CFRunLoopTimeout(void *arg)
{
    struct __timeout_context *context = (struct __timeout_context *)arg;
    context->termTSR = 0ULL;
    CFRUNLOOP_WAKEUP_FOR_TIMEOUT();
    CFRunLoopWakeUp(context->rl);
    // The interval is DISPATCH_TIME_FOREVER, so this won't fire again
    // The interval is DISPATCH_TIME_FOREVER, so this won't fire again。因为runloop的执行时长是forever，所有runloop永远不会超时，也就说函数__CFRunLoopTimeout永远不会执行到。
}

/* rl, rlm are locked on entrance and exit */
/**
 *  运行run loop
 *
 *  @param rl              运行的RunLoop对象
 *  @param rlm             运行的mode
 *  @param seconds         run loop超时时间
 *  @param stopAfterHandle true:run loop处理完事件就退出  false:一直运行直到超时或者被手动终止
 *  @param previousMode    上一次运行的mode
 *
 *  @return 返回4种状态
 */
/**RunLoop的运行的最核心函数（进入和退出时runloop和runloopMode都会被加锁）
 * rl: 运行的runloop
 * rlm: runloop Mode
 * seconds: runloop超时时间
 * stopAfterHandle: 处理完时间后runloop是否stop，默认为false
 * previousMode: runloop上次运行的mode
 */
static int32_t __CFRunLoopRun(CFRunLoopRef rl, CFRunLoopModeRef rlm, CFTimeInterval seconds, Boolean stopAfterHandle, CFRunLoopModeRef previousMode)
{
    //获取系统启动后的CPU运行时间，用于控制超时时间
    // 获取基于系统启动后的时钟"嘀嗒"数，其单位是纳秒
    uint64_t startTSR = mach_absolute_time();
    //如果RunLoop或者mode是stop状态，则直接return，不进入循环
    // 状态判断
    if (__CFRunLoopIsStopped(rl))
    {
        __CFRunLoopUnsetStopped(rl);
        return kCFRunLoopRunStopped;
    }
    else if (rlm->_stopped)
    {
        rlm->_stopped = false;
        return kCFRunLoopRunStopped;
    }

// 获取主线程接收消息的port备用。如果runLoop是mainRunLoop且后续内核唤醒的port等于主线程接收消息的port，主线程就处理这个消息

    //声明 dispatchPort 变量，作为一个 mach_port 通信的端口，初始化值为 MACH_PORT_NULL
    //mach端口，在内核中，消息在端口之间传递。 初始为0
    mach_port_name_t dispatchPort = MACH_PORT_NULL;

    // 检测是否在主线程 && ( (是队列发的消息&&mode为null)||(不是队列发的消息&&不在主队列))
    Boolean libdispatchQSafe = pthread_main_np() && ((HANDLE_DISPATCH_ON_BASE_INVOCATION_ONLY && NULL == previousMode) || (!HANDLE_DISPATCH_ON_BASE_INVOCATION_ONLY && 0 == _CFGetTSD(__CFTSDKeyIsInGCDMainQ)));
    //如果是队列安全的，并且是主线程runloop,设置它对应的通信端口
    //如果在主线程 && runloop是主线程的runloop && 该mode是commonMode，则给mach端口赋值为主线程收发消息的端口
    if (libdispatchQSafe && (CFRunLoopGetMain() == rl) && CFSetContainsValue(rl->_commonModes, rlm->_name))
        dispatchPort = _dispatch_get_main_queue_port_4CF();

#if USE_DISPATCH_SOURCE_FOR_TIMERS
    // 初始化获取timer的port(source1)
    // 如果这个port和mach_msg发消息的livePort相等则说明timer时间到了，处理timer
    mach_port_name_t modeQueuePort = MACH_PORT_NULL;
    if (rlm->_queue)
    {
        modeQueuePort = _dispatch_runloop_root_queue_get_port_4CF(rlm->_queue);
        if (!modeQueuePort)
        {
            CRASH("Unable to get port for run loop mode queue (%d)", -1);
        }
    }
#endif
    //GCD管理的定时器，用于实现runloop超时机制
    // 使用GCD实现runloop超时功能
    dispatch_source_t timeout_timer = NULL;
    struct __timeout_context *timeout_context = (struct __timeout_context *)malloc(sizeof(*timeout_context));
     // seconds是设置的runloop超时时间，一般为1.0e10，11.574万年，所以不会超时
    //立即超时
    if (seconds <= 0.0)
    { // instant timeout
        seconds = 0.0;
        timeout_context->termTSR = 0ULL;
    } //seconds为超时时间，超时时执行__CFRunLoopTimeout函数
    else if (seconds <= TIMER_INTERVAL_LIMIT)
    {
        dispatch_queue_t queue = pthread_main_np() ? __CFDispatchQueueGetGenericMatchingMain() : __CFDispatchQueueGetGenericBackground();
        timeout_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);
        dispatch_retain(timeout_timer);
        timeout_context->ds = timeout_timer;
        timeout_context->rl = (CFRunLoopRef)CFRetain(rl);
         // 设置超时的时间点（从现在开始 + 允许运行的时长）
        timeout_context->termTSR = startTSR + __CFTimeIntervalToTSR(seconds);
        dispatch_set_context(timeout_timer, timeout_context); // source gets ownership of context
        dispatch_source_set_event_handler_f(timeout_timer, __CFRunLoopTimeout);
        dispatch_source_set_cancel_handler_f(timeout_timer, __CFRunLoopTimeoutCancel);
        uint64_t ns_at = (uint64_t)((__CFTSRToTimeInterval(startTSR) + seconds) * 1000000000ULL);
        dispatch_source_set_timer(timeout_timer, dispatch_time(1, ns_at), DISPATCH_TIME_FOREVER, 1000ULL);
        dispatch_resume(timeout_timer);
    }
    else
    { // infinite timeout
        //永不超时
        seconds = 9999999999.0;
        timeout_context->termTSR = UINT64_MAX;
    }

    Boolean didDispatchPortLastTime = true;
    //记录最后runloop状态，用于return
    // returnValue 标识runloop状态，如果returnValue不为0就不退出。
    // returnValue可能的值：
    // enum {
    //     kCFRunLoopRunFinished = 1,
    //     kCFRunLoopRunStopped = 2,
    //     kCFRunLoopRunTimedOut = 3,
    //     kCFRunLoopRunHandledSource = 4
    // };
    int32_t retVal = 0;
    do
    {
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_EMBEDDED_MINI
        voucher_mach_msg_state_t voucherState = VOUCHER_MACH_MSG_STATE_UNCHANGED;
        voucher_t voucherCopy = NULL;
#endif
        //初始化一个存放内核消息的缓冲池 // 消息缓冲区，用户缓存内核发的消息
        uint8_t msg_buffer[3 * 1024];
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_EMBEDDED_MINI
        // 消息缓冲区指针，用于指向msg_buffer
        mach_msg_header_t *msg = NULL;
        // 用于保存被内核唤醒的端口（调用mach_msg函数时会把livePort地址传进去供内核写数据）
        mach_port_t livePort = MACH_PORT_NULL;
#elif DEPLOYMENT_TARGET_WINDOWS
        HANDLE livePort = NULL;
        Boolean windowsMessageReceived = false;
#endif
        //取所有需要监听的port
        __CFPortSet waitSet = rlm->_portSet;
        //设置RunLoop为可以被唤醒状态
        __CFRunLoopUnsetIgnoreWakeUps(rl);

        // 2. 通知 Observers: RunLoop 即将触发 Timer 回调。
        // __CFRunLoopDoObservers内部会调用__CFRUNLOOP_IS_CALLING_OUT_TO_AN_OBSERVER_CALLBACK_FUNCTION__这个函数，这个函数的参数包括observer的回调函数、observer、runloop状态
        if (rlm->_observerMask & kCFRunLoopBeforeTimers)
            __CFRunLoopDoObservers(rl, rlm, kCFRunLoopBeforeTimers);
        if (rlm->_observerMask & kCFRunLoopBeforeSources)
         // 3. 通知 Observers: RunLoop 即将触发 Source0 (非port) 回调。
            __CFRunLoopDoObservers(rl, rlm, kCFRunLoopBeforeSources);
        //执行加入当前runloop的block
        // 外部通过调用CFRunLoopPerformBlock函数向当前runloop增加block。新增加的block保存咋runloop.blocks_head链表里。
        // __CFRunLoopDoBlocks会遍历链表取出每一个block，如果block被指定执行的mode和当前的mode一致，则调用__CFRUNLOOP_IS_CALLING_OUT_TO_A_BLOCK__执行之
    
        __CFRunLoopDoBlocks(rl, rlm);

        // 4. RunLoop 触发 Source0 (非port) 回调
        // __CFRunLoopDoSources0函数内部会调用__CFRUNLOOP_IS_CALLING_OUT_TO_A_SOURCE0_PERFORM_FUNCTION__函数
        // __CFRUNLOOP_IS_CALLING_OUT_TO_A_SOURCE0_PERFORM_FUNCTION__函数会调用source0的perform回调函数，即rls->context.version0.perform
        
        //有事件处理返回true，没有事件返回false
        Boolean sourceHandledThisLoop = __CFRunLoopDoSources0(rl, rlm, stopAfterHandle);
        if (sourceHandledThisLoop)
        {
             // 如果rl处理了source0事件，那再处理source0之后的block
            //执行加入当前runloop的block
            __CFRunLoopDoBlocks(rl, rlm);
        }

        //如果没有Sources0事件处理 并且 没有超时，poll为false
        //如果有Sources0事件处理 或者 超时，poll都为true
        // 标记是否需要轮询，如果处理了source0则轮询，否则休眠
        Boolean poll = sourceHandledThisLoop || (0ULL == timeout_context->termTSR);
    
        //第一次do..whil循环不会走该分支，因为didDispatchPortLastTime初始化是true
        if (MACH_PORT_NULL != dispatchPort && !didDispatchPortLastTime)
        {
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_EMBEDDED_MINI
            //从缓冲区读取消息
            msg = (mach_msg_header_t *)msg_buffer;
            //5.接收dispatchPort端口的消息，（接收source1事件）
            // 5. 如果有 Source1 (基于port的source) 处于 ready 状态，直接处理这个 Source1 然后跳转到第9步去处理消息。
            // __CFRunLoopServiceMachPort函数内部调用了mach_msg，mach_msg函数会监听内核给端口发送的消息
            // 如果mach_msg监听到消息就会执行goto跳转去处理这个消息
            // 第五个参数为0代表不休眠立即返回
            if (__CFRunLoopServiceMachPort(dispatchPort, &msg, sizeof(msg_buffer), &livePort, 0, &voucherState, NULL))
            {
                //如果接收到了消息的话，前往第9步开始处理msg
                goto handle_msg;
            }
#elif DEPLOYMENT_TARGET_WINDOWS
            if (__CFRunLoopWaitForMultipleObjects(NULL, &dispatchPort, 0, 0, &livePort, NULL))
            {
                goto handle_msg;
            }
#endif
        }

        didDispatchPortLastTime = false;
        //6.通知观察者RunLoop即将进入休眠
        // 6. 通知 Observers: RunLoop 的线程即将进入休眠(sleep)。
        // 根据上面第4步是否处理过source0，来判断如果也没有source1消息的时候是否让线程进入睡眠
        if (!poll && (rlm->_observerMask & kCFRunLoopBeforeWaiting))
            __CFRunLoopDoObservers(rl, rlm, kCFRunLoopBeforeWaiting);
        //设置RunLoop为休眠状态
        __CFRunLoopSetSleeping(rl);
        // do not do any user callouts after this point (after notifying of sleeping)

        // Must push the local-to-this-activation ports in on every loop
        // iteration, as this mode could be run re-entrantly and we don't
        // want these ports to get serviced.
        // 通知进入休眠状态后，不要做任何用户级回调
        __CFPortSetInsert(dispatchPort, waitSet);

        __CFRunLoopModeUnlock(rlm);
        __CFRunLoopUnlock(rl);
        // 标记休眠开始时间
        CFAbsoluteTime sleepStart = poll ? 0.0 : CFAbsoluteTimeGetCurrent();

#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_EMBEDDED_MINI
#if USE_DISPATCH_SOURCE_FOR_TIMERS

        //这里有个内循环，用于接收等待端口的消息
        //进入此循环后，线程进入休眠，直到收到新消息才跳出该循环，继续执行run loop
        do
        {
            if (kCFUseCollectableAllocator)
            {
                // objc_clear_stack(0);
                // <rdar://problem/16393959>
                memset(msg_buffer, 0, sizeof(msg_buffer));
            }
            msg = (mach_msg_header_t *)msg_buffer;
             // 7. __CFRunLoopServiceMachPort内部调用mach_msg函数等待接受mach_port的消息。随即线程将进入休眠，等待被唤醒。 以下事件会会唤醒runloop:
            // mach_msg接收到来自内核的消息。本质上是内核向我们的port发送了一条消息。即收到一个基于port的Source事件（source1）。
            // 一个timer的时间到了（处理timer）
            // RunLoop自身的超时时间到了（几乎不可能）
            // 被其他调用者手动唤醒（source0）

            //7.接收waitSet端口的消息
            __CFRunLoopServiceMachPort(waitSet, &msg, sizeof(msg_buffer), &livePort, poll ? 0 : TIMEOUT_INFINITY, &voucherState, &voucherCopy);
            //收到消息之后，livePort的值为msg->msgh_local_port，
            if (modeQueuePort != MACH_PORT_NULL && livePort == modeQueuePort)
            {
                // Drain the internal queue. If one of the callout blocks sets the timerFired flag, break out and service the timer.
                while (_dispatch_runloop_root_queue_perform_4CF(rlm->_queue))
                    ;
                if (rlm->_timerFired)
                {
                    // Leave livePort as the queue port, and service timers below
                    rlm->_timerFired = false;
                    break;
                }
                else
                {
                    if (msg && msg != (mach_msg_header_t *)msg_buffer)
                        free(msg);
                }
            }
            else
            {
                // Go ahead and leave the inner loop.
                break;
            }
        } while (1);
#else
        if (kCFUseCollectableAllocator)
        {
            // objc_clear_stack(0);
            // <rdar://problem/16393959>
            memset(msg_buffer, 0, sizeof(msg_buffer));
        }
        msg = (mach_msg_header_t *)msg_buffer;
        __CFRunLoopServiceMachPort(waitSet, &msg, sizeof(msg_buffer), &livePort, poll ? 0 : TIMEOUT_INFINITY, &voucherState, &voucherCopy);
#endif

#elif DEPLOYMENT_TARGET_WINDOWS
        // Here, use the app-supplied message queue mask. They will set this if they are interested in having this run loop receive windows messages.
        __CFRunLoopWaitForMultipleObjects(waitSet, NULL, poll ? 0 : TIMEOUT_INFINITY, rlm->_msgQMask, &livePort, &windowsMessageReceived);
#endif

        __CFRunLoopLock(rl);
        __CFRunLoopModeLock(rlm);

        // 计算线程沉睡的时长
        rl->_sleepTime += (poll ? 0.0 : (CFAbsoluteTimeGetCurrent() - sleepStart));

        // Must remove the local-to-this-activation ports in on every loop
        // iteration, as this mode could be run re-entrantly and we don't
        // want these ports to get serviced. Also, we don't want them left
        // in there if this function returns.

        __CFPortSetRemove(dispatchPort, waitSet);

        __CFRunLoopSetIgnoreWakeUps(rl);
        //取消runloop的休眠状态   runloop置为唤醒状态
        // user callouts now OK again
        __CFRunLoopUnsetSleeping(rl);
        //8.通知观察者runloop被唤醒
         // 8. 通知 Observers: RunLoop对应的线程刚被唤醒。
        if (!poll && (rlm->_observerMask & kCFRunLoopAfterWaiting))
            __CFRunLoopDoObservers(rl, rlm, kCFRunLoopAfterWaiting);
        //9.处理收到的消息
        // 9. 收到&处理source1消息（第5步的goto会到达这里开始处理source1）
    handle_msg:;
     // 忽略端口唤醒runloop，避免在处理source1时通过其他线程或进程唤醒runloop(保证线程安全)
        __CFRunLoopSetIgnoreWakeUps(rl);

#if DEPLOYMENT_TARGET_WINDOWS
        if (windowsMessageReceived)
        {
            // These Win32 APIs cause a callout, so make sure we're unlocked first and relocked after
            __CFRunLoopModeUnlock(rlm);
            __CFRunLoopUnlock(rl);

            if (rlm->_msgPump)
            {
                rlm->_msgPump();
            }
            else
            {
                MSG msg;
                if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE | PM_NOYIELD))
                {
                    TranslateMessage(&msg);
                    DispatchMessage(&msg);
                }
            }

            __CFRunLoopLock(rl);
            __CFRunLoopModeLock(rlm);
            sourceHandledThisLoop = true;

            // To prevent starvation of sources other than the message queue, we check again to see if any other sources need to be serviced
            // Use 0 for the mask so windows messages are ignored this time. Also use 0 for the timeout, because we're just checking to see if the things are signalled right now -- we will wait on them again later.
            // NOTE: Ignore the dispatch source (it's not in the wait set anymore) and also don't run the observers here since we are polling.
            __CFRunLoopSetSleeping(rl);
            __CFRunLoopModeUnlock(rlm);
            __CFRunLoopUnlock(rl);

            __CFRunLoopWaitForMultipleObjects(waitSet, NULL, 0, 0, &livePort, NULL);

            __CFRunLoopLock(rl);
            __CFRunLoopModeLock(rlm);
            __CFRunLoopUnsetSleeping(rl);
            // If we have a new live port then it will be handled below as normal
        }

#endif
        if (MACH_PORT_NULL == livePort)
        {
             // livePort为null则什么也不做
            CFRUNLOOP_WAKEUP_FOR_NOTHING();
            // handle nothing
        } //通过CFRunloopWake唤醒
        else if (livePort == rl->_wakeUpPort)
        {
            // livePort为wakeUpPort则只需要简单的唤醒runloop（rl->_wakeUpPort是专门用来唤醒runloop的）
            CFRUNLOOP_WAKEUP_FOR_WAKEUP();
            // do nothing on Mac OS //什么都不干，跳回2重新循环
#if DEPLOYMENT_TARGET_WINDOWS
            // Always reset the wake up port, or risk spinning forever
            ResetEvent(rl->_wakeUpPort);
#endif
        }
#if USE_DISPATCH_SOURCE_FOR_TIMERS
        //如果是定时器事件
        else if (modeQueuePort != MACH_PORT_NULL && livePort == modeQueuePort)
        {
            CFRUNLOOP_WAKEUP_FOR_TIMER();
            //9.1 处理timer事件
             // 9.1 如果一个 Timer 到时间了，触发这个Timer的回调
            // __CFRunLoopDoTimers返回值代表是否处理了这个timer
            if (!__CFRunLoopDoTimers(rl, rlm, mach_absolute_time()))
            {
                // Re-arm the next timer, because we apparently fired early
                __CFArmNextTimerInMode(rlm, rl);
            }
        }
#endif
#if USE_MK_TIMER_TOO
        //如果是定时器事件
        else if (rlm->_timerPort != MACH_PORT_NULL && livePort == rlm->_timerPort)
        {
            
            CFRUNLOOP_WAKEUP_FOR_TIMER();
            // On Windows, we have observed an issue where the timer port is set before the time which we requested it to be set. For example, we set the fire time to be TSR 167646765860, but it is actually observed firing at TSR 167646764145, which is 1715 ticks early. The result is that, when __CFRunLoopDoTimers checks to see if any of the run loop timers should be firing, it appears to be 'too early' for the next timer, and no timers are handled.
            // In this case, the timer port has been automatically reset (since it was returned from MsgWaitForMultipleObjectsEx), and if we do not re-arm it, then no timers will ever be serviced again unless something adjusts the timer list (e.g. adding or removing timers). The fix for the issue is to reset the timer here if CFRunLoopDoTimers did not handle a timer itself. 9308754
       
            if (!__CFRunLoopDoTimers(rl, rlm, mach_absolute_time()))
            {
                // Re-arm the next timer
                __CFArmNextTimerInMode(rlm, rl);
            }
        }
#endif
        //如果是dispatch到main queue的block
        else if (livePort == dispatchPort)
        {
    
            CFRUNLOOP_WAKEUP_FOR_DISPATCH();
            __CFRunLoopModeUnlock(rlm);
            __CFRunLoopUnlock(rl);
            _CFSetTSD(__CFTSDKeyIsInGCDMainQ, (void *)6, NULL);
#if DEPLOYMENT_TARGET_WINDOWS
            void *msg = 0;
#endif
 /// 9.2 如果有dispatch到main_queue的block，执行block（也就是处理GCD通过port提交到主线程的事件）。
            __CFRUNLOOP_IS_SERVICING_THE_MAIN_DISPATCH_QUEUE__(msg);
            _CFSetTSD(__CFTSDKeyIsInGCDMainQ, (void *)0, NULL);
            __CFRunLoopLock(rl);
            __CFRunLoopModeLock(rlm);
            sourceHandledThisLoop = true;
            didDispatchPortLastTime = true;
        }
        else
        {
            CFRUNLOOP_WAKEUP_FOR_SOURCE();


            // If we received a voucher from this mach_msg, then put a copy of the new voucher into TSD. CFMachPortBoost will look in the TSD for the voucher. By using the value in the TSD we tie the CFMachPortBoost to this received mach_msg explicitly without a chance for anything in between the two pieces of code to set the voucher again.
            voucher_t previousVoucher = _CFSetTSD(__CFTSDKeyMachMessageHasVoucher, (void *)voucherCopy, os_release);

        /// 9.3 如果一个 Source1 (基于port) 发出事件了，处理这个事件
            // 根据livePort获取source（不需要name，从mode->_portToV1SourceMap字典中以port作为key即可取到source）
            
            // Despite the name, this works for windows handles as well
            CFRunLoopSourceRef rls = __CFRunLoopModeFindSourceForMachPort(rl, rlm, livePort);
            // 有source1事件待处理
            if (rls)
            {
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_EMBEDDED_MINI
                mach_msg_header_t *reply = NULL;
                // 处理source1事件（触发source1的回调）
                // runloop 触发source1的回调，__CFRunLoopDoSource1内部会调用__CFRUNLOOP_IS_CALLING_OUT_TO_A_SOURCE1_PERFORM_FUNCTION__
                
                sourceHandledThisLoop = __CFRunLoopDoSource1(rl, rlm, rls, msg, msg->msgh_size, &reply) || sourceHandledThisLoop;
                if (NULL != reply)
                {
                    // 如果__CFRunLoopDoSource1响应的数据reply不为空则通过mach_msg 再send给内核
                    //9.2 处理source1事件
                    (void)mach_msg(reply, MACH_SEND_MSG, reply->msgh_size, 0, MACH_PORT_NULL, 0, MACH_PORT_NULL);
                    CFAllocatorDeallocate(kCFAllocatorSystemDefault, reply);
                }
#elif DEPLOYMENT_TARGET_WINDOWS
                sourceHandledThisLoop = __CFRunLoopDoSource1(rl, rlm, rls) || sourceHandledThisLoop;
#endif
            }

            // Restore the previous voucher
            _CFSetTSD(__CFTSDKeyMachMessageHasVoucher, previousVoucher, os_release);
        }
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_EMBEDDED_MINI
        if (msg && msg != (mach_msg_header_t *)msg_buffer)
            free(msg);
#endif

/// 执行加入到Loop的block
        __CFRunLoopDoBlocks(rl, rlm);

        if (sourceHandledThisLoop && stopAfterHandle)
        {
            //进入run loop时传入的参数，处理完事件就返回
            retVal = kCFRunLoopRunHandledSource;
        }
        else if (timeout_context->termTSR < mach_absolute_time())
        {
            /// 超出传入参数标记的超时时间了
            //run loop超时
            retVal = kCFRunLoopRunTimedOut;
        }
        else if (__CFRunLoopIsStopped(rl))
        {
             /// 被外部调用者强制停止了
            //run loop被手动终止
            __CFRunLoopUnsetStopped(rl);
            retVal = kCFRunLoopRunStopped;
        }
        else if (rlm->_stopped)
        {
            //mode被终止
            // 调用了_CFRunLoopStopMode将mode停止了
            rlm->_stopped = false;
            retVal = kCFRunLoopRunStopped;
        }
        else if (__CFRunLoopModeIsEmpty(rl, rlm, previousMode))
        {
             // source/timer/observer一个都没有了
            //mode中没有要处理的事件
            retVal = kCFRunLoopRunFinished;
        }

#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_EMBEDDED_MINI
        voucher_mach_msg_revert(voucherState);
        os_release(voucherCopy);
#endif

    // 如果retVal不是0，即未超时，mode不是空，loop也没被停止，那继续loop
        //除了上面这几种情况，都继续循环
    } while (0 == retVal);

    if (timeout_timer)
    {
        dispatch_source_cancel(timeout_timer);
        dispatch_release(timeout_timer);
    }
    else
    {
        free(timeout_context);
    }

    return retVal;
}
/// RunLoop的实现
SInt32 CFRunLoopRunSpecific(CFRunLoopRef rl, CFStringRef modeName, CFTimeInterval seconds, Boolean returnAfterSourceHandled)
{ /* DOES CALLOUT */
    CHECK_FOR_FORK();
    if (__CFRunLoopIsDeallocating(rl))
        return kCFRunLoopRunFinished;
    __CFRunLoopLock(rl);

    // 根据指定的modeName获取指定的mode，也就是将要运行的mode
    //根据modeName找到本次运行的mode
    CFRunLoopModeRef currentMode = __CFRunLoopFindMode(rl, modeName, false);
    //如果没找到 || mode中没有注册任何事件，则就此停止，不进入循环
    // 出现以下情况就不会return finish：
    // 1>.将要运行的mode不为空
    // 以下这几条是在__CFRunLoopModeIsEmpty函数中判断的:
    // 2>.将要运行的currentMode是source0、source1、timer任一个不为空
    // 3>.待执行的block的mode和将要运行的mode相同
    // 4>.待执行的block的mode是commonMode且待运行的mode包含在commonMode中
    // 5>.待执行的block的mode包含待运行的mode
    // 6>.待执行的block的mode包含commonMode且待运行的mode包含在commonMode中
    // 所谓待执行的block是外部(开发者)通过调用CFRunLoopPerformBlock函数添加到runloop中的

    if (NULL == currentMode || __CFRunLoopModeIsEmpty(rl, currentMode, rl->_currentMode))
    {
        Boolean did = false;
        if (currentMode)
            __CFRunLoopModeUnlock(currentMode);
        __CFRunLoopUnlock(rl);
        return did ? kCFRunLoopRunHandledSource : kCFRunLoopRunFinished;
    }
    volatile _per_run_data *previousPerRun = __CFRunLoopPushPerRunData(rl);
    //取上一次运行的mode
    CFRunLoopModeRef previousMode = rl->_currentMode;

    rl->_currentMode = currentMode;
    //初始化一个result为kCFRunLoopRunFinished
    int32_t result = kCFRunLoopRunFinished;

    /// 1. 通知 Observers: RunLoop 即将进入 loop。
     // 1.通知observer即将进入runloop
  // 这里使用currentMode->_observerMask 和 kCFRunLoopEntry按位与操作
  // 如果按位与的结果不是0则说明即将进入runloop
  // 而currentMode->_observerMask是个什么东西呢？
  // currentMode->_observerMask本质上是Int类型的变量，标识当前mode的CFRunLoopActivity状态
  // 那么currentMode->_observerMask是在哪里赋值的呢？
  // 调用CFRunLoopAddObserver函数向runloop添加observer的时候会把observer的activities按位或赋值给mode->_observerMask
    if (currentMode->_observerMask & kCFRunLoopEntry)
        __CFRunLoopDoObservers(rl, currentMode, kCFRunLoopEntry);

    // 2. runloop run ,直到return返回
    // RunLoop的运行的最核心函数
    result = __CFRunLoopRun(rl, currentMode, seconds, returnAfterSourceHandled, previousMode);
    //10.通知observer已退出runloop
    if (currentMode->_observerMask & kCFRunLoopExit)
        __CFRunLoopDoObservers(rl, currentMode, kCFRunLoopExit);

    __CFRunLoopModeUnlock(currentMode);
    __CFRunLoopPopPerRunData(rl, previousPerRun);
    rl->_currentMode = previousMode;
    __CFRunLoopUnlock(rl);
    return result;
}
/// 用DefaultMode启动
void CFRunLoopRun(void)
{ /* DOES CALLOUT */
    int32_t result;
    do
    {
        //默认在kCFRunLoopDefaultMode下运行runloop
        result = CFRunLoopRunSpecific(CFRunLoopGetCurrent(), kCFRunLoopDefaultMode, 1.0e10, false);
        CHECK_FOR_FORK();
    } while (kCFRunLoopRunStopped != result && kCFRunLoopRunFinished != result);
}
/// 用指定的Mode启动，允许设置RunLoop超时时间
SInt32 CFRunLoopRunInMode(CFStringRef modeName, CFTimeInterval seconds, Boolean returnAfterSourceHandled)
{ /* DOES CALLOUT */
    CHECK_FOR_FORK();
    return CFRunLoopRunSpecific(CFRunLoopGetCurrent(), modeName, seconds, returnAfterSourceHandled);
}

CFAbsoluteTime CFRunLoopGetNextTimerFireDate(CFRunLoopRef rl, CFStringRef modeName)
{
    CHECK_FOR_FORK();
    __CFRunLoopLock(rl);
    CFRunLoopModeRef rlm = __CFRunLoopFindMode(rl, modeName, false);
    CFAbsoluteTime at = 0.0;
    CFRunLoopTimerRef nextTimer = (rlm && rlm->_timers && 0 < CFArrayGetCount(rlm->_timers)) ? (CFRunLoopTimerRef)CFArrayGetValueAtIndex(rlm->_timers, 0) : NULL;
    if (nextTimer)
    {
        at = CFRunLoopTimerGetNextFireDate(nextTimer);
    }
    if (rlm)
        __CFRunLoopModeUnlock(rlm);
    __CFRunLoopUnlock(rl);
    return at;
}

Boolean CFRunLoopIsWaiting(CFRunLoopRef rl)
{
    CHECK_FOR_FORK();
    return __CFRunLoopIsSleeping(rl);
}

void CFRunLoopWakeUp(CFRunLoopRef rl)
{
    CHECK_FOR_FORK();
    // This lock is crucial to ignorable wakeups, do not remove it.
    __CFRunLoopLock(rl);
    if (__CFRunLoopIsIgnoringWakeUps(rl))
    {
        __CFRunLoopUnlock(rl);
        return;
    }
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_EMBEDDED_MINI
    kern_return_t ret;
    /* We unconditionally try to send the message, since we don't want
     * to lose a wakeup, but the send may fail if there is already a
     * wakeup pending, since the queue length is 1. */
    // __CFSendTrivialMachMessage内部调用mach_msg函数向runloop的wakeUpPort发送消息以唤醒runloop
    ret = __CFSendTrivialMachMessage(rl->_wakeUpPort, 0, MACH_SEND_TIMEOUT, 0);
    if (ret != MACH_MSG_SUCCESS && ret != MACH_SEND_TIMED_OUT)
        CRASH("*** Unable to send message to wake up port. (%d) ***", ret);
#elif DEPLOYMENT_TARGET_WINDOWS
    SetEvent(rl->_wakeUpPort);
#endif
    __CFRunLoopUnlock(rl);
}

//调用了CFRunLoopStop代表runloop被强制终止了。即便调用了CFRunLoopWakeUp，当前的runloop也永远不会被唤醒了**。因为CFRunLoopStop函数内部调用了_ _CFRunLoopSetStopped函数。而__CFRunLoopSetStopped的实现是rl->_perRunData->stopped = 0x53544F50; // 'STOP'。加之CFRunLoopWakeUp函数中通过调用__CFRunLoopIsIgnoringWakeUps(rl)检查了rl->_perRunData->stopped的值是否为true，

void CFRunLoopStop(CFRunLoopRef rl)
{
    Boolean doWake = false;
    CHECK_FOR_FORK();
    __CFRunLoopLock(rl);
    if (rl->_currentMode)
    {
        __CFRunLoopSetStopped(rl);
        doWake = true;
    }
    __CFRunLoopUnlock(rl);
    if (doWake)
    {
        CFRunLoopWakeUp(rl);
    }
}

CF_EXPORT void _CFRunLoopStopMode(CFRunLoopRef rl, CFStringRef modeName)
{
    CHECK_FOR_FORK();
    CFRunLoopModeRef rlm;
    __CFRunLoopLock(rl);
    rlm = __CFRunLoopFindMode(rl, modeName, true);
    if (NULL != rlm)
    {
        rlm->_stopped = true;
        __CFRunLoopModeUnlock(rlm);
    }
    __CFRunLoopUnlock(rl);
    CFRunLoopWakeUp(rl);
}

CF_EXPORT Boolean _CFRunLoopModeContainsMode(CFRunLoopRef rl, CFStringRef modeName, CFStringRef candidateContainedName)
{
    CHECK_FOR_FORK();
    return false;
}

/* 让runloop执行某个block
 * 本质上是把block插入到一个由runloop维护的block对象组成的链表中，在runloop运行中取出链表里被指定在当前mode下运行的block，逐一执行。
 */
void CFRunLoopPerformBlock(CFRunLoopRef rl, CFTypeRef mode, void (^block)(void))
{
    CHECK_FOR_FORK();
    if (CFStringGetTypeID() == CFGetTypeID(mode))
    {
        mode = CFStringCreateCopy(kCFAllocatorSystemDefault, (CFStringRef)mode);
        __CFRunLoopLock(rl);
        // ensure mode exists
        // 根据modeName找mode
        CFRunLoopModeRef currentMode = __CFRunLoopFindMode(rl, (CFStringRef)mode, true);
        if (currentMode)
            __CFRunLoopModeUnlock(currentMode);
        __CFRunLoopUnlock(rl);
    }
    else if (CFArrayGetTypeID() == CFGetTypeID(mode))
    {
        //mode是modeName的容器.
        CFIndex cnt = CFArrayGetCount((CFArrayRef)mode);
        const void **values = (const void **)malloc(sizeof(const void *) * cnt);
        CFArrayGetValues((CFArrayRef)mode, CFRangeMake(0, cnt), values);
        mode = CFSetCreate(kCFAllocatorSystemDefault, values, cnt, &kCFTypeSetCallBacks);
        __CFRunLoopLock(rl);
        // ensure modes exist
        for (CFIndex idx = 0; idx < cnt; idx++)
        {
            CFRunLoopModeRef currentMode = __CFRunLoopFindMode(rl, (CFStringRef)values[idx], true);
            if (currentMode)
                __CFRunLoopModeUnlock(currentMode);
        }
        __CFRunLoopUnlock(rl);
        free(values);
    }
    else if (CFSetGetTypeID() == CFGetTypeID(mode))
    {
        CFIndex cnt = CFSetGetCount((CFSetRef)mode);
        const void **values = (const void **)malloc(sizeof(const void *) * cnt);
        CFSetGetValues((CFSetRef)mode, values);
        mode = CFSetCreate(kCFAllocatorSystemDefault, values, cnt, &kCFTypeSetCallBacks);
        __CFRunLoopLock(rl);
        // ensure modes exist
        for (CFIndex idx = 0; idx < cnt; idx++)
        {
            CFRunLoopModeRef currentMode = __CFRunLoopFindMode(rl, (CFStringRef)values[idx], true);
            if (currentMode)
                __CFRunLoopModeUnlock(currentMode);
        }
        __CFRunLoopUnlock(rl);
        free(values);
    }
    else
    {
        mode = NULL;
    }
    block = Block_copy(block);
    if (!mode || !block)
    {
        if (mode)
            CFRelease(mode);
        if (block)
            Block_release(block);
        return;
    }
    __CFRunLoopLock(rl);
    struct _block_item *new_item = (struct _block_item *)malloc(sizeof(struct _block_item));
    new_item->_next = NULL;
    new_item->_mode = mode;
    new_item->_block = block;
    if (!rl->_blocks_tail)
    {
        rl->_blocks_head = new_item;
    }
    else
    {
        rl->_blocks_tail->_next = new_item;
    }
    rl->_blocks_tail = new_item;
    __CFRunLoopUnlock(rl);
}

Boolean CFRunLoopContainsSource(CFRunLoopRef rl, CFRunLoopSourceRef rls, CFStringRef modeName)
{
    CHECK_FOR_FORK();
    CFRunLoopModeRef rlm;
    Boolean hasValue = false;
    __CFRunLoopLock(rl);
    if (modeName == kCFRunLoopCommonModes)
    {
        if (NULL != rl->_commonModeItems)
        {
            hasValue = CFSetContainsValue(rl->_commonModeItems, rls);
        }
    }
    else
    {
        rlm = __CFRunLoopFindMode(rl, modeName, false);
        if (NULL != rlm)
        {
            hasValue = (rlm->_sources0 ? CFSetContainsValue(rlm->_sources0, rls) : false) || (rlm->_sources1 ? CFSetContainsValue(rlm->_sources1, rls) : false);
            __CFRunLoopModeUnlock(rlm);
        }
    }
    __CFRunLoopUnlock(rl);
    return hasValue;
}

/*添加source事件,
1. 如果指定的mode是commonMode,就添加到_commonModeItems中
*/
void CFRunLoopAddSource(CFRunLoopRef rl, CFRunLoopSourceRef rls, CFStringRef modeName)
{ /* DOES CALLOUT */
    CHECK_FOR_FORK();
    if (__CFRunLoopIsDeallocating(rl))
        return;
    if (!__CFIsValid(rls))
        return;
    Boolean doVer0Callout = false;
    __CFRunLoopLock(rl);
    //如果是kCFRunLoopCommonModes
    if (modeName == kCFRunLoopCommonModes)
    {
        //如果runloop的_commonModes存在，则copy一个新的复制给set
        CFSetRef set = rl->_commonModes ? CFSetCreateCopy(kCFAllocatorSystemDefault, rl->_commonModes) : NULL;
        // 初始化创建commonModeItems（如果_commonModeItems为空）
        if (NULL == rl->_commonModeItems)
        {
            //先初始化
            rl->_commonModeItems = CFSetCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeSetCallBacks);
        }
        //把传入的CFRunLoopSourceRef加入_commonModeItems
        CFSetAddValue(rl->_commonModeItems, rls);
        //如果刚才set copy到的数组里有数据
        if (NULL != set)
        {
            // 创建一个长度为2的数组，分别存储runloop和runloopSource
            CFTypeRef context[2] = {rl, rls};

        // 添加新的item也就是runloopSource到所有的commonMode中
        // set是commonMode集合，CFSetApplyFunction遍历set，添加runloopSource到所有被标记为commonMode的mode->source0(或source1)中

            /* add new item to all common-modes */
            //则把set里的所有mode都执行一遍__CFRunLoopAddItemToCommonModes函数
            CFSetApplyFunction(set, (__CFRunLoopAddItemToCommonModes), (void *)context);
            CFRelease(set);
        }
    }
    //以上分支的逻辑就是，如果你往kCFRunLoopCommonModes里面添加一个source，那么所有_commonModes里的mode都会添加这个source
    else
    {
        // 走到这里说明modeName不是commonMode
        // 根据modeName和runloop获取runloop的mode
        CFRunLoopModeRef rlm = __CFRunLoopFindMode(rl, modeName, true);
        if (NULL != rlm && NULL == rlm->_sources0)
        {
            // 初始化创建runloopMode的source0 & source1这个集合(如果为空)
            //如果_sources0不存在，则初始化_sources0，_sources0和_portToV1SourceMap
            rlm->_sources0 = CFSetCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeSetCallBacks);
            rlm->_sources1 = CFSetCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeSetCallBacks);
            rlm->_portToV1SourceMap = CFDictionaryCreateMutable(kCFAllocatorSystemDefault, 0, NULL, NULL);
        }
         // 如果runloopMode的sources0集合和sources1都不包含将要添加的runloopSource则把runloopSource添加到对应的集合中
        if (NULL != rlm && !CFSetContainsValue(rlm->_sources0, rls) && !CFSetContainsValue(rlm->_sources1, rls))
        {
            //如果version是0，则加到_sources0
            if (0 == rls->_context.version0.version)
            {
                CFSetAddValue(rlm->_sources0, rls);
            }
            //如果version是1，则加到_sources1
            else if (1 == rls->_context.version0.version)
            {
                CFSetAddValue(rlm->_sources1, rls);
                __CFPort src_port = rls->_context.version1.getPort(rls->_context.version1.info);
                if (CFPORT_NULL != src_port)
                {
                    // key是src_port，value是rls，存储到runloopMode的_portToV1SourceMap字典中

                    //此处只有在加到source1的时候才会把souce和一个mach_port_t对应起来
                    //可以理解为，source1可以通过内核向其端口发送消息来主动唤醒runloop
                    CFDictionarySetValue(rlm->_portToV1SourceMap, (const void *)(uintptr_t)src_port, rls);
                    __CFPortSetInsert(src_port, rlm->_portSet);
                }
            }
            __CFRunLoopSourceLock(rls);
            //把runloop加入到source的_runLoops中
            if (NULL == rls->_runLoops)
            {
                // source有一个集合成员变量runLoops。source每被添加进一个runloop，都会把runloop添加到他的这个集合中
            // 如官方注释所言：sources retain run loops!（source会持有runloop！）
                rls->_runLoops = CFBagCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeBagCallBacks); // sources retain run loops!
            }

    // 更新runloopSource的runLoops集合，将rl添加到rls->_runloops中
            CFBagAddValue(rls->_runLoops, rl);
            __CFRunLoopSourceUnlock(rls);
            if (0 == rls->_context.version0.version)
            {
                if (NULL != rls->_context.version0.schedule)
                {
                     // 如果rls是source0则doVer0Callout标记置为true，即需要向外调用回调
                    doVer0Callout = true;
                }
            }
        }
        if (NULL != rlm)
        {
            __CFRunLoopModeUnlock(rlm);
        }
    }
    __CFRunLoopUnlock(rl);
    if (doVer0Callout)
    {
         // 如果是source0，则向外层（上层）调用source0的schedule回调函数
        // although it looses some protection for the source, we have no choice but
        // to do this after unlocking the run loop and mode locks, to avoid deadlocks
        // where the source wants to take a lock which is already held in another
        // thread which is itself waiting for a run loop/mode lock
        rls->_context.version0.schedule(rls->_context.version0.info, rl, modeName); /* CALLOUT */
    }
}

void CFRunLoopRemoveSource(CFRunLoopRef rl, CFRunLoopSourceRef rls, CFStringRef modeName)
{ /* DOES CALLOUT */
    CHECK_FOR_FORK();
    Boolean doVer0Callout = false, doRLSRelease = false;
    __CFRunLoopLock(rl);
    if (modeName == kCFRunLoopCommonModes)
    {
        if (NULL != rl->_commonModeItems && CFSetContainsValue(rl->_commonModeItems, rls))
        {
            CFSetRef set = rl->_commonModes ? CFSetCreateCopy(kCFAllocatorSystemDefault, rl->_commonModes) : NULL;
            CFSetRemoveValue(rl->_commonModeItems, rls);
            if (NULL != set)
            {
                CFTypeRef context[2] = {rl, rls};
                /* remove new item from all common-modes */
                CFSetApplyFunction(set, (__CFRunLoopRemoveItemFromCommonModes), (void *)context);
                CFRelease(set);
            }
        }
        else
        {
        }
    }
    else
    {
        CFRunLoopModeRef rlm = __CFRunLoopFindMode(rl, modeName, false);
        if (NULL != rlm && ((NULL != rlm->_sources0 && CFSetContainsValue(rlm->_sources0, rls)) || (NULL != rlm->_sources1 && CFSetContainsValue(rlm->_sources1, rls))))
        {
            CFRetain(rls);
            if (1 == rls->_context.version0.version)
            {
                __CFPort src_port = rls->_context.version1.getPort(rls->_context.version1.info);
                if (CFPORT_NULL != src_port)
                {
                    CFDictionaryRemoveValue(rlm->_portToV1SourceMap, (const void *)(uintptr_t)src_port);
                    __CFPortSetRemove(src_port, rlm->_portSet);
                }
            }
            CFSetRemoveValue(rlm->_sources0, rls);
            CFSetRemoveValue(rlm->_sources1, rls);
            __CFRunLoopSourceLock(rls);
            if (NULL != rls->_runLoops)
            {
                CFBagRemoveValue(rls->_runLoops, rl);
            }
            __CFRunLoopSourceUnlock(rls);
            if (0 == rls->_context.version0.version)
            {
                if (NULL != rls->_context.version0.cancel)
                {
                    doVer0Callout = true;
                }
            }
            doRLSRelease = true;
        }
        if (NULL != rlm)
        {
            __CFRunLoopModeUnlock(rlm);
        }
    }
    __CFRunLoopUnlock(rl);
    if (doVer0Callout)
    {
        // although it looses some protection for the source, we have no choice but
        // to do this after unlocking the run loop and mode locks, to avoid deadlocks
        // where the source wants to take a lock which is already held in another
        // thread which is itself waiting for a run loop/mode lock
        rls->_context.version0.cancel(rls->_context.version0.info, rl, modeName); /* CALLOUT */
    }
    if (doRLSRelease)
        CFRelease(rls);
}

static void __CFRunLoopRemoveSourcesFromCommonMode(const void *value, void *ctx)
{
    CFStringRef modeName = (CFStringRef)value;
    CFRunLoopRef rl = (CFRunLoopRef)ctx;
    __CFRunLoopRemoveAllSources(rl, modeName);
}

static void __CFRunLoopRemoveSourceFromMode(const void *value, void *ctx)
{
    CFRunLoopSourceRef rls = (CFRunLoopSourceRef)value;
    CFRunLoopRef rl = (CFRunLoopRef)(((CFTypeRef *)ctx)[0]);
    CFStringRef modeName = (CFStringRef)(((CFTypeRef *)ctx)[1]);
    CFRunLoopRemoveSource(rl, rls, modeName);
}

static void __CFRunLoopRemoveAllSources(CFRunLoopRef rl, CFStringRef modeName)
{
    CHECK_FOR_FORK();
    CFRunLoopModeRef rlm;
    __CFRunLoopLock(rl);
    if (modeName == kCFRunLoopCommonModes)
    {
        if (NULL != rl->_commonModeItems)
        {
            CFSetRef set = rl->_commonModes ? CFSetCreateCopy(kCFAllocatorSystemDefault, rl->_commonModes) : NULL;
            if (NULL != set)
            {
                CFSetApplyFunction(set, (__CFRunLoopRemoveSourcesFromCommonMode), (void *)rl);
                CFRelease(set);
            }
        }
        else
        {
        }
    }
    else
    {
        rlm = __CFRunLoopFindMode(rl, modeName, false);
        if (NULL != rlm && NULL != rlm->_sources0)
        {
            CFSetRef set = CFSetCreateCopy(kCFAllocatorSystemDefault, rlm->_sources0);
            CFTypeRef context[2] = {rl, modeName};
            CFSetApplyFunction(set, (__CFRunLoopRemoveSourceFromMode), (void *)context);
            CFRelease(set);
        }
        if (NULL != rlm && NULL != rlm->_sources1)
        {
            CFSetRef set = CFSetCreateCopy(kCFAllocatorSystemDefault, rlm->_sources1);
            CFTypeRef context[2] = {rl, modeName};
            CFSetApplyFunction(set, (__CFRunLoopRemoveSourceFromMode), (void *)context);
            CFRelease(set);
        }
        if (NULL != rlm)
        {
            __CFRunLoopModeUnlock(rlm);
        }
    }
    __CFRunLoopUnlock(rl);
}

Boolean CFRunLoopContainsObserver(CFRunLoopRef rl, CFRunLoopObserverRef rlo, CFStringRef modeName)
{
    CHECK_FOR_FORK();
    CFRunLoopModeRef rlm;
    Boolean hasValue = false;
    __CFRunLoopLock(rl);
    if (modeName == kCFRunLoopCommonModes)
    {
        if (NULL != rl->_commonModeItems)
        {
            hasValue = CFSetContainsValue(rl->_commonModeItems, rlo);
        }
    }
    else
    {
        rlm = __CFRunLoopFindMode(rl, modeName, false);
        if (NULL != rlm && NULL != rlm->_observers)
        {
            hasValue = CFArrayContainsValue(rlm->_observers, CFRangeMake(0, CFArrayGetCount(rlm->_observers)), rlo);
        }
        if (NULL != rlm)
        {
            __CFRunLoopModeUnlock(rlm);
        }
    }
    __CFRunLoopUnlock(rl);
    return hasValue;
}

void CFRunLoopAddObserver(CFRunLoopRef rl, CFRunLoopObserverRef rlo, CFStringRef modeName)
{
    CHECK_FOR_FORK();
    CFRunLoopModeRef rlm;
    if (__CFRunLoopIsDeallocating(rl))
        return;
    if (!__CFIsValid(rlo) || (NULL != rlo->_runLoop && rlo->_runLoop != rl))
        return;
    __CFRunLoopLock(rl);
    if (modeName == kCFRunLoopCommonModes)
    {
        // 导出runloop的commonModes
        CFSetRef set = rl->_commonModes ? CFSetCreateCopy(kCFAllocatorSystemDefault, rl->_commonModes) : NULL;
        if (NULL == rl->_commonModeItems)
        {
            // 初始化创建commonModeItems
            rl->_commonModeItems = CFSetCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeSetCallBacks);
        }
        // 添加observer到commonModeItems
        CFSetAddValue(rl->_commonModeItems, rlo);
        if (NULL != set)
        {
            CFTypeRef context[2] = {rl, rlo};
            /* add new item to all common-modes */
             // 添加observer到所有被标记为commonMode的mode中
            CFSetApplyFunction(set, (__CFRunLoopAddItemToCommonModes), (void *)context);
            CFRelease(set);
        }
    }
    else
    {
        rlm = __CFRunLoopFindMode(rl, modeName, true);
        if (NULL != rlm && NULL == rlm->_observers)
        {
            rlm->_observers = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks);
        }
        if (NULL != rlm && !CFArrayContainsValue(rlm->_observers, CFRangeMake(0, CFArrayGetCount(rlm->_observers)), rlo))
        {
            Boolean inserted = false;
            for (CFIndex idx = CFArrayGetCount(rlm->_observers); idx--;)
            {
                CFRunLoopObserverRef obs = (CFRunLoopObserverRef)CFArrayGetValueAtIndex(rlm->_observers, idx);
                if (obs->_order <= rlo->_order)
                {
                    CFArrayInsertValueAtIndex(rlm->_observers, idx + 1, rlo);
                    inserted = true;
                    break;
                }
            }
            if (!inserted)
            {
                CFArrayInsertValueAtIndex(rlm->_observers, 0, rlo);
            }
            // 设置runloopMode的_observerMask为观察者的_activities（CFRunLoopActivity状态
            rlm->_observerMask |= rlo->_activities;
            __CFRunLoopObserverSchedule(rlo, rl, rlm);
        }
        if (NULL != rlm)
        {
            __CFRunLoopModeUnlock(rlm);
        }
    }
    __CFRunLoopUnlock(rl);
}

void CFRunLoopRemoveObserver(CFRunLoopRef rl, CFRunLoopObserverRef rlo, CFStringRef modeName)
{
    CHECK_FOR_FORK();
    CFRunLoopModeRef rlm;
    __CFRunLoopLock(rl);
    if (modeName == kCFRunLoopCommonModes)
    {
        if (NULL != rl->_commonModeItems && CFSetContainsValue(rl->_commonModeItems, rlo))
        {
            CFSetRef set = rl->_commonModes ? CFSetCreateCopy(kCFAllocatorSystemDefault, rl->_commonModes) : NULL;
            CFSetRemoveValue(rl->_commonModeItems, rlo);
            if (NULL != set)
            {
                CFTypeRef context[2] = {rl, rlo};
                /* remove new item from all common-modes */
                CFSetApplyFunction(set, (__CFRunLoopRemoveItemFromCommonModes), (void *)context);
                CFRelease(set);
            }
        }
        else
        {
        }
    }
    else
    {
        rlm = __CFRunLoopFindMode(rl, modeName, false);
        if (NULL != rlm && NULL != rlm->_observers)
        {
            CFRetain(rlo);
            CFIndex idx = CFArrayGetFirstIndexOfValue(rlm->_observers, CFRangeMake(0, CFArrayGetCount(rlm->_observers)), rlo);
            if (kCFNotFound != idx)
            {
                CFArrayRemoveValueAtIndex(rlm->_observers, idx);
                __CFRunLoopObserverCancel(rlo, rl, rlm);
            }
            CFRelease(rlo);
        }
        if (NULL != rlm)
        {
            __CFRunLoopModeUnlock(rlm);
        }
    }
    __CFRunLoopUnlock(rl);
}

Boolean CFRunLoopContainsTimer(CFRunLoopRef rl, CFRunLoopTimerRef rlt, CFStringRef modeName)
{
    CHECK_FOR_FORK();
    if (NULL == rlt->_runLoop || rl != rlt->_runLoop)
        return false;
    Boolean hasValue = false;
    __CFRunLoopLock(rl);
    if (modeName == kCFRunLoopCommonModes)
    {
        if (NULL != rl->_commonModeItems)
        {
            hasValue = CFSetContainsValue(rl->_commonModeItems, rlt);
        }
    }
    else
    {
        CFRunLoopModeRef rlm = __CFRunLoopFindMode(rl, modeName, false);
        if (NULL != rlm)
        {
            if (NULL != rlm->_timers)
            {
                CFIndex idx = CFArrayGetFirstIndexOfValue(rlm->_timers, CFRangeMake(0, CFArrayGetCount(rlm->_timers)), rlt);
                hasValue = (kCFNotFound != idx);
            }
            __CFRunLoopModeUnlock(rlm);
        }
    }
    __CFRunLoopUnlock(rl);
    return hasValue;
}

// 添加timer到runloopMode中，添加timer到rl->commonModeItems中
void CFRunLoopAddTimer(CFRunLoopRef rl, CFRunLoopTimerRef rlt, CFStringRef modeName)
{
    CHECK_FOR_FORK();
    if (__CFRunLoopIsDeallocating(rl))
        return;
    if (!__CFIsValid(rlt) || (NULL != rlt->_runLoop && rlt->_runLoop != rl))
        return;
    __CFRunLoopLock(rl);
     // 导出runloop的commonMode(如果modeName是commonMode)
    if (modeName == kCFRunLoopCommonModes)
    {
    
        CFSetRef set = rl->_commonModes ? CFSetCreateCopy(kCFAllocatorSystemDefault, rl->_commonModes) : NULL;
        if (NULL == rl->_commonModeItems)
        {
            // 如果rl->_commonModeItems为空就初始化rl->commonModeItems
            rl->_commonModeItems = CFSetCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeSetCallBacks);
        }
        CFSetAddValue(rl->_commonModeItems, rlt);
        if (NULL != set)
        {
            // 长度为2的数组，分别存放rl和rlt
            CFTypeRef context[2] = {rl, rlt};
            /* add new item to all common-modes */
             // 添加新的item也就是timer到所有的commonMode中
        // set是commonMode集合，CFSetApplyFunction遍历set，添加context[1]存放的rlt添加到所有被标记为commonMode的mode中
            CFSetApplyFunction(set, (__CFRunLoopAddItemToCommonModes), (void *)context);
            CFRelease(set);
        }
    }
    else
    {
        CFRunLoopModeRef rlm = __CFRunLoopFindMode(rl, modeName, true);
        if (NULL != rlm)
        {
            if (NULL == rlm->_timers)
            {
                CFArrayCallBacks cb = kCFTypeArrayCallBacks;
                cb.equal = NULL;
                rlm->_timers = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &cb);
            }
        }
        if (NULL != rlm && !CFSetContainsValue(rlt->_rlModes, rlm->_name))
        {
            // 走到这里说明modeName不是commonMode
      // 根据runloop和modeName查找对用的mode

            __CFRunLoopTimerLock(rlt);
            if (NULL == rlt->_runLoop)
            {
                rlt->_runLoop = rl;
            }
            else if (rl != rlt->_runLoop)
            {
                __CFRunLoopTimerUnlock(rlt);
                __CFRunLoopModeUnlock(rlm);
                __CFRunLoopUnlock(rl);
                return;
            }
             // 更新rlt的rlModes集合。将rlm->name添加到name中
            CFSetAddValue(rlt->_rlModes, rlm->_name);
            __CFRunLoopTimerUnlock(rlt);
            __CFRunLoopTimerFireTSRLock();

            // Reposition释义复位。所以顾名思义该函数用于复位timer
            // 此处调用该函数本质上是按照timer下次触发时间长短，计算timer需要插入到runloopMode->timers数组中的位置，然后把timer插入到runloopMode->timers数组中

            __CFRepositionTimerInMode(rlm, rlt, false);
            __CFRunLoopTimerFireTSRUnlock();
            if (!_CFExecutableLinkedOnOrAfter(CFSystemVersionLion))
            {
                // Normally we don't do this on behalf of clients, but for
                // backwards compatibility due to the change in timer handling...
                // 为了向后兼容，如果系统版本低于CFSystemVersionLion且timer执行的rl不是当前runloop，则唤醒rl
                if (rl != CFRunLoopGetCurrent())
                    CFRunLoopWakeUp(rl);
            }
        }
        if (NULL != rlm)
        {
            __CFRunLoopModeUnlock(rlm);
        }
    }
    __CFRunLoopUnlock(rl);
}

void CFRunLoopRemoveTimer(CFRunLoopRef rl, CFRunLoopTimerRef rlt, CFStringRef modeName)
{
    CHECK_FOR_FORK();
    __CFRunLoopLock(rl);
    if (modeName == kCFRunLoopCommonModes)
    {
        if (NULL != rl->_commonModeItems && CFSetContainsValue(rl->_commonModeItems, rlt))
        {
            CFSetRef set = rl->_commonModes ? CFSetCreateCopy(kCFAllocatorSystemDefault, rl->_commonModes) : NULL;
            CFSetRemoveValue(rl->_commonModeItems, rlt);
            if (NULL != set)
            {
                CFTypeRef context[2] = {rl, rlt};
                /* remove new item from all common-modes */
                CFSetApplyFunction(set, (__CFRunLoopRemoveItemFromCommonModes), (void *)context);
                CFRelease(set);
            }
        }
        else
        {
        }
    }
    else
    {
        CFRunLoopModeRef rlm = __CFRunLoopFindMode(rl, modeName, false);
        CFIndex idx = kCFNotFound;
        CFMutableArrayRef timerList = NULL;
        if (NULL != rlm)
        {
            timerList = rlm->_timers;
            if (NULL != timerList)
            {
                idx = CFArrayGetFirstIndexOfValue(timerList, CFRangeMake(0, CFArrayGetCount(timerList)), rlt);
            }
        }
        if (kCFNotFound != idx)
        {
            __CFRunLoopTimerLock(rlt);
            CFSetRemoveValue(rlt->_rlModes, rlm->_name);
            if (0 == CFSetGetCount(rlt->_rlModes))
            {
                rlt->_runLoop = NULL;
            }
            __CFRunLoopTimerUnlock(rlt);
            CFArrayRemoveValueAtIndex(timerList, idx);
            __CFArmNextTimerInMode(rlm, rl);
        }
        if (NULL != rlm)
        {
            __CFRunLoopModeUnlock(rlm);
        }
    }
    __CFRunLoopUnlock(rl);
}

/* CFRunLoopSource */

static Boolean __CFRunLoopSourceEqual(CFTypeRef cf1, CFTypeRef cf2)
{ /* DOES CALLOUT */
    CFRunLoopSourceRef rls1 = (CFRunLoopSourceRef)cf1;
    CFRunLoopSourceRef rls2 = (CFRunLoopSourceRef)cf2;
    if (rls1 == rls2)
        return true;
    if (__CFIsValid(rls1) != __CFIsValid(rls2))
        return false;
    if (rls1->_order != rls2->_order)
        return false;
    if (rls1->_context.version0.version != rls2->_context.version0.version)
        return false;
    if (rls1->_context.version0.hash != rls2->_context.version0.hash)
        return false;
    if (rls1->_context.version0.equal != rls2->_context.version0.equal)
        return false;
    if (0 == rls1->_context.version0.version && rls1->_context.version0.perform != rls2->_context.version0.perform)
        return false;
    if (1 == rls1->_context.version0.version && rls1->_context.version1.perform != rls2->_context.version1.perform)
        return false;
    if (rls1->_context.version0.equal)
        return rls1->_context.version0.equal(rls1->_context.version0.info, rls2->_context.version0.info);
    return (rls1->_context.version0.info == rls2->_context.version0.info);
}

static CFHashCode __CFRunLoopSourceHash(CFTypeRef cf)
{ /* DOES CALLOUT */
    CFRunLoopSourceRef rls = (CFRunLoopSourceRef)cf;
    if (rls->_context.version0.hash)
        return rls->_context.version0.hash(rls->_context.version0.info);
    return (CFHashCode)rls->_context.version0.info;
}

static CFStringRef __CFRunLoopSourceCopyDescription(CFTypeRef cf)
{ /* DOES CALLOUT */
    CFRunLoopSourceRef rls = (CFRunLoopSourceRef)cf;
    CFStringRef result;
    CFStringRef contextDesc = NULL;
    if (NULL != rls->_context.version0.copyDescription)
    {
        contextDesc = rls->_context.version0.copyDescription(rls->_context.version0.info);
    }
    if (NULL == contextDesc)
    {
        void *addr = rls->_context.version0.version == 0 ? (void *)rls->_context.version0.perform : (rls->_context.version0.version == 1 ? (void *)rls->_context.version1.perform : NULL);
#if DEPLOYMENT_TARGET_WINDOWS
        contextDesc = CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("<CFRunLoopSource context>{version = %ld, info = %p, callout = %p}"), rls->_context.version0.version, rls->_context.version0.info, addr);
#elif DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_EMBEDDED_MINI
        Dl_info info;
        const char *name = (dladdr(addr, &info) && info.dli_saddr == addr && info.dli_sname) ? info.dli_sname : "???";
        contextDesc = CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("<CFRunLoopSource context>{version = %ld, info = %p, callout = %s (%p)}"), rls->_context.version0.version, rls->_context.version0.info, name, addr);
#endif
    }
#if DEPLOYMENT_TARGET_WINDOWS
    result = CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("<CFRunLoopSource %p [%p]>{signalled = %s, valid = %s, order = %d, context = %@}"), cf, CFGetAllocator(rls), __CFRunLoopSourceIsSignaled(rls) ? "Yes" : "No", __CFIsValid(rls) ? "Yes" : "No", rls->_order, contextDesc);
#else
    result = CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("<CFRunLoopSource %p [%p]>{signalled = %s, valid = %s, order = %ld, context = %@}"), cf, CFGetAllocator(rls), __CFRunLoopSourceIsSignaled(rls) ? "Yes" : "No", __CFIsValid(rls) ? "Yes" : "No", (unsigned long)rls->_order, contextDesc);
#endif
    CFRelease(contextDesc);
    return result;
}

static void __CFRunLoopSourceDeallocate(CFTypeRef cf)
{ /* DOES CALLOUT */
    CFRunLoopSourceRef rls = (CFRunLoopSourceRef)cf;
    CFRunLoopSourceInvalidate(rls);
    if (rls->_context.version0.release)
    {
        rls->_context.version0.release(rls->_context.version0.info);
    }
    pthread_mutex_destroy(&rls->_lock);
    memset((char *)cf + sizeof(CFRuntimeBase), 0, sizeof(struct __CFRunLoopSource) - sizeof(CFRuntimeBase));
}

static const CFRuntimeClass __CFRunLoopSourceClass = {
    _kCFRuntimeScannedObject,
    "CFRunLoopSource",
    NULL, // init
    NULL, // copy
    __CFRunLoopSourceDeallocate,
    __CFRunLoopSourceEqual,
    __CFRunLoopSourceHash,
    NULL, //
    __CFRunLoopSourceCopyDescription};

CFTypeID CFRunLoopSourceGetTypeID(void)
{
    static dispatch_once_t initOnce;
    dispatch_once(&initOnce, ^{
      __kCFRunLoopSourceTypeID = _CFRuntimeRegisterClass(&__CFRunLoopSourceClass);
    });
    return __kCFRunLoopSourceTypeID;
}

CFRunLoopSourceRef CFRunLoopSourceCreate(CFAllocatorRef allocator, CFIndex order, CFRunLoopSourceContext *context)
{
    CHECK_FOR_FORK();
    CFRunLoopSourceRef memory;
    uint32_t size;
    if (NULL == context)
        CRASH("*** NULL context value passed to CFRunLoopSourceCreate(). (%d) ***", -1);

    size = sizeof(struct __CFRunLoopSource) - sizeof(CFRuntimeBase);
    memory = (CFRunLoopSourceRef)_CFRuntimeCreateInstance(allocator, CFRunLoopSourceGetTypeID(), size, NULL);
    if (NULL == memory)
    {
        return NULL;
    }
    __CFSetValid(memory);
    __CFRunLoopSourceUnsetSignaled(memory);
    __CFRunLoopLockInit(&memory->_lock);
    memory->_bits = 0;
    memory->_order = order;
    memory->_runLoops = NULL;
    size = 0;
    switch (context->version)
    {
    case 0:
        size = sizeof(CFRunLoopSourceContext);
        break;
    case 1:
        size = sizeof(CFRunLoopSourceContext1);
        break;
    }
    objc_memmove_collectable(&memory->_context, context, size);
    if (context->retain)
    {
        memory->_context.version0.info = (void *)context->retain(context->info);
    }
    return memory;
}

CFIndex CFRunLoopSourceGetOrder(CFRunLoopSourceRef rls)
{
    CHECK_FOR_FORK();
    __CFGenericValidateType(rls, CFRunLoopSourceGetTypeID());
    return rls->_order;
}

static void __CFRunLoopSourceWakeUpLoop(const void *value, void *context)
{
    CFRunLoopWakeUp((CFRunLoopRef)value);
}

static void __CFRunLoopSourceRemoveFromRunLoop(const void *value, void *context)
{
    CFRunLoopRef rl = (CFRunLoopRef)value;
    CFTypeRef *params = (CFTypeRef *)context;
    CFRunLoopSourceRef rls = (CFRunLoopSourceRef)params[0];
    CFIndex idx;
    if (rl == params[1])
        return;

    // CFRunLoopRemoveSource will lock the run loop while it
    // needs that, but we also lock it out here to keep
    // changes from occurring for this whole sequence.
    __CFRunLoopLock(rl);
    CFArrayRef array = CFRunLoopCopyAllModes(rl);
    for (idx = CFArrayGetCount(array); idx--;)
    {
        CFStringRef modeName = (CFStringRef)CFArrayGetValueAtIndex(array, idx);
        CFRunLoopRemoveSource(rl, rls, modeName);
    }
    CFRunLoopRemoveSource(rl, rls, kCFRunLoopCommonModes);
    __CFRunLoopUnlock(rl);
    CFRelease(array);
    params[1] = rl;
}

void CFRunLoopSourceInvalidate(CFRunLoopSourceRef rls)
{
    CHECK_FOR_FORK();
    __CFGenericValidateType(rls, CFRunLoopSourceGetTypeID());
    __CFRunLoopSourceLock(rls);
    CFRetain(rls);
    if (__CFIsValid(rls))
    {
        CFBagRef rloops = rls->_runLoops;
        __CFUnsetValid(rls);
        __CFRunLoopSourceUnsetSignaled(rls);
        if (NULL != rloops)
        {
            // To avoid A->B, B->A lock ordering issues when coming up
            // towards the run loop from a source, the source has to be
            // unlocked, which means we have to protect from object
            // invalidation.
            rls->_runLoops = NULL; // transfer ownership to local stack
            __CFRunLoopSourceUnlock(rls);
            CFTypeRef params[2] = {rls, NULL};
            CFBagApplyFunction(rloops, (__CFRunLoopSourceRemoveFromRunLoop), params);
            CFRelease(rloops);
            __CFRunLoopSourceLock(rls);
        }
        /* for hashing- and equality-use purposes, can't actually release the context here */
    }
    __CFRunLoopSourceUnlock(rls);
    CFRelease(rls);
}

Boolean CFRunLoopSourceIsValid(CFRunLoopSourceRef rls)
{
    CHECK_FOR_FORK();
    __CFGenericValidateType(rls, CFRunLoopSourceGetTypeID());
    return __CFIsValid(rls);
}

void CFRunLoopSourceGetContext(CFRunLoopSourceRef rls, CFRunLoopSourceContext *context)
{
    CHECK_FOR_FORK();
    __CFGenericValidateType(rls, CFRunLoopSourceGetTypeID());
    CFAssert1(0 == context->version || 1 == context->version, __kCFLogAssertion, "%s(): context version not initialized to 0 or 1", __PRETTY_FUNCTION__);
    CFIndex size = 0;
    switch (context->version)
    {
    case 0:
        size = sizeof(CFRunLoopSourceContext);
        break;
    case 1:
        size = sizeof(CFRunLoopSourceContext1);
        break;
    }
    memmove(context, &rls->_context, size);
}

void CFRunLoopSourceSignal(CFRunLoopSourceRef rls)
{
    CHECK_FOR_FORK();
    __CFRunLoopSourceLock(rls);
    if (__CFIsValid(rls))
    {
        __CFRunLoopSourceSetSignaled(rls);
    }
    __CFRunLoopSourceUnlock(rls);
}

Boolean CFRunLoopSourceIsSignalled(CFRunLoopSourceRef rls)
{
    CHECK_FOR_FORK();
    __CFRunLoopSourceLock(rls);
    Boolean ret = __CFRunLoopSourceIsSignaled(rls) ? true : false;
    __CFRunLoopSourceUnlock(rls);
    return ret;
}

CF_PRIVATE void _CFRunLoopSourceWakeUpRunLoops(CFRunLoopSourceRef rls)
{
    CFBagRef loops = NULL;
    __CFRunLoopSourceLock(rls);
    if (__CFIsValid(rls) && NULL != rls->_runLoops)
    {
        loops = CFBagCreateCopy(kCFAllocatorSystemDefault, rls->_runLoops);
    }
    __CFRunLoopSourceUnlock(rls);
    if (loops)
    {
        CFBagApplyFunction(loops, __CFRunLoopSourceWakeUpLoop, NULL);
        CFRelease(loops);
    }
}

/* CFRunLoopObserver */

static CFStringRef __CFRunLoopObserverCopyDescription(CFTypeRef cf)
{ /* DOES CALLOUT */
    CFRunLoopObserverRef rlo = (CFRunLoopObserverRef)cf;
    CFStringRef result;
    CFStringRef contextDesc = NULL;
    if (NULL != rlo->_context.copyDescription)
    {
        contextDesc = rlo->_context.copyDescription(rlo->_context.info);
    }
    if (!contextDesc)
    {
        contextDesc = CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("<CFRunLoopObserver context %p>"), rlo->_context.info);
    }
#if DEPLOYMENT_TARGET_WINDOWS
    result = CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("<CFRunLoopObserver %p [%p]>{valid = %s, activities = 0x%x, repeats = %s, order = %d, callout = %p, context = %@}"), cf, CFGetAllocator(rlo), __CFIsValid(rlo) ? "Yes" : "No", rlo->_activities, __CFRunLoopObserverRepeats(rlo) ? "Yes" : "No", rlo->_order, rlo->_callout, contextDesc);
#elif DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_EMBEDDED_MINI
    void *addr = rlo->_callout;
    Dl_info info;
    const char *name = (dladdr(addr, &info) && info.dli_saddr == addr && info.dli_sname) ? info.dli_sname : "???";
    result = CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("<CFRunLoopObserver %p [%p]>{valid = %s, activities = 0x%lx, repeats = %s, order = %ld, callout = %s (%p), context = %@}"), cf, CFGetAllocator(rlo), __CFIsValid(rlo) ? "Yes" : "No", (long)rlo->_activities, __CFRunLoopObserverRepeats(rlo) ? "Yes" : "No", (long)rlo->_order, name, addr, contextDesc);
#endif
    CFRelease(contextDesc);
    return result;
}

static void __CFRunLoopObserverDeallocate(CFTypeRef cf)
{ /* DOES CALLOUT */
    CFRunLoopObserverRef rlo = (CFRunLoopObserverRef)cf;
    CFRunLoopObserverInvalidate(rlo);
    pthread_mutex_destroy(&rlo->_lock);
}

static const CFRuntimeClass __CFRunLoopObserverClass = {
    0,
    "CFRunLoopObserver",
    NULL, // init
    NULL, // copy
    __CFRunLoopObserverDeallocate,
    NULL,
    NULL,
    NULL, //
    __CFRunLoopObserverCopyDescription};

CFTypeID CFRunLoopObserverGetTypeID(void)
{
    static dispatch_once_t initOnce;
    dispatch_once(&initOnce, ^{
      __kCFRunLoopObserverTypeID = _CFRuntimeRegisterClass(&__CFRunLoopObserverClass);
    });
    return __kCFRunLoopObserverTypeID;
}

CFRunLoopObserverRef CFRunLoopObserverCreate(CFAllocatorRef allocator, CFOptionFlags activities, Boolean repeats, CFIndex order, CFRunLoopObserverCallBack callout, CFRunLoopObserverContext *context)
{
    CHECK_FOR_FORK();
    CFRunLoopObserverRef memory;
    UInt32 size;
    size = sizeof(struct __CFRunLoopObserver) - sizeof(CFRuntimeBase);
    memory = (CFRunLoopObserverRef)_CFRuntimeCreateInstance(allocator, CFRunLoopObserverGetTypeID(), size, NULL);
    if (NULL == memory)
    {
        return NULL;
    }
    __CFSetValid(memory);
    __CFRunLoopObserverUnsetFiring(memory);
    if (repeats)
    {
        __CFRunLoopObserverSetRepeats(memory);
    }
    else
    {
        __CFRunLoopObserverUnsetRepeats(memory);
    }
    __CFRunLoopLockInit(&memory->_lock);
    memory->_runLoop = NULL;
    memory->_rlCount = 0;
    memory->_activities = activities;
    memory->_order = order;
    memory->_callout = callout;
    if (context)
    {
        if (context->retain)
        {
            memory->_context.info = (void *)context->retain(context->info);
        }
        else
        {
            memory->_context.info = context->info;
        }
        memory->_context.retain = context->retain;
        memory->_context.release = context->release;
        memory->_context.copyDescription = context->copyDescription;
    }
    else
    {
        memory->_context.info = 0;
        memory->_context.retain = 0;
        memory->_context.release = 0;
        memory->_context.copyDescription = 0;
    }
    return memory;
}

static void _runLoopObserverWithBlockContext(CFRunLoopObserverRef observer, CFRunLoopActivity activity, void *opaqueBlock)
{
    typedef void (^observer_block_t)(CFRunLoopObserverRef observer, CFRunLoopActivity activity);
    observer_block_t block = (observer_block_t)opaqueBlock;
    block(observer, activity);
}

CFRunLoopObserverRef CFRunLoopObserverCreateWithHandler(CFAllocatorRef allocator, CFOptionFlags activities, Boolean repeats, CFIndex order,
                                                        void (^block)(CFRunLoopObserverRef observer, CFRunLoopActivity activity))
{
    CFRunLoopObserverContext blockContext;
    blockContext.version = 0;
    blockContext.info = (void *)block;
    blockContext.retain = (const void *(*)(const void *info))_Block_copy;
    blockContext.release = (void (*)(const void *info))_Block_release;
    blockContext.copyDescription = NULL;
    return CFRunLoopObserverCreate(allocator, activities, repeats, order, _runLoopObserverWithBlockContext, &blockContext);
}

CFOptionFlags CFRunLoopObserverGetActivities(CFRunLoopObserverRef rlo)
{
    CHECK_FOR_FORK();
    __CFGenericValidateType(rlo, CFRunLoopObserverGetTypeID());
    return rlo->_activities;
}

CFIndex CFRunLoopObserverGetOrder(CFRunLoopObserverRef rlo)
{
    CHECK_FOR_FORK();
    __CFGenericValidateType(rlo, CFRunLoopObserverGetTypeID());
    return rlo->_order;
}

Boolean CFRunLoopObserverDoesRepeat(CFRunLoopObserverRef rlo)
{
    CHECK_FOR_FORK();
    __CFGenericValidateType(rlo, CFRunLoopObserverGetTypeID());
    return __CFRunLoopObserverRepeats(rlo);
}

void CFRunLoopObserverInvalidate(CFRunLoopObserverRef rlo)
{ /* DOES CALLOUT */
    CHECK_FOR_FORK();
    __CFGenericValidateType(rlo, CFRunLoopObserverGetTypeID());
    __CFRunLoopObserverLock(rlo);
    CFRetain(rlo);
    if (__CFIsValid(rlo))
    {
        CFRunLoopRef rl = rlo->_runLoop;
        void *info = rlo->_context.info;
        rlo->_context.info = NULL;
        __CFUnsetValid(rlo);
        if (NULL != rl)
        {
            // To avoid A->B, B->A lock ordering issues when coming up
            // towards the run loop from an observer, it has to be
            // unlocked, which means we have to protect from object
            // invalidation.
            CFRetain(rl);
            __CFRunLoopObserverUnlock(rlo);
            // CFRunLoopRemoveObserver will lock the run loop while it
            // needs that, but we also lock it out here to keep
            // changes from occurring for this whole sequence.
            __CFRunLoopLock(rl);
            CFArrayRef array = CFRunLoopCopyAllModes(rl);
            for (CFIndex idx = CFArrayGetCount(array); idx--;)
            {
                CFStringRef modeName = (CFStringRef)CFArrayGetValueAtIndex(array, idx);
                CFRunLoopRemoveObserver(rl, rlo, modeName);
            }
            CFRunLoopRemoveObserver(rl, rlo, kCFRunLoopCommonModes);
            __CFRunLoopUnlock(rl);
            CFRelease(array);
            CFRelease(rl);
            __CFRunLoopObserverLock(rlo);
        }
        if (NULL != rlo->_context.release)
        {
            rlo->_context.release(info); /* CALLOUT */
        }
    }
    __CFRunLoopObserverUnlock(rlo);
    CFRelease(rlo);
}

Boolean CFRunLoopObserverIsValid(CFRunLoopObserverRef rlo)
{
    CHECK_FOR_FORK();
    return __CFIsValid(rlo);
}

void CFRunLoopObserverGetContext(CFRunLoopObserverRef rlo, CFRunLoopObserverContext *context)
{
    CHECK_FOR_FORK();
    __CFGenericValidateType(rlo, CFRunLoopObserverGetTypeID());
    CFAssert1(0 == context->version, __kCFLogAssertion, "%s(): context version not initialized to 0", __PRETTY_FUNCTION__);
    *context = rlo->_context;
}

#pragma mark -
#pragma mark CFRunLoopTimer

static CFStringRef __CFRunLoopTimerCopyDescription(CFTypeRef cf)
{ /* DOES CALLOUT */
    CFRunLoopTimerRef rlt = (CFRunLoopTimerRef)cf;
    CFStringRef contextDesc = NULL;
    if (NULL != rlt->_context.copyDescription)
    {
        contextDesc = rlt->_context.copyDescription(rlt->_context.info);
    }
    if (NULL == contextDesc)
    {
        contextDesc = CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("<CFRunLoopTimer context %p>"), rlt->_context.info);
    }
    void *addr = (void *)rlt->_callout;
    char libraryName[2048];
    char functionName[2048];
    void *functionPtr = NULL;
    libraryName[0] = '?';
    libraryName[1] = '\0';
    functionName[0] = '?';
    functionName[1] = '\0';
    CFStringRef result = CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL,
                                                  CFSTR("<CFRunLoopTimer %p [%p]>{valid = %s, firing = %s, interval = %0.09g, tolerance = %0.09g, next fire date = %0.09g (%0.09g @ %lld), callout = %s (%p / %p) (%s), context = %@}"),
                                                  cf,
                                                  CFGetAllocator(rlt),
                                                  __CFIsValid(rlt) ? "Yes" : "No",
                                                  __CFRunLoopTimerIsFiring(rlt) ? "Yes" : "No",
                                                  rlt->_interval,
                                                  rlt->_tolerance,
                                                  rlt->_nextFireDate,
                                                  rlt->_nextFireDate - CFAbsoluteTimeGetCurrent(),
                                                  rlt->_fireTSR,
                                                  functionName,
                                                  addr,
                                                  functionPtr,
                                                  libraryName,
                                                  contextDesc);
    CFRelease(contextDesc);
    return result;
}

static void __CFRunLoopTimerDeallocate(CFTypeRef cf)
{ /* DOES CALLOUT */
    //CFLog(6, CFSTR("__CFRunLoopTimerDeallocate(%p)"), cf);
    CFRunLoopTimerRef rlt = (CFRunLoopTimerRef)cf;
    __CFRunLoopTimerSetDeallocating(rlt);
    CFRunLoopTimerInvalidate(rlt); /* DOES CALLOUT */
    CFRelease(rlt->_rlModes);
    rlt->_rlModes = NULL;
    pthread_mutex_destroy(&rlt->_lock);
}

static const CFRuntimeClass __CFRunLoopTimerClass = {
    0,
    "CFRunLoopTimer",
    NULL, // init
    NULL, // copy
    __CFRunLoopTimerDeallocate,
    NULL, // equal
    NULL,
    NULL, //
    __CFRunLoopTimerCopyDescription};

CFTypeID CFRunLoopTimerGetTypeID(void)
{
    static dispatch_once_t initOnce;
    dispatch_once(&initOnce, ^{
      __kCFRunLoopTimerTypeID = _CFRuntimeRegisterClass(&__CFRunLoopTimerClass);
    });
    return __kCFRunLoopTimerTypeID;
}

CFRunLoopTimerRef CFRunLoopTimerCreate(CFAllocatorRef allocator, CFAbsoluteTime fireDate, CFTimeInterval interval, CFOptionFlags flags, CFIndex order, CFRunLoopTimerCallBack callout, CFRunLoopTimerContext *context)
{
    CHECK_FOR_FORK();
    if (isnan(interval))
    {
        CRSetCrashLogMessage("NaN was used as an interval for a CFRunLoopTimer");
        HALT;
    }
    CFRunLoopTimerRef memory;
    UInt32 size;
    size = sizeof(struct __CFRunLoopTimer) - sizeof(CFRuntimeBase);
    memory = (CFRunLoopTimerRef)_CFRuntimeCreateInstance(allocator, CFRunLoopTimerGetTypeID(), size, NULL);
    if (NULL == memory)
    {
        return NULL;
    }
    __CFSetValid(memory);
    __CFRunLoopTimerUnsetFiring(memory);
    __CFRunLoopLockInit(&memory->_lock);
    memory->_runLoop = NULL;
    memory->_rlModes = CFSetCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeSetCallBacks);
    memory->_order = order;
    if (interval < 0.0)
        interval = 0.0;
    memory->_interval = interval;
    memory->_tolerance = 0.0;
    if (TIMER_DATE_LIMIT < fireDate)
        fireDate = TIMER_DATE_LIMIT;
    memory->_nextFireDate = fireDate;
    memory->_fireTSR = 0ULL;
    uint64_t now2 = mach_absolute_time();
    CFAbsoluteTime now1 = CFAbsoluteTimeGetCurrent();
    if (fireDate < now1)
    {
        memory->_fireTSR = now2;
    }
    else if (TIMER_INTERVAL_LIMIT < fireDate - now1)
    {
        memory->_fireTSR = now2 + __CFTimeIntervalToTSR(TIMER_INTERVAL_LIMIT);
    }
    else
    {
        memory->_fireTSR = now2 + __CFTimeIntervalToTSR(fireDate - now1);
    }
    memory->_callout = callout;
    if (NULL != context)
    {
        if (context->retain)
        {
            memory->_context.info = (void *)context->retain(context->info);
        }
        else
        {
            memory->_context.info = context->info;
        }
        memory->_context.retain = context->retain;
        memory->_context.release = context->release;
        memory->_context.copyDescription = context->copyDescription;
    }
    else
    {
        memory->_context.info = 0;
        memory->_context.retain = 0;
        memory->_context.release = 0;
        memory->_context.copyDescription = 0;
    }
    return memory;
}

static void _runLoopTimerWithBlockContext(CFRunLoopTimerRef timer, void *opaqueBlock)
{
    typedef void (^timer_block_t)(CFRunLoopTimerRef timer);
    timer_block_t block = (timer_block_t)opaqueBlock;
    block(timer);
}

CFRunLoopTimerRef CFRunLoopTimerCreateWithHandler(CFAllocatorRef allocator, CFAbsoluteTime fireDate, CFTimeInterval interval, CFOptionFlags flags, CFIndex order,
                                                  void (^block)(CFRunLoopTimerRef timer))
{

    CFRunLoopTimerContext blockContext;
    blockContext.version = 0;
    blockContext.info = (void *)block;
    blockContext.retain = (const void *(*)(const void *info))_Block_copy;
    blockContext.release = (void (*)(const void *info))_Block_release;
    blockContext.copyDescription = NULL;
    return CFRunLoopTimerCreate(allocator, fireDate, interval, flags, order, _runLoopTimerWithBlockContext, &blockContext);
}

CFAbsoluteTime CFRunLoopTimerGetNextFireDate(CFRunLoopTimerRef rlt)
{
    CHECK_FOR_FORK();
    CF_OBJC_FUNCDISPATCHV(CFRunLoopTimerGetTypeID(), CFAbsoluteTime, (NSTimer *)rlt, _cffireTime);
    __CFGenericValidateType(rlt, CFRunLoopTimerGetTypeID());
    CFAbsoluteTime at = 0.0;
    __CFRunLoopTimerLock(rlt);
    __CFRunLoopTimerFireTSRLock();
    if (__CFIsValid(rlt))
    {
        at = rlt->_nextFireDate;
    }
    __CFRunLoopTimerFireTSRUnlock();
    __CFRunLoopTimerUnlock(rlt);
    return at;
}

void CFRunLoopTimerSetNextFireDate(CFRunLoopTimerRef rlt, CFAbsoluteTime fireDate)
{
    // 触发日期大于最大限制时间，则把触发日期调整为最大触发时间
    CHECK_FOR_FORK();
    if (!__CFIsValid(rlt))
        return;
    if (TIMER_DATE_LIMIT < fireDate)
        fireDate = TIMER_DATE_LIMIT;

    uint64_t nextFireTSR = 0ULL;
    uint64_t now2 = mach_absolute_time();
    CFAbsoluteTime now1 = CFAbsoluteTimeGetCurrent();
    if (fireDate < now1)
    {
        // 下次触发时间小于现在则立即触发
        nextFireTSR = now2;
    }
    else if (TIMER_INTERVAL_LIMIT < fireDate - now1)
    {
        // 下次触发时间间隔大于允许的最大间隔TIMER_INTERVAL_LIMIT，则将下次触发时间调整为now + TIMER_INTERVAL_LIMIT
        nextFireTSR = now2 + __CFTimeIntervalToTSR(TIMER_INTERVAL_LIMIT);
    }
    else
    {
        nextFireTSR = now2 + __CFTimeIntervalToTSR(fireDate - now1);
    }
    __CFRunLoopTimerLock(rlt);
    if (NULL != rlt->_runLoop)
    {
         // 获取runloopMode个数
        CFIndex cnt = CFSetGetCount(rlt->_rlModes);
        // 声明名为modes的栈结构
        STACK_BUFFER_DECL(CFTypeRef, modes, cnt);
         // rlt->rlModes赋值给modes栈结构
        CFSetGetValues(rlt->_rlModes, (const void **)modes);
        // To avoid A->B, B->A lock ordering issues when coming up
        // towards the run loop from a source, the timer has to be
        // unlocked, which means we have to protect from object
        // invalidation, although that's somewhat expensive.
        for (CFIndex idx = 0; idx < cnt; idx++)
        {
            CFRetain(modes[idx]);
        }
        
        CFRunLoopRef rl = (CFRunLoopRef)CFRetain(rlt->_runLoop);
        __CFRunLoopTimerUnlock(rlt);
        __CFRunLoopLock(rl);
        // 把modes集合中存储的modeName转换为mode结构体实例，然后再存入modes集合
        for (CFIndex idx = 0; idx < cnt; idx++)
        {
            CFStringRef name = (CFStringRef)modes[idx];
            modes[idx] = __CFRunLoopFindMode(rl, name, false);
            CFRelease(name);
        }
        __CFRunLoopTimerFireTSRLock();
         // 把上面计算好的下次触发时间设置给rlt
        rlt->_fireTSR = nextFireTSR;
        rlt->_nextFireDate = fireDate;
        for (CFIndex idx = 0; idx < cnt; idx++)
        {
            CFRunLoopModeRef rlm = (CFRunLoopModeRef)modes[idx];
            if (rlm)
            {
                // Reposition释义复位。所以顾名思义该函数用于复位timer，所谓复位，就是调整timer在runloopMode->timers数组中的位置
                // 此处调用该函数本质上是先移除timer，然后按照timer下次触发时间长短计算timer需要插入到runloopMode->timers数组中的位置，最后把timer插入到runloopMode->timers数组中
                __CFRepositionTimerInMode(rlm, rlt, true);
            }
        }
        __CFRunLoopTimerFireTSRUnlock();
        for (CFIndex idx = 0; idx < cnt; idx++)
        {
            __CFRunLoopModeUnlock((CFRunLoopModeRef)modes[idx]);
        }
        __CFRunLoopUnlock(rl);
        // This is setting the date of a timer, not a direct
        // interaction with a run loop, so we'll do a wakeup
        // (which may be costly) for the caller, just in case.
        // (And useful for binary compatibility with older
        // code used to the older timer implementation.)
        // 以上注释的意思是：这行代码的是为了给timer设置date，但不直接作用于runloop
        // 以防万一，我们手动唤醒runloop，尽管有可能这个代价是高昂的
        // 另一方面，这么做的目的也是为了兼容timer的之前的实现方式
        // 如果timer执行的rl不是当前的runloop，则手动唤醒
        if (rl != CFRunLoopGetCurrent())
            CFRunLoopWakeUp(rl);
        CFRelease(rl);
    }
    else
    {
        __CFRunLoopTimerFireTSRLock();
        rlt->_fireTSR = nextFireTSR;
        rlt->_nextFireDate = fireDate;
        __CFRunLoopTimerFireTSRUnlock();
        __CFRunLoopTimerUnlock(rlt);
    }
}

CFTimeInterval CFRunLoopTimerGetInterval(CFRunLoopTimerRef rlt)
{
    CHECK_FOR_FORK();
    CF_OBJC_FUNCDISPATCHV(CFRunLoopTimerGetTypeID(), CFTimeInterval, (NSTimer *)rlt, timeInterval);
    __CFGenericValidateType(rlt, CFRunLoopTimerGetTypeID());
    return rlt->_interval;
}

Boolean CFRunLoopTimerDoesRepeat(CFRunLoopTimerRef rlt)
{
    CHECK_FOR_FORK();
    __CFGenericValidateType(rlt, CFRunLoopTimerGetTypeID());
    return (0.0 < rlt->_interval);
}

CFIndex CFRunLoopTimerGetOrder(CFRunLoopTimerRef rlt)
{
    CHECK_FOR_FORK();
    __CFGenericValidateType(rlt, CFRunLoopTimerGetTypeID());
    return rlt->_order;
}

void CFRunLoopTimerInvalidate(CFRunLoopTimerRef rlt)
{ /* DOES CALLOUT */
    CHECK_FOR_FORK();
    CF_OBJC_FUNCDISPATCHV(CFRunLoopTimerGetTypeID(), void, (NSTimer *)rlt, invalidate);
    __CFGenericValidateType(rlt, CFRunLoopTimerGetTypeID());
    __CFRunLoopTimerLock(rlt);
    if (!__CFRunLoopTimerIsDeallocating(rlt))
    {
        CFRetain(rlt);
    }
    if (__CFIsValid(rlt))
    {
        CFRunLoopRef rl = rlt->_runLoop;
        void *info = rlt->_context.info;
        rlt->_context.info = NULL;
        __CFUnsetValid(rlt);
        if (NULL != rl)
        {
            CFIndex cnt = CFSetGetCount(rlt->_rlModes);
            STACK_BUFFER_DECL(CFStringRef, modes, cnt);
            CFSetGetValues(rlt->_rlModes, (const void **)modes);
            // To avoid A->B, B->A lock ordering issues when coming up
            // towards the run loop from a source, the timer has to be
            // unlocked, which means we have to protect from object
            // invalidation, although that's somewhat expensive.
            for (CFIndex idx = 0; idx < cnt; idx++)
            {
                CFRetain(modes[idx]);
            }
            CFRetain(rl);
            __CFRunLoopTimerUnlock(rlt);
            // CFRunLoopRemoveTimer will lock the run loop while it
            // needs that, but we also lock it out here to keep
            // changes from occurring for this whole sequence.
            __CFRunLoopLock(rl);
            for (CFIndex idx = 0; idx < cnt; idx++)
            {
                CFRunLoopRemoveTimer(rl, rlt, modes[idx]);
            }
            CFRunLoopRemoveTimer(rl, rlt, kCFRunLoopCommonModes);
            __CFRunLoopUnlock(rl);
            for (CFIndex idx = 0; idx < cnt; idx++)
            {
                CFRelease(modes[idx]);
            }
            CFRelease(rl);
            __CFRunLoopTimerLock(rlt);
        }
        if (NULL != rlt->_context.release)
        {
            rlt->_context.release(info); /* CALLOUT */
        }
    }
    __CFRunLoopTimerUnlock(rlt);
    if (!__CFRunLoopTimerIsDeallocating(rlt))
    {
        CFRelease(rlt);
    }
}

Boolean CFRunLoopTimerIsValid(CFRunLoopTimerRef rlt)
{
    CHECK_FOR_FORK();
    CF_OBJC_FUNCDISPATCHV(CFRunLoopTimerGetTypeID(), Boolean, (NSTimer *)rlt, isValid);
    __CFGenericValidateType(rlt, CFRunLoopTimerGetTypeID());
    return __CFIsValid(rlt);
}

void CFRunLoopTimerGetContext(CFRunLoopTimerRef rlt, CFRunLoopTimerContext *context)
{
    CHECK_FOR_FORK();
    __CFGenericValidateType(rlt, CFRunLoopTimerGetTypeID());
    CFAssert1(0 == context->version, __kCFLogAssertion, "%s(): context version not initialized to 0", __PRETTY_FUNCTION__);
    *context = rlt->_context;
}

CFTimeInterval CFRunLoopTimerGetTolerance(CFRunLoopTimerRef rlt)
{
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_EMBEDDED_MINI
    CHECK_FOR_FORK();
    CF_OBJC_FUNCDISPATCHV(CFRunLoopTimerGetTypeID(), CFTimeInterval, (NSTimer *)rlt, tolerance);
    __CFGenericValidateType(rlt, CFRunLoopTimerGetTypeID());
    return rlt->_tolerance;
#else
    return 0.0;
#endif
}

void CFRunLoopTimerSetTolerance(CFRunLoopTimerRef rlt, CFTimeInterval tolerance)
{
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_EMBEDDED || DEPLOYMENT_TARGET_EMBEDDED_MINI
    CHECK_FOR_FORK();
    CF_OBJC_FUNCDISPATCHV(CFRunLoopTimerGetTypeID(), void, (NSTimer *)rlt, setTolerance
                          : tolerance);
    __CFGenericValidateType(rlt, CFRunLoopTimerGetTypeID());
    /*
     * dispatch rules:
     *
     * For the initial timer fire at 'start', the upper limit to the allowable
     * delay is set to 'leeway' nanoseconds. For the subsequent timer fires at
     * 'start' + N * 'interval', the upper limit is MIN('leeway','interval'/2).
     */
    if (rlt->_interval > 0)
    {
        rlt->_tolerance = MIN(tolerance, rlt->_interval / 2);
    }
    else
    {
        // Tolerance must be a positive value or zero
        if (tolerance < 0)
            tolerance = 0.0;
        rlt->_tolerance = tolerance;
    }
#endif
}
