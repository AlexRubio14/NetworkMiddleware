#!/usr/bin/env bash
# run_final_benchmark.sh — P-4.5 Scalability Gauntlet
#
# Runs two back-to-back scenarios under maximum load (100 bots, 100ms / 5% loss):
#   1. Sequential baseline  — ./NetServer --sequential
#   2. Parallel (Split-Phase) — ./NetServer
#
# Both run for 60 seconds each.  Results are saved side-by-side to
#   benchmarks/results/final_<timestamp>_<hash>.md
#
# Usage:
#   bash scripts/run_final_benchmark.sh               # full build + 2 scenarios
#   bash scripts/run_final_benchmark.sh --skip-build  # reuse last build
#
# Requirements: WSL2/Linux, cmake, g++, libsfml-dev, iproute2 (tc)
# sudo is needed for tc netem — you will be prompted once at startup.

set -euo pipefail

# ── Paths ─────────────────────────────────────────────────────────────────────

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="/tmp/nm-final-build"
LOG_DIR="/tmp/nm-final-logs"
SERVER_BIN="$BUILD_DIR/Server/NetServer"
BOT_BIN="$BUILD_DIR/HeadlessBot/HeadlessBot"

# ── Args ──────────────────────────────────────────────────────────────────────

SKIP_BUILD=false
for arg in "$@"; do
    [[ "$arg" == "--skip-build" ]] && SKIP_BUILD=true
done

# ── Colors ────────────────────────────────────────────────────────────────────

CYAN='\033[0;36m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; BOLD='\033[1m'; NC='\033[0m'
step() { echo -e "${CYAN}[gauntlet]${NC} $*"; }
ok()   { echo -e "${GREEN}[   OK   ]${NC} $*"; }
warn() { echo -e "${YELLOW}[  WARN  ]${NC} $*"; }

# ── Build ─────────────────────────────────────────────────────────────────────

if ! $SKIP_BUILD; then
    step "Configuring CMake (Release, no tests)..."
    mkdir -p "$BUILD_DIR"
    cmake -S "$REPO_ROOT" -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTS=OFF \
        > "$BUILD_DIR/cmake.log" 2>&1

    step "Building NetServer + HeadlessBot ($(nproc) cores)..."
    cmake --build "$BUILD_DIR" \
        --target NetServer HeadlessBot \
        -- -j"$(nproc)" \
        > "$BUILD_DIR/build.log" 2>&1

    ok "Build complete."
else
    step "Skipping build (--skip-build)."
fi

[[ -f "$SERVER_BIN" ]] || { echo -e "${RED}[FAIL]${NC} Server not found: $SERVER_BIN"; exit 1; }
[[ -f "$BOT_BIN"    ]] || { echo -e "${RED}[FAIL]${NC} Bot not found: $BOT_BIN";    exit 1; }

mkdir -p "$LOG_DIR"

# ── sudo cache ────────────────────────────────────────────────────────────────

step "Requesting sudo credentials for tc netem (enter your password once):"
sudo -v

# ── Cleanup trap ──────────────────────────────────────────────────────────────

SERVER_PID=""
BOT_PIDS=()
cleanup() {
    [[ -n "$SERVER_PID" ]] && kill "$SERVER_PID" 2>/dev/null || true
    [[ ${#BOT_PIDS[@]} -gt 0 ]] && kill "${BOT_PIDS[@]}" 2>/dev/null || true
    sudo tc qdisc del dev lo root 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# ── Gauntlet configuration ────────────────────────────────────────────────────

BOT_COUNT=100
DELAY_MS=100
LOSS_PCT=5
DURATION=60

# ── Results storage (indexed 0=Sequential, 1=Parallel) ───────────────────────

R_MODE=(); R_CLIENTS=(); R_TICK=(); R_BUDGET=()
R_OUT=(); R_IN=(); R_CRCERR=(); R_EFF=()

# ── run_gauntlet_scenario ─────────────────────────────────────────────────────
# Args: <mode_label> <server_extra_flag>
#   mode_label:       "Sequential" or "Parallel"
#   server_extra_flag: "--sequential" or ""

run_gauntlet_scenario() {
    local mode_label="$1"
    local extra_flag="$2"
    local safe_label
    safe_label=$(echo "$mode_label" | tr ' :/' '___')
    local server_log="$LOG_DIR/server_${safe_label}.log"

    echo ""
    step "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    step "Gauntlet — $mode_label"
    step "  bots=${BOT_COUNT}  delay=${DELAY_MS}ms  loss=${LOSS_PCT}%  duration=${DURATION}s"
    [[ -n "$extra_flag" ]] && step "  server flag: $extra_flag"
    step "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

    # Apply tc netem
    sudo tc qdisc del dev lo root 2>/dev/null || true
    sudo tc qdisc add dev lo root netem \
        delay "${DELAY_MS}ms" \
        loss "${LOSS_PCT}%"
    ok "tc netem: delay=${DELAY_MS}ms loss=${LOSS_PCT}%"

    # Start server
    > "$server_log"
    SERVER_PID=""
    BOT_PIDS=()
    if [[ -n "$extra_flag" ]]; then
        SERVER_PORT=7777 "$SERVER_BIN" "$extra_flag" >> "$server_log" 2>&1 &
    else
        SERVER_PORT=7777 "$SERVER_BIN" >> "$server_log" 2>&1 &
    fi
    SERVER_PID=$!
    ok "Server PID=$SERVER_PID → $server_log"

    sleep 2  # let server bind before bots connect

    # Start bots
    for ((i = 0; i < BOT_COUNT; i++)); do
        SERVER_HOST=127.0.0.1 SERVER_PORT=7777 \
            "$BOT_BIN" >> "$LOG_DIR/bot_${safe_label}_${i}.log" 2>&1 &
        BOT_PIDS+=($!)
    done
    ok "${BOT_COUNT} bots launched. Collecting for ${DURATION}s..."

    sleep "$DURATION"

    # Stop bots first (so profiler snapshot reflects stable client count)
    [[ ${#BOT_PIDS[@]} -gt 0 ]] && kill "${BOT_PIDS[@]}" 2>/dev/null || true
    BOT_PIDS=()

    # Give server 2s to emit its final profiler report, then stop it
    sleep 2
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=""

    # Clean tc
    sudo tc qdisc del dev lo root 2>/dev/null || true

    # Parse last [PROFILER] line
    local last_line
    last_line=$(grep '\[PROFILER\]' "$server_log" 2>/dev/null | tail -1 || true)

    if [[ -z "$last_line" ]]; then
        warn "No [PROFILER] line found. Check $server_log"
        R_MODE+=("$mode_label")
        R_CLIENTS+=("?/${BOT_COUNT}"); R_TICK+=("N/A"); R_BUDGET+=("N/A")
        R_OUT+=("N/A"); R_IN+=("N/A"); R_CRCERR+=("N/A"); R_EFF+=("N/A")
        return
    fi

    ok "Last snapshot: $last_line"

    local clients avg_tick out_kbps in_kbps crc_err delta_eff
    clients=$(   echo "$last_line" | grep -oP 'Clients: \K[0-9]+')
    avg_tick=$(  echo "$last_line" | grep -oP 'Avg Tick: \K[0-9.]+')
    out_kbps=$(  echo "$last_line" | grep -oP 'Out: \K[0-9.]+kbps')
    in_kbps=$(   echo "$last_line" | grep -oP 'In: \K[0-9.]+kbps')
    crc_err=$(   echo "$last_line" | grep -oP 'CRC Err: \K[0-9]+')
    delta_eff=$( echo "$last_line" | grep -oP 'Delta Efficiency: \K[0-9]+%')

    # Tick budget % = avg_tick_ms / 10ms × 100
    local budget="?"
    if [[ -n "$avg_tick" ]]; then
        budget=$(awk "BEGIN { printf \"%.1f%%\", ($avg_tick / 10.0) * 100 }")
    fi

    R_MODE+=("$mode_label")
    R_CLIENTS+=("${clients:-?}/${BOT_COUNT}")
    R_TICK+=("${avg_tick:-?}ms")
    R_BUDGET+=("${budget}")
    R_OUT+=("${out_kbps:-?}")
    R_IN+=("${in_kbps:-?}")
    R_CRCERR+=("${crc_err:-?}")
    R_EFF+=("${delta_eff:-?}")
}

# ── Run both scenarios ────────────────────────────────────────────────────────

run_gauntlet_scenario "Sequential" "--sequential"
run_gauntlet_scenario "Parallel"   ""

# ── Print results table ───────────────────────────────────────────────────────

echo ""
echo -e "${BOLD}${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${BOLD}${CYAN}  SCALABILITY GAUNTLET — NetworkMiddleware P-4.5${NC}"
echo -e "${BOLD}${CYAN}  $(uname -r) | $(date '+%Y-%m-%d %H:%M')${NC}"
echo -e "${BOLD}${CYAN}  Scenario: ${BOT_COUNT} bots | ${DELAY_MS}ms latency | ${LOSS_PCT}% loss | ${DURATION}s${NC}"
echo -e "${BOLD}${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo ""
printf "${BOLD}%-14s %-14s %-11s %-10s %-14s %-14s %-10s %-14s${NC}\n" \
    "Mode" "Connected" "Avg Tick" "Budget%" "Out" "In" "CRC Err" "Delta Eff."
echo "─────────────────────────────────────────────────────────────────────────────────────────────────────────"
for i in "${!R_MODE[@]}"; do
    printf "%-14s %-14s %-11s %-10s %-14s %-14s %-10s %-14s\n" \
        "${R_MODE[$i]}" "${R_CLIENTS[$i]}" "${R_TICK[$i]}" "${R_BUDGET[$i]}" \
        "${R_OUT[$i]}" "${R_IN[$i]}" "${R_CRCERR[$i]}" "${R_EFF[$i]}"
done
echo "─────────────────────────────────────────────────────────────────────────────────────────────────────────"
echo ""
ok "Raw server logs: $LOG_DIR"

# ── Save results to benchmarks/results/ ──────────────────────────────────────

RESULTS_DIR="$REPO_ROOT/benchmarks/results"
mkdir -p "$RESULTS_DIR"

GIT_HASH=$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo "unknown")
RUN_TS=$(date '+%Y-%m-%d_%Hh%M')
RESULT_FILE="$RESULTS_DIR/final_${RUN_TS}_${GIT_HASH}.md"

{
    echo "---"
    echo "date: $(date '+%Y-%m-%d %H:%M')"
    echo "commit: $GIT_HASH"
    echo "branch: $(git -C "$REPO_ROOT" rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)"
    echo "platform: WSL2 $(uname -r)"
    echo "build: Release"
    echo "scenario: ${BOT_COUNT} bots | ${DELAY_MS}ms / ${LOSS_PCT}% loss | ${DURATION}s"
    echo "notes: Auto-generated by run_final_benchmark.sh (P-4.5 Scalability Gauntlet)"
    echo "---"
    echo ""
    echo "# P-4.5 Scalability Gauntlet — $RUN_TS — commit $GIT_HASH"
    echo ""
    echo "**Scenario:** ${BOT_COUNT} bots | ${DELAY_MS}ms latency | ${LOSS_PCT}% packet loss | ${DURATION}s per mode"
    echo ""
    echo "## Sequential vs Parallel Comparison"
    echo ""
    printf "| %-12s | %-13s | %-9s | %-8s | %-12s | %-12s | %-9s | %-12s |\n" \
        "Mode" "Connected" "Avg Tick" "Budget%" "Out" "In" "CRC Err" "Delta Eff."
    echo "|--------------|---------------|-----------|---------|--------------|--------------|-----------|--------------|"
    for i in "${!R_MODE[@]}"; do
        printf "| %-12s | %-13s | %-9s | %-8s | %-12s | %-12s | %-9s | %-12s |\n" \
            "${R_MODE[$i]}" "${R_CLIENTS[$i]}" "${R_TICK[$i]}" "${R_BUDGET[$i]}" \
            "${R_OUT[$i]}" "${R_IN[$i]}" "${R_CRCERR[$i]}" "${R_EFF[$i]}"
    done
    echo ""
    echo "## Notes"
    echo ""
    echo "- **Sequential**: \`SendSnapshot()\` called on main thread per client (P-4.3-style baseline)"
    echo "- **Parallel**: Split-Phase pipeline — Phase A serializes concurrently via Job System workers,"
    echo "  Phase B commits + sends on main thread (P-4.4)"
    echo "- **CRC Err**: packets discarded due to CRC32 mismatch (P-4.5); should be ~0 on loopback"
    echo "- **Budget%**: Avg Tick / 10ms × 100 — target < 10% under max load"
    echo ""
    echo "## Profiler raw output"
    echo ""
    echo '```'
    for i in "${!R_MODE[@]}"; do
        local safe
        safe=$(echo "${R_MODE[$i]}" | tr ' :/' '___')
        local raw
        raw=$(grep '\[PROFILER\]' "$LOG_DIR/server_${safe}.log" 2>/dev/null | tail -1 || echo "(no data)")
        echo "${R_MODE[$i]}: $raw"
    done
    echo '```'
} > "$RESULT_FILE"

ok "Results saved: $RESULT_FILE"
ok "Done."
