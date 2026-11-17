#ifndef MAP_PLATFORM_H
#define MAP_PLATFORM_H

/* ============================================================
   catsena —— 平台抽象层(Win32 / POSIX)
   封装锁/条件变量/线程三种同步原语, 让 map.c/mapworker.c
   一份源码双平台编译。Linux 部署见 DEPENDENCIES.md。
   依赖: Windows = kernel32(自带); Linux = pthread(-lpthread, glibc 2.34+ 无需显式链接)
   ============================================================ */

#include <stdint.h>   /* uint64_t(节点 id 分配器) */

#ifdef _WIN32

/* _WIN32_WINNT 不再自定义: 沿用工具链默认(MinGW 0x0601 / MSVC 对应值)。
   SRWLock/CONDITION_VARIABLE 需要 >=0x0600, 默认基线均满足; 避免与库定义打架。 */
#include <windows.h>

typedef SRWLOCK           map_rwlock_t;
typedef CRITICAL_SECTION  map_mutex_t;
typedef CONDITION_VARIABLE map_cond_t;
typedef HANDLE            map_thread_t;
typedef DWORD (WINAPI *map_thread_fn)(LPVOID);
#define MAP_THREAD_RET DWORD WINAPI
#define MAP_THREAD_RETURN(x) return (x)   /* x=0 表示线程退出 */

static inline void map_rwlock_init(map_rwlock_t* l) { InitializeSRWLock(l); }
static inline void map_rwlock_rd(map_rwlock_t* l)  { AcquireSRWLockShared(l); }
static inline void map_rwlock_wr(map_rwlock_t* l)  { AcquireSRWLockExclusive(l); }
static inline void map_rwlock_rdun(map_rwlock_t* l){ ReleaseSRWLockShared(l); }
static inline void map_rwlock_wrun(map_rwlock_t* l){ ReleaseSRWLockExclusive(l); }
static inline void map_mutex_init(map_mutex_t* m)   { InitializeCriticalSection(m); }
static inline void map_mutex_lock(map_mutex_t* m)   { EnterCriticalSection(m); }
static inline void map_mutex_unlock(map_mutex_t* m) { LeaveCriticalSection(m); }
static inline void map_mutex_destroy(map_mutex_t* m){ DeleteCriticalSection(m); }
static inline void map_cond_wait(map_cond_t* c, map_mutex_t* m) { SleepConditionVariableCS(c, m, INFINITE); }
static inline void map_cond_wake(map_cond_t* c)     { WakeConditionVariable(c); }
static inline void map_cond_wakeall(map_cond_t* c)  { WakeAllConditionVariable(c); }
static inline map_thread_t map_thread_start(map_thread_fn fn, void* arg) {
    return CreateThread(NULL, 0, fn, arg, 0, NULL);
}
static inline void map_thread_wait(map_thread_t* t) { if (t && *t) WaitForSingleObject(*t, INFINITE); }
static inline void map_thread_close(map_thread_t* t){ if (t && *t) CloseHandle(*t); }

#else  /* ---- POSIX(Linux/macOS) ---- */

#include <pthread.h>
#include <unistd.h>

typedef void* LPVOID;   /* 兼容 Windows 习惯的调用点(mapworker 线程参数) */

typedef pthread_rwlock_t map_rwlock_t;
typedef pthread_mutex_t  map_mutex_t;
typedef pthread_cond_t   map_cond_t;
typedef pthread_t        map_thread_t;
typedef void* (*map_thread_fn)(void*);
#define MAP_THREAD_RET void*
#define MAP_THREAD_RETURN(x) return (void*)(x)

static inline void map_rwlock_init(map_rwlock_t* l) { pthread_rwlock_init(l, NULL); }
static inline void map_rwlock_rd(map_rwlock_t* l)  { pthread_rwlock_rdlock(l); }
static inline void map_rwlock_wr(map_rwlock_t* l)  { pthread_rwlock_wrlock(l); }
static inline void map_rwlock_rdun(map_rwlock_t* l){ pthread_rwlock_unlock(l); }
static inline void map_rwlock_wrun(map_rwlock_t* l){ pthread_rwlock_unlock(l); }
static inline void map_mutex_init(map_mutex_t* m)   { pthread_mutex_init(m, NULL); }
static inline void map_mutex_lock(map_mutex_t* m)   { pthread_mutex_lock(m); }
static inline void map_mutex_unlock(map_mutex_t* m) { pthread_mutex_unlock(m); }
static inline void map_mutex_destroy(map_mutex_t* m){ pthread_mutex_destroy(m); }
static inline void map_cond_wait(map_cond_t* c, map_mutex_t* m) { pthread_cond_wait(c, m); }
static inline void map_cond_wake(map_cond_t* c)     { pthread_cond_signal(c); }
static inline void map_cond_wakeall(map_cond_t* c)  { pthread_cond_broadcast(c); }
static inline map_thread_t map_thread_start(map_thread_fn fn, void* arg) {
    pthread_t t; pthread_create(&t, NULL, fn, arg); return t;
}
static inline void map_thread_wait(map_thread_t* t) { if (t) pthread_join(*t, NULL); }
static inline void map_thread_close(map_thread_t* t){ (void)t; }

#endif

/* ================= 节点 id 分配(64 位原子递增) =================
   id 是「持久句柄」: 内容怎么改, id 不变 —— 视图态/书签/撤销用它做 key。
   与 uuid(内容指纹, 内容一变就换)的分工见 DATA_MODEL.md §1。
   两个原语:
     map_id_alloc : 取一个新 id(并发建节点安全)
     map_id_bump  : 声明"used 这个 id 已被占用", 保证后续分配都 > used
                    (载入存档还原 id 后调用, 否则新节点会与旧 id 撞车) */

#if defined(_WIN32)

#include <intrin.h>
static inline uint64_t map_id_alloc(volatile uint64_t* p) {
    return (uint64_t)_InterlockedIncrement64((volatile __int64*)p) - 1;
}
static inline void map_id_bump(volatile uint64_t* p, uint64_t used) {
    volatile __int64* q = (volatile __int64*)p;
    for (;;) {
        __int64 cur = *q;                                   /* 8 字节对齐读, x86/x64 原子 */
        if ((uint64_t)cur > used) return;
        if (_InterlockedCompareExchange64(q, (__int64)(used + 1), cur) == cur) return;
    }
}

#else  /* gcc/clang 内建原子 */

static inline uint64_t map_id_alloc(volatile uint64_t* p) {
    return __atomic_fetch_add(p, 1, __ATOMIC_RELAXED);
}
static inline void map_id_bump(volatile uint64_t* p, uint64_t used) {
    uint64_t cur = __atomic_load_n(p, __ATOMIC_RELAXED);
    while (cur <= used) {   /* CAS 失败时 cur 被写成实际值, 重新判断即可 */
        if (__atomic_compare_exchange_n(p, &cur, used + 1, 0,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return;
    }
}

#endif
#endif /* MAP_PLATFORM_H */
