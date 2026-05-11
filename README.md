# Hospital Patient Triage & Bed Allocator
**CL2006 — Operating Systems Lab | FAST-NUCES CFD Campus | Spring 2026**

---

## Overview

A fully-featured Linux system simulation of a hospital emergency room, demonstrating core OS concepts:

| OS Concept | Implementation |
|---|---|
| Process Management | `fork()` + `execv()` per patient, `SIGCHLD` + `waitpid(WNOHANG)` |
| IPC – Pipes | Anonymous pipe from `triage.sh` to admissions |
| IPC – FIFOs | Named FIFO for discharge notification |
| IPC – Shared Memory | `shmget`/`shmat` bed bitmap shared with patient processes |
| POSIX Threads | Receptionist, Scheduler, Nurse pool (3 threads min) |
| Mutex + Cond Vars | Protect bed bitmap; signal on bed-freed |
| Semaphores | ICU/Isolation capacity limits + bounded queue |
| CPU Scheduling | FCFS, Priority, SJF, Round Robin (all 4 simulated) |
| Memory Management | Best-Fit / First-Fit / Worst-Fit + free-list + coalescing |
| Paging | Page table simulation with internal fragmentation report |
| Virtual Memory | `mmap`-based `patient_records.dat` log (bonus) |

---

## File Structure

```
project/
├── src/
│   ├── common.h              # Shared constants, macros, structs
│   ├── queue.h / queue.c     # Thread-safe min-heap priority queue
│   ├── ipc.h / ipc.c         # Shared mem, FIFOs, semaphores, mmap
│   ├── scheduler.h / .c      # FCFS, Priority, SJF, Round Robin
│   ├── memory_allocator.h/.c # Best/First/Worst-Fit + coalescing
│   ├── admissions.c          # Central controller (threads + IPC)
│   └── patient_simulator.c   # Per-patient child process
├── scripts/
│   ├── triage.sh             # Accept patient, compute priority, pipe to admissions
│   ├── start_hospital.sh     # Bootstrap IPC + launch admissions
│   ├── stop_hospital.sh      # SIGTERM + cleanup + summary
│   └── stress_test.sh        # 20 rapid patient arrivals
├── logs/                     # schedule_log.txt, memory_log.txt, admissions.log
├── Makefile
└── README.md
```

---

## Ubuntu Setup & Dependencies

```bash
# Install required packages
sudo apt update
sudo apt install -y gcc make valgrind

# Verify GCC version (tested on 11+)
gcc --version
```

---

## Build

```bash
# From project root
make all
```

Zero-warning compilation with:
```
gcc -Wall -Wextra -pthread -g -I./src
```

---

## Run

### Quick start (Best-Fit strategy)

```bash
make run
# or
./scripts/start_hospital.sh --strategy best
```

### Admit a patient

```bash
./scripts/triage.sh "Ali Hassan" 34 8
# Format: ./scripts/triage.sh "<name>" <age> <severity 1-10>
```

### Severity → Priority mapping

| Severity | Priority | Bed Type  |
|----------|----------|-----------|
| 9–10     | 1        | ICU       |
| 7–8      | 2        | ICU       |
| 5–6      | 3        | Isolation |
| 3–4      | 4        | General   |
| 1–2      | 5        | General   |

### Allocation strategy selection

```bash
./scripts/start_hospital.sh --strategy first   # First-Fit
./scripts/start_hospital.sh --strategy worst   # Worst-Fit
./scripts/start_hospital.sh --strategy best    # Best-Fit (default)
```

### Stop

```bash
./scripts/stop_hospital.sh
```

---

## Test

```bash
make test       # 5-patient smoke test
make stress     # 20-patient stress test
```

---

## Valgrind

```bash
make valgrind
```

Or manually:
```bash
valgrind --leak-check=full --show-leak-kinds=all \
         --track-origins=yes \
         ./bin/admissions --strategy best
```

Expected: **0 memory leaks**, **0 errors** (pending all patient processes have exited).

---

## Output Files

| File | Contents |
|---|---|
| `logs/admissions.log` | Full admissions manager log |
| `logs/schedule_log.txt` | Gantt charts + avg waiting/turnaround for all 4 algorithms |
| `logs/memory_log.txt` | Timestamped fragmentation stats per event |
| `logs/patient_records.dat` | mmap binary patient record log |

---

## Example Terminal Output

```
╔══════════════════════════════════════════════════╗
║    Hospital Patient Triage & Bed Allocator       ║
║    CL2006 — Operating Systems Lab  Spring 2026   ║
╠══════════════════════════════════════════════════╣
║  Ward Capacity:                                  ║
║    ICU        :  4 beds  (3 care units each)     ║
║    Isolation  :  4 beds  (2 care units each)     ║
║    General    : 12 beds  (1 care unit  each)     ║
║    Total units: 44                               ║
║  Strategy: Best-Fit                              ║
╚══════════════════════════════════════════════════╝

[08:00:01] Shared memory created: shmid=1234  size=... beds=20
[08:00:01] FIFO created: /tmp/discharge_fifo
[08:00:01] FIFO created: /tmp/triage_fifo
[08:00:01] Named semaphore created: /sem_icu_limit  (initial=4)
[08:00:01] Named semaphore created: /sem_iso_limit  (initial=4)
[08:00:01] [Receptionist] Thread started
[08:00:01] [Scheduler] Thread started
[08:00:01] [Nurse-0] Thread started — monitoring ICU beds
[08:00:01] [Nurse-1] Thread started — monitoring ISOLATION beds
[08:00:01] [Nurse-2] Thread started — monitoring GENERAL beds

[08:00:05] [Receptionist] Received patient: Ali Hassan  age=34  sev=8  pri=2
[08:00:05] [Scheduler] Dequeued patient 1 (Ali Hassan) priority=2
[08:00:05] [Scheduler] Waiting on ICU semaphore for patient 1
[08:00:05] [ICU] Bed 1 allocated to patient 1 (Ali Hassan) | units 3-5
[08:00:05] Spawned patient_simulator pid=5678 for patient 1
[08:00:05] [Patient 1] ARRIVED — priority=2  bed=1  type=ICU  pid=5678
[08:00:05] [Patient 1] TREATMENT STARTED in ICU bed 1
```

---

## OS Concepts Demonstrated

### fork() + execv()
Each admitted patient causes `admissions` to call `fork()`. The child process calls `execv("./bin/patient_simulator", args)` to replace itself with the simulator executable, passing `patient_id`, `priority`, `bed_id`, and `bed_type` as arguments.

### SIGCHLD + waitpid(WNOHANG)
`admissions` installs a `SIGCHLD` handler that calls `waitpid(-1, &status, WNOHANG)` in a loop, reaping all available zombie children without blocking the main event loop.

### Producer-Consumer (Threads + Semaphores)
- **Receptionist thread** (producer): reads from `TRIAGE_FIFO`, decrements `sem_queue_slots`, enqueues into the priority queue.
- **Scheduler thread** (consumer): calls `queue_dequeue()` (blocks on `pthread_cond_wait` when empty), allocates bed, spawns patient.
- **Bounded semaphore** (`sem_queue_slots`, initial = 32) prevents the queue from overflowing under rapid arrivals.

### Bed-Freed Condition Variable
When all ICU beds are occupied, the scheduler calls `pthread_cond_wait(&g_bed_freed, &g_ward_lock)`. When a nurse thread processes a discharge, it calls `pthread_cond_broadcast(&g_bed_freed)`, waking the scheduler to retry allocation.

### Coalescing
When a patient is discharged, `coalesce_free()` walks the sorted free list and merges the freed `FreeNode` with adjacent free nodes (left and right), reducing external fragmentation.
