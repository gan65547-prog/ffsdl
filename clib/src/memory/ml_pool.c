/*
 * Copyright (c) 2026 Gan
 * SPDX-License-Identifier: MIT
 */

#include "core/ml_os.h"
#include "memory/ml_pool.h"
#include "core/ml_code.h"

#include <assert.h>

static_assert(sizeof(ml_memory_t) >= sizeof(void *) * 3,
    "ml_memory_t layout is unexpectedly small");





static ml_page_t *  ml_pool_create_page(ml_page_head_t * pool, ml_uint64_t size)
{
    void * p;
    ml_uint8_t * cur;
    ml_page_t * page;
    ml_uint64_t length;

    length = ( (size > (sizeof(ml_page_t))) && (size < pool->m_page_size) ) ? size : pool->m_page_size;

    p = ml_alloc(length);
    if (p) {
        page = (ml_page_t *)p;
        cur  = (ml_uint8_t *)p;

        page->m_first = cur + sizeof(ml_page_t);
        page->m_data  = page->m_first;
        page->m_last  = cur + length;

        page->m_next = pool->m_page_first;
        pool->m_page_first = page;

        return page;
    }

    return NULL;
}


static ml_memory_t * ml_alloc_from_page(ml_page_head_t * pool, ml_uint64_t size)
{
    ml_uint64_t length;
    ml_page_t * cur;
    ml_memory_t * m;

    if (size > (ml_uint64_t)-1 - sizeof(ml_memory_t))
        return NULL;
    length = size + sizeof(ml_memory_t);

    /* 从页内存中查找空间 */
    cur = pool->m_page_first;
    while (cur) {

        if (cur->m_data <= cur->m_last
            && (ml_uint64_t)(cur->m_last - cur->m_data) >= length) {
            m = (ml_memory_t *)cur->m_data;
            m->m_set = MEMORY_STATUS_SETOUT;
            m->m_used = MEMORY_STATUS_UNUSED;
            m->m_next_free = NULL;

            /* 更新页内存中的data指针 */
            cur->m_data += length;
            pool->m_page_left -= length;

            m->m_size = length;
            return m;
        }
        cur = cur->m_next;
    }


    /* 创建新页内存 */
    cur  = ml_pool_create_page(pool, pool->m_page_size);
    if (cur) {
        if (cur->m_data <= cur->m_last
            && (ml_uint64_t)(cur->m_last - cur->m_data) >= length) {
            m = (ml_memory_t *)cur->m_data;
            m->m_set = MEMORY_STATUS_SETOUT;
            m->m_used = MEMORY_STATUS_UNUSED;
            m->m_next_free = NULL;

            /* 更新页内存中的data指针 */
            cur->m_data += length;
            pool->m_page_left -= length;

            m->m_size = length;
            return m;
        }
    }

    return NULL;
}



static ml_memory_t * ml_alloc_from_large(ml_large_head_t * pool, ml_uint64_t size)
{
    ml_memory_t * m;
    ml_uint64_t length, key;
    ml_uint8_t * cur;
    ml_large_t * large;
    void * p;

    if (size > (ml_uint64_t)-1 - sizeof(ml_memory_t))
        return NULL;
    key = size + sizeof(ml_memory_t);
    if (key > (ml_uint64_t)-1 - sizeof(ml_large_t))
        return NULL;
    length = key + sizeof(ml_large_t);

    p = ml_alloc(length);
    if (p) {
        cur = (ml_uint8_t *)p;
        large = (ml_large_t*)p;
        large->m_next = pool->m_first;
        pool->m_first = large;

        m = (ml_memory_t *)(cur + sizeof(ml_large_t));

        m->m_size = key;
        m->m_set = MEMORY_STATUS_SETOUT;
        m->m_used = MEMORY_STATUS_UNUSED;
        m->m_next_free = NULL;
        return m;
    }

    return NULL;
}



static ml_memory_t * ml_alloc_from_free_list(ml_pool_t * pool, ml_uint64_t size)
{
    ml_memory_t *prev = NULL;
    ml_memory_t *m = pool->m_free_first;
    ml_uint64_t required = size + sizeof(ml_memory_t);

    while (m) {
        if (m->m_size >= required && m->m_used == MEMORY_STATUS_UNUSED) {
            if (prev) prev->m_next_free = m->m_next_free;
            else pool->m_free_first = m->m_next_free;
            m->m_next_free = NULL;
            m->m_set = MEMORY_STATUS_SETOUT;
            m->m_used = MEMORY_STATUS_USED;
            return m;
        }
        prev = m;
        m = m->m_next_free;
    }
    return NULL;
}


static void * ml_pool_alloc(ml_pool_t * pool, ml_uint64_t size)
{
    ml_memory_t * m;
    ml_uint64_t length;
    void * ptr = NULL;


    /* 申请的内存对齐 */
    if (size > (ml_uint64_t)-1 - (ML_POOL_ALIGNLENGTH - 1))
        return NULL;
    size = ML_POOL_ALIGNMENT(size, ML_POOL_ALIGNLENGTH);
    /* 实际申请的内存大小 */
    if (size > (ml_uint64_t)-1 - sizeof(ml_memory_t))
        return NULL;
    length = size + sizeof(ml_memory_t);

    m = ml_alloc_from_free_list(pool, size);
    if (m) {
        ptr = (ml_uint8_t*)m + sizeof(ml_memory_t);
    }
    else {
        /* 从页内存申请 */
        if (length < pool->m_page.m_page_left) {
            m = ml_alloc_from_page(&pool->m_page, size);
        }
        /* 从大内存申请 */
        else {
            m = ml_alloc_from_large(&pool->m_large, size);
        }

        /* 设置内存状态为,不在容器中 */
        if (m) {
            SET_MEMORY_SETD(m, MEMORY_STATUS_SETOUT);
            SET_MEMORY_USED(m, MEMORY_STATUS_USED);
            ptr = (ml_uint8_t*)m + sizeof(ml_memory_t);
        }
    }

    return ptr;
}


static void ml_pool_free(ml_pool_t * pool, void * p)
{
    ml_memory_t * m;

    if (!p)
        return;

    m = (ml_memory_t *)((ml_uint8_t*)p - sizeof(ml_memory_t));

    /* 设置容器中的内存状态为未使用 */
    SET_MEMORY_USED(m, MEMORY_STATUS_UNUSED);

    /* 放入空闲单链表，避免重复插入 */
    if (MEMORY_STATUS_SETOUT == GET_MEMORY_SETD(m)) {
        SET_MEMORY_SETD(m, MEMORY_STATUS_SETIN);
        m->m_next_free = pool->m_free_first;
        pool->m_free_first = m;
    }
}


static void ml_pool_page_destroy(ml_pool_t * pool)
{
    ml_page_t * p, * n, * h;

    h = pool->m_page.m_page_first;
    
    for (p = h; p; p = n) {
        n = p->m_next;
        ml_free(p);
    }

    pool->m_page.m_page_first = NULL;
}


static void ml_pool_large_destroy(ml_pool_t * pool)
{
    ml_large_t * p, * n, * h;

    h = pool->m_large.m_first;
    for (p = h; p; p = n) {
        n = p->m_next;
        ml_free(p);
    }
}




int  ml_pool_create(ml_pool_t * pool, ml_uint64_t size)
{
    /* The pool is single-threaded; callers must synchronize shared access. */
    pool->m_page.m_page_size     = (size > sizeof(ml_page_t)) ? size : (ml_uint64_t)ml_pagesize();
    pool->m_page.m_page_left     = pool->m_page.m_page_size - sizeof(ml_page_t);
    pool->m_page.m_page_first    = NULL;

    pool->m_large.m_first        = NULL;
    pool->m_free_first           = NULL;

    pool->f_alloc = ml_pool_alloc;
    pool->f_free  = ml_pool_free;

    /* 创建一个新页 */
    if (ml_pool_create_page(&pool->m_page, size)) {
        return ML_OK;
    }

    return ML_ENOMEM;
}


void ml_pool_destroy(ml_pool_t * pool)
{
    ml_pool_page_destroy(pool);
    ml_pool_large_destroy(pool);

    pool->m_page.m_page_size     = ml_pagesize();
    pool->m_page.m_page_left     = pool->m_page.m_page_size - sizeof(ml_page_t);
    pool->m_page.m_page_first    = NULL;

    pool->m_large.m_first        = NULL;
    pool->m_free_first           = NULL;

    pool->f_alloc = ml_pool_alloc;
    pool->f_free  = ml_pool_free;
}


char *ml_pool_strdup(ml_pool_t *pool, const char *s)
{
    size_t len;
    char *copy;

    if (!pool || !s)
        return NULL;
    len = strlen(s) + 1;
    copy = pool->f_alloc(pool, len);
    if (copy) memcpy(copy, s, len);
    return copy;
}


char *ml_pool_strndup(ml_pool_t *pool, const char *s, size_t n)
{
    size_t len;
    char *copy;

    if (!pool || !s)
        return NULL;
    for (len = 0; len < n && s[len]; len++) {}
    copy = pool->f_alloc(pool, len + 1);
    if (copy) {
        memcpy(copy, s, len);
        copy[len] = '\0';
    }
    return copy;
}
