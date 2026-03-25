#!/usr/bin/env bash
# run_p5_regression.sh — P-5.x Regression Benchmark
#
# Runs the same 3 scenarios as the P-4.3 stress test (commit aaf2667) so the
# P-5.x results can be compared directly against the historical baseline:
#
#   A: Clean Lab    — 10 bots | 0ms   | 0% loss
#   B: Real World   — 10 bots | 50ms  | 5% loss
#   C: Stress/Zerg  — 50 bots | 100ms | 2% loss
#
# The P-4.3 baseline numbers are embedded in this script for side-by-side
# comparison in the final results table.
#
# Usage:
#   bash scripts/run_p5_regression.sh               # full build + 3 scenarios
#   bash scripts/run_p5_regression.sh --skip-build  # reuse last build
#
# Requirements: WSL2/Linux, cmake, g++, libsfml-dev, iproute2 (tc)
# sudo is needed for tc netem — you will be prompted once at startup.

set -euo pipefail

# ── Paths ─────────────────────────────────────────────────────────────────────

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="/tmp/nm-p5-regression-build"
LOG_DIR="/tmp/nm-p5-regression-logs"
SERVER_BIN="$BUILD_DIR/Server/NetServer"
BOT_BIN="$BUILD_DIR/HeadlessBot/HeadlessBot"

# ── P-4.3 baseline (commit aaf2667, 2026-03-22) ───────────────────────────────
# Source: benchmarks/results/2026-03-22_21h17_aaf2667.md
# Phase at that commit: P-4.3 (no FOW, no Kalman, no LOD, no Lag Comp)

BL_TICK=("0.05ms"  "0.05ms"  "0.11ms")
BL_BUDG=("0.5%"    "0.5%"    "1.1%")
BL_OUT=( "1.1kbps" "1.0kbps" "4.9kbps")
BL_EFF=( "99%"     "99%"     "99%")
BL_CONN=("10/10"   "10/10"   "47/50")

# ── Args ──────────────────────────────────────────────────────────────────────

SKIP_BUILD=false
for arg in "$@"; do
    [[ "$arg" == "--skip-build" ]] && SKIP_BUILD=true
done

# ── Colors ────────────────────────────────────────────────────────────────────

CYAN='\033[0;36m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
RED='\033[0;31m'; BOLD='\033[1m'; NC='\033[0m'
step() { echo -e "${CYAN}[p5-reg]${NC} $*"; }
ok()   { echo -e "${GREEN}[  OK  ]${NC} $*"; }
warn() { echo -e "${YELLOW}[ WARN ]${NC} $*"; }

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

# ── Results (parallel arrays indexed 0-2) ────────────────────────────────────

R_LABEL=(); R_CLIENTS=(); R_LOST=(); R_NETEM=()
R_TICK=(); R_BUDGET=(); R_OUT=(); R_IN=(); R_RETRANS=(); R_EFF=()

# ── run_scenario ──────────────────────────────────────────────────────────────
# Args: <label> <bot_count> <delay_ms> <loss_pct> [<duration_s>]

run_scenario() {
    local label="$1"
    local bot_count="$2"
    local delay_ms="$3"
    local loss_pct="$4"
    local duration="${5:-40}"

    local safe_label
    safe_label=$(echo "$label" | tr ' :/' '___')
    local server_log="$LOG_DIR/server_${safe_label}.log"

    echo ""
    step "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    step "Scenario $label"
    step "  bots=${bot_count}  delay=${delay_ms}ms  loss=${loss_pct}%  duration=${duration}s"
    step "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

    # Apply tc netem
    sudo tc qdisc del dev lo root 2>/dev/null || true
    if [[ "$delay_ms" -gt 0 || "$loss_pct" -gt 0 ]]; then
        sudo tc qdisc add dev lo root netem \
            delay "${delay_ms}ms" \
            loss "${loss_pct}%"
        ok "tc netem: delay=${delay_ms}ms loss=${loss_pct}%"
    else
        ok "tc netem: clean (no degradation)"
    fi

    # Start server
    > "$server_log"
    SERVER_PID=""
    BOT_PIDS=()
    SERVER_PORT=7777 "$SERVER_BIN" >> "$server_log" 2>&1 &
    SERVER_PID=$!
    ok "Server PID=$SERVER_PID → $server_log"

    sleep 1.5  # let server bind before bots connect

    # Start bots
    for ((i = 0; i < bot_count; i++)); do
        SERVER_HOST=127.0.0.1 SERVER_PORT=7777 \
            "$BOT_BIN" >> "$LOG_DIR/bot_${safe_label}_${i}.log" 2>&1 &
        BOT_PIDS+=($!)
    done
    ok "$bot_count bots launched. Collecting for ${duration}s..."

    sleep "$duration"

    # Stop bots first (so profiler snapshot reflects stable client count)
    [[ ${#BOT_PIDS[@]} -gt 0 ]] && kill "${BOT_PIDS[@]}" 2>/dev/null || true
    BOT_PIDS=()

    sleep 1
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=""

    sudo tc qdisc del dev lo root 2>/dev/null || true

    # Parse last [PROFILER] line
    local last_line
    last_line=$(grep '\[PROFILER\]' "$server_log" 2>/dev/null | tail -1 || true)

    if [[ -z "$last_line" ]]; then
        warn "No [PROFILER] line found. Check $server_log"
        R_LABEL+=("$label"); R_CLIENTS+=("?"); R_LOST+=("?")
        R_NETEM+=("${delay_ms}ms/${loss_pct}%")
        R_TICK+=("N/A"); R_BUDGET+=("N/A")
        R_OUT+=("N/A"); R_IN+=("N/A"); R_RETRANS+=("N/A"); R_EFF+=("N/A")
        return
    fi

    ok "Last snapshot: $last_line"

    local clients avg_tick out_kbps in_kbps retries delta_eff
    clients=$(   echo "$last_line" | grep -oP 'Clients: \K[0-9]+')
    avg_tick=$(  echo "$last_line" | grep -oP 'Avg Tick: \K[0-9.]+')
    out_kbps=$(  echo "$last_line" | grep -oP 'Out: \K[0-9.]+kbps')
    in_kbps=$(   echo "$last_line" | grep -oP 'In: \K[0-9.]+kbps')
    retries=$(   echo "$last_line" | grep -oP 'Retries: \K[0-9]+')
    delta_eff=$( echo "$last_line" | grep -oP 'Delta Efficiency: \K[0-9]+%')

    local budget="?"
    if [[ -n "$avg_tick" ]]; then
        budget=$(awk "BEGIN { printf \"%.1f%%\", ($avg_tick / 10.0) * 100 }")
    fi

    local lost="?"
    if [[ -n "$clients" ]]; then
        lost=$(( bot_count - clients ))
    fi

    R_LABEL+=("$label")
    R_CLIENTS+=("${clients:-?}/${bot_count}")
    R_LOST+=("${lost:-?}")
    R_NETEM+=("${delay_ms}ms / ${loss_pct}%")
    R_TICK+=("${avg_tick:-?}ms")
    R_BUDGET+=("${budget}")
    R_OUT+=("${out_kbps:-?}")
    R_IN+=("${in_kbps:-?}")
    R_RETRANS+=("${retries:-?}")
    R_EFF+=("${delta_eff:-?}")
}

# ── Run the 3 benchmark scenarios (identical to P-4.3 run) ───────────────────

run_scenario "A: Clean Lab"    10   0  0  40
run_scenario "B: Real World"   10  50  5  40
run_scenario "C: Stress/Zerg"  50 100  2  40

# ── Helper: delta label (↓ / ↑ / =) ─────────────────────────────────────────
# Computes numeric delta between two kbps strings (e.g. "4.9kbps" vs "3.1kbps").
# Prints "↓ X.Xkbps" / "↑ X.Xkbps" / "=" for non-numeric values.

kbps_delta() {
    local old="$1" new="$2"
    local old_n new_n
    old_n=$(echo "$old" | grep -oP '[0-9.]+' || echo "")
    new_n=$(echo "$new" | grep -oP '[0-9.]+' || echo "")
    if [[ -z "$old_n" || -z "$new_n" ]]; then
        echo "?"
        return
    fi
    awk -v o="$old_n" -v n="$new_n" 'BEGIN {
        d = o - n
        if (d > 0.05)       printf "↓ %.1fkbps", d
        else if (d < -0.05) printf "↑ %.1fkbps", -d
        else                printf "="
    }'
}

# ── Print results table ───────────────────────────────────────────────────────

echo ""
echo -e "${BOLD}${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${BOLD}${CYAN}  P-5.x REGRESSION — NetworkMiddleware${NC}"
echo -e "${BOLD}${CYAN}  Baseline: P-4.3 (commit aaf2667, 2026-03-22) — no FOW / no Kalman / no LOD${NC}"
echo -e "${BOLD}${CYAN}  Current:  P-5.4 ($(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null)) — FOW + Kalman + Lag Comp + LOD${NC}"
echo -e "${BOLD}${CYAN}  $(uname -r) | $(date '+%Y-%m-%d %H:%M')${NC}"
echo -e "${BOLD}${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo ""
printf "${BOLD}%-22s  %-14s %-14s  %-12s %-12s  %-12s %-12s  %-14s %-14s${NC}\n" \
    "Scenario" "P-4.3 Tick" "P-5.x Tick" "P-4.3 Out" "P-5.x Out" "P-4.3 Eff" "P-5.x Eff" "Out Δ" "Conn"
echo "────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────"
for i in "${!R_LABEL[@]}"; do
    delta=$(kbps_delta "${BL_OUT[$i]}" "${R_OUT[$i]}")
    printf "%-22s  %-14s %-14s  %-12s %-12s  %-12s %-12s  %-14s %-14s\n" \
        "${R_LABEL[$i]}" \
        "${BL_TICK[$i]}" "${R_TICK[$i]}" \
        "${BL_OUT[$i]}"  "${R_OUT[$i]}" \
        "${BL_EFF[$i]}"  "${R_EFF[$i]}" \
        "$delta" \
        "${R_CLIENTS[$i]}"
done
echo "────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────"
echo ""
ok "Raw server logs: $LOG_DIR"

# ── Save results to benchmarks/results/ ──────────────────────────────────────

RESULTS_DIR="$REPO_ROOT/benchmarks/results"
mkdir -p "$RESULTS_DIR"

GIT_HASH=$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo "unknown")
RUN_TS=$(date '+%Y-%m-%d_%Hh%M')
RESULT_FILE="$RESULTS_DIR/p5_regression_${RUN_TS}_${GIT_HASH}.md"

{
    echo "---"
    echo "date: $(date '+%Y-%m-%d %H:%M')"
    echo "commit: $GIT_HASH"
    echo "branch: $(git -C "$REPO_ROOT" rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)"
    echo "platform: WSL2 $(uname -r)"
    echo "build: Release"
    echo "baseline: P-4.3 commit aaf2667 (2026-03-22)"
    echo "notes: Auto-generated by run_p5_regression.sh"
    echo "---"
    echo ""
    echo "# P-5.x Regression Benchmark — $RUN_TS — commit $GIT_HASH"
    echo ""
    echo "Comparing P-5.4 (FOW + Kalman + Lag Compensation + Network LOD) against the"
    echo "P-4.3 baseline (commit \`aaf2667\`, no FOW / no Kalman / no LOD)."
    echo ""
    echo "## Results"
    echo ""
    printf "| %-20s | %-10s | %-10s | %-10s | %-10s | %-10s | %-10s | %-12s | %-12s |\n" \
        "Scenario" "P-4.3 Tick" "P-5.x Tick" "P-4.3 Out" "P-5.x Out" "P-4.3 Eff" "P-5.x Eff" "Out Δ" "Connected"
    echo "|----------------------|------------|------------|------------|------------|------------|------------|--------------|--------------|"
    for i in "${!R_LABEL[@]}"; do
        delta=$(kbps_delta "${BL_OUT[$i]}" "${R_OUT[$i]}")
        printf "| %-20s | %-10s | %-10s | %-10s | %-10s | %-10s | %-10s | %-12s | %-12s |\n" \
            "${R_LABEL[$i]}" \
            "${BL_TICK[$i]}" "${R_TICK[$i]}" \
            "${BL_OUT[$i]}"  "${R_OUT[$i]}" \
            "${BL_EFF[$i]}"  "${R_EFF[$i]}" \
            "$delta" \
            "${R_CLIENTS[$i]}"
    done
    echo ""
    echo "## Notes"
    echo ""
    echo "- **P-4.3 baseline** (aaf2667): delta compression only — no game-loop snapshots."
    echo "  Bots had no active game state to serialize, so Out ≈ 1kbps and Delta Eff = 99%"
    echo "  reflected near-empty snapshot batches."
    echo "- **P-5.x current**: bots now receive real HeroState snapshots each tick."
    echo "  FOW (P-5.1) culls non-visible entities; LOD (P-5.4) gates tier 1/2 entities"
    echo "  to 50Hz / 20Hz respectively.  Out kbps reflects actual game traffic."
    echo "- **Tick budget**: P-5.x adds Phase 0b (PriorityEvaluator per observer, O(N²))"
    echo "  and the rewind-buffer RecordTick call.  Any increase here is the cost of those."
    echo "- **Delta Efficiency**: expected to drop from 99% as heroes now move and their"
    echo "  positions change each tick; FOW and LOD partially offset this."
    echo ""
    echo "## Profiler raw output"
    echo ""
    echo '```'
    for i in "${!R_LABEL[@]}"; do
        local safe
        safe=$(echo "${R_LABEL[$i]}" | tr ' :/' '___')
        local raw
        raw=$(grep '\[PROFILER\]' "$LOG_DIR/server_${safe}.log" 2>/dev/null | tail -1 || echo "(no data)")
        echo "${R_LABEL[$i]}: $raw"
    done
    echo '```'
} > "$RESULT_FILE"

ok "Results saved: $RESULT_FILE"
ok "Done."
