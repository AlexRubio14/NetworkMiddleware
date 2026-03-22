#!/usr/bin/env bash
# run_stress_test.sh — Automated P-4.3 stress benchmark for WSL2/Linux
#
# Compiles natively, runs 3 scenarios with tc netem, parses profiler output
# and prints a results table at the end.
#
# Usage:
#   bash scripts/run_stress_test.sh               # full build + 3 scenarios
#   bash scripts/run_stress_test.sh --skip-build  # reuse last build
#
# Requirements: WSL2/Linux, cmake, g++, libsfml-dev, iproute2 (tc)
# sudo is needed for tc netem — you will be prompted once at startup.

set -euo pipefail

# ── Paths ─────────────────────────────────────────────────────────────────────

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="/tmp/nm-stress-build"
LOG_DIR="/tmp/nm-stress-logs"
SERVER_BIN="$BUILD_DIR/Server/NetServer"
BOT_BIN="$BUILD_DIR/HeadlessBot/HeadlessBot"

# ── Args ──────────────────────────────────────────────────────────────────────

SKIP_BUILD=false
for arg in "$@"; do
    [[ "$arg" == "--skip-build" ]] && SKIP_BUILD=true
done

# ── Colors ────────────────────────────────────────────────────────────────────

CYAN='\033[0;36m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; BOLD='\033[1m'; NC='\033[0m'
step() { echo -e "${CYAN}[stress]${NC} $*"; }
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

R_LABEL=(); R_CLIENTS=(); R_NETEM=(); R_TICK=(); R_OUT=(); R_RETRANS=(); R_EFF=()

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

    # Wait for benchmark duration
    sleep "$duration"

    # Stop bots first (so profiler snapshot reflects stable client count)
    [[ ${#BOT_PIDS[@]} -gt 0 ]] && kill "${BOT_PIDS[@]}" 2>/dev/null || true
    BOT_PIDS=()

    # Give server 1s to emit its final profiler report, then stop it
    sleep 1
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
        R_LABEL+=("$label"); R_CLIENTS+=("?")
        R_NETEM+=("${delay_ms}ms/${loss_pct}%")
        R_TICK+=("N/A"); R_OUT+=("N/A"); R_RETRANS+=("N/A"); R_EFF+=("N/A")
        return
    fi

    ok "Last snapshot: $last_line"

    local clients avg_tick out retries delta_eff
    clients=$(   echo "$last_line" | grep -oP 'Clients: \K[0-9]+')
    avg_tick=$(  echo "$last_line" | grep -oP 'Avg Tick: \K[0-9.]+ms')
    out=$(       echo "$last_line" | grep -oP 'Out: \K[0-9.]+kbps')
    retries=$(   echo "$last_line" | grep -oP 'Retries: \K[0-9]+')
    delta_eff=$( echo "$last_line" | grep -oP 'Delta Efficiency: \K[0-9]+%')

    R_LABEL+=("$label")
    R_CLIENTS+=("${clients:-?}")
    R_NETEM+=("${delay_ms}ms / ${loss_pct}%")
    R_TICK+=("${avg_tick:-?}")
    R_OUT+=("${out:-?}")
    R_RETRANS+=("${retries:-?}")
    R_EFF+=("${delta_eff:-?}")
}

# ── Run the 3 benchmark scenarios ─────────────────────────────────────────────

run_scenario "A: Clean Lab"    10   0  0  40
run_scenario "B: Real World"   10  50  5  40
run_scenario "C: Stress/Zerg"  50 100  2  40

# ── Print results table ───────────────────────────────────────────────────────

echo ""
echo -e "${BOLD}${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${BOLD}${CYAN}  BENCHMARK RESULTS — NetworkMiddleware P-4.3${NC}"
echo -e "${BOLD}${CYAN}  $(uname -r) | $(date '+%Y-%m-%d %H:%M')${NC}"
echo -e "${BOLD}${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo ""
printf "${BOLD}%-22s %-9s %-16s %-11s %-12s %-11s %-18s${NC}\n" \
    "Scenario" "Clients" "tc netem" "Avg Tick" "Out (kbps)" "Retrans." "Delta Efficiency"
echo "──────────────────────────────────────────────────────────────────────────────────────"
for i in "${!R_LABEL[@]}"; do
    printf "%-22s %-9s %-16s %-11s %-12s %-11s %-18s\n" \
        "${R_LABEL[$i]}" "${R_CLIENTS[$i]}" "${R_NETEM[$i]}" \
        "${R_TICK[$i]}" "${R_OUT[$i]}" "${R_RETRANS[$i]}" "${R_EFF[$i]}"
done
echo "──────────────────────────────────────────────────────────────────────────────────────"
echo ""
ok "Raw server logs: $LOG_DIR"
ok "Done."
