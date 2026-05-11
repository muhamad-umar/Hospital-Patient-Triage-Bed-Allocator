/*
 * ============================================================
 * Project : Hospital Patient Triage & Bed Allocator
 * File    : queue.c
 * Purpose : Thread-safe min-heap priority queue implementation.
 *           Ordering: lower priority number first (1=critical).
 *           Ties broken by arrival_time (FCFS).
 * Compile : gcc -Wall -Wextra -pthread -c queue.c
 * ============================================================
 */

#include "queue.h"

/* -------------------------------------------------------
 * Internal helpers — NOT thread-safe; caller must hold lock
 * ------------------------------------------------------- */

/*
 * compare — return 1 if patient a should be dequeued BEFORE b.
 * Lower priority number wins; ties go to earlier arrival_time.
 */
static int compare(const PatientRecord *a, const PatientRecord *b)
{
    if (a->priority != b->priority)
        return a->priority < b->priority;   /* smaller number = higher urgency */
    return a->arrival_time < b->arrival_time; /* FCFS tiebreak */
}

/* Swap two heap nodes */
static void swap_nodes(PatientRecord *a, PatientRecord *b)
{
    PatientRecord tmp = *a;
    *a = *b;
    *b = tmp;
}

/* Bubble the node at index i upward to restore heap property */
static void sift_up(PriorityQueue *q, int i)
{
    while (i > 0) {
        int parent = (i - 1) / 2;
        if (compare(&q->heap[i], &q->heap[parent])) {
            swap_nodes(&q->heap[i], &q->heap[parent]);
            i = parent;
        } else {
            break;
        }
    }
}

/* Bubble the node at index i downward to restore heap property */
static void sift_down(PriorityQueue *q, int i)
{
    int n = q->size;
    while (1) {
        int left  = 2 * i + 1;
        int right = 2 * i + 2;
        int best  = i;

        if (left  < n && compare(&q->heap[left],  &q->heap[best]))
            best = left;
        if (right < n && compare(&q->heap[right], &q->heap[best]))
            best = right;

        if (best == i)
            break;

        swap_nodes(&q->heap[i], &q->heap[best]);
        i = best;
    }
}

/* -------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------- */

int queue_init(PriorityQueue *q)
{
    if (!q) return -1;

    memset(q, 0, sizeof(*q));
    q->size = 0;

    /* Initialise mutex */
    if (pthread_mutex_init(&q->lock, NULL) != 0) {
        LOG_ERR("queue_init: pthread_mutex_init failed");
        return -1;
    }

    /* Condition variable signalled when an item is added */
    if (pthread_cond_init(&q->not_empty, NULL) != 0) {
        LOG_ERR("queue_init: pthread_cond_init(not_empty) failed");
        pthread_mutex_destroy(&q->lock);
        return -1;
    }

    /* Condition variable signalled when an item is removed */
    if (pthread_cond_init(&q->not_full, NULL) != 0) {
        LOG_ERR("queue_init: pthread_cond_init(not_full) failed");
        pthread_mutex_destroy(&q->lock);
        pthread_cond_destroy(&q->not_empty);
        return -1;
    }

    /*
     * Bounded semaphore: producer (receptionist) decrements before
     * enqueuing; consumer (scheduler) increments after dequeuing.
     * Starts at QUEUE_CAPACITY so the first QUEUE_CAPACITY enqueues
     * proceed without blocking.
     */
    sem_unlink(SEM_QUEUE_NAME);   /* remove any stale semaphore */
    q->sem_slots = sem_open(SEM_QUEUE_NAME, O_CREAT | O_EXCL,
                            0644, QUEUE_CAPACITY);
    if (q->sem_slots == SEM_FAILED) {
        LOG_ERR("queue_init: sem_open failed");
        pthread_mutex_destroy(&q->lock);
        pthread_cond_destroy(&q->not_empty);
        pthread_cond_destroy(&q->not_full);
        return -1;
    }

    LOG("Priority queue initialised (capacity=%d)", QUEUE_CAPACITY);
    return 0;
}

void queue_destroy(PriorityQueue *q)
{
    if (!q) return;

    pthread_mutex_lock(&q->lock);
    q->size = 0;
    /* Wake any threads blocked in dequeue so they can exit */
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->lock);

    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);

    if (q->sem_slots && q->sem_slots != SEM_FAILED) {
        sem_close(q->sem_slots);
        sem_unlink(SEM_QUEUE_NAME);
        q->sem_slots = NULL;
    }
}

/* -------------------------------------------------------
 * Operations
 * ------------------------------------------------------- */

int queue_enqueue(PriorityQueue *q, const PatientRecord *p)
{
    if (!q || !p) return -1;

    /*
     * Producer-consumer: decrement the bounded semaphore.
     * If the queue is full (QUEUE_CAPACITY items waiting) this
     * call blocks until the scheduler consumes one.
     */
    if (q->sem_slots && sem_wait(q->sem_slots) != 0) {
        LOG_ERR("queue_enqueue: sem_wait failed");
        return -1;
    }

    pthread_mutex_lock(&q->lock);

    if (q->size >= HEAP_CAPACITY) {
        /* Safety guard — should not happen with correct semaphore usage */
        pthread_mutex_unlock(&q->lock);
        if (q->sem_slots) sem_post(q->sem_slots); /* give slot back */
        LOG_ERR("queue_enqueue: heap full (patient_id=%d)", p->patient_id);
        return -1;
    }

    /* Insert at end then sift up to restore heap property */
    q->heap[q->size] = *p;
    q->size++;
    sift_up(q, q->size - 1);

    /* Signal the scheduler that at least one patient is waiting */
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);

    return 0;
}

int queue_dequeue(PriorityQueue *q, PatientRecord *out)
{
    if (!q || !out) return -1;

    pthread_mutex_lock(&q->lock);

    /* Block until the queue has at least one patient */
    while (q->size == 0) {
        pthread_cond_wait(&q->not_empty, &q->lock);
        /*
         * After waking, re-check size because a broadcast during
         * queue_destroy will set size to 0 and we need to bail out.
         */
        if (q->size == 0) {
            pthread_mutex_unlock(&q->lock);
            return -1;   /* queue destroyed / shutdown */
        }
    }

    /* The root of the min-heap is the highest-priority patient */
    *out = q->heap[0];

    /* Replace root with last element and sift down */
    q->size--;
    if (q->size > 0) {
        q->heap[0] = q->heap[q->size];
        sift_down(q, 0);
    }

    /* Signal producer that a slot is now available */
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->lock);

    /* Return the semaphore slot so a new producer can enqueue */
    if (q->sem_slots) sem_post(q->sem_slots);

    return 0;
}

int queue_try_dequeue(PriorityQueue *q, PatientRecord *out)
{
    if (!q || !out) return -1;

    pthread_mutex_lock(&q->lock);

    if (q->size == 0) {
        pthread_mutex_unlock(&q->lock);
        return -1;
    }

    *out = q->heap[0];
    q->size--;
    if (q->size > 0) {
        q->heap[0] = q->heap[q->size];
        sift_down(q, 0);
    }

    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->lock);

    if (q->sem_slots) sem_post(q->sem_slots);

    return 0;
}

int queue_size(PriorityQueue *q)
{
    if (!q) return 0;
    pthread_mutex_lock(&q->lock);
    int s = q->size;
    pthread_mutex_unlock(&q->lock);
    return s;
}

int queue_is_empty(PriorityQueue *q)
{
    return queue_size(q) == 0;
}
