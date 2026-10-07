/*
 * Copyright (c) 2026 Gan
 * SPDX-License-Identifier: MIT
 *
 * ml_pool — Slab-style memory pool with small-page / large-block strategy.
 *
 * Thread safety: NOT thread-safe.  f_alloc / f_free and all pool management
 * functions must be externally synchronized if the same pool is shared across
 * threads.  It is recommended to use one pool per thread or protect access
 * with a mutex.
 */

#ifndef __ML_POOL_H__
#define __ML_POOL_H__

#include <stdlib.h>
#include <string.h>
#include "core/ml_types.h"

#ifdef __cplusplus
extern "C" {
#endif


/**********************************************************
  内存池优点：
  1）提供统一的内存的管理
  2）减少内存碎片的产生
  3）减少应用程序在申请、释放内存时在用户态与内核态中切换
  4）提高内存申请、释放的性能
  5）最大程度的减少内存泄露问题
    
  内存池设计原理：
  1）将内存划分为页内存与大内存
  2）申请小内存时，从页内存中申请
  3）申请大内存时，从堆空间中申请
  4）提供单链表，用于存储释放的内存
  5）下一次申请时，从单链表中查找可复用的内存
**********************************************************/




/* 内存数据，用于描述一块申请的内存 */
typedef struct ml_memory_s                ml_memory_t;

/* 页内存，用于描述一页申请的内存 */
typedef struct ml_page_s                  ml_page_t;

/* 页内存首部 */
typedef struct ml_page_head_s             ml_page_head_t;

/* 大内存首部 */
typedef struct ml_large_head_s            ml_large_head_t;

/* 内存池数据结构 */
typedef struct ml_pool_s                  ml_pool_t;

/* 大内存结构 */
typedef struct ml_large_s                 ml_large_t;



typedef void * (*ml_pool_alloc_pt)(ml_pool_t * pool, ml_uint64_t size);
typedef void   (*ml_pool_free_pt)(ml_pool_t * pool, void * p);




struct ml_memory_s {
    /* 是否在空闲链表中 */
    ml_uint64_t            m_set:1;
    /* 是否使用的标志位 */
    ml_uint64_t            m_used:1;
    ml_uint64_t            m_size;
    /* 空闲链表指针 */
    struct ml_memory_s       *m_next_free;
};


struct ml_page_s {
    /* 内存起始地址 */
    ml_uint8_t  *          m_first;
    /* 内存结束地址 */
    ml_uint8_t  *          m_last;
    /* 内存申请过程中动态地址 */
    ml_uint8_t  *          m_data;
    struct ml_page_s *     m_next;
};


struct ml_page_head_s {
    /* 系统页内存大小 */
    ml_uint64_t            m_page_size;
    /* 可用的页内存大小 */
    ml_uint64_t            m_page_left;
    ml_page_t  *           m_page_first;
};


struct ml_large_s {
    struct ml_large_s   *  m_next;
};



struct ml_large_head_s {
    struct ml_large_s   *  m_first;
};


struct ml_pool_s {
    ml_page_head_t         m_page;
    ml_large_head_t        m_large;
    ml_memory_t              *m_free_first;

    ml_pool_alloc_pt       f_alloc;
    ml_pool_free_pt        f_free;
};


/* 内存已使用 */
#define MEMORY_STATUS_USED                          1
/* 内存未使用 */
#define MEMORY_STATUS_UNUSED                        0
/* 内存已在容器 */
#define MEMORY_STATUS_SETIN                         1
/* 内存未在容器 */
#define MEMORY_STATUS_SETOUT                        0


#define GET_MEMORY_USED(M)                          ((M)->m_used)
#define SET_MEMORY_USED(M, N)                       (((M)->m_used) = (N))
#define GET_MEMORY_SETD(M)                          ((M)->m_set)
#define SET_MEMORY_SETD(M, N)                       (((M)->m_set) = (N))


#define ml_alloc(n)                                malloc(n)
#define ml_calloc(n, s)                            calloc(n, s)
#define ml_realloc(p, n)                           realloc(p, n)
#define ml_free                                    free
#define ml_strdup(s)                               strdup(s)  /* deprecated: use ml_pool_strdup */
#define ml_memzero(buf, n)                         (void) memset(buf, 0, n)
#define ml_memset(buf, c, n)                       (void) memset(buf, c, n)
#define ml_memcpy(s, d, l)                         (void) memcpy(s, d, l)
#define ml_memcmp(p1, p2, l)                       memcmp(p1, p2, l)




#define ML_POOL_ALIGNLENGTH                         8
#define ML_POOL_ALIGNMENT(d, a)                    (((d) + (a - 1)) & ~(a - 1))


/* 内存池构造函数，创建内存池，size为初始页大小 */
extern int  ml_pool_create(ml_pool_t * pool, ml_uint64_t size);


/* 内存池析构函数，销毁内存池 */
extern void ml_pool_destroy(ml_pool_t * pool);

/* pool-owned string duplication — allocated from pool, caller must not free */
extern char *ml_pool_strdup(ml_pool_t *pool, const char *s);
extern char *ml_pool_strndup(ml_pool_t *pool, const char *s, size_t n);

#ifdef __cplusplus
}
#endif

#endif
