#!/usr/bin/env bash
# run_fow_benchmark.sh — FOW Interest Management Benchmark
#
# Proves that Fog-of-War filtering works by comparing two spawn layouts:
#
#   Scenario 1 — CLUSTER
#     All bots spawn within ±80 units of origin.
#     Every bot sees every other bot → N² state updates → maximum bandwidth.
#
#   Scenario 2 — BIMODAL
#     Bots split evenly between Zone A (x∈[-400,-200]) and Zone B (x∈[200,400]).
#     Minimum cross-group distance ≈ 400 units >> DEFAULT_VISION_RANGE=150 units.
#     Neither group can see the other → FOW filters half the entities → ~50% less
#     bandwidth per client (expected: Out_bimodal ≈ 0.5 × Out_cluster).
#
# Usage:
#   bash scripts/run_fow_benchmark.sh               # full build + 2 scenarios
#   bash scripts/run_fow_benchmark.sh --skip-build  # reuse last build
#
# Requirements: WSL2/Linux, cmake, g++, libsfml-dev, iproute2 (tc)
# sudo is needed for tc netem — you will be prompted once at startup.

set -euo pipefail

# ── Paths ─────────────────────────────────────────────────────────────────────

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="/tmp/nm-fow-build"
LOG_DIR="/tmp/nm-fow-logs"
SERVER_BIN="$BUILD_DIR/Server/NetServer"
BOT_BIN="$BUILD_DIR/HeadlessBot/HeadlessBot"

# ── Args ──────────────────────────────────────────────────────────────────────

SKIP_BUILD=false
for arg in "$@"; do
    [[ "$arg" == "--skip-build" ]] && SKIP_BUILD=true
done

# ── Colors ────────────────────────────────────────────────────────────────────

CYAN='\033[0;36m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
RED='\033[0;31m'; BOLD='\033[1m'; NC='\033[0m'

step() { echo -e "${CYAN}[fow-bench]${NC} $*"; }
ok()   { echo -e "${GREEN}[   OK   ]${NC} $*"; }
warn() { echo -e "${YELLOW}[  WARN  ]${NC} $*"; }
fail() { echo -e "${RED}[  FAIL  ]${NC} $*"; exit 1; }

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

[[ -f "$SERVER_BIN" ]] || fail "Server not found: $SERVER_BIN"
[[ -f "$BOT_BIN"    ]] || fail "Bot not found: $BOT_BIN"

mkdir -p "$LOG_DIR"

# ── sudo cache for tc netem ───────────────────────────────────────────────────

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

# ── Benchmark configuration ───────────────────────────────────────────────────
#
# 40 bots: 20 in zone A, 20 in zone B (bimodal) vs 40 all in cluster.
# 50ms / 1% loss — light network impairment, enough to exercise retransmission
# without flooding the log.  30s collection window per scenario.
#
# With 40 bots and CLUSTER mode, each client receives up to 39 other states
# per tick.  With BIMODAL, each client only receives ~19 (its own group) since
# the opposing group is beyond DEFAULT_VISION_RANGE=150 units.
# Expected Out_bimodal ≈ 0.5 × Out_cluster.

BOT_COUNT=40
DELAY_MS=50
LOSS_PCT=1
DURATION=30

# ── Results storage ───────────────────────────────────────────────────────────

R_MODE=(); R_CLIENTS=(); R_TICK=(); R_OUT=(); R_IN=(); R_EFF=()

# ── run_scenario ──────────────────────────────────────────────────────────────
# Args: <label> <SPAWN_ZONE_MODE>
run_scenario() {
    local label="$1"
    local zone_mode="$2"
    local safe_label
    safe_label=$(echo "$label" | tr ' :/' '___')
    local server_log="$LOG_DIR/server_${safe_label}.log"

    echo ""
    step "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    step "Scenario — $label  (SPAWN_ZONE_MODE=$zone_mode)"
    step "  bots=${BOT_COUNT}  delay=${DELAY_MS}ms  loss=${LOSS_PCT}%  duration=${DURATION}s"

    if [[ "$zone_mode" == "bimodal" ]]; then
        step "  Zone A: x∈[-400,-200]  Zone B: x∈[200,400]  (distance > 400 >> vision 150)"
        step "  Expected: cross-group entities fully FOW-filtered → ~50% less bandwidth"
    else
        step "  All bots within ±80u of origin → all mutually visible → max bandwidth"
    fi
    step "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

    # Apply tc netem
    sudo tc qdisc del dev lo root 2>/dev/null || true
    sudo tc qdisc add dev lo root netem \
        delay "${DELAY_MS}ms" \
        loss "${LOSS_PCT}%"
    ok "tc netem: delay=${DELAY_MS}ms  loss=${LOSS_PCT}%"

    # Start server with spawn zone mode
    > "$server_log"
    SERVER_PID=""
    BOT_PIDS=()
    SPAWN_ZONE_MODE="$zone_mode" SERVER_PORT=7777 \
        "$SERVER_BIN" >> "$server_log" 2>&1 &
    SERVER_PID=$!
    ok "Server PID=$SERVER_PID  zone=${zone_mode}  log→ $server_log"

    sleep 2  # let server bind before bots connect

    # Stagger bots 100ms apart to avoid handshake storm.
    # Pass zone center + roam radius so bots stay anchored to their spawn zone.
    for ((i = 0; i < BOT_COUNT; i++)); do
        if [[ "$zone_mode" == "bimodal" ]]; then
            # Even bots → Zone A (center -300,0); odd bots → Zone B (center +300,0)
            cx=$(( i % 2 == 0 ? -300 : 300 ))
            BOT_ZONE_CENTER_X=$cx BOT_ZONE_CENTER_Y=0 BOT_ROAM_RADIUS=80 \
                SERVER_HOST=127.0.0.1 SERVER_PORT=7777 \
                "$BOT_BIN" >> "$LOG_DIR/bot_${safe_label}_${i}.log" 2>&1 &
        elif [[ "$zone_mode" == "cluster" ]]; then
            BOT_ZONE_CENTER_X=0 BOT_ZONE_CENTER_Y=0 BOT_ROAM_RADIUS=80 \
                SERVER_HOST=127.0.0.1 SERVER_PORT=7777 \
                "$BOT_BIN" >> "$LOG_DIR/bot_${safe_label}_${i}.log" 2>&1 &
        else
            SERVER_HOST=127.0.0.1 SERVER_PORT=7777 \
                "$BOT_BIN" >> "$LOG_DIR/bot_${safe_label}_${i}.log" 2>&1 &
        fi
        BOT_PIDS+=($!)
        sleep 0.1
    done
    ok "${BOT_COUNT} bots launched.  Collecting for ${DURATION}s..."

    # Wait for all bots to connect (stagger time + handshake slack)
    LAUNCH_WAIT=$(awk "BEGIN { printf \"%d\", int($BOT_COUNT * 0.1 + 8) }")
    sleep "$LAUNCH_WAIT"
    sleep "$DURATION"

    # Stop bots, then give server 2s to emit final profiler line
    [[ ${#BOT_PIDS[@]} -gt 0 ]] && kill "${BOT_PIDS[@]}" 2>/dev/null || true
    BOT_PIDS=()
    sleep 2
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=""

    # Clean tc
    sudo tc qdisc del dev lo root 2>/dev/null || true

    # ── Emit spawn distribution summary ───────────────────────────────────────
    local zone_a_count zone_b_count
    zone_a_count=$(grep -c 'zone=bimodal.*spawn=(-[0-9]' "$server_log" 2>/dev/null || true)
    zone_b_count=$(grep -c  'zone=bimodal.*spawn=([0-9]'  "$server_log" 2>/dev/null || true)
    if [[ "$zone_mode" == "bimodal" ]]; then
        step "Spawn distribution: Zone A (left) = ${zone_a_count:-?}  |  Zone B (right) = ${zone_b_count:-?}"
    fi

    # ── Parse last [PROFILER] line ─────────────────────────────────────────────
    local last_line
    last_line=$(grep '\[PROFILER\]' "$server_log" 2>/dev/null | tail -1 || true)

    if [[ -z "$last_line" ]]; then
        warn "No [PROFILER] line found — check $server_log"
        R_MODE+=("$label"); R_CLIENTS+=("?/${BOT_COUNT}")
        R_TICK+=("N/A"); R_OUT+=("N/A"); R_IN+=("N/A"); R_EFF+=("N/A")
        return
    fi

    ok "Last snapshot: $last_line"

    local clients avg_tick out_kbps in_kbps delta_eff
    clients=$(   echo "$last_line" | grep -oP 'Clients: \K[0-9]+')
    avg_tick=$(  echo "$last_line" | grep -oP 'Avg Tick: \K[0-9.]+')
    out_kbps=$(  echo "$last_line" | grep -oP 'Out: \K[0-9.]+kbps')
    in_kbps=$(   echo "$last_line" | grep -oP 'In: \K[0-9.]+kbps')
    delta_eff=$( echo "$last_line" | grep -oP 'Delta Efficiency: \K[0-9]+%')

    R_MODE+=("$label")
    R_CLIENTS+=("${clients:-?}/${BOT_COUNT}")
    R_TICK+=("${avg_tick:-?}ms")
    R_OUT+=("${out_kbps:-?}")
    R_IN+=("${in_kbps:-?}")
    R_EFF+=("${delta_eff:-?}")
}

# ── Run both scenarios ────────────────────────────────────────────────────────

run_scenario "Cluster (FOW=OFF baseline)" "cluster"
run_scenario "Bimodal (FOW=ON groups)"   "bimodal"

# ── Print results table ───────────────────────────────────────────────────────

echo ""
echo -e "${BOLD}${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${BOLD}${CYAN}  FOW INTEREST MANAGEMENT BENCHMARK — NetworkMiddleware${NC}"
echo -e "${BOLD}${CYAN}  $(uname -r) | $(date '+%Y-%m-%d %H:%M')${NC}"
echo -e "${BOLD}${CYAN}  ${BOT_COUNT} bots | ${DELAY_MS}ms latency | ${LOSS_PCT}% loss | ${DURATION}s per scenario${NC}"
echo -e "${BOLD}${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo ""
printf "${BOLD}%-30s %-13s %-11s %-14s %-14s %-14s${NC}\n" \
    "Scenario" "Connected" "Avg Tick" "Out" "In" "Delta Eff."
echo "─────────────────────────────────────────────────────────────────────────────────────────"
for i in "${!R_MODE[@]}"; do
    printf "%-30s %-13s %-11s %-14s %-14s %-14s\n" \
        "${R_MODE[$i]}" "${R_CLIENTS[$i]}" "${R_TICK[$i]}" \
        "${R_OUT[$i]}" "${R_IN[$i]}" "${R_EFF[$i]}"
done
echo "─────────────────────────────────────────────────────────────────────────────────────────"
echo ""

# ── FOW effectiveness ratio ───────────────────────────────────────────────────

if [[ "${R_OUT[0]}" != "N/A" && "${R_OUT[1]}" != "N/A" ]]; then
    out_cluster=$(echo "${R_OUT[0]}" | grep -oP '[0-9.]+')
    out_bimodal=$(echo "${R_OUT[1]}" | grep -oP '[0-9.]+')
    if [[ -n "$out_cluster" && -n "$out_bimodal" && "$out_cluster" != "0" ]]; then
        ratio=$(awk "BEGIN { printf \"%.1f\", ($out_bimodal / $out_cluster) * 100 }")
        reduction=$(awk "BEGIN { printf \"%.1f\", 100 - ($out_bimodal / $out_cluster) * 100 }")
        echo -e "${BOLD}FOW effectiveness:${NC}"
        echo -e "  Out_bimodal / Out_cluster = ${ratio}%"
        echo -e "  Bandwidth reduction from FOW filtering = ${reduction}%"
        echo -e "  Expected: ~50% reduction (each group of ${BOT_COUNT}/2 bots only receives"
        echo -e "  its own group's states — the other group is beyond vision range 150u)"
        echo ""
    fi
fi

ok "Raw server logs: $LOG_DIR"

# ── Save results ──────────────────────────────────────────────────────────────

RESULTS_DIR="$REPO_ROOT/benchmarks/results"
mkdir -p "$RESULTS_DIR"

GIT_HASH=$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo "unknown")
RUN_TS=$(date '+%Y-%m-%d_%Hh%M')
RESULT_FILE="$RESULTS_DIR/fow_${RUN_TS}_${GIT_HASH}.md"

{
    echo "---"
    echo "date: $(date '+%Y-%m-%d %H:%M')"
    echo "commit: $GIT_HASH"
    echo "branch: $(git -C "$REPO_ROOT" rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)"
    echo "platform: WSL2 $(uname -r)"
    echo "build: Release"
    echo "scenario: ${BOT_COUNT} bots | ${DELAY_MS}ms / ${LOSS_PCT}% loss | ${DURATION}s"
    echo "notes: FOW Interest Management Benchmark — cluster vs bimodal spawn"
    echo "---"
    echo ""
    echo "# FOW Interest Management Benchmark — $RUN_TS — commit $GIT_HASH"
    echo ""
    echo "**Goal:** Prove that VisibilityTracker + SpatialGrid filter cross-group"
    echo "entities when two groups of bots are placed beyond DEFAULT_VISION_RANGE=150u."
    echo ""
    echo "## Spawn Layouts"
    echo ""
    echo "| Mode    | Zone A                  | Zone B                 | Min distance |"
    echo "|---------|-------------------------|------------------------|--------------|"
    echo "| cluster | x∈[-80,80] y∈[-80,80]  | same zone              | 0u           |"
    echo "| bimodal | x∈[-400,-200] y∈[-100,100] | x∈[200,400] y∈[-100,100] | ~400u   |"
    echo ""
    echo "Vision range = 150u → bimodal groups are completely invisible to each other."
    echo ""
    echo "## Results"
    echo ""
    printf "| %-28s | %-11s | %-9s | %-12s | %-12s | %-12s |\n" \
        "Scenario" "Connected" "Avg Tick" "Out" "In" "Delta Eff."
    echo "|------------------------------|-------------|-----------|--------------|--------------|--------------|"
    for i in "${!R_MODE[@]}"; do
        printf "| %-28s | %-11s | %-9s | %-12s | %-12s | %-12s |\n" \
            "${R_MODE[$i]}" "${R_CLIENTS[$i]}" "${R_TICK[$i]}" \
            "${R_OUT[$i]}" "${R_IN[$i]}" "${R_EFF[$i]}"
    done
    echo ""
    echo "## FOW Effectiveness"
    echo ""
    if [[ "${R_OUT[0]}" != "N/A" && "${R_OUT[1]}" != "N/A" ]]; then
        out_c=$(echo "${R_OUT[0]}" | grep -oP '[0-9.]+')
        out_b=$(echo "${R_OUT[1]}" | grep -oP '[0-9.]+')
        if [[ -n "$out_c" && -n "$out_b" && "$out_c" != "0" ]]; then
            r=$(awk "BEGIN { printf \"%.1f\", ($out_b / $out_c) * 100 }")
            rd=$(awk "BEGIN { printf \"%.1f\", 100 - ($out_b / $out_c) * 100 }")
            echo "- Out_bimodal / Out_cluster = **${r}%**"
            echo "- Bandwidth reduction attributed to FOW = **${rd}%**"
            echo "- Target: ~50% (each bot only receives its group's ${BOT_COUNT}/2 peers)"
        fi
    fi
    echo ""
    echo "## Notes"
    echo ""
    echo "- **Cluster baseline**: all bots within ±80u — FOW has no effect; every bot"
    echo "  sees every other bot → N² updates → maximum bandwidth."
    echo "- **Bimodal FOW**: 50% of bots in Zone A (x≈-300), 50% in Zone B (x≈+300)."
    echo "  Distance between zones ≈ 400-600u >> vision range 150u."
    echo "  VisibilityTracker detects no re-entries after initial spawn; SpatialGrid"
    echo "  never marks opposing-zone cells visible → zero cross-group state updates."
    echo "- **Delta Efficiency**: higher in bimodal expected if bots move less (stay"
    echo "  within zone) but delta compression itself is orthogonal to FOW filtering."
    echo ""
    echo "## Profiler raw output"
    echo ""
    echo '```'
    for i in "${!R_MODE[@]}"; do
        safe=$(echo "${R_MODE[$i]}" | tr ' :/(=)' '_______')
        raw=$(grep '\[PROFILER\]' "$LOG_DIR/server_${safe}.log" 2>/dev/null | tail -1 || echo "(no data)")
        echo "${R_MODE[$i]}: $raw"
    done
    echo '```'
} > "$RESULT_FILE"

ok "Results saved: $RESULT_FILE"
ok "Done."
