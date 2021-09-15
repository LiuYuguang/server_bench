
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#ifndef _QUEUE_H_
#define _QUEUE_H_

#include <stddef.h>

typedef struct queue_s  queue_t;

struct queue_s {
    queue_t  *prev;
    queue_t  *next;
};


#define queue_init(q)                                                     \
    (q)->prev = q;                                                            \
    (q)->next = q


#define queue_empty(h)                                                    \
    (h == (h)->prev)


#define queue_insert_head(h, x)                                           \
    (x)->next = (h)->next;                                                    \
    (x)->next->prev = x;                                                      \
    (x)->prev = h;                                                            \
    (h)->next = x


#define queue_insert_after   queue_insert_head


#define queue_insert_tail(h, x)                                           \
    (x)->prev = (h)->prev;                                                    \
    (x)->prev->next = x;                                                      \
    (x)->next = h;                                                            \
    (h)->prev = x


#define queue_head(h)                                                     \
    (h)->next


#define queue_last(h)                                                     \
    (h)->prev


#define queue_sentinel(h)                                                 \
    (h)


#define queue_next(q)                                                     \
    (q)->next


#define queue_prev(q)                                                     \
    (q)->prev

#define queue_remove(x)                                                   \
    (x)->next->prev = (x)->prev;                                              \
    (x)->prev->next = (x)->next;                                              \
    (x)->prev = x;                                                         \
    (x)->next = x



#define queue_split(h, q, n)                                              \
    (n)->prev = (h)->prev;                                                    \
    (n)->prev->next = n;                                                      \
    (n)->next = q;                                                            \
    (h)->prev = (q)->prev;                                                    \
    (h)->prev->next = h;                                                      \
    (q)->prev = n;


#define queue_add(h, n)                                                   \
    (h)->prev->next = (n)->next;                                              \
    (n)->next->prev = (h)->prev;                                              \
    (h)->prev = (n)->prev;                                                    \
    (h)->prev->next = h;


#define queue_data(q, type, link)                                         \
    (type *) ((u_char *) q - offsetof(type, link))

#endif /* _NGX_QUEUE_H_INCLUDED_ */
