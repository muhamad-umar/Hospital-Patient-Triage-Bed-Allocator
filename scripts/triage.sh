#!/bin/bash
# ============================================================
# Project : Hospital Patient Triage & Bed Allocator
# Script  : triage.sh
# Purpose : Accept patient input, validate it, compute triage
#           priority (1-5), and pipe the record to admissions
#           via the named FIFO (TRIAGE_FIFO).
# Usage   : ./scripts/triage.sh <name> <age> <severity 1-10>
# Example : ./scripts/triage.sh "Ali Hassan" 34 8
# ============================================================

TRIAGE_FIFO="/tmp/triage_fifo"

# -------------------------------------------------------
# Usage check
# -------------------------------------------------------
if [ "$#" -lt 3 ]; then
    echo "Usage: $0 <patient_name> <age> <severity 1-10>"
    echo "Example: $0 \"Ali Hassan\" 34 8"
    exit 1
fi

PATIENT_NAME="$1"
AGE="$2"
SEVERITY="$3"

# -------------------------------------------------------
# Input validation
# -------------------------------------------------------

# Name: must not be empty
if [ -z "$PATIENT_NAME" ]; then
    echo "ERROR: Patient name cannot be empty." >&2
    exit 1
fi

# Remove non-printable characters from name for safety
PATIENT_NAME=$(echo "$PATIENT_NAME" | tr -dc '[:print:]')
if [ -z "$PATIENT_NAME" ]; then
    echo "ERROR: Patient name contains no valid characters." >&2
    exit 1
fi

# Age: must be a positive integer in range [0, 150]
if ! [[ "$AGE" =~ ^[0-9]+$ ]]; then
    echo "ERROR: Age must be a non-negative integer (got: '$AGE')." >&2
    exit 1
fi
if [ "$AGE" -lt 0 ] || [ "$AGE" -gt 150 ]; then
    echo "ERROR: Age must be between 0 and 150 (got: $AGE)." >&2
    exit 1
fi

# Severity: must be an integer in range [1, 10]
if ! [[ "$SEVERITY" =~ ^[0-9]+$ ]]; then
    echo "ERROR: Severity must be a numeric value 1-10 (got: '$SEVERITY')." >&2
    exit 1
fi
if [ "$SEVERITY" -lt 1 ] || [ "$SEVERITY" -gt 10 ]; then
    echo "ERROR: Severity must be between 1 and 10 (got: $SEVERITY)." >&2
    exit 1
fi

# -------------------------------------------------------
# Compute triage priority (1=critical, 5=minor)
#   Severity 9-10 -> Priority 1 (Immediate)
#   Severity 7-8  -> Priority 2 (Emergent)
#   Severity 5-6  -> Priority 3 (Urgent)
#   Severity 3-4  -> Priority 4 (Less Urgent)
#   Severity 1-2  -> Priority 5 (Non-Urgent)
# -------------------------------------------------------
if   [ "$SEVERITY" -ge 9 ]; then PRIORITY=1; BED_TYPE="ICU"
elif [ "$SEVERITY" -ge 7 ]; then PRIORITY=2; BED_TYPE="ICU"
elif [ "$SEVERITY" -ge 5 ]; then PRIORITY=3; BED_TYPE="ISOLATION"
elif [ "$SEVERITY" -ge 3 ]; then PRIORITY=4; BED_TYPE="GENERAL"
else                              PRIORITY=5; BED_TYPE="GENERAL"
fi

# -------------------------------------------------------
# Display triage result
# -------------------------------------------------------
TIMESTAMP=$(date '+%Y-%m-%d %H:%M:%S')
echo "============================================"
echo "  TRIAGE ASSESSMENT"
echo "============================================"
echo "  Timestamp : $TIMESTAMP"
echo "  Name      : $PATIENT_NAME"
echo "  Age       : $AGE"
echo "  Severity  : $SEVERITY / 10"
echo "  Priority  : $PRIORITY (1=Critical, 5=Minor)"
echo "  Bed Type  : $BED_TYPE"
echo "============================================"

# -------------------------------------------------------
# Check that admissions is running (FIFO must exist)
# -------------------------------------------------------
if [ ! -p "$TRIAGE_FIFO" ]; then
    echo "ERROR: Triage FIFO not found at $TRIAGE_FIFO" >&2
    echo "Is the hospital running?  Try: ./scripts/start_hospital.sh" >&2
    exit 1
fi

# -------------------------------------------------------
# Send patient record to admissions via named FIFO.
# Format: "name:age:severity\n"
# The admissions receptionist thread parses this format.
# -------------------------------------------------------
RECORD="${PATIENT_NAME}:${AGE}:${SEVERITY}"
echo "$RECORD" > "$TRIAGE_FIFO"

if [ $? -eq 0 ]; then
    echo "[OK] Patient record sent to admissions: $RECORD"
else
    echo "ERROR: Failed to write to triage FIFO." >&2
    exit 1
fi

exit 0
