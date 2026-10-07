/*
 * ml_thread — Portable threading primitives wrapper.
 *
 * Thin wrappers around pthreads for mutex, spinlock, rwlock, and condition
 * variables.  On Linux spinlocks use the native pthread_spinlock_t; on macOS
 * they fall back to a pthread_mutex_t.
 */
#ifndef ML_THREAD_H
#define ML_THREAD_H

#include "core/ml_types.h"

#include <pthread.h>
#include <sched.h>

/* Convenience macros for common pthread operations */
#define ml_pthread_exit          pthread_exit
#define ml_pthread_create        pthread_create
#define ml_pthread_yield()       sched_yield()
#define ml_pthread_sleep(n)      usleep(n)
#define ml_pthread_join          pthread_join

typedef pthread_t                 ml_thread_tid_t;
typedef pthread_mutex_t           ml_thread_mutex_t;
typedef pthread_cond_t            ml_thread_cond_t;
typedef pthread_rwlock_t          ml_thread_rwlock_t;
typedef pthread_mutex_t           ml_mutexlock_t;
typedef pthread_mutexattr_t       ml_mutexattr_t;
#ifdef __linux__
typedef pthread_spinlock_t        ml_spinlock_t;
#else
typedef pthread_mutex_t           ml_spinlock_t;  /* macOS fallback */
#endif

/* ---- Spinlock ---- */
ml_int_t ml_thread_spinlock_create(ml_spinlock_t * spn);
ml_int_t ml_thread_spinlock_destroy(ml_spinlock_t * spn);
ml_int_t ml_thread_spinlock_lock(ml_spinlock_t * spn);
ml_int_t ml_thread_spinlock_unlock(ml_spinlock_t * spn);

/* ---- Mutex ---- */
ml_int_t  ml_thread_mutex_create(ml_thread_mutex_t *mtx);
ml_int_t  ml_thread_mutex_destroy(ml_thread_mutex_t *mtx);
ml_int_t  ml_thread_mutex_lock(ml_thread_mutex_t *mtx);
ml_int_t  ml_thread_mutex_unlock(ml_thread_mutex_t *mtx);

/* ---- Condition variable ---- */
ml_int_t  ml_thread_cond_create(ml_thread_cond_t *cond);
ml_int_t  ml_thread_cond_destroy(ml_thread_cond_t *cond);
ml_int_t  ml_thread_cond_signal(ml_thread_cond_t *cond);
ml_int_t  ml_thread_cond_broadcast(ml_thread_cond_t *cond);
ml_int_t  ml_thread_cond_wait(ml_thread_cond_t *cond, ml_thread_mutex_t *mtx);

/* ---- Read-write lock ---- */
ml_int_t  ml_thread_rwlock_create(ml_thread_rwlock_t *rwl);
ml_int_t  ml_thread_rwlock_destroy(ml_thread_rwlock_t *rwl);
ml_int_t  ml_thread_rwlock_rlock(ml_thread_rwlock_t *rwl);
ml_int_t  ml_thread_rwlock_wlock(ml_thread_rwlock_t *rwl);
ml_int_t  ml_thread_rwlock_unlock(ml_thread_rwlock_t *rwl);

#endif
