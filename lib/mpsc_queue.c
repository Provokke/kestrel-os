#include "mpsc_queue.h"

void mpsc_init(struct mpsc_queue *q)
{
    q->stub.next = NULL;
    q->head = &q->stub;
    q->tail = &q->stub;
}

void mpsc_push(struct mpsc_queue *q, struct mpsc_node *n)
{
    __atomic_store_n(&n->next, NULL, __ATOMIC_RELAXED);
    struct mpsc_node *prev = __atomic_exchange_n(&q->head, n, __ATOMIC_ACQ_REL);
    /* window: queue is "busy" until this link is published */
    __atomic_store_n(&prev->next, n, __ATOMIC_RELEASE);
}

struct mpsc_node *mpsc_pop(struct mpsc_queue *q)
{
    struct mpsc_node *tail = q->tail;
    struct mpsc_node *next = __atomic_load_n(&tail->next, __ATOMIC_ACQUIRE);
    if (tail == &q->stub) {
        if (!next)
            return NULL;
        q->tail = next;
        tail = next;
        next = __atomic_load_n(&next->next, __ATOMIC_ACQUIRE);
    }
    if (next) {
        q->tail = next;
        return tail;
    }
    struct mpsc_node *head = __atomic_load_n(&q->head, __ATOMIC_ACQUIRE);
    if (tail != head)
        return NULL; /* a producer is mid-push */
    mpsc_push(q, &q->stub);
    next = __atomic_load_n(&tail->next, __ATOMIC_ACQUIRE);
    if (next) {
        q->tail = next;
        return tail;
    }
    return NULL;
}

bool mpsc_maybe_nonempty(struct mpsc_queue *q)
{
    struct mpsc_node *tail = q->tail;
    return __atomic_load_n(&q->head, __ATOMIC_ACQUIRE) != &q->stub || tail != &q->stub ||
           __atomic_load_n(&tail->next, __ATOMIC_ACQUIRE) != NULL;
}
