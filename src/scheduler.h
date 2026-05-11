/*
 * ============================================================
 * Project : Hospital Patient Triage & Bed Allocator
 * File    : scheduler.h
 * Purpose : Interface for CPU scheduling simulations.
 *           Implements FCFS, Priority, SJF, and Round Robin.
 *           Outputs a Gantt-style log to schedule_log.txt and
 *           computes average waiting time / turnaround time.
 * ============================================================
 */

#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "common.h"

/* Scheduling algorithm identifiers */
typedef enum {
    HOSP_SCHED_FCFS     = 0,
    HOSP_SCHED_PRIORITY = 1,
    HOSP_SCHED_SJF      = 2,
    HOSP_SCHED_RR       = 3
} SchedAlgorithm;

/*
 * SchedEntry — a single patient as seen by the scheduler.
 * Mirrors the PatientRecord fields relevant to scheduling.
 */
typedef struct {
    int    patient_id;
    char   name[64];
    int    priority;          /* 1 = highest urgency */
    int    burst_time;        /* estimated treatment duration (sec) */
    time_t arrival_time;
    int    waiting_time;      /* computed by scheduler */
    int    turnaround_time;   /* waiting_time + burst_time */
    int    remaining_time;    /* for Round Robin */
    int    start_time;        /* absolute time simulation unit when admitted */
    int    finish_time;       /* absolute time simulation unit when discharged */
} SchedEntry;

/*
 * SchedulerResult — metrics computed after one scheduling run.
 */
typedef struct {
    SchedAlgorithm algorithm;
    int            n_patients;
    double         avg_waiting_time;
    double         avg_turnaround_time;
    SchedEntry     entries[MAX_PATIENTS];
} SchedulerResult;

/* -------------------------------------------------------
 * Scheduling functions
 * ------------------------------------------------------- */

/*
 * schedule_run — run the chosen algorithm on a copy of the patient
 * array.  Fills in result->entries[].waiting_time and
 * result->entries[].turnaround_time and computes averages.
 *
 * patients : array of SchedEntry (input, not modified)
 * n        : number of entries
 * algo     : algorithm to use
 * quantum  : time quantum for Round Robin (ignored for others)
 * result   : output struct filled by this function
 */
void schedule_run(const SchedEntry *patients, int n,
                  SchedAlgorithm algo, int quantum,
                  SchedulerResult *result);

/*
 * schedule_log_write — append the result's Gantt chart and metrics
 * to schedule_log.txt.
 */
void schedule_log_write(const SchedulerResult *result, const char *logpath);

/*
 * schedule_print_result — print a summary table to stdout.
 */
void schedule_print_result(const SchedulerResult *result);

/*
 * algo_name — return a human-readable name for the algorithm.
 */
const char *algo_name(SchedAlgorithm a);

#endif /* SCHEDULER_H */
