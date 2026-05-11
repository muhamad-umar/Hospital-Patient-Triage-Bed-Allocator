/*
 * ============================================================
 * Project : Hospital Patient Triage & Bed Allocator
 * File    : patient_simulator.c
 * Purpose : Simulates one patient's stay in the hospital.
 *           Spawned by admissions via fork() + execv().
 *           Receives: patient_id  triage_priority  bed_id  bed_type
 *           Steps:
 *             1. Print arrival message
 *             2. Attach to shared memory, update bed status
 *             3. Sleep random duration (treatment simulation)
 *             4. Print discharge message
 *             5. Write patient_id to discharge FIFO
 *             6. Detach shared memory and exit
 * Compile : gcc -Wall -Wextra -pthread -o bin/patient_simulator
 *               src/patient_simulator.c src/ipc.c -lpthread
 * ============================================================
 */

#include "common.h"
#include "ipc.h"

/* -------------------------------------------------------
 * Helpers
 * ------------------------------------------------------- */

/*
 * get_sleep_duration — return a random treatment duration (seconds)
 * based on bed type.
 *   ICU       : 5-15 sec
 *   ISOLATION : 3-10 sec
 *   GENERAL   : 2-8  sec
 */
static int get_sleep_duration(const char *bed_type)
{
    srand((unsigned int)(time(NULL) ^ (getpid() << 4)));

    if (strcmp(bed_type, BED_ICU) == 0)
        return 5 + rand() % 11;           /* [5,15] */
    if (strcmp(bed_type, BED_ISOLATION) == 0)
        return 3 + rand() % 8;            /* [3,10] */
    return 2 + rand() % 7;               /* GENERAL: [2,8] */
}

/* -------------------------------------------------------
 * main
 * ------------------------------------------------------- */

int main(int argc, char *argv[])
{
    if (argc < 5) {
        fprintf(stderr,
            "Usage: %s <patient_id> <triage_priority> <bed_id> <bed_type>\n",
            argv[0]);
        return EXIT_FAILURE;
    }

    /* Parse arguments passed by admissions via execv() */
    int patient_id      = atoi(argv[1]);
    int triage_priority = atoi(argv[2]);
    int bed_id          = atoi(argv[3]);
    const char *bed_type = argv[4];

    LOG("[Patient %d] ARRIVED — priority=%d  bed=%d  type=%s  pid=%d",
        patient_id, triage_priority, bed_id, bed_type, getpid());

    /* Attach to shared memory so we can update bed status */
    SharedWard *ward = shm_attach();
    if (!ward) {
        fprintf(stderr, "[Patient %d] Failed to attach shared memory\n",
                patient_id);
        return EXIT_FAILURE;
    }

    /* Validate bed index */
    if (bed_id < 0 || bed_id >= TOTAL_BEDS) {
        fprintf(stderr, "[Patient %d] Invalid bed_id %d\n", patient_id, bed_id);
        shm_detach(ward);
        return EXIT_FAILURE;
    }

    /* --- Treatment start --- */
    LOG("[Patient %d] TREATMENT STARTED in %s bed %d",
        patient_id, bed_type, bed_id);

    /* Simulate treatment time */
    int duration = get_sleep_duration(bed_type);
    LOG("[Patient %d] Treatment duration: %d seconds", patient_id, duration);
    sleep((unsigned int)duration);

    /* --- Discharge --- */
    LOG("[Patient %d] DISCHARGE from %s bed %d after %d sec treatment",
        patient_id, bed_type, bed_id, duration);

    /*
     * Notify admissions manager of discharge via the named FIFO.
     * We write a simple integer (patient_id) as text so the
     * admissions process knows which bed to free.
     */
    int fifo_fd = fifo_open_write(DISCHARGE_FIFO);
    if (fifo_fd == -1) {
        fprintf(stderr, "[Patient %d] Cannot open discharge FIFO\n", patient_id);
    } else {
        char msg[32];
        int len = snprintf(msg, sizeof(msg), "%d\n", patient_id);
        if (write(fifo_fd, msg, (size_t)len) != len) {
            fprintf(stderr, "[Patient %d] Failed to write discharge FIFO\n",
                    patient_id);
        }
        close(fifo_fd);
        LOG("[Patient %d] Discharge notification sent via FIFO", patient_id);
    }

    /* Detach from shared memory */
    shm_detach(ward);

    LOG("[Patient %d] Process exiting (pid=%d)", patient_id, getpid());
    return EXIT_SUCCESS;
}
