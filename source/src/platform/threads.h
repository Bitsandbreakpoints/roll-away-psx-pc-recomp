/*
 * threads.h - a small pool of worker threads for splitting work (the
 * high-resolution renderer draws horizontal bands in parallel).
 */
#ifndef PLATFORM_THREADS_H
#define PLATFORM_THREADS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Number of tasks worth running at once (logical processors, max 16). */
int threads_count(void);

/* Runs fn(task, context) for task = 0 .. tasks-1 on the pool and the
 * calling thread; returns when all have finished. */
void threads_run(int tasks, void (*fn)(int task, void *context), void *context);

/* A lock (it may be taken again by the thread holding it). */
typedef struct ThreadsLock ThreadsLock;

ThreadsLock *threads_lock_create(void);
void threads_lock(ThreadsLock *lock);
void threads_unlock(ThreadsLock *lock);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_THREADS_H */
