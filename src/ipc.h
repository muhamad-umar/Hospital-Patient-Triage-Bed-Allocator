/*
 * ============================================================
 * Project : Hospital Patient Triage & Bed Allocator
 * File    : ipc.h
 * Purpose : Interface for all IPC helpers:
 *             - POSIX shared memory (SharedWard)
 *             - Named FIFOs (triage_fifo, discharge_fifo)
 *             - Named semaphores (ICU, Isolation, queue)
 *             - mmap-based patient record log (bonus)
 * ============================================================
 */

#ifndef IPC_H
#define IPC_H

#include "common.h"

/* -------------------------------------------------------
 * Shared Memory
 * ------------------------------------------------------- */

/*
 * shm_create — create and initialise the SharedWard segment.
 * Called once by start_hospital.sh / admissions at startup.
 * Returns pointer to mapped segment, or NULL on failure.
 */
SharedWard *shm_create(void);

/*
 * shm_attach — attach to an existing SharedWard segment.
 * Used by patient_simulator processes.
 * Returns pointer to mapped segment, or NULL on failure.
 */
SharedWard *shm_attach(void);

/*
 * shm_detach — detach the segment from this process's address space.
 */
void shm_detach(SharedWard *ward);

/*
 * shm_destroy — detach and remove the shared memory segment.
 * Called by admissions on clean shutdown.
 */
void shm_destroy(SharedWard *ward);

/* -------------------------------------------------------
 * Named FIFOs
 * ------------------------------------------------------- */

/*
 * fifo_create — create a named FIFO if it does not exist.
 * path: e.g. DISCHARGE_FIFO or TRIAGE_FIFO
 * Returns 0 on success, -1 on failure.
 */
int fifo_create(const char *path);

/*
 * fifo_remove — unlink a named FIFO.
 */
void fifo_remove(const char *path);

/*
 * fifo_open_write — open a FIFO for writing (blocks until reader ready).
 * Returns file descriptor, or -1 on error.
 */
int fifo_open_write(const char *path);

/*
 * fifo_open_read — open a FIFO for reading (non-blocking).
 * Returns file descriptor, or -1 on error.
 */
int fifo_open_read(const char *path);

/* -------------------------------------------------------
 * Named Semaphores
 * ------------------------------------------------------- */

/*
 * sem_create_named — create (or open) a named semaphore.
 * name   : semaphore name (e.g. SEM_ICU_NAME)
 * initial: initial count
 * Returns sem_t pointer or SEM_FAILED on error.
 */
sem_t *sem_create_named(const char *name, unsigned int initial);

/*
 * sem_open_named — open an existing named semaphore.
 */
sem_t *sem_open_named(const char *name);

/*
 * sem_close_named — close a named semaphore handle (does not unlink).
 */
void sem_close_named(sem_t *s);

/*
 * sem_destroy_named — close and unlink a named semaphore.
 */
void sem_destroy_named(sem_t *s, const char *name);

/* -------------------------------------------------------
 * mmap-based patient record log (Phase 4 bonus)
 * ------------------------------------------------------- */

/* Maximum number of records stored in the mmap file */
#define MMAP_MAX_RECORDS 256

typedef struct {
    int           record_count;
    PatientRecord records[MMAP_MAX_RECORDS];
} PatientLog;

/*
 * mmap_open_log — create/open patient_records.dat and mmap it.
 * Returns pointer to PatientLog, or NULL on failure.
 */
PatientLog *mmap_open_log(const char *path);

/*
 * mmap_append_record — write a PatientRecord into the mapped log.
 * Thread-safe via an internal mutex.
 * Returns 0 on success, -1 if log is full.
 */
int mmap_append_record(PatientLog *log, const PatientRecord *p);

/*
 * mmap_close_log — msync and munmap the log file.
 * fd: the file descriptor returned alongside the mapping (passed
 *     by the caller who opened it).
 */
void mmap_close_log(PatientLog *log, int fd);

#endif /* IPC_H */
