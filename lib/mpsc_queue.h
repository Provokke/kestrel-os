/* Intrusive multi-producer / single-consumer queue built on one atomic
 * exchange per push (Vyukov-style, with a stub node). Producers never block
 * each other; the consumer may briefly observe a "busy" state while a producer
 * is between its exchange and its link store, in which case pop returns NULL
 * and mpsc_maybe_nonempty() stays true. */
#ifndef KESTREL_MPSC_QUEUE_H
#define KESTREL_MPSC_QUEUE_H
#include <stdbool.h>
#include <stddef.h>

struct mpsc_node {
    struct mpsc_node *next;
};

struct mpsc_queue {
    _Alignas(64) struct mpsc_node *head; /* producers exchange here */
    _Alignas(64) struct mpsc_node *tail; /* consumer-private */
    struct mpsc_node stub;
};

void mpsc_init(struct mpsc_queue *q);
void mpsc_push(struct mpsc_queue *q, struct mpsc_node *n);
struct mpsc_node *mpsc_pop(struct mpsc_queue *q);
bool mpsc_maybe_nonempty(struct mpsc_queue *q);

#define mpsc_entry(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#endif
