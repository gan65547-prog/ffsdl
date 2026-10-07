/*
 * Copyright (c) 2026 Gan
 * SPDX-License-Identifier: MIT
 */




#include "core/ml_types.h"
#include "core/ml_code.h"
#include "runtime/ml_thread.h"








int ml_thread_spinlock_create(ml_spinlock_t * spn)
{
    int err;

#ifdef __linux__
    err = pthread_spin_init(spn, PTHREAD_PROCESS_PRIVATE);
#else
    /* macOS: use mutex as spinlock fallback */
    pthread_mutexattr_t attr;
    err = pthread_mutexattr_init(&attr);
    if (err)
        return ML_ERROR;
    err = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
    if (err) { pthread_mutexattr_destroy(&attr); return ML_ERROR; }
    err = pthread_mutex_init(spn, &attr);
    pthread_mutexattr_destroy(&attr);
#endif
    if (err)
        return ML_ERROR;

    return ML_OK;
}



int ml_thread_spinlock_destroy(ml_spinlock_t * spn)
{
    int err;

#ifdef __linux__
    err = pthread_spin_destroy(spn);
#else
    err = pthread_mutex_destroy(spn);
#endif

    return (err) ? ML_ERROR : ML_OK;
}



int ml_thread_spinlock_lock(ml_spinlock_t * spn)
{
    int err;

#ifdef __linux__
    err = pthread_spin_lock(spn);
#else
    err = pthread_mutex_lock(spn);
#endif

    return (err) ? ML_ERROR : ML_OK;
}



int ml_thread_spinlock_unlock(ml_spinlock_t * spn)
{
    int err;

#ifdef __linux__
    err = pthread_spin_unlock(spn);
#else
    err = pthread_mutex_unlock(spn);
#endif

    return (err) ? ML_ERROR : ML_OK;
}






int   ml_thread_mutex_create(ml_thread_mutex_t *mtx)
{
    pthread_mutexattr_t  attr;
    int err;

    err = pthread_mutexattr_init(&attr);
    if (err != 0) {
        return ML_ERROR;
    }

    err = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
    if (err != 0) {
        pthread_mutexattr_destroy(&attr);
        return ML_ERROR;
    }

    err = pthread_mutex_init(mtx, &attr);
    pthread_mutexattr_destroy(&attr);
    if (err != 0) {
        return ML_ERROR;
    }

    return ML_OK;
}



int   ml_thread_mutex_destroy(ml_thread_mutex_t *mtx)
{
    int  err;

    err = pthread_mutex_destroy(mtx);
    if (err != 0) {
        return ML_ERROR;
    }

    return ML_OK;
}



int   ml_thread_mutex_lock(ml_thread_mutex_t *mtx)
{
    int  err;

    err = pthread_mutex_lock(mtx);
    if (err == 0) {
        return ML_OK;
    }

    return ML_ERROR;
}


int   ml_thread_mutex_unlock(ml_thread_mutex_t *mtx)
{
    int  err;

    err = pthread_mutex_unlock(mtx);
    if (err == 0) {
        return ML_OK;
    }

    return ML_ERROR;
}


int   ml_thread_cond_create(ml_thread_cond_t *cond)
{
    int  err;

    err = pthread_cond_init(cond, NULL);
    if (err == 0) {
        return ML_OK;
    }

    return ML_ERROR;
}



int   ml_thread_cond_destroy(ml_thread_cond_t *cond)
{
    int  err;

    err = pthread_cond_destroy(cond);
    if (err == 0) {
        return ML_OK;
    }

    return ML_ERROR;
}



int   ml_thread_cond_signal(ml_thread_cond_t *cond)
{
    int  err;

    err = pthread_cond_signal(cond);
    if (err == 0) {
        return ML_OK;
    }

    return ML_ERROR;
}


int   ml_thread_cond_broadcast(ml_thread_cond_t *cond)
{
    int  err;

    err = pthread_cond_broadcast(cond);
    if (err == 0) {
        return ML_OK;
    }

    return ML_ERROR;
}



int   ml_thread_cond_wait(ml_thread_cond_t *cond, ml_thread_mutex_t *mtx)
{
    int  err;

    err = pthread_cond_wait(cond, mtx);
    if (err == 0) {
        return ML_OK;
    }
    
    return ML_ERROR;
}



int   ml_thread_rwlock_create(ml_thread_rwlock_t *rwl)
{
    int err;

    err = pthread_rwlock_init(rwl, NULL);
    if (err == 0) {
        return ML_OK;
    }
    
    return ML_ERROR;
}



int   ml_thread_rwlock_destroy(ml_thread_rwlock_t *rwl)
{
    int err;

    err = pthread_rwlock_destroy(rwl);
    if (err == 0) {
        return ML_OK;
    }
    
    return ML_ERROR;    
}





int   ml_thread_rwlock_rlock(ml_thread_rwlock_t *rwl)
{
    int err;
    
    err = pthread_rwlock_rdlock(rwl);
    if (err == 0) {
        return ML_OK;
    }

    return ML_ERROR;
}




int   ml_thread_rwlock_wlock(ml_thread_rwlock_t *rwl)
{
    int err;
    
    err = pthread_rwlock_wrlock(rwl);
    if (err == 0) {
        return ML_OK;
    }
    
    return ML_ERROR;
}




int   ml_thread_rwlock_unlock(ml_thread_rwlock_t *rwl)
{
    int err;
    
    err = pthread_rwlock_unlock(rwl);
    if (err == 0) {
        return ML_OK;
    }
    
    return ML_ERROR;
}

