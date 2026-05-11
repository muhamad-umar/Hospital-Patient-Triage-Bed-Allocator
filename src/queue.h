/*
 * ============================================================
 * Project : Hospital Patient Triage & Bed Allocator
 * File    : queue.h
 * Purpose : Interface for the thread-safe priority queue used
 *           to hold waiting patients before bed assignment.
 *           Lower priority number = higher urgency (1 = critical).
 * ============================================================
 */

#ifndef QUEUE_H
#define QUEUE_H

#include "common.h"

/* Maximum nodes the heap can hold */
#define HEAP_CAPACITY MAX_PATIENTS

/*
 * PriorityQueue — min-heap ordered by (priority, arrival_time).
 * A patient with priority 1 is always dequeued before priority 2.
 * Ties are broken by FCFS (earlier arrival_time wins).
 *
 * The queue is protected internally by a mutex + condition variable
 * so it is safe to call enqueue/dequeue from multiple threads.
 */
typedef struct {
    PatientRecord   heap[HEAP_CAPACITY];  /* heap array */
    int             size;                 /* current element count */
    pthread_mutex_t lock;                 /* protects heap reads/writes */
    pthread_cond_t  not_empty;            /* signalled when item added */
    pthread_cond_t  not_full;             /* signalled when item removed */
    sem_t          *sem_slots;            /* bounded semaphore (QUEUE_CAPACITY) */
} PriorityQueue;

/* -------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------- */

/*
 * queue_init — initialise the priority queue.
 * Must be called once before any enqueue/dequeue.
 * Returns 0 on success, -1 on failure.
 */
int queue_init(PriorityQueue *q);

/*
 * queue_destroy — release all resources held by the queue.
 */
void queue_destroy(PriorityQueue *q);

/* -------------------------------------------------------
 * Operations
 * ------------------------------------------------------- */

/*
 * queue_enqueue — insert a PatientRecord into the heap.
 * Blocks if the queue is full (producer-consumer pattern).
 * Returns 0 on success, -1 if queue is full after timeout.
 */
int queue_enqueue(PriorityQueue *q, const PatientRecord *p);

/*
 * queue_dequeue — remove and return the highest-priority patient.
 * Blocks until an item is available (consumer side).
 * Returns 0 on success, -1 if the queue was destroyed while waiting.
 */
int queue_dequeue(PriorityQueue *q, PatientRecord *out);

/*
 * queue_try_dequeue — non-blocking dequeue.
 * Returns 0 and fills *out if an item was available, -1 otherwise.
 */
int queue_try_dequeue(PriorityQueue *q, PatientRecord *out);

/*
 * queue_size — return the current number of waiting patients.
 */
int queue_size(PriorityQueue *q);

/*
 * queue_is_empty — return 1 if empty, 0 otherwise.
 */
int queue_is_empty(PriorityQueue *q);

#endif /* QUEUE_H */
