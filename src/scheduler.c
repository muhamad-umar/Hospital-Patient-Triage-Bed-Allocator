/*
 * ============================================================
 * Project : Hospital Patient Triage & Bed Allocator
 * File    : scheduler.c
 * Purpose : CPU scheduling simulation:
 *             - FCFS (First Come First Served)
 *             - Priority Scheduling (triage level)
 *             - SJF (Shortest Job First — non-preemptive)
 *             - Round Robin with configurable quantum
 *           Outputs Gantt chart + metrics to schedule_log.txt.
 * Compile : gcc -Wall -Wextra -pthread -c scheduler.c
 * ============================================================
 */

#include "scheduler.h"
#include <string.h>

/* -------------------------------------------------------
 * Internal sort helpers
 * ------------------------------------------------------- */

/* Compare by arrival_time (FCFS) */
static int cmp_arrival(const void *a, const void *b)
{
    const SchedEntry *ea = (const SchedEntry *)a;
    const SchedEntry *eb = (const SchedEntry *)b;
    return (int)(ea->arrival_time - eb->arrival_time);
}

/* Compare by priority (lowest number first = most urgent) */
static int cmp_priority(const void *a, const void *b)
{
    const SchedEntry *ea = (const SchedEntry *)a;
    const SchedEntry *eb = (const SchedEntry *)b;
    if (ea->priority != eb->priority)
        return ea->priority - eb->priority;
    /* Tie: earlier arrival first */
    return (int)(ea->arrival_time - eb->arrival_time);
}

/* Compare by burst_time (SJF) */
static int cmp_burst(const void *a, const void *b)
{
    const SchedEntry *ea = (const SchedEntry *)a;
    const SchedEntry *eb = (const SchedEntry *)b;
    if (ea->burst_time != eb->burst_time)
        return ea->burst_time - eb->burst_time;
    return (int)(ea->arrival_time - eb->arrival_time);
}

/* -------------------------------------------------------
 * FCFS
 * ------------------------------------------------------- */

static void run_fcfs(SchedEntry *arr, int n, SchedulerResult *r)
{
    /* Sort by arrival time */
    qsort(arr, (size_t)n, sizeof(SchedEntry), cmp_arrival);

    int current_time = 0;
    double total_wait = 0.0, total_turn = 0.0;

    for (int i = 0; i < n; i++) {
        /* If CPU is idle until this patient arrives */
        if (current_time < (int)arr[i].arrival_time)
            current_time = (int)arr[i].arrival_time;

        arr[i].start_time      = current_time;
        arr[i].waiting_time    = current_time - (int)arr[i].arrival_time;
        arr[i].finish_time     = current_time + arr[i].burst_time;
        arr[i].turnaround_time = arr[i].finish_time - (int)arr[i].arrival_time;

        current_time = arr[i].finish_time;
        total_wait  += arr[i].waiting_time;
        total_turn  += arr[i].turnaround_time;
    }

    r->avg_waiting_time    = (n > 0) ? total_wait  / n : 0.0;
    r->avg_turnaround_time = (n > 0) ? total_turn / n : 0.0;
}

/* -------------------------------------------------------
 * Priority Scheduling (non-preemptive)
 * ------------------------------------------------------- */

static void run_priority(SchedEntry *arr, int n, SchedulerResult *r)
{
    /* Sort by priority then arrival */
    qsort(arr, (size_t)n, sizeof(SchedEntry), cmp_priority);

    int current_time = 0;
    double total_wait = 0.0, total_turn = 0.0;

    for (int i = 0; i < n; i++) {
        if (current_time < (int)arr[i].arrival_time)
            current_time = (int)arr[i].arrival_time;

        arr[i].start_time      = current_time;
        arr[i].waiting_time    = current_time - (int)arr[i].arrival_time;
        arr[i].finish_time     = current_time + arr[i].burst_time;
        arr[i].turnaround_time = arr[i].finish_time - (int)arr[i].arrival_time;

        current_time = arr[i].finish_time;
        total_wait  += arr[i].waiting_time;
        total_turn  += arr[i].turnaround_time;
    }

    r->avg_waiting_time    = (n > 0) ? total_wait  / n : 0.0;
    r->avg_turnaround_time = (n > 0) ? total_turn / n : 0.0;
}

/* -------------------------------------------------------
 * SJF (non-preemptive)
 * ------------------------------------------------------- */

static void run_sjf(SchedEntry *arr, int n, SchedulerResult *r)
{
    /* Sort by burst time then arrival */
    qsort(arr, (size_t)n, sizeof(SchedEntry), cmp_burst);

    int current_time = 0;
    double total_wait = 0.0, total_turn = 0.0;

    for (int i = 0; i < n; i++) {
        if (current_time < (int)arr[i].arrival_time)
            current_time = (int)arr[i].arrival_time;

        arr[i].start_time      = current_time;
        arr[i].waiting_time    = current_time - (int)arr[i].arrival_time;
        arr[i].finish_time     = current_time + arr[i].burst_time;
        arr[i].turnaround_time = arr[i].finish_time - (int)arr[i].arrival_time;

        current_time = arr[i].finish_time;
        total_wait  += arr[i].waiting_time;
        total_turn  += arr[i].turnaround_time;
    }

    r->avg_waiting_time    = (n > 0) ? total_wait  / n : 0.0;
    r->avg_turnaround_time = (n > 0) ? total_turn / n : 0.0;
}

/* -------------------------------------------------------
 * Round Robin
 * ------------------------------------------------------- */

static void run_rr(SchedEntry *arr, int n, int quantum, SchedulerResult *r)
{
    if (quantum <= 0) quantum = RR_QUANTUM;

    /* Sort by arrival time initially */
    qsort(arr, (size_t)n, sizeof(SchedEntry), cmp_arrival);

    /* Initialise remaining times */
    for (int i = 0; i < n; i++)
        arr[i].remaining_time = arr[i].burst_time;

    /* Track whether each process has started */
    int started[MAX_PATIENTS];
    memset(started, 0, sizeof(int) * (size_t)n);

    int done     = 0;
    int current  = 0;
    double total_wait = 0.0, total_turn = 0.0;

    while (done < n) {
        int progress = 0;
        for (int i = 0; i < n; i++) {
            if (arr[i].remaining_time <= 0) continue;

            /* Record start time on first run */
            if (!started[i]) {
                arr[i].start_time = current;
                started[i] = 1;
            }

            int slice = (arr[i].remaining_time < quantum) ?
                         arr[i].remaining_time : quantum;
            current += slice;
            arr[i].remaining_time -= slice;

            if (arr[i].remaining_time == 0) {
                arr[i].finish_time     = current;
                arr[i].turnaround_time = current - (int)arr[i].arrival_time;
                arr[i].waiting_time    = arr[i].turnaround_time - arr[i].burst_time;
                if (arr[i].waiting_time < 0) arr[i].waiting_time = 0;
                total_wait += arr[i].waiting_time;
                total_turn += arr[i].turnaround_time;
                done++;
            }
            progress = 1;
        }
        if (!progress) break; /* no runnable processes — avoid infinite loop */
    }

    r->avg_waiting_time    = (n > 0) ? total_wait  / n : 0.0;
    r->avg_turnaround_time = (n > 0) ? total_turn / n : 0.0;
}

/* -------------------------------------------------------
 * Public API
 * ------------------------------------------------------- */

void schedule_run(const SchedEntry *patients, int n,
                  SchedAlgorithm algo, int quantum,
                  SchedulerResult *result)
{
    if (!patients || n <= 0 || !result) return;

    /* Work on a mutable copy so we can sort */
    SchedEntry arr[MAX_PATIENTS];
    int count = (n > MAX_PATIENTS) ? MAX_PATIENTS : n;
    memcpy(arr, patients, (size_t)count * sizeof(SchedEntry));

    memset(result, 0, sizeof(*result));
    result->algorithm  = algo;
    result->n_patients = count;

    switch (algo) {
        case HOSP_SCHED_FCFS:     run_fcfs(arr, count, result);              break;
        case HOSP_SCHED_PRIORITY: run_priority(arr, count, result);          break;
        case HOSP_SCHED_SJF:      run_sjf(arr, count, result);               break;
        case HOSP_SCHED_RR:       run_rr(arr, count, quantum, result);       break;
        default:             run_fcfs(arr, count, result);              break;
    }

    /* Copy sorted/computed entries back to result */
    memcpy(result->entries, arr, (size_t)count * sizeof(SchedEntry));
}

const char *algo_name(SchedAlgorithm a)
{
    switch (a) {
        case HOSP_SCHED_PRIORITY: return "Priority Scheduling";
        case HOSP_SCHED_SJF:      return "Shortest Job First";
        case HOSP_SCHED_RR:       return "Round Robin";
        default:             return "First-Come First-Served";
    }
}

/* -------------------------------------------------------
 * Output
 * ------------------------------------------------------- */

void schedule_print_result(const SchedulerResult *result)
{
    if (!result) return;

    printf("\n========================================\n");
    printf("  Scheduling: %s\n", algo_name(result->algorithm));
    printf("  Patients  : %d\n", result->n_patients);
    printf("========================================\n");
    printf("%-5s %-20s %-8s %-8s %-8s %-8s %-8s\n",
           "ID", "Name", "Priority", "Burst", "Start", "Finish", "Wait");
    printf("%-5s %-20s %-8s %-8s %-8s %-8s %-8s\n",
           "--", "----", "--------", "-----", "-----", "------", "----");

    for (int i = 0; i < result->n_patients; i++) {
        const SchedEntry *e = &result->entries[i];
        printf("%-5d %-20s %-8d %-8d %-8d %-8d %-8d\n",
               e->patient_id, e->name, e->priority, e->burst_time,
               e->start_time, e->finish_time, e->waiting_time);
    }

    printf("----------------------------------------\n");
    printf("  Avg Waiting Time   : %.2f sec\n", result->avg_waiting_time);
    printf("  Avg Turnaround Time: %.2f sec\n", result->avg_turnaround_time);
    printf("========================================\n\n");
}

void schedule_log_write(const SchedulerResult *result, const char *logpath)
{
    if (!result || !logpath) return;

    FILE *f = fopen(logpath, "a");
    if (!f) {
        LOG_ERR("schedule_log_write: cannot open %s", logpath);
        return;
    }

    time_t now = time(NULL);
    char tbuf[32];
    struct tm *tm_info = localtime(&now);
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", tm_info);

    fprintf(f, "\n=== %s | %s | %d patients ===\n",
            tbuf, algo_name(result->algorithm), result->n_patients);

    /* Gantt-style bar */
    fprintf(f, "Gantt Chart:\n|");
    for (int i = 0; i < result->n_patients; i++) {
        const SchedEntry *e = &result->entries[i];
        fprintf(f, " P%-2d[%d-%d] |", e->patient_id,
                e->start_time, e->finish_time);
    }
    fprintf(f, "\n");

    /* Per-patient table */
    fprintf(f, "%-5s %-20s %-8s %-8s %-10s %-12s\n",
            "ID", "Name", "Burst", "Wait", "Turnaround", "Priority");
    for (int i = 0; i < result->n_patients; i++) {
        const SchedEntry *e = &result->entries[i];
        fprintf(f, "%-5d %-20s %-8d %-8d %-10d %-12d\n",
                e->patient_id, e->name, e->burst_time,
                e->waiting_time, e->turnaround_time, e->priority);
    }

    fprintf(f, "Avg Waiting Time   : %.2f\n", result->avg_waiting_time);
    fprintf(f, "Avg Turnaround Time: %.2f\n\n", result->avg_turnaround_time);

    fclose(f);
    LOG("Schedule log written to %s", logpath);
}
