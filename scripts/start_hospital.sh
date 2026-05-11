#!/bin/bash
# ============================================================
# Project : Hospital Patient Triage & Bed Allocator
# Script  : start_hospital.sh
# Purpose : Bootstrap the hospital simulation environment:
#             1. Check binaries are built
#             2. Create logs/ directory
#             3. Create FIFOs (discharge and triage)
#             4. Launch admissions manager in background
#             5. Print startup banner
# Usage   : ./scripts/start_hospital.sh [--strategy best|first|worst]
# ============================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

ADMISSIONS_BIN="$PROJECT_ROOT/bin/admissions"
LOGS_DIR="$PROJECT_ROOT/logs"
PID_FILE="$PROJECT_ROOT/logs/admissions.pid"
TRIAGE_FIFO="/tmp/triage_fifo"
DISCHARGE_FIFO="/tmp/discharge_fifo"

STRATEGY="best"

# Parse --strategy argument
for arg in "$@"; do
    case "$arg" in
        --strategy)
            shift
            STRATEGY="$1"
            ;;
    esac
done

# -------------------------------------------------------
# Banner
# -------------------------------------------------------
echo ""
echo "╔══════════════════════════════════════════════════╗"
echo "║     Hospital Patient Triage & Bed Allocator      ║"
echo "║     FAST-NUCES CFD  —  OS Lab  Spring 2026       ║"
echo "╠══════════════════════════════════════════════════╣"
echo "║  Starting hospital system...                     ║"
echo "╚══════════════════════════════════════════════════╝"
echo ""

# -------------------------------------------------------
# Sanity checks
# -------------------------------------------------------
if [ ! -f "$ADMISSIONS_BIN" ]; then
    echo "ERROR: admissions binary not found at $ADMISSIONS_BIN"
    echo "       Run 'make all' first."
    exit 1
fi

# Check if already running
if [ -f "$PID_FILE" ]; then
    OLD_PID=$(cat "$PID_FILE")
    if kill -0 "$OLD_PID" 2>/dev/null; then
        echo "WARNING: Hospital already running (pid=$OLD_PID)"
        echo "         Run ./scripts/stop_hospital.sh first."
        exit 1
    fi
fi

# -------------------------------------------------------
# Setup
# -------------------------------------------------------
mkdir -p "$LOGS_DIR"
touch "$LOGS_DIR/schedule_log.txt"
touch "$LOGS_DIR/memory_log.txt"

# Remove any stale FIFOs and IPC resources from previous runs
rm -f "$TRIAGE_FIFO" "$DISCHARGE_FIFO"

# Remove stale shared memory
SHM_KEY=0xBEDF00D
if ipcs -m 2>/dev/null | grep -q "$SHM_KEY" 2>/dev/null; then
    SHMID=$(ipcs -m | awk -v key="$SHM_KEY" '$1 == key {print $2}')
    if [ -n "$SHMID" ]; then
        ipcrm -m "$SHMID" 2>/dev/null
        echo "[Setup] Removed stale shared memory segment $SHMID"
    fi
fi

# Remove stale named semaphores
sem_unlink() { rm -f "/dev/shm/sem.${1#/}" 2>/dev/null; true; }
sem_unlink "sem_icu_limit"
sem_unlink "sem_iso_limit"
sem_unlink "sem_queue_slots"

echo "[Setup] Logs directory   : $LOGS_DIR"
echo "[Setup] Allocation strategy: $STRATEGY"

# -------------------------------------------------------
# Launch admissions manager
# -------------------------------------------------------
echo "[Start] Launching admissions manager..."

cd "$PROJECT_ROOT" || exit 1

"$ADMISSIONS_BIN" --strategy "$STRATEGY" \
    >> "$LOGS_DIR/admissions.log" 2>&1 &

ADMISSIONS_PID=$!
echo "$ADMISSIONS_PID" > "$PID_FILE"

# Give it a moment to initialise
sleep 1

if kill -0 "$ADMISSIONS_PID" 2>/dev/null; then
    echo "[OK]    Admissions manager started (pid=$ADMISSIONS_PID)"
    echo ""
    echo "  Ward Capacity:"
    echo "    ICU       : 4 beds  (care units = 3 each)"
    echo "    Isolation : 4 beds  (care units = 2 each)"
    echo "    General   : 12 beds (care units = 1 each)"
    echo "    Total     : 44 care units"
    echo ""
    echo "  To admit a patient:"
    echo "    ./scripts/triage.sh \"Patient Name\" <age> <severity 1-10>"
    echo ""
    echo "  To stop:"
    echo "    ./scripts/stop_hospital.sh"
    echo ""
else
    echo "ERROR: Admissions manager failed to start."
    echo "       Check $LOGS_DIR/admissions.log for details."
    rm -f "$PID_FILE"
    exit 1
fi

exit 0
