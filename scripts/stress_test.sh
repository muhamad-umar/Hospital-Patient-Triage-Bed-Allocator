#!/bin/bash
# ============================================================
# Project : Hospital Patient Triage & Bed Allocator
# Script  : stress_test.sh
# Purpose : Rapid stress test — spawns 20 patient arrivals
#           in quick succession to test concurrent processing,
#           semaphore blocking, and queue saturation.
# Usage   : ./scripts/stress_test.sh
# ============================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

TRIAGE_SH="$SCRIPT_DIR/triage.sh"
TRIAGE_FIFO="/tmp/triage_fifo"

if [ ! -p "$TRIAGE_FIFO" ]; then
    echo "ERROR: Hospital is not running. Run ./scripts/start_hospital.sh first."
    exit 1
fi

echo ""
echo "=== Stress Test: 20 Rapid Patient Arrivals ==="
echo ""

# Arrays of realistic patient names and severities
NAMES=(
    "Ahmed Khan"    "Sara Ali"     "Omar Hassan"   "Fatima Malik"
    "Bilal Raza"    "Ayesha Noor"  "Usman Butt"    "Sana Sheikh"
    "Zain Mirza"    "Hira Iqbal"   "Kamran Dar"    "Nadia Qureshi"
    "Asad Javed"    "Maria Hussain" "Rizwan Anwar" "Amna Chaudhry"
    "Hamza Farooq" "Sadia Niaz"   "Faisal Waqar"  "Rabia Tariq"
)

SEVERITIES=(10 2 8 5 9 3 7 6 10 1 8 4 9 5 7 3 10 6 8 2)
AGES=(25 34 45 60 18 72 38 50 29 41 55 27 67 33 48 21 76 39 52 44)

TOTAL=20

for i in $(seq 0 $((TOTAL - 1))); do
    NAME="${NAMES[$i]}"
    AGE="${AGES[$i]}"
    SEV="${SEVERITIES[$i]}"

    echo "[$((i+1))/$TOTAL] Registering: \"$NAME\"  age=$AGE  severity=$SEV"

    # Run triage.sh in background for maximum concurrency
    "$TRIAGE_SH" "$NAME" "$AGE" "$SEV" &

    # Stagger slightly so the queue doesn't overflow in one burst
    sleep 0.15
done

# Wait for all triage background jobs to complete
wait

echo ""
echo "=== Stress test complete: $TOTAL patients submitted ==="
echo "Monitor admissions output for processing results."
echo ""
exit 0
