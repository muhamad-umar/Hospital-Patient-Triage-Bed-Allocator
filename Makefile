# ============================================================
# Project : Hospital Patient Triage & Bed Allocator
# File    : Makefile
# Purpose : Build system for all C source files.
#           Targets: all, clean, run, test
# Compile : make all
# ============================================================

CC      = gcc
CFLAGS  = -Wall -Wextra -pthread -g -I./src
LDFLAGS = -lpthread -lm

SRC_DIR = src
BIN_DIR = bin
LOG_DIR = logs

# Source files
ADMISSIONS_SRCS = \
    $(SRC_DIR)/admissions.c        \
    $(SRC_DIR)/ipc.c               \
    $(SRC_DIR)/queue.c             \
    $(SRC_DIR)/scheduler.c         \
    $(SRC_DIR)/memory_allocator.c

SIMULATOR_SRCS = \
    $(SRC_DIR)/patient_simulator.c \
    $(SRC_DIR)/ipc.c

# Output binaries
ADMISSIONS_BIN = $(BIN_DIR)/admissions
SIMULATOR_BIN  = $(BIN_DIR)/patient_simulator

# Default target: build everything
.PHONY: all
all: dirs $(ADMISSIONS_BIN) $(SIMULATOR_BIN) scripts
	@echo ""
	@echo "=== Build successful ==="
	@echo "  Binaries : $(ADMISSIONS_BIN)  $(SIMULATOR_BIN)"
	@echo "  Run with : make run"
	@echo ""

# Create output directories
.PHONY: dirs
dirs:
	@mkdir -p $(BIN_DIR) $(LOG_DIR)

# Build admissions manager
$(ADMISSIONS_BIN): $(ADMISSIONS_SRCS) \
    $(SRC_DIR)/common.h \
    $(SRC_DIR)/ipc.h    \
    $(SRC_DIR)/queue.h  \
    $(SRC_DIR)/scheduler.h \
    $(SRC_DIR)/memory_allocator.h
	$(CC) $(CFLAGS) -o $@ $(ADMISSIONS_SRCS) $(LDFLAGS)
	@echo "[CC] $@"

# Build patient simulator
$(SIMULATOR_BIN): $(SIMULATOR_SRCS) \
    $(SRC_DIR)/common.h \
    $(SRC_DIR)/ipc.h
	$(CC) $(CFLAGS) -o $@ $(SIMULATOR_SRCS) $(LDFLAGS)
	@echo "[CC] $@"

# Make shell scripts executable
.PHONY: scripts
scripts:
	@chmod +x scripts/triage.sh
	@chmod +x scripts/start_hospital.sh
	@chmod +x scripts/stop_hospital.sh
	@chmod +x scripts/stress_test.sh
	@echo "[OK] Shell scripts marked executable"

# -------------------------------------------------------
# run — start the hospital simulation
# -------------------------------------------------------
.PHONY: run
run: all
	@echo "Starting hospital simulation (Best-Fit strategy)..."
	@./scripts/start_hospital.sh --strategy best

# -------------------------------------------------------
# test — run a quick functional smoke test
# -------------------------------------------------------
.PHONY: test
test: all
	@echo "=== Functional Smoke Test ==="
	@./scripts/start_hospital.sh --strategy best
	@sleep 2
	@echo "Admitting 5 test patients..."
	@./scripts/triage.sh "Critical Patient"  30 10
	@sleep 0.2
	@./scripts/triage.sh "Urgent Patient"    45 6
	@sleep 0.2
	@./scripts/triage.sh "General Patient"   25 2
	@sleep 0.2
	@./scripts/triage.sh "ICU Patient"       60 9
	@sleep 0.2
	@./scripts/triage.sh "Isolation Patient" 50 5
	@echo "Waiting 5 seconds for processing..."
	@sleep 5
	@echo "Stopping hospital..."
	@./scripts/stop_hospital.sh
	@echo ""
	@echo "=== Test complete ==="
	@echo "Check logs/ for output files."

# -------------------------------------------------------
# stress — stress test with 20 concurrent patients
# -------------------------------------------------------
.PHONY: stress
stress: all
	@./scripts/start_hospital.sh --strategy best
	@sleep 2
	@./scripts/stress_test.sh
	@sleep 20
	@./scripts/stop_hospital.sh

# -------------------------------------------------------
# valgrind — memory leak check
# -------------------------------------------------------
.PHONY: valgrind
valgrind: all
	@echo "Running valgrind on admissions (5 seconds then SIGTERM)..."
	@valgrind --leak-check=full --show-leak-kinds=all \
	    --track-origins=yes --error-exitcode=1 \
	    ./$(ADMISSIONS_BIN) --strategy best &
	@VPID=$$!; sleep 5; kill -TERM $$VPID; wait $$VPID

# -------------------------------------------------------
# clean — remove binaries and logs
# -------------------------------------------------------
.PHONY: clean
clean:
	@rm -f $(BIN_DIR)/admissions $(BIN_DIR)/patient_simulator
	@rm -f $(LOG_DIR)/*.log $(LOG_DIR)/*.txt $(LOG_DIR)/*.dat
	@rm -f /tmp/triage_fifo /tmp/discharge_fifo
	@echo "[clean] Binaries and logs removed"

# -------------------------------------------------------
# distclean — also remove bin/ and logs/
# -------------------------------------------------------
.PHONY: distclean
distclean: clean
	@rm -rf $(BIN_DIR) $(LOG_DIR)
	@echo "[distclean] Directories removed"
