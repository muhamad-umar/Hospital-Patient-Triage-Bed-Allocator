/*
 * ============================================================
 * Project : Hospital Patient Triage & Bed Allocator
 * File    : memory_allocator.c
 * Purpose : Ward memory allocator implementation.
 *           - Best-Fit / First-Fit / Worst-Fit strategies
 *           - Free-list with coalescing
 *           - Fragmentation reporting (logged to memory_log.txt)
 *           - Paging simulation with internal fragmentation
 * Compile : gcc -Wall -Wextra -pthread -c memory_allocator.c
 * ============================================================
 */

#include "memory_allocator.h"
#include <math.h>   /* for fragmentation % — link with -lm */

/* -------------------------------------------------------
 * Internal free-list helpers
 * ------------------------------------------------------- */

/* Allocate a new FreeNode */
static FreeNode *make_free_node(int start, int size)
{
    FreeNode *n = (FreeNode *)malloc(sizeof(FreeNode));
    if (!n) { LOG_ERR("make_free_node: malloc failed"); return NULL; }
    n->start_unit = start;
    n->size       = size;
    n->prev       = NULL;
    n->next       = NULL;
    return n;
}

/* Insert node into the free list, sorted ascending by start_unit */
static void free_list_insert(Allocator *a, FreeNode *node)
{
    if (!a->free_list_head) {
        a->free_list_head = node;
        node->prev = node->next = NULL;
        return;
    }

    FreeNode *cur = a->free_list_head;
    FreeNode *prev_node = NULL;

    while (cur && cur->start_unit < node->start_unit) {
        prev_node = cur;
        cur = cur->next;
    }

    /* Insert before cur */
    node->next = cur;
    node->prev = prev_node;
    if (prev_node) prev_node->next = node;
    else           a->free_list_head = node;
    if (cur)       cur->prev = node;
}

/* Remove and free a FreeNode from the list */
static void free_list_remove(Allocator *a, FreeNode *node)
{
    if (node->prev) node->prev->next = node->next;
    else            a->free_list_head = node->next;
    if (node->next) node->next->prev = node->prev;
    free(node);
}

/* Build the initial free list from the allocator's partition array */
static void build_initial_free_list(Allocator *a)
{
    /* Each free partition becomes a FreeNode */
    for (int i = 0; i < a->num_partitions; i++) {
        BedPartition *p = &a->partitions[i];
        if (p->is_free) {
            FreeNode *n = make_free_node(p->start_unit, p->size);
            if (n) free_list_insert(a, n);
        }
    }
}

/* Return the BedPartition whose start_unit matches, or NULL */
static BedPartition *find_partition_by_unit(Allocator *a, int start_unit)
{
    for (int i = 0; i < a->num_partitions; i++) {
        if (a->partitions[i].start_unit == start_unit)
            return &a->partitions[i];
    }
    return NULL;
}

/* -------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------- */

int allocator_init(Allocator *a, AllocationStrategy s)
{
    if (!a) return -1;

    memset(a, 0, sizeof(*a));
    a->strategy        = s;
    a->free_list_head  = NULL;
    a->num_partitions  = TOTAL_BEDS;

    /* Initialise care-unit array: -1 = free */
    for (int i = 0; i < TOTAL_CARE_UNITS; i++)
        a->care_units[i] = -1;

    /* Mirror the ward layout into our local partition array */
    int unit = 0;
    for (int i = 0; i < ICU_BEDS; i++) {
        a->partitions[i].partition_id = i;
        a->partitions[i].start_unit   = unit;
        a->partitions[i].size         = ICU_CARE_UNITS;
        a->partitions[i].is_free      = 1;
        a->partitions[i].patient_id   = -1;
        strncpy(a->partitions[i].bed_type, BED_ICU,
                sizeof(a->partitions[i].bed_type) - 1);
        unit += ICU_CARE_UNITS;
    }
    for (int i = 0; i < ISOLATION_BEDS; i++) {
        int idx = ICU_BEDS + i;
        a->partitions[idx].partition_id = idx;
        a->partitions[idx].start_unit   = unit;
        a->partitions[idx].size         = ISOLATION_CARE_UNITS;
        a->partitions[idx].is_free      = 1;
        a->partitions[idx].patient_id   = -1;
        strncpy(a->partitions[idx].bed_type, BED_ISOLATION,
                sizeof(a->partitions[idx].bed_type) - 1);
        unit += ISOLATION_CARE_UNITS;
    }
    for (int i = 0; i < GENERAL_BEDS; i++) {
        int idx = ICU_BEDS + ISOLATION_BEDS + i;
        a->partitions[idx].partition_id = idx;
        a->partitions[idx].start_unit   = unit;
        a->partitions[idx].size         = GENERAL_CARE_UNITS;
        a->partitions[idx].is_free      = 1;
        a->partitions[idx].patient_id   = -1;
        strncpy(a->partitions[idx].bed_type, BED_GENERAL,
                sizeof(a->partitions[idx].bed_type) - 1);
        unit += GENERAL_CARE_UNITS;
    }

    /* Build page table */
    for (int pg = 0; pg < PAGE_TABLE_SIZE; pg++) {
        a->page_table[pg].page_number  = pg;
        a->page_table[pg].patient_id   = -1;
        a->page_table[pg].internal_frag = 0;
    }

    /* Build initial free list */
    build_initial_free_list(a);

    /* Initialise mutex */
    if (pthread_mutex_init(&a->allocator_mutex, NULL) != 0) {
        LOG_ERR("allocator_init: pthread_mutex_init failed");
        return -1;
    }

    /* Open memory log file */
    a->mem_log = fopen(MEMORY_LOG, "a");
    if (!a->mem_log) {
        LOG_ERR("allocator_init: cannot open %s", MEMORY_LOG);
        /* Non-fatal — continue without log */
    }

    LOG("Allocator ready: strategy=%s  total_units=%d  beds=%d",
        strategy_name(s), TOTAL_CARE_UNITS, TOTAL_BEDS);
    return 0;
}

void allocator_destroy(Allocator *a)
{
    if (!a) return;

    pthread_mutex_lock(&a->allocator_mutex);

    /* Free all FreeNode entries */
    FreeNode *cur = a->free_list_head;
    while (cur) {
        FreeNode *nxt = cur->next;
        free(cur);
        cur = nxt;
    }
    a->free_list_head = NULL;

    if (a->mem_log) {
        fclose(a->mem_log);
        a->mem_log = NULL;
    }

    pthread_mutex_unlock(&a->allocator_mutex);
    pthread_mutex_destroy(&a->allocator_mutex);
}

/* -------------------------------------------------------
 * Strategy selection
 * ------------------------------------------------------- */

AllocationStrategy strategy_from_string(const char *s)
{
    if (!s) return STRATEGY_BEST_FIT;
    if (strcmp(s, "first") == 0) return STRATEGY_FIRST_FIT;
    if (strcmp(s, "worst") == 0) return STRATEGY_WORST_FIT;
    return STRATEGY_BEST_FIT; /* default */
}

const char *strategy_name(AllocationStrategy s)
{
    switch (s) {
        case STRATEGY_FIRST_FIT: return "First-Fit";
        case STRATEGY_WORST_FIT: return "Worst-Fit";
        default:                 return "Best-Fit";
    }
}

/* -------------------------------------------------------
 * Allocation
 * ------------------------------------------------------- */

int allocate_bed(Allocator *a, PatientRecord *p)
{
    if (!a || !p) return -1;

    pthread_mutex_lock(&a->allocator_mutex);

    /*
     * Determine the required bed type from priority:
     *   priority 1-2 -> ICU  (3 care units)
     *   priority 3   -> ISOLATION (2 care units)
     *   priority 4-5 -> GENERAL   (1 care unit)
     */
    int required_units;
    const char *required_type;

    if (p->priority <= 2) {
        required_units = ICU_CARE_UNITS;
        required_type  = BED_ICU;
    } else if (p->priority == 3) {
        required_units = ISOLATION_CARE_UNITS;
        required_type  = BED_ISOLATION;
    } else {
        required_units = GENERAL_CARE_UNITS;
        required_type  = BED_GENERAL;
    }

    p->care_units = required_units;

    /*
     * Scan the free list for a suitable FreeNode according to strategy.
     * We look for partitions of the correct type and size.
     */
    FreeNode *chosen = NULL;

    if (a->strategy == STRATEGY_FIRST_FIT) {
        /* First free partition whose type and size fit */
        for (FreeNode *n = a->free_list_head; n; n = n->next) {
            BedPartition *bp = find_partition_by_unit(a, n->start_unit);
            if (!bp) continue;
            if (bp->size >= required_units &&
                strcmp(bp->bed_type, required_type) == 0) {
                chosen = n;
                break;
            }
        }
    } else if (a->strategy == STRATEGY_WORST_FIT) {
        /* Largest partition of correct type */
        int max_sz = -1;
        for (FreeNode *n = a->free_list_head; n; n = n->next) {
            BedPartition *bp = find_partition_by_unit(a, n->start_unit);
            if (!bp) continue;
            if (bp->size >= required_units &&
                strcmp(bp->bed_type, required_type) == 0 &&
                n->size > max_sz) {
                max_sz = n->size;
                chosen  = n;
            }
        }
    } else {
        /* Best-Fit: smallest partition >= required_units of correct type */
        int min_sz = TOTAL_CARE_UNITS + 1;
        for (FreeNode *n = a->free_list_head; n; n = n->next) {
            BedPartition *bp = find_partition_by_unit(a, n->start_unit);
            if (!bp) continue;
            if (bp->size >= required_units &&
                strcmp(bp->bed_type, required_type) == 0 &&
                n->size < min_sz) {
                min_sz = n->size;
                chosen  = n;
            }
        }
    }

    if (!chosen) {
        LOG("allocate_bed: no %s bed available for patient %d (priority %d)",
            required_type, p->patient_id, p->priority);
        pthread_mutex_unlock(&a->allocator_mutex);
        return -1;
    }

    /* Mark the partition occupied */
    BedPartition *bp = find_partition_by_unit(a, chosen->start_unit);
    bp->is_free    = 0;
    bp->patient_id = p->patient_id;
    p->bed_id      = bp->partition_id;
    strncpy(p->bed_type, bp->bed_type, sizeof(p->bed_type) - 1);

    /* Mark care units in the care_units array */
    for (int u = bp->start_unit; u < bp->start_unit + bp->size; u++)
        a->care_units[u] = p->patient_id;

    /* Update page table */
    int first_page = bp->start_unit / PAGE_SIZE;
    int last_page  = (bp->start_unit + bp->size - 1) / PAGE_SIZE;
    for (int pg = first_page; pg <= last_page; pg++) {
        a->page_table[pg].patient_id = p->patient_id;
        /* Internal fragmentation: if the last page is not fully used */
        int page_start = pg * PAGE_SIZE;
        int page_end   = page_start + PAGE_SIZE;
        int used_in_page = 0;
        for (int u = page_start; u < page_end && u < TOTAL_CARE_UNITS; u++) {
            if (a->care_units[u] == p->patient_id) used_in_page++;
        }
        a->page_table[pg].internal_frag = PAGE_SIZE - used_in_page;
        if (a->page_table[pg].internal_frag < 0)
            a->page_table[pg].internal_frag = 0;
    }

    /* Remove from free list */
    free_list_remove(a, chosen);

    LOG("[%s] Bed %d allocated to patient %d (%s) | units %d-%d | strategy=%s",
        required_type, bp->partition_id, p->patient_id, p->name,
        bp->start_unit, bp->start_unit + bp->size - 1,
        strategy_name(a->strategy));

    pthread_mutex_unlock(&a->allocator_mutex);

    report_fragmentation(a);
    return bp->partition_id;
}

/* -------------------------------------------------------
 * Deallocation
 * ------------------------------------------------------- */

int free_bed(Allocator *a, int partition_id, int patient_id)
{
    if (!a || partition_id < 0 || partition_id >= a->num_partitions)
        return -1;

    pthread_mutex_lock(&a->allocator_mutex);

    BedPartition *bp = &a->partitions[partition_id];

    if (bp->is_free) {
        LOG("free_bed: bed %d is already free", partition_id);
        pthread_mutex_unlock(&a->allocator_mutex);
        return -1;
    }

    if (bp->patient_id != patient_id) {
        LOG("free_bed: bed %d belongs to patient %d not %d",
            partition_id, bp->patient_id, patient_id);
        pthread_mutex_unlock(&a->allocator_mutex);
        return -1;
    }

    LOG("Freeing bed %d (was occupied by patient %d)", partition_id, patient_id);

    /* Clear care units */
    for (int u = bp->start_unit; u < bp->start_unit + bp->size; u++)
        a->care_units[u] = -1;

    /* Clear page table entries */
    int first_page = bp->start_unit / PAGE_SIZE;
    int last_page  = (bp->start_unit + bp->size - 1) / PAGE_SIZE;
    for (int pg = first_page; pg <= last_page; pg++) {
        a->page_table[pg].patient_id    = -1;
        a->page_table[pg].internal_frag = 0;
    }

    /* Mark partition free */
    bp->is_free    = 1;
    bp->patient_id = -1;

    /* Insert back into free list */
    FreeNode *node = make_free_node(bp->start_unit, bp->size);
    if (node) free_list_insert(a, node);

    pthread_mutex_unlock(&a->allocator_mutex);

    /* Coalesce after releasing the lock to avoid double-lock */
    coalesce_free(a, bp->start_unit);

    report_fragmentation(a);
    return 0;
}

/* -------------------------------------------------------
 * Coalescing
 * ------------------------------------------------------- */

void coalesce_free(Allocator *a, int start_unit)
{
    pthread_mutex_lock(&a->allocator_mutex);

    printf("\n--- Coalescing: before ---\n");
    /* Print ward state without the lock-aware wrapper */
    for (int i = 0; i < TOTAL_CARE_UNITS; i++) {
        if (a->care_units[i] == -1) printf("[FREE]");
        else                         printf("[P%3d]", a->care_units[i]);
    }
    printf("\n");

    /* Find the FreeNode for start_unit */
    FreeNode *target = NULL;
    for (FreeNode *n = a->free_list_head; n; n = n->next) {
        if (n->start_unit == start_unit) { target = n; break; }
    }
    if (!target) {
        pthread_mutex_unlock(&a->allocator_mutex);
        return;
    }

    /*
     * Right-coalesce: if the node immediately after target in the
     * free list is adjacent (start == target.start + target.size),
     * merge them.
     */
    while (target->next &&
           target->next->start_unit == target->start_unit + target->size) {
        FreeNode *right = target->next;

        /* Update the BedPartition for right neighbour */
        BedPartition *rbp = find_partition_by_unit(a, right->start_unit);
        if (rbp) {
            /* Mark it absorbed — we don't actually remove the partition
             * struct from the array; instead we just adjust the free list
             * to reflect the merged free region. */
        }

        target->size += right->size;
        free_list_remove(a, right);   /* removes and frees the node */
        LOG("  Right-coalesced: merged %d units starting at unit %d",
            target->size, target->start_unit);
    }

    /*
     * Left-coalesce: if the node immediately before target is
     * adjacent, merge target into its left neighbour.
     */
    while (target->prev &&
           target->prev->start_unit + target->prev->size == target->start_unit) {
        FreeNode *left = target->prev;
        left->size += target->size;
        free_list_remove(a, target);
        target = left;
        LOG("  Left-coalesced: merged %d units starting at unit %d",
            target->size, target->start_unit);
    }

    printf("\n--- Coalescing: after  ---\n");
    for (int i = 0; i < TOTAL_CARE_UNITS; i++) {
        if (a->care_units[i] == -1) printf("[FREE]");
        else                         printf("[P%3d]", a->care_units[i]);
    }
    printf("\n\n");

    pthread_mutex_unlock(&a->allocator_mutex);
}

/* -------------------------------------------------------
 * Fragmentation reporting
 * ------------------------------------------------------- */

void report_fragmentation(Allocator *a)
{
    if (!a) return;

    pthread_mutex_lock(&a->allocator_mutex);

    int total_free  = 0;
    int largest     = 0;

    for (FreeNode *n = a->free_list_head; n; n = n->next) {
        total_free += n->size;
        if (n->size > largest) largest = n->size;
    }

    double ext_frag = 0.0;
    if (total_free > 0)
        ext_frag = (1.0 - (double)largest / (double)total_free) * 100.0;

    time_t now = time(NULL);
    char tbuf[32];
    struct tm *tm_info = localtime(&now);
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", tm_info);

    printf("[MEM] Total free: %d units | Largest block: %d units | "
           "Ext. fragmentation: %.1f%%\n",
           total_free, largest, ext_frag);

    if (a->mem_log) {
        fprintf(a->mem_log,
                "%s | free=%d | largest=%d | ext_frag=%.1f%%\n",
                tbuf, total_free, largest, ext_frag);
        fflush(a->mem_log);
    }

    pthread_mutex_unlock(&a->allocator_mutex);
}

void report_paging(Allocator *a)
{
    if (!a) return;

    pthread_mutex_lock(&a->allocator_mutex);

    printf("\n=== Page Table ===\n");
    printf("%-8s %-12s %-15s\n", "Page", "Patient", "Internal Frag");
    printf("%-8s %-12s %-15s\n", "----", "-------", "-------------");

    int total_internal = 0;
    for (int pg = 0; pg < PAGE_TABLE_SIZE; pg++) {
        if (a->page_table[pg].patient_id != -1) {
            printf("%-8d %-12d %-15d\n",
                   pg,
                   a->page_table[pg].patient_id,
                   a->page_table[pg].internal_frag);
            total_internal += a->page_table[pg].internal_frag;
        }
    }
    printf("Total internal fragmentation: %d units\n\n", total_internal);

    pthread_mutex_unlock(&a->allocator_mutex);
}

void print_ward_map(Allocator *a)
{
    if (!a) return;

    pthread_mutex_lock(&a->allocator_mutex);

    printf("\n=== Ward Map (care units 0..%d) ===\n", TOTAL_CARE_UNITS - 1);
    for (int i = 0; i < TOTAL_CARE_UNITS; i++) {
        if (a->care_units[i] == -1)
            printf("[----]");
        else
            printf("[P%3d]", a->care_units[i]);
        if ((i + 1) % 10 == 0) printf("\n");
    }
    printf("\n");

    pthread_mutex_unlock(&a->allocator_mutex);
}
