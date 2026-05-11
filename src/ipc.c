/*
 * ============================================================
 * Project : Hospital Patient Triage & Bed Allocator
 * File    : ipc.c
 * Purpose : Implementation of all IPC primitives:
 *             - POSIX System V shared memory (shmget/shmat)
 *             - Named FIFOs (mkfifo/open)
 *             - POSIX named semaphores (sem_open)
 *             - mmap patient record log (mmap/msync/munmap)
 * Compile : gcc -Wall -Wextra -pthread -c ipc.c
 * ============================================================
 */

#include "ipc.h"
#include <sys/mman.h>

/* -------------------------------------------------------
 * Internal: shared memory ID cache so shm_destroy can
 * call shmctl(IPC_RMID) with the correct id.
 * ------------------------------------------------------- */
static int g_shm_id = -1;

/* -------------------------------------------------------
 * Shared Memory — System V shmget / shmat
 * ------------------------------------------------------- */

SharedWard *shm_create(void)
{
    /* Create a new shared memory segment (fail if already exists) */
    g_shm_id = shmget((key_t)SHM_KEY, sizeof(SharedWard),
                      IPC_CREAT | IPC_EXCL | 0666);
    if (g_shm_id == -1) {
        /*
         * If it already exists (e.g. previous crash), try to attach
         * to it and then remove it before creating fresh.
         */
        g_shm_id = shmget((key_t)SHM_KEY, sizeof(SharedWard), 0666);
        if (g_shm_id != -1) {
            shmctl(g_shm_id, IPC_RMID, NULL);
        }
        g_shm_id = shmget((key_t)SHM_KEY, sizeof(SharedWard),
                          IPC_CREAT | IPC_EXCL | 0666);
        if (g_shm_id == -1) {
            LOG_ERR("shm_create: shmget failed");
            return NULL;
        }
    }

    /* Attach the segment to our address space */
    SharedWard *ward = (SharedWard *)shmat(g_shm_id, NULL, 0);
    if (ward == (SharedWard *)-1) {
        LOG_ERR("shm_create: shmat failed");
        shmctl(g_shm_id, IPC_RMID, NULL);
        return NULL;
    }

    /* Zero-initialise and set defaults */
    memset(ward, 0, sizeof(SharedWard));
    ward->total_beds            = TOTAL_BEDS;
    ward->occupied_beds         = 0;
    ward->total_patients_served = 0;
    ward->running               = 1;
    ward->admissions_pid        = getpid();

    /*
     * Initialise the bed partition array.
     * Layout: [0..ICU_BEDS-1] ICU, [ICU_BEDS..ICU+ISO-1] ISOLATION,
     *         [ICU+ISO..TOTAL_BEDS-1] GENERAL
     */
    int unit = 0;

    /* ICU beds */
    for (int i = 0; i < ICU_BEDS; i++) {
        ward->partitions[i].partition_id = i;
        ward->partitions[i].start_unit   = unit;
        ward->partitions[i].size         = ICU_CARE_UNITS;
        ward->partitions[i].is_free      = 1;
        ward->partitions[i].patient_id   = -1;
        strncpy(ward->partitions[i].bed_type, BED_ICU,
                sizeof(ward->partitions[i].bed_type) - 1);
        unit += ICU_CARE_UNITS;
    }

    /* Isolation beds */
    for (int i = 0; i < ISOLATION_BEDS; i++) {
        int idx = ICU_BEDS + i;
        ward->partitions[idx].partition_id = idx;
        ward->partitions[idx].start_unit   = unit;
        ward->partitions[idx].size         = ISOLATION_CARE_UNITS;
        ward->partitions[idx].is_free      = 1;
        ward->partitions[idx].patient_id   = -1;
        strncpy(ward->partitions[idx].bed_type, BED_ISOLATION,
                sizeof(ward->partitions[idx].bed_type) - 1);
        unit += ISOLATION_CARE_UNITS;
    }

    /* General ward beds */
    for (int i = 0; i < GENERAL_BEDS; i++) {
        int idx = ICU_BEDS + ISOLATION_BEDS + i;
        ward->partitions[idx].partition_id = idx;
        ward->partitions[idx].start_unit   = unit;
        ward->partitions[idx].size         = GENERAL_CARE_UNITS;
        ward->partitions[idx].is_free      = 1;
        ward->partitions[idx].patient_id   = -1;
        strncpy(ward->partitions[idx].bed_type, BED_GENERAL,
                sizeof(ward->partitions[idx].bed_type) - 1);
        unit += GENERAL_CARE_UNITS;
    }

    LOG("Shared memory created: shmid=%d  size=%zu bytes  beds=%d  units=%d",
        g_shm_id, sizeof(SharedWard), TOTAL_BEDS, TOTAL_CARE_UNITS);
    return ward;
}

SharedWard *shm_attach(void)
{
    g_shm_id = shmget((key_t)SHM_KEY, sizeof(SharedWard), 0666);
    if (g_shm_id == -1) {
        LOG_ERR("shm_attach: shmget failed (hospital not running?)");
        return NULL;
    }

    SharedWard *ward = (SharedWard *)shmat(g_shm_id, NULL, 0);
    if (ward == (SharedWard *)-1) {
        LOG_ERR("shm_attach: shmat failed");
        return NULL;
    }
    return ward;
}

void shm_detach(SharedWard *ward)
{
    if (ward && ward != (SharedWard *)-1) {
        if (shmdt((void *)ward) == -1)
            LOG_ERR("shm_detach: shmdt failed");
    }
}

void shm_destroy(SharedWard *ward)
{
    shm_detach(ward);

    /* Retrieve the shmid if we lost it (e.g., called from stop script) */
    if (g_shm_id == -1) {
        g_shm_id = shmget((key_t)SHM_KEY, sizeof(SharedWard), 0666);
    }

    if (g_shm_id != -1) {
        if (shmctl(g_shm_id, IPC_RMID, NULL) == 0)
            LOG("Shared memory segment removed (shmid=%d)", g_shm_id);
        else
            LOG_ERR("shm_destroy: shmctl IPC_RMID failed");
        g_shm_id = -1;
    }
}

/* -------------------------------------------------------
 * Named FIFOs
 * ------------------------------------------------------- */

int fifo_create(const char *path)
{
    /* Remove any stale FIFO first */
    unlink(path);

    if (mkfifo(path, 0666) == -1) {
        LOG_ERR("fifo_create: mkfifo(%s) failed", path);
        return -1;
    }
    LOG("FIFO created: %s", path);
    return 0;
}

void fifo_remove(const char *path)
{
    if (unlink(path) == 0)
        LOG("FIFO removed: %s", path);
}

int fifo_open_write(const char *path)
{
    /*
     * Open in O_WRONLY | O_NONBLOCK so we do not block waiting for
     * a reader.  If the admissions process has not opened the read
     * end yet, we get ENXIO and retry briefly.
     */
    int fd = -1;
    int retries = 20;
    while (retries-- > 0) {
        fd = open(path, O_WRONLY | O_NONBLOCK);
        if (fd != -1) return fd;
        if (errno != ENXIO) break;
        usleep(100000); /* 100 ms */
    }
    LOG_ERR("fifo_open_write: open(%s) failed", path);
    return -1;
}

int fifo_open_read(const char *path)
{
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd == -1)
        LOG_ERR("fifo_open_read: open(%s) failed", path);
    return fd;
}

/* -------------------------------------------------------
 * Named Semaphores
 * ------------------------------------------------------- */

sem_t *sem_create_named(const char *name, unsigned int initial)
{
    /* Remove any stale semaphore from a previous run */
    sem_unlink(name);

    sem_t *s = sem_open(name, O_CREAT | O_EXCL, 0644, initial);
    if (s == SEM_FAILED) {
        LOG_ERR("sem_create_named: sem_open(%s) failed", name);
        return SEM_FAILED;
    }
    LOG("Named semaphore created: %s  (initial=%u)", name, initial);
    return s;
}

sem_t *sem_open_named(const char *name)
{
    sem_t *s = sem_open(name, 0);
    if (s == SEM_FAILED)
        LOG_ERR("sem_open_named: sem_open(%s) failed", name);
    return s;
}

void sem_close_named(sem_t *s)
{
    if (s && s != SEM_FAILED)
        sem_close(s);
}

void sem_destroy_named(sem_t *s, const char *name)
{
    sem_close_named(s);
    if (name) {
        if (sem_unlink(name) == 0)
            LOG("Named semaphore removed: %s", name);
    }
}

/* -------------------------------------------------------
 * mmap patient record log (Phase 4 bonus)
 * ------------------------------------------------------- */

/* Internal mutex so multiple threads can call mmap_append_record */
static pthread_mutex_t g_mmap_lock = PTHREAD_MUTEX_INITIALIZER;

PatientLog *mmap_open_log(const char *path)
{
    /* Open (or create) the backing file */
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd == -1) {
        LOG_ERR("mmap_open_log: open(%s) failed", path);
        return NULL;
    }

    /* Ensure the file is large enough for the PatientLog struct */
    size_t sz = sizeof(PatientLog);
    if (ftruncate(fd, (off_t)sz) == -1) {
        LOG_ERR("mmap_open_log: ftruncate failed");
        close(fd);
        return NULL;
    }

    PatientLog *log = (PatientLog *)mmap(NULL, sz,
                                         PROT_READ | PROT_WRITE,
                                         MAP_SHARED, fd, 0);
    if (log == MAP_FAILED) {
        LOG_ERR("mmap_open_log: mmap failed");
        close(fd);
        return NULL;
    }

    /* Do NOT close fd here — caller needs it for munmap size */
    LOG("mmap patient log opened: %s  (capacity=%d records)", path,
        MMAP_MAX_RECORDS);
    return log;
}

int mmap_append_record(PatientLog *log, const PatientRecord *p)
{
    if (!log || !p) return -1;

    pthread_mutex_lock(&g_mmap_lock);

    if (log->record_count >= MMAP_MAX_RECORDS) {
        pthread_mutex_unlock(&g_mmap_lock);
        LOG_ERR("mmap_append_record: log full (%d records)", MMAP_MAX_RECORDS);
        return -1;
    }

    log->records[log->record_count] = *p;
    log->record_count++;

    pthread_mutex_unlock(&g_mmap_lock);
    return 0;
}

void mmap_close_log(PatientLog *log, int fd)
{
    if (!log || log == MAP_FAILED) return;

    /* Flush dirty pages to the underlying file */
    if (msync(log, sizeof(PatientLog), MS_SYNC) == -1)
        LOG_ERR("mmap_close_log: msync failed");

    if (munmap(log, sizeof(PatientLog)) == -1)
        LOG_ERR("mmap_close_log: munmap failed");
    else
        LOG("mmap patient log closed and synced to disk");

    if (fd != -1) close(fd);
}
