/*
 * ============================================================
 * Project : Hospital Patient Triage & Bed Allocator
 * File    : common.h
 * Purpose : Shared constants, structs, and macros used across
 *           all modules in the hospital simulation system.
 * Compile : Included via -I flag; not compiled standalone.
 * ============================================================
 */

#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>

/* -------------------------------------------------------
 * Named IPC resources
 * ------------------------------------------------------- */
#define DISCHARGE_FIFO     "/tmp/discharge_fifo"
#define TRIAGE_FIFO        "/tmp/triage_fifo"
#define SHM_KEY            0xBEDF00D       /* Shared memory key */
#define SEM_ICU_NAME       "/sem_icu_limit"
#define SEM_ISO_NAME       "/sem_iso_limit"
#define SEM_QUEUE_NAME     "/sem_queue_slots"
#define PATIENT_RECORD_DAT "logs/patient_records.dat"

/* -------------------------------------------------------
 * Ward / bed configuration
 * ------------------------------------------------------- */
#define ICU_BEDS           4
#define ISOLATION_BEDS     4
#define GENERAL_BEDS       12
#define TOTAL_BEDS         (ICU_BEDS + ISOLATION_BEDS + GENERAL_BEDS)

/* Care units each bed type consumes */
#define ICU_CARE_UNITS     3
#define ISOLATION_CARE_UNITS 2
#define GENERAL_CARE_UNITS 1

/* Total care-unit capacity of the ward */
#define TOTAL_CARE_UNITS   (ICU_BEDS * ICU_CARE_UNITS + \
                            ISOLATION_BEDS * ISOLATION_CARE_UNITS + \
                            GENERAL_BEDS * GENERAL_CARE_UNITS)

/* Page size for paging simulation (care units per page) */
#define PAGE_SIZE          2

/* -------------------------------------------------------
 * Priority & severity mappings
 * ------------------------------------------------------- */
#define MAX_PRIORITY       5          /* 1 = critical, 5 = minor */
#define MAX_SEVERITY       10

/* -------------------------------------------------------
 * Scheduling constants
 * ------------------------------------------------------- */
#define MAX_PATIENTS       64         /* Max patients tracked per run */
#define RR_QUANTUM         3          /* Round Robin time quantum (sec) */
#define MAX_CONCURRENT     10         /* Max concurrent patient processes */

/* -------------------------------------------------------
 * Thread / semaphore limits
 * ------------------------------------------------------- */
#define NURSE_THREAD_COUNT 3          /* One per bed type */
#define QUEUE_CAPACITY     32         /* Bounded producer-consumer queue */

/* -------------------------------------------------------
 * Bed type string literals
 * ------------------------------------------------------- */
#define BED_ICU        "ICU"
#define BED_ISOLATION  "ISOLATION"
#define BED_GENERAL    "GENERAL"

/* -------------------------------------------------------
 * Log file paths
 * ------------------------------------------------------- */
#define SCHEDULE_LOG   "logs/schedule_log.txt"
#define MEMORY_LOG     "logs/memory_log.txt"

/* -------------------------------------------------------
 * Utility macros
 * ------------------------------------------------------- */
#define UNUSED(x)  (void)(x)

/* Print with timestamp prefix */
#define LOG(fmt, ...) \
    do { \
        time_t _t = time(NULL); \
        struct tm *_tm = localtime(&_t); \
        char _buf[32]; \
        strftime(_buf, sizeof(_buf), "%H:%M:%S", _tm); \
        fprintf(stdout, "[%s] " fmt "\n", _buf, ##__VA_ARGS__); \
        fflush(stdout); \
    } while (0)

#define LOG_ERR(fmt, ...) \
    do { \
        fprintf(stderr, "[ERROR] " fmt ": %s\n", ##__VA_ARGS__, strerror(errno)); \
        fflush(stderr); \
    } while (0)

/* -------------------------------------------------------
 * Core data structures
 * ------------------------------------------------------- */

/*
 * PatientRecord — transferred via IPC between triage and admissions,
 * and stored in mapped patient_records.dat.
 */
typedef struct {
    int     patient_id;
    char    name[64];
    int     age;
    int     severity;        /* raw severity 1-10 from triage.sh */
    int     priority;        /* computed triage priority 1-5 */
    int     care_units;      /* memory units required for bed */
    time_t  arrival_time;
    time_t  start_time;      /* when treatment began */
    time_t  discharge_time;  /* when patient was discharged */
    int     burst_time;      /* estimated treatment duration (sec) */
    int     wait_time;       /* time spent waiting in queue (sec) */
    char    bed_type[16];    /* "ICU", "GENERAL", "ISOLATION" */
    int     bed_id;          /* index of assigned bed partition */
} PatientRecord;

/*
 * BedPartition — one element in the ward's contiguous memory model.
 * Maintained in shared memory and the local free-list allocator.
 */
typedef struct {
    int  partition_id;
    int  start_unit;    /* index in ward array (0-based) */
    int  size;          /* number of care units in this partition */
    int  is_free;       /* 1 = FREE, 0 = OCCUPIED */
    int  patient_id;    /* -1 if free */
    char bed_type[16];  /* "ICU", "GENERAL", "ISOLATION" */
} BedPartition;

/*
 * SharedWard — the shared memory segment layout.
 * Admissions and all patient processes map this segment.
 */
typedef struct {
    BedPartition partitions[TOTAL_BEDS];  /* bed partition array */
    int          total_beds;
    int          occupied_beds;
    int          total_patients_served;
    int          running;                 /* 1 = system up, 0 = shutdown */
    pid_t        admissions_pid;          /* PID of admissions manager */
} SharedWard;

#endif /* COMMON_H */
