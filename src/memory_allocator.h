/*
 * ============================================================
 * Project : Hospital Patient Triage & Bed Allocator
 * File    : memory_allocator.h
 * Purpose : Interface for the ward memory allocator.
 *           Models the ward as a contiguous array of care units.
 *           Supports Best-Fit, First-Fit, and Worst-Fit strategies
 *           selected at runtime.  Includes:
 *             - Free-list management
 *             - Bed (partition) allocation & deallocation
 *             - Coalescing of adjacent free partitions
 *             - Fragmentation reporting
 *             - Paging simulation with internal fragmentation
 * ============================================================
 */

#ifndef MEMORY_ALLOCATOR_H
#define MEMORY_ALLOCATOR_H

#include "common.h"

/* Allocation strategy identifiers */
typedef enum {
    STRATEGY_BEST_FIT  = 0,
    STRATEGY_FIRST_FIT = 1,
    STRATEGY_WORST_FIT = 2
} AllocationStrategy;

/*
 * FreeNode — one entry in the free list doubly-linked list.
 * Tracks a contiguous range of care units not currently assigned.
 */
typedef struct FreeNode {
    int            start_unit;   /* first care unit index */
    int            size;         /* number of contiguous free units */
    struct FreeNode *prev;
    struct FreeNode *next;
} FreeNode;

/*
 * PageTableEntry — one page in the paging simulation.
 * page_size = PAGE_SIZE care units.
 */
typedef struct {
    int page_number;
    int patient_id;    /* -1 if page is free */
    int internal_frag; /* wasted units within this page */
} PageTableEntry;

#define PAGE_TABLE_SIZE  ((TOTAL_CARE_UNITS + PAGE_SIZE - 1) / PAGE_SIZE)

/*
 * Allocator — the ward memory manager.
 * Thread-safe: all public functions lock allocator_mutex.
 */
typedef struct {
    int                care_units[TOTAL_CARE_UNITS]; /* -1=free, else patient_id */
    BedPartition       partitions[TOTAL_BEDS];
    int                num_partitions;
    FreeNode          *free_list_head;               /* doubly-linked free list */
    AllocationStrategy strategy;
    pthread_mutex_t    allocator_mutex;
    PageTableEntry     page_table[PAGE_TABLE_SIZE];
    FILE              *mem_log;                      /* memory_log.txt handle */
} Allocator;

/* -------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------- */

/*
 * allocator_init — initialise the allocator, build initial free list,
 * open memory_log.txt, and configure the allocation strategy.
 * Returns 0 on success, -1 on failure.
 */
int  allocator_init(Allocator *a, AllocationStrategy s);

/*
 * allocator_destroy — free all FreeNode list memory, close log.
 */
void allocator_destroy(Allocator *a);

/* -------------------------------------------------------
 * Allocation & deallocation
 * ------------------------------------------------------- */

/*
 * allocate_bed — find a free partition for a patient using the
 * configured strategy and mark it occupied.
 * Returns partition index on success, -1 if no suitable bed found.
 */
int  allocate_bed(Allocator *a, PatientRecord *p);

/*
 * free_bed — mark a partition as free, update the free list,
 * and trigger coalescing.
 * Returns 0 on success, -1 on error.
 */
int  free_bed(Allocator *a, int partition_id, int patient_id);

/* -------------------------------------------------------
 * Coalescing
 * ------------------------------------------------------- */

/*
 * coalesce_free — merge the FreeNode containing start_unit with
 * its immediate left and right neighbours if they are also free.
 * Prints a before/after ward map to stdout.
 */
void coalesce_free(Allocator *a, int start_unit);

/* -------------------------------------------------------
 * Fragmentation & paging
 * ------------------------------------------------------- */

/*
 * report_fragmentation — compute and print:
 *   - Total free care units
 *   - Largest contiguous free block
 *   - External fragmentation %
 * Also appends a timestamped line to memory_log.txt.
 */
void report_fragmentation(Allocator *a);

/*
 * report_paging — print the page table and internal fragmentation
 * for every page that is partially filled.
 */
void report_paging(Allocator *a);

/*
 * print_ward_map — print an ASCII map of the ward's care units.
 */
void print_ward_map(Allocator *a);

/* -------------------------------------------------------
 * Strategy selection
 * ------------------------------------------------------- */

/*
 * strategy_from_string — parse "--strategy best|first|worst".
 * Returns the AllocationStrategy enum value, or STRATEGY_BEST_FIT
 * as default if the string is unrecognised.
 */
AllocationStrategy strategy_from_string(const char *s);

const char *strategy_name(AllocationStrategy s);

#endif /* MEMORY_ALLOCATOR_H */
