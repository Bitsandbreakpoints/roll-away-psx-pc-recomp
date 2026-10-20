/*
 * threads.cpp - the worker pool behind threads_run().
 */
#include <windows.h>
#include "platform/threads.h"

#define MAX_THREADS             16

static HANDLE s_threads[MAX_THREADS];
static int s_thread_count = -1;         /* workers besides the caller */
static CRITICAL_SECTION s_lock;
static CONDITION_VARIABLE s_work_ready, s_work_done;
static void (*s_fn)(int, void *);
static void *s_context;
static int s_tasks, s_next_task, s_running;
static unsigned s_generation;

/* takes tasks until none are left; called with s_lock held */
static void work(void)
{
    while (s_next_task < s_tasks) {
        int task = s_next_task++;
        s_running++;
        LeaveCriticalSection(&s_lock);
        s_fn(task, s_context);
        EnterCriticalSection(&s_lock);
        s_running--;
    }
    if (s_running == 0) {
        WakeAllConditionVariable(&s_work_done);
    }
}

static DWORD WINAPI worker(void *)
{
    unsigned seen = 0;
    EnterCriticalSection(&s_lock);
    for (;;) {
        while (s_generation == seen) {
            SleepConditionVariableCS(&s_work_ready, &s_lock, INFINITE);
        }
        seen = s_generation;
        work();
    }
}

int threads_count(void)
{
    if (s_thread_count < 0) {
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        int n = (int) info.dwNumberOfProcessors;
        n = n < 1 ? 1 : n > MAX_THREADS ? MAX_THREADS : n;
        InitializeCriticalSection(&s_lock);
        InitializeConditionVariable(&s_work_ready);
        InitializeConditionVariable(&s_work_done);
        s_thread_count = 0;
        for (int i = 0; i < n - 1; i++) {
            s_threads[i] = CreateThread(NULL, 0, worker, NULL, 0, NULL);
            if (s_threads[i]) {
                s_thread_count++;
            }
        }
    }
    return s_thread_count + 1;
}

void threads_run(int tasks, void (*fn)(int, void *), void *context)
{
    if (threads_count() == 1 || tasks <= 1) {
        for (int i = 0; i < tasks; i++) {
            fn(i, context);
        }
        return;
    }
    EnterCriticalSection(&s_lock);
    s_fn = fn;
    s_context = context;
    s_tasks = tasks;
    s_next_task = 0;
    s_running = 0;
    s_generation++;
    WakeAllConditionVariable(&s_work_ready);
    work();
    while (s_next_task < s_tasks || s_running > 0) {
        SleepConditionVariableCS(&s_work_done, &s_lock, INFINITE);
    }
    LeaveCriticalSection(&s_lock);
}

struct ThreadsLock {
    CRITICAL_SECTION cs;
};

ThreadsLock *threads_lock_create(void)
{
    ThreadsLock *lock = new ThreadsLock;
    InitializeCriticalSection(&lock->cs);
    return lock;
}

void threads_lock(ThreadsLock *lock)
{
    EnterCriticalSection(&lock->cs);
}

void threads_unlock(ThreadsLock *lock)
{
    LeaveCriticalSection(&lock->cs);
}
