#!/bin/bash
# ============================================================
# Project : Hospital Patient Triage & Bed Allocator
# Script  : stop_hospital.sh
# Purpose : Gracefully stop the hospital simulation:
#             1. Send SIGTERM to admissions manager
#             2. Wait for it to exit (it triggers cleanup)
#             3. Force-remove any lingering IPC resources
#             4. Print final summary
# Usage   : ./scripts/stop_hospital.sh
# ============================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
PID_FILE="$PROJECT_ROOT/logs/admissions.pid"
LOGS_DIR="$PROJECT_ROOT/logs"
TRIAGE_FIFO="/tmp/triage_fifo"
DISCHARGE_FIFO="/tmp/discharge_fifo"

echo ""
echo "╔══════════════════════════════════════════════════╗"
echo "║     Hospital Shutdown Sequence                   ║"
echo "╚══════════════════════════════════════════════════╝"
echo ""

# -------------------------------------------------------
# Send SIGTERM to admissions manager
# -------------------------------------------------------
if [ ! -f "$PID_FILE" ]; then
    echo "WARNING: PID file not found ($PID_FILE). Hospital may not be running."
else
    ADMISSIONS_PID=$(cat "$PID_FILE")

    if kill -0 "$ADMISSIONS_PID" 2>/dev/null; then
        echo "[Stop]  Sending SIGTERM to admissions manager (pid=$ADMISSIONS_PID)..."
        kill -SIGTERM "$ADMISSIONS_PID"

        # Wait up to 15 seconds for graceful shutdown
        WAIT=0
        while kill -0 "$ADMISSIONS_PID" 2>/dev/null && [ $WAIT -lt 15 ]; do
            sleep 1
            WAIT=$((WAIT + 1))
            printf "."
        done
        echo ""

        if kill -0 "$ADMISSIONS_PID" 2>/dev/null; then
            echo "[Force] Admissions did not exit — sending SIGKILL"
            kill -SIGKILL "$ADMISSIONS_PID" 2>/dev/null
        else
            echo "[OK]    Admissions manager exited cleanly"
        fi
    else
        echo "[Info]  Admissions process not running (stale PID file?)"
    fi

    rm -f "$PID_FILE"
fi

# -------------------------------------------------------
# Kill any lingering patient_simulator processes
# -------------------------------------------------------
SIMS=$(pgrep -f "patient_simulator" 2>/dev/null)
if [ -n "$SIMS" ]; then
    echo "[Stop]  Killing lingering patient_simulator processes: $SIMS"
    pkill -f "patient_simulator" 2>/dev/null
    sleep 1
fi

# -------------------------------------------------------
# Remove named FIFOs
# -------------------------------------------------------
for FIFO in "$TRIAGE_FIFO" "$DISCHARGE_FIFO"; do
    if [ -p "$FIFO" ]; then
        rm -f "$FIFO"
        echo "[Clean] Removed FIFO: $FIFO"
    fi
done

# -------------------------------------------------------
# Remove shared memory segments (key 0xBEDF00D)
# -------------------------------------------------------
SHMID=$(ipcs -m 2>/dev/null | awk 'NR>3 {print $1, $2}' | while read -r key id; do
    if [ "$key" = "0xbedf00d" ] || [ "$key" = "200933389" ]; then echo "$id"; fi
done)

if [ -n "$SHMID" ]; then
    ipcrm -m "$SHMID" 2>/dev/null && echo "[Clean] Removed shared memory segment $SHMID"
fi

# -------------------------------------------------------
# Remove named semaphores
# -------------------------------------------------------
for SEM in sem_icu_limit sem_iso_limit sem_queue_slots; do
    SEM_FILE="/dev/shm/sem.$SEM"
    if [ -f "$SEM_FILE" ]; then
        rm -f "$SEM_FILE"
        echo "[Clean] Removed semaphore: /$SEM"
    fi
done

# -------------------------------------------------------
# Print final summary from logs
# -------------------------------------------------------
echo ""
echo "╔══════════════════════════════════════════════════╗"
echo "║     Final Ward Summary                           ║"
echo "╠══════════════════════════════════════════════════╣"

if [ -f "$LOGS_DIR/admissions.log" ]; then
    SERVED=$(grep -o "total_patients_served.*" "$LOGS_DIR/admissions.log" \
             2>/dev/null | tail -1 | grep -o '[0-9]*' | head -1)
    [ -n "$SERVED" ] && echo "║  Total patients served : $SERVED" || true
fi

if [ -f "$LOGS_DIR/schedule_log.txt" ]; then
    RUNS=$(grep -c "===" "$LOGS_DIR/schedule_log.txt" 2>/dev/null || echo "0")
    echo "║  Scheduling runs logged: $RUNS"
fi

if [ -f "$LOGS_DIR/memory_log.txt" ]; then
    EVENTS=$(wc -l < "$LOGS_DIR/memory_log.txt" 2>/dev/null || echo "0")
    echo "║  Memory events logged  : $EVENTS"
fi

echo "║"
echo "║  Log files:"
echo "║    $LOGS_DIR/admissions.log"
echo "║    $LOGS_DIR/schedule_log.txt"
echo "║    $LOGS_DIR/memory_log.txt"
echo "╚══════════════════════════════════════════════════╝"
echo ""
echo "Hospital shutdown complete."
exit 0
