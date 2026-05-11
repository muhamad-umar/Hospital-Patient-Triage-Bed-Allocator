/*
 * ============================================================
 * Project : Hospital Patient Triage & Bed Allocator
 * File    : admissions.c
 * Purpose : Central admissions manager.
 *           Implements:
 *             - Receptionist thread (reads triage FIFO, enqueues)
 *             - Scheduler thread (dequeues, allocates bed, fork+exec)
 *             - Nurse thread pool (monitors discharge FIFO, frees beds)
 *             - SIGCHLD handler with waitpid(WNOHANG)
 *             - Named semaphores for ICU/Isolation capacity
 *             - Shared memory ward
 *             - All four scheduling simulations
 *             - Memory allocator with Best/First/Worst-Fit
 *             - mmap patient record log (Phase 4 bonus)
 * Compile : gcc -Wall -Wextra -pthread -o bin/admissions
 *               src/admissions.c src/ipc.c src/queue.c
 *               src/scheduler.c src/memory_allocator.c -lpthread -lm
 * Usage   : ./bin/admissions [--strategy best|first|worst]
 * ============================================================
 */

#include "common.h"
#include "ipc.h"
#include "queue.h"
#include "scheduler.h"
#include "memory_allocator.h"

#include <sys/mman.h>
#include <stdint.h>

/* -------------------------------------------------------
 * Global state
 * ------------------------------------------------------- */

/* Shared memory ward (mapped at startup) */
static SharedWard *g_ward        = NULL;

/* Priority queue of waiting patients */
static PriorityQueue g_patient_queue;

/* Ward memory allocator */
static Allocator g_allocator;

/* Named semaphores — capacity limits */
static sem_t *g_sem_icu  = NULL;
static sem_t *g_sem_iso  = NULL;

/* Discharge FIFO file descriptor (read end) */
static int g_discharge_fd = -1;

/* Mutex protecting g_ward (shared bed bitmap) */
static pthread_mutex_t g_ward_lock = PTHREAD_MUTEX_INITIALIZER;

/* Condition variable: broadcast by nurse when bed freed */
static pthread_cond_t  g_bed_freed = PTHREAD_COND_INITIALIZER;

/* Scheduling records for post-run simulation */
static SchedEntry  g_sched_entries[MAX_PATIENTS];
static int         g_sched_count = 0;
static pthread_mutex_t g_sched_lock = PTHREAD_MUTEX_INITIALIZER;

/* Patient record mmap log (Phase 4 bonus) */
static PatientLog *g_pat_log   = NULL;
static int         g_pat_log_fd = -1;

/* Running flag — set to 0 on SIGTERM */
static volatile sig_atomic_t g_running = 1;

/* Track child PIDs to map patient_id -> pid for waitpid */
typedef struct {
    pid_t pid;
    int   patient_id;
    int   bed_id;
} ChildEntry;

static ChildEntry    g_children[MAX_PATIENTS * 4];
static int           g_child_count = 0;
static pthread_mutex_t g_child_lock = PTHREAD_MUTEX_INITIALIZER;

/* Allocation strategy from --strategy flag */
static AllocationStrategy g_strategy = STRATEGY_BEST_FIT;

/* Atomic patient ID counter */
static int g_next_patient_id = 1;
static pthread_mutex_t g_id_lock = PTHREAD_MUTEX_INITIALIZER;

/* -------------------------------------------------------
 * Signal handlers
 * ------------------------------------------------------- */

/*
 * sigchld_handler — reaps zombie child processes.
 * Uses waitpid with WNOHANG so we do not block.
 * Called asynchronously; only async-signal-safe functions used.
 */
static void sigchld_handler(int sig)
{
    UNUSED(sig);
    int status;
    pid_t pid;
    /* Reap all available zombies */
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        /* write() is async-signal-safe; printf is not */
        char buf[64];
        int len = snprintf(buf, sizeof(buf),
                           "[SIGCHLD] Reaped child pid=%d\n", (int)pid);
        if (len > 0) {
            ssize_t r = write(STDOUT_FILENO, buf, (size_t)len);
            (void)r;
        }
    }
}

/*
 * sigterm_handler — graceful shutdown on SIGTERM/SIGINT.
 */
static void sigterm_handler(int sig)
{
    UNUSED(sig);
    g_running = 0;
    /* Signal queue condition variables to unblock sleeping threads */
}

/* -------------------------------------------------------
 * Helper: compute priority from severity
 * ------------------------------------------------------- */
static int compute_priority(int severity)
{
    if (severity >= 9) return 1;
    if (severity >= 7) return 2;
    if (severity >= 5) return 3;
    if (severity >= 3) return 4;
    return 5;
}

/* -------------------------------------------------------
 * Helper: compute care_units from priority
 * ------------------------------------------------------- */
static int care_units_for(int priority)
{
    if (priority <= 2) return ICU_CARE_UNITS;
    if (priority == 3) return ISOLATION_CARE_UNITS;
    return GENERAL_CARE_UNITS;
}

/* -------------------------------------------------------
 * Helper: get next unique patient ID
 * ------------------------------------------------------- */
static int next_patient_id(void)
{
    pthread_mutex_lock(&g_id_lock);
    int id = g_next_patient_id++;
    pthread_mutex_unlock(&g_id_lock);
    return id;
}

/* -------------------------------------------------------
 * Helper: add scheduling entry for post-run simulation
 * ------------------------------------------------------- */
static void add_sched_entry(const PatientRecord *p)
{
    pthread_mutex_lock(&g_sched_lock);
    if (g_sched_count < MAX_PATIENTS) {
        SchedEntry *e = &g_sched_entries[g_sched_count++];
        e->patient_id    = p->patient_id;
        strncpy(e->name, p->name, sizeof(e->name) - 1);
        e->priority      = p->priority;
        e->burst_time    = p->burst_time;
        e->arrival_time  = p->arrival_time;
        e->remaining_time = p->burst_time;
        e->waiting_time  = 0;
        e->turnaround_time = 0;
        e->start_time    = 0;
        e->finish_time   = 0;
    }
    pthread_mutex_unlock(&g_sched_lock);
}

/* -------------------------------------------------------
 * Helper: spawn patient_simulator via fork() + execv()
 * ------------------------------------------------------- */
static pid_t spawn_patient(const PatientRecord *p)
{
    pid_t pid = fork();

    if (pid < 0) {
        LOG_ERR("spawn_patient: fork() failed");
        return -1;
    }

    if (pid == 0) {
        /* ---- CHILD PROCESS ---- */

        /* Build argv for execv */
        char id_str[16], pri_str[16], bed_str[16];
        snprintf(id_str,  sizeof(id_str),  "%d", p->patient_id);
        snprintf(pri_str, sizeof(pri_str), "%d", p->priority);
        snprintf(bed_str, sizeof(bed_str), "%d", p->bed_id);

        char *const args[] = {
            "./bin/patient_simulator",
            id_str,
            pri_str,
            bed_str,
            (char *)p->bed_type,
            NULL
        };

        execv("./bin/patient_simulator", args);

        /* execv only returns on error */
        perror("execv");
        _exit(EXIT_FAILURE);
    }

    /* ---- PARENT PROCESS ---- */
    LOG("Spawned patient_simulator pid=%d for patient %d", pid, p->patient_id);

    /* Record the child */
    pthread_mutex_lock(&g_child_lock);
    if (g_child_count < (int)(sizeof(g_children) / sizeof(g_children[0]))) {
        g_children[g_child_count].pid        = pid;
        g_children[g_child_count].patient_id = p->patient_id;
        g_children[g_child_count].bed_id     = p->bed_id;
        g_child_count++;
    }
    pthread_mutex_unlock(&g_child_lock);

    return pid;
}

/* -------------------------------------------------------
 * Receptionist Thread
 * Reads patient records from TRIAGE_FIFO and enqueues them.
 * Producer side of the producer-consumer pattern.
 * ------------------------------------------------------- */
static void *receptionist_thread(void *arg)
{
    UNUSED(arg);
    LOG("[Receptionist] Thread started, reading from %s", TRIAGE_FIFO);

    int fd = open(TRIAGE_FIFO, O_RDONLY | O_NONBLOCK);
    if (fd == -1) {
        LOG_ERR("[Receptionist] Cannot open triage FIFO");
        return NULL;
    }

    char buf[512];

    while (g_running) {
        ssize_t n = read(fd, buf, sizeof(buf) - 1);

        if (n > 0) {
            buf[n] = '\0';

            /*
             * Expected format (from triage.sh / pipe):
             *   "name:age:severity\n"
             * Multiple records may arrive in one read.
             */
            char *line = strtok(buf, "\n");
            while (line && g_running) {
                char name[64] = {0};
                int  age = 0, severity = 0;

                if (sscanf(line, "%63[^:]:%d:%d", name, &age, &severity) == 3) {
                    PatientRecord p;
                    memset(&p, 0, sizeof(p));
                    p.patient_id   = next_patient_id();
                    strncpy(p.name, name, sizeof(p.name) - 1);
                    p.age          = age;
                    p.severity     = severity;
                    p.priority     = compute_priority(severity);
                    p.care_units   = care_units_for(p.priority);
                    p.arrival_time = time(NULL);

                    /* Estimate burst_time from bed type */
                    if (p.priority <= 2)      p.burst_time = 5 + rand() % 11;
                    else if (p.priority == 3) p.burst_time = 3 + rand() % 8;
                    else                      p.burst_time = 2 + rand() % 7;

                    LOG("[Receptionist] Received patient: %s  age=%d  sev=%d  pri=%d",
                        p.name, p.age, p.severity, p.priority);

                    /* Enqueue — blocks if queue is full (producer-consumer) */
                    if (queue_enqueue(&g_patient_queue, &p) == 0) {
                        add_sched_entry(&p);
                        LOG("[Receptionist] Patient %d (%s) queued  queue_size=%d",
                            p.patient_id, p.name,
                            queue_size(&g_patient_queue));
                    } else {
                        LOG("[Receptionist] Queue full, patient %d dropped", p.patient_id);
                    }
                }
                line = strtok(NULL, "\n");
            }
        } else if (n == 0 || (n == -1 && errno == EAGAIN)) {
            /* FIFO empty or no writer; wait briefly */
            usleep(200000); /* 200 ms */
        } else {
            LOG_ERR("[Receptionist] read error");
            break;
        }
    }

    close(fd);
    LOG("[Receptionist] Thread exiting");
    return NULL;
}

/* -------------------------------------------------------
 * Scheduler Thread
 * Dequeues highest-priority patient, runs Best-Fit allocator,
 * acquires the appropriate semaphore, then spawns a patient process.
 * Consumer side of the producer-consumer pattern.
 * ------------------------------------------------------- */
static void *scheduler_thread(void *arg)
{
    UNUSED(arg);
    LOG("[Scheduler] Thread started");

    while (g_running) {
        PatientRecord p;

        /* Block until a patient is waiting */
        if (queue_dequeue(&g_patient_queue, &p) != 0) {
            if (!g_running) break;
            usleep(100000);
            continue;
        }

        LOG("[Scheduler] Dequeued patient %d (%s) priority=%d",
            p.patient_id, p.name, p.priority);

        /*
         * Acquire the correct semaphore to enforce capacity limits.
         * sem_wait blocks if the ward section is full; it releases
         * when a nurse posts after a patient is discharged.
         */
        sem_t *cap_sem = NULL;
        if (p.priority <= 2) {
            cap_sem = g_sem_icu;
            LOG("[Scheduler] Waiting on ICU semaphore for patient %d", p.patient_id);
        } else if (p.priority == 3) {
            cap_sem = g_sem_iso;
            LOG("[Scheduler] Waiting on Isolation semaphore for patient %d", p.patient_id);
        }

        if (cap_sem) {
            if (sem_wait(cap_sem) != 0) {
                LOG_ERR("[Scheduler] sem_wait failed for patient %d", p.patient_id);
                continue;
            }
        }

        /* Allocate bed using the configured memory strategy */
        int bed_id = -1;
        pthread_mutex_lock(&g_ward_lock);

        /*
         * If no suitable bed, wait on g_bed_freed until a nurse
         * signals that a bed of the right type is available.
         */
        while ((bed_id = allocate_bed(&g_allocator, &p)) == -1 && g_running) {
            LOG("[Scheduler] No bed for patient %d — waiting for discharge", p.patient_id);
            pthread_cond_wait(&g_bed_freed, &g_ward_lock);
        }
        pthread_mutex_unlock(&g_ward_lock);

        if (bed_id == -1) {
            /* Shutdown during wait */
            if (cap_sem) sem_post(cap_sem);
            break;
        }

        /* Mirror allocation into shared memory ward */
        pthread_mutex_lock(&g_ward_lock);
        if (bed_id < TOTAL_BEDS) {
            g_ward->partitions[bed_id].is_free    = 0;
            g_ward->partitions[bed_id].patient_id = p.patient_id;
            g_ward->occupied_beds++;
        }
        pthread_mutex_unlock(&g_ward_lock);

        p.start_time = time(NULL);
        p.wait_time  = (int)(p.start_time - p.arrival_time);

        LOG("[Scheduler] Admitting patient %d to bed %d (%s)  wait=%d sec",
            p.patient_id, bed_id, p.bed_type, p.wait_time);

        /* Write record to mmap log */
        if (g_pat_log) mmap_append_record(g_pat_log, &p);

        /* Spawn patient_simulator */
        spawn_patient(&p);
    }

    LOG("[Scheduler] Thread exiting");
    return NULL;
}

/* -------------------------------------------------------
 * Nurse Thread (one per bed type)
 * Reads from discharge FIFO, frees beds, coalesces memory,
 * and posts semaphores to unblock the scheduler.
 * ------------------------------------------------------- */
typedef struct {
    const char *bed_type;
    sem_t      *cap_sem;
    int         nurse_id;
} NurseArg;

static void *nurse_thread(void *arg)
{
    NurseArg *na = (NurseArg *)arg;
    LOG("[Nurse-%d] Thread started — monitoring %s beds", na->nurse_id, na->bed_type);

    while (g_running) {
        char buf[32];
        ssize_t n = read(g_discharge_fd, buf, sizeof(buf) - 1);

        if (n > 0) {
            buf[n] = '\0';

            /* Parse one or more patient IDs from the read */
            char *tok = strtok(buf, "\n ");
            while (tok) {
                int pid_discharged = atoi(tok);
                if (pid_discharged <= 0) { tok = strtok(NULL, "\n "); continue; }

                LOG("[Nurse-%d] Discharge notice for patient %d",
                    na->nurse_id, pid_discharged);

                /* Find the bed assigned to this patient */
                pthread_mutex_lock(&g_ward_lock);
                int found_bed = -1;
                for (int i = 0; i < TOTAL_BEDS; i++) {
                    if (!g_ward->partitions[i].is_free &&
                        g_ward->partitions[i].patient_id == pid_discharged) {
                        found_bed = i;
                        /* Only handle our bed type */
                        if (strcmp(g_ward->partitions[i].bed_type,
                                   na->bed_type) != 0) {
                            found_bed = -1; /* let the right nurse handle it */
                        }
                        break;
                    }
                }
                pthread_mutex_unlock(&g_ward_lock);

                if (found_bed == -1) {
                    /* Another nurse will handle this patient */
                    tok = strtok(NULL, "\n ");
                    continue;
                }

                /* Free the bed in both allocator and shared memory */
                free_bed(&g_allocator, found_bed, pid_discharged);

                pthread_mutex_lock(&g_ward_lock);
                g_ward->partitions[found_bed].is_free    = 1;
                g_ward->partitions[found_bed].patient_id = -1;
                g_ward->occupied_beds--;
                g_ward->total_patients_served++;
                pthread_mutex_unlock(&g_ward_lock);

                LOG("[Nurse-%d] Bed %d freed — broadcast bed_freed",
                    na->nurse_id, found_bed);

                /* Broadcast: scheduler waiting on bed_freed may now proceed */
                pthread_mutex_lock(&g_ward_lock);
                pthread_cond_broadcast(&g_bed_freed);
                pthread_mutex_unlock(&g_ward_lock);

                /* Post semaphore so a new patient can be admitted */
                if (na->cap_sem) sem_post(na->cap_sem);

                tok = strtok(NULL, "\n ");
            }
        } else if (n == 0 || (n == -1 && errno == EAGAIN)) {
            usleep(300000); /* 300 ms poll */
        } else {
            if (g_running) LOG_ERR("[Nurse-%d] read error on discharge FIFO", na->nurse_id);
            break;
        }
    }

    LOG("[Nurse-%d] Thread exiting", na->nurse_id);
    return NULL;
}

/* -------------------------------------------------------
 * Print ward startup banner
 * ------------------------------------------------------- */
static void print_banner(void)
{
    printf("\n");
    printf("╔══════════════════════════════════════════════════╗\n");
    printf("║    Hospital Patient Triage & Bed Allocator       ║\n");
    printf("║    CL2006 — Operating Systems Lab  Spring 2026   ║\n");
    printf("╠══════════════════════════════════════════════════╣\n");
    printf("║  Ward Capacity:                                   ║\n");
    printf("║    ICU        : %2d beds  (%d care units each)      ║\n",
           ICU_BEDS, ICU_CARE_UNITS);
    printf("║    Isolation  : %2d beds  (%d care units each)      ║\n",
           ISOLATION_BEDS, ISOLATION_CARE_UNITS);
    printf("║    General    : %2d beds  (%d care unit  each)      ║\n",
           GENERAL_BEDS, GENERAL_CARE_UNITS);
    printf("║    Total units: %2d                                 ║\n",
           TOTAL_CARE_UNITS);
    printf("║  Strategy: %-12s                          ║\n",
           strategy_name(g_strategy));
    printf("╚══════════════════════════════════════════════════╝\n\n");
    fflush(stdout);
}

/* -------------------------------------------------------
 * Run all scheduling simulations on collected records
 * ------------------------------------------------------- */
static void run_scheduling_simulations(void)
{
    pthread_mutex_lock(&g_sched_lock);
    int n = g_sched_count;
    SchedEntry snap[MAX_PATIENTS];
    memcpy(snap, g_sched_entries, (size_t)n * sizeof(SchedEntry));
    pthread_mutex_unlock(&g_sched_lock);

    if (n == 0) {
        LOG("No patients recorded — skipping scheduling simulations");
        return;
    }

    LOG("Running scheduling simulations on %d patient records", n);

    SchedulerResult result;
    SchedAlgorithm algos[] = { HOSP_SCHED_FCFS, HOSP_SCHED_PRIORITY, HOSP_SCHED_SJF, HOSP_SCHED_RR };
    int n_algos = (int)(sizeof(algos) / sizeof(algos[0]));

    for (int i = 0; i < n_algos; i++) {
        schedule_run(snap, n, algos[i], RR_QUANTUM, &result);
        schedule_print_result(&result);
        schedule_log_write(&result, SCHEDULE_LOG);
    }
}

/* -------------------------------------------------------
 * Cleanup: reap remaining children, free IPC resources
 * ------------------------------------------------------- */
static void cleanup(void)
{
    LOG("=== Admissions Manager: Clean Shutdown ===");

    /* Signal ward shutdown */
    if (g_ward) {
        pthread_mutex_lock(&g_ward_lock);
        g_ward->running = 0;
        pthread_mutex_unlock(&g_ward_lock);
    }

    /* Wake queue threads so they can exit */
    pthread_cond_broadcast(&g_bed_freed);

    /* Reap any remaining child processes */
    LOG("Reaping remaining child processes...");
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0)
        ;
    /* Block until all children finish */
    while (wait(&status) > 0)
        ;

    /* Run scheduling simulations before shutdown */
    run_scheduling_simulations();
    report_paging(&g_allocator);

    /* Print final ward summary */
    printf("\n=== Final Ward Summary ===\n");
    if (g_ward) {
        printf("  Total patients served: %d\n", g_ward->total_patients_served);
        printf("  Occupied beds        : %d / %d\n",
               g_ward->occupied_beds, g_ward->total_beds);
    }

    /* Destroy queue */
    queue_destroy(&g_patient_queue);

    /* Destroy allocator */
    allocator_destroy(&g_allocator);

    /* Close discharge FIFO */
    if (g_discharge_fd != -1) close(g_discharge_fd);

    /* Close mmap log */
    if (g_pat_log) mmap_close_log(g_pat_log, g_pat_log_fd);

    /* Remove FIFOs */
    fifo_remove(DISCHARGE_FIFO);
    fifo_remove(TRIAGE_FIFO);

    /* Destroy named semaphores */
    sem_destroy_named(g_sem_icu, SEM_ICU_NAME);
    sem_destroy_named(g_sem_iso, SEM_ISO_NAME);

    /* Detach and destroy shared memory */
    shm_destroy(g_ward);

    /* Destroy mutexes */
    pthread_mutex_destroy(&g_ward_lock);
    pthread_mutex_destroy(&g_sched_lock);
    pthread_mutex_destroy(&g_id_lock);
    pthread_mutex_destroy(&g_child_lock);
    pthread_cond_destroy(&g_bed_freed);

    LOG("Admissions Manager shutdown complete");
}

/* -------------------------------------------------------
 * main
 * ------------------------------------------------------- */
int main(int argc, char *argv[])
{
    /* Parse --strategy flag */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--strategy") == 0 && i + 1 < argc) {
            g_strategy = strategy_from_string(argv[i + 1]);
            i++;
        }
    }

    /* Install signal handlers */
    struct sigaction sa_chld, sa_term;
    memset(&sa_chld, 0, sizeof(sa_chld));
    memset(&sa_term, 0, sizeof(sa_term));

    sa_chld.sa_handler = sigchld_handler;
    sa_chld.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigemptyset(&sa_chld.sa_mask);
    sigaction(SIGCHLD, &sa_chld, NULL);

    sa_term.sa_handler = sigterm_handler;
    sigemptyset(&sa_term.sa_mask);
    sigaction(SIGTERM, &sa_term, NULL);
    sigaction(SIGINT,  &sa_term, NULL);

    print_banner();

    /* Initialise shared memory */
    g_ward = shm_create();
    if (!g_ward) {
        fprintf(stderr, "FATAL: Cannot create shared memory\n");
        return EXIT_FAILURE;
    }

    /* Create FIFOs */
    if (fifo_create(DISCHARGE_FIFO) != 0 ||
        fifo_create(TRIAGE_FIFO)    != 0) {
        fprintf(stderr, "FATAL: Cannot create FIFOs\n");
        shm_destroy(g_ward);
        return EXIT_FAILURE;
    }

    /* Open discharge FIFO for reading */
    g_discharge_fd = open(DISCHARGE_FIFO, O_RDONLY | O_NONBLOCK);
    if (g_discharge_fd == -1) {
        LOG_ERR("Cannot open discharge FIFO for reading");
        shm_destroy(g_ward);
        return EXIT_FAILURE;
    }

    /* Create named semaphores */
    g_sem_icu = sem_create_named(SEM_ICU_NAME, (unsigned)ICU_BEDS);
    g_sem_iso = sem_create_named(SEM_ISO_NAME, (unsigned)ISOLATION_BEDS);
    if (g_sem_icu == SEM_FAILED || g_sem_iso == SEM_FAILED) {
        fprintf(stderr, "FATAL: Cannot create named semaphores\n");
        shm_destroy(g_ward);
        return EXIT_FAILURE;
    }

    /* Initialise priority queue */
    if (queue_init(&g_patient_queue) != 0) {
        fprintf(stderr, "FATAL: Cannot initialise patient queue\n");
        shm_destroy(g_ward);
        return EXIT_FAILURE;
    }

    /* Initialise memory allocator */
    if (allocator_init(&g_allocator, g_strategy) != 0) {
        fprintf(stderr, "FATAL: Cannot initialise allocator\n");
        shm_destroy(g_ward);
        return EXIT_FAILURE;
    }

    /* Open mmap patient log (Phase 4 bonus) */
    g_pat_log_fd = open(PATIENT_RECORD_DAT, O_RDWR | O_CREAT, 0644);
    if (g_pat_log_fd != -1) {
        ftruncate(g_pat_log_fd, (off_t)sizeof(PatientLog));
        g_pat_log = (PatientLog *)mmap(NULL, sizeof(PatientLog),
                                        PROT_READ | PROT_WRITE,
                                        MAP_SHARED, g_pat_log_fd, 0);
        if (g_pat_log == MAP_FAILED) { g_pat_log = NULL; g_pat_log_fd = -1; }
        else { memset(g_pat_log, 0, sizeof(PatientLog)); }
    }

    LOG("Admissions Manager started (pid=%d)  strategy=%s",
        getpid(), strategy_name(g_strategy));

    /* ---- Create POSIX threads ---- */

    /* 1. Receptionist thread */
    pthread_t th_receptionist;
    if (pthread_create(&th_receptionist, NULL, receptionist_thread, NULL) != 0) {
        LOG_ERR("Cannot create receptionist thread");
        cleanup();
        return EXIT_FAILURE;
    }

    /* 2. Scheduler thread */
    pthread_t th_scheduler;
    if (pthread_create(&th_scheduler, NULL, scheduler_thread, NULL) != 0) {
        LOG_ERR("Cannot create scheduler thread");
        g_running = 0;
        pthread_join(th_receptionist, NULL);
        cleanup();
        return EXIT_FAILURE;
    }

    /* 3. Nurse thread pool — one per bed type */
    pthread_t th_nurses[NURSE_THREAD_COUNT];
    NurseArg  nurse_args[NURSE_THREAD_COUNT] = {
        { BED_ICU,       g_sem_icu, 0 },
        { BED_ISOLATION, g_sem_iso, 1 },
        { BED_GENERAL,   NULL,      2 }
    };

    for (int i = 0; i < NURSE_THREAD_COUNT; i++) {
        if (pthread_create(&th_nurses[i], NULL, nurse_thread,
                           &nurse_args[i]) != 0) {
            LOG_ERR("Cannot create nurse thread %d", i);
        }
    }

    LOG("All threads running.  Waiting for SIGTERM / SIGINT to shut down.");
    LOG("Send patients via:  echo 'name:age:severity' > %s", TRIAGE_FIFO);

    /* ---- Main loop: reap zombies and wait for shutdown ---- */
    while (g_running) {
        int status;
        /* Non-blocking reap — SIGCHLD handler does most reaping */
        while (waitpid(-1, &status, WNOHANG) > 0)
            ;
        sleep(1);
    }

    /* ---- Shutdown sequence ---- */
    LOG("Shutdown signal received — stopping threads");
    g_running = 0;

    /* Unblock receptionist and scheduler */
    pthread_cond_broadcast(&g_bed_freed);

    /* Push a dummy wakeup into queue so scheduler thread unblocks */
    PatientRecord dummy;
    memset(&dummy, 0, sizeof(dummy));
    dummy.patient_id = -1;
    dummy.priority   = MAX_PRIORITY;
    dummy.arrival_time = time(NULL);
    queue_enqueue(&g_patient_queue, &dummy);

    pthread_join(th_receptionist, NULL);
    pthread_join(th_scheduler, NULL);
    for (int i = 0; i < NURSE_THREAD_COUNT; i++)
        pthread_join(th_nurses[i], NULL);

    cleanup();
    return EXIT_SUCCESS;
}
