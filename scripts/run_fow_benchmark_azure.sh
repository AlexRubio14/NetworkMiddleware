#!/usr/bin/env bash
# run_fow_benchmark_azure.sh — FOW Interest Management Benchmark (Azure)
#
# Proves that FOW filtering works by comparing two spawn layouts on the live
# Azure VM.  The bots run locally (WSL2) and connect over real WAN — no tc netem.
#
#   Scenario 1 — CLUSTER
#     All bots spawn within ±80 units of origin.  Every bot sees every other
#     bot → N² state updates → maximum bandwidth baseline.
#
#   Scenario 2 — BIMODAL
#     Bots alternate between Zone A (x∈[-400,-200]) and Zone B (x∈[200,400]).
#     Minimum cross-group distance ≈ 400 units >> DEFAULT_VISION_RANGE=150 units.
#     Neither group sees the other → FOW filters ~half the entities per client
#     → expected Out_bimodal ≈ 0.5 × Out_cluster.
#
# How it works:
#   - SSHes into the Azure VM.
#   - For each scenario: stops the running 'netserver' container, starts a
#     temporary container with SPAWN_ZONE_MODE=<mode>, runs bots, collects
#     profiler data via docker logs, stops temp container.
#   - Restores the original 'netserver' container at the end.
#
# Usage:
#   bash scripts/run_fow_benchmark_azure.sh               # 20 bots, 60s
#   bash scripts/run_fow_benchmark_azure.sh --bots 40     # 40 bots
#   bash scripts/run_fow_benchmark_azure.sh --skip-build  # reuse last build
#
# Env vars (override defaults):
#   AZURE_VM_IP    — public IP    (default: 20.208.47.230)
#   AZURE_VM_USER  — SSH username (default: alexrp14)
#   AZURE_SSH_KEY  — private key  (default: ~/.ssh/azure_key)
#   BOT_BIN        — path to HeadlessBot binary (auto-detected)
#   SERVER_IMAGE   — Docker image (default: ghcr.io/alexrubio14/netserver:latest)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# ── Config ────────────────────────────────────────────────────────────────────

VM_IP="${AZURE_VM_IP:-20.208.47.230}"
VM_USER="${AZURE_VM_USER:-alexrp14}"
SSH_KEY="${AZURE_SSH_KEY:-$HOME/.ssh/azure_key}"
SERVER_IMAGE="${SERVER_IMAGE:-ghcr.io/alexrubio14/netserver:latest}"
BOT_COUNT=20
DURATION=60
SKIP_BUILD=false

# ── Parse args ────────────────────────────────────────────────────────────────

while [[ $# -gt 0 ]]; do
    case "$1" in
        --bots)       BOT_COUNT="$2";  shift 2 ;;
        --duration)   DURATION="$2";   shift 2 ;;
        --skip-build) SKIP_BUILD=true; shift ;;
        *) echo "Unknown arg: $1"; exit 1 ;;
    esac
done

# ── Auto-detect HeadlessBot binary ───────────────────────────────────────────

if [[ -n "${BOT_BIN:-}" && -f "$BOT_BIN" ]]; then
    :
elif [[ -f "$REPO_ROOT/cmake-build-wsl/HeadlessBot/HeadlessBot" ]]; then
    BOT_BIN="$REPO_ROOT/cmake-build-wsl/HeadlessBot/HeadlessBot"
elif [[ -f "$REPO_ROOT/cmake-build-debug/HeadlessBot/HeadlessBot" ]]; then
    BOT_BIN="$REPO_ROOT/cmake-build-debug/HeadlessBot/HeadlessBot"
elif [[ -f "/tmp/nm-fow-build/HeadlessBot/HeadlessBot" ]]; then
    BOT_BIN="/tmp/nm-fow-build/HeadlessBot/HeadlessBot"
else
    BUILD_BOT=true
fi

# ── Build bots if needed ──────────────────────────────────────────────────────

if [[ "${BUILD_BOT:-false}" == true ]] || ! $SKIP_BUILD && [[ -z "${BOT_BIN:-}" ]]; then
    BUILD_DIR="/tmp/nm-fow-build"
    echo "[fow-azure] Building HeadlessBot (Release)..."
    mkdir -p "$BUILD_DIR"
    cmake -S "$REPO_ROOT" -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTS=OFF \
        > "$BUILD_DIR/cmake.log" 2>&1
    cmake --build "$BUILD_DIR" \
        --target HeadlessBot \
        -- -j"$(nproc)" \
        >> "$BUILD_DIR/build.log" 2>&1
    BOT_BIN="$BUILD_DIR/HeadlessBot/HeadlessBot"
fi

[[ -f "${BOT_BIN:-}" ]] || { echo "[FAIL] HeadlessBot not found. Build first or set BOT_BIN=..."; exit 1; }

# ── SSH ControlMaster ─────────────────────────────────────────────────────────

SSH_CTL="/tmp/azure_fow_bench_ctl_$$"
SSH_OPTS="-i $SSH_KEY -o StrictHostKeyChecking=no -o ConnectTimeout=10 -o BatchMode=yes \
    -o ControlMaster=auto -o ControlPath=$SSH_CTL -o ControlPersist=300"

ssh_cmd() { ssh $SSH_OPTS "${VM_USER}@${VM_IP}" "$@"; }

# ── Colors ────────────────────────────────────────────────────────────────────

CYAN='\033[0;36m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
RED='\033[0;31m'; BOLD='\033[1m'; NC='\033[0m'

step() { echo -e "${CYAN}[fow-azure]${NC} $*"; }
ok()   { echo -e "${GREEN}[   OK   ]${NC} $*"; }
warn() { echo -e "${YELLOW}[  WARN  ]${NC} $*"; }
fail() { echo -e "${RED}[  FAIL  ]${NC} $*"; exit 1; }

# ── Cleanup trap ──────────────────────────────────────────────────────────────

BOT_PIDS=()
PROFILER_LOG=""
SSH_TAIL_PID=""
BENCH_CONTAINER=""

cleanup() {
    [[ -n "$SSH_TAIL_PID" ]] && kill "$SSH_TAIL_PID" 2>/dev/null || true
    [[ ${#BOT_PIDS[@]} -gt 0 ]] && kill "${BOT_PIDS[@]}" 2>/dev/null || true
    # Stop and remove any leftover bench container
    if [[ -n "$BENCH_CONTAINER" ]]; then
        ssh_cmd "docker stop $BENCH_CONTAINER 2>/dev/null; docker rm $BENCH_CONTAINER 2>/dev/null" || true
    fi
    # Restore original netserver
    ssh_cmd "docker start netserver 2>/dev/null" || true
    [[ -n "$PROFILER_LOG" ]] && rm -f "$PROFILER_LOG" || true
    ssh -O stop -o ControlPath="$SSH_CTL" "${VM_USER}@${VM_IP}" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# ── Header ────────────────────────────────────────────────────────────────────

echo ""
echo -e "${BOLD}${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${BOLD}${CYAN}  FOW Interest Management Benchmark — Azure (CLUSTER vs BIMODAL)${NC}"
echo -e "${BOLD}${CYAN}  Target: ${VM_IP}:7777 | Bots: ${BOT_COUNT} | Duration: ${DURATION}s${NC}"
echo -e "${BOLD}${CYAN}  Image: ${SERVER_IMAGE}${NC}"
echo -e "${BOLD}${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo ""

# ── Verify SSH + server reachable ─────────────────────────────────────────────

step "Opening SSH connection to ${VM_IP}..."
ssh_cmd "echo OK" > /dev/null || fail "SSH connection failed"
ok "SSH connected."

# Kill any stale local bots
step "Clearing any leftover local HeadlessBot processes..."
pkill -x HeadlessBot 2>/dev/null || true
ok "Done."

# ── Results storage ───────────────────────────────────────────────────────────

R_MODE=(); R_CLIENTS=(); R_TICK=(); R_FULL=(); R_OUT=(); R_IN=(); R_EFF=()

# ── run_scenario ──────────────────────────────────────────────────────────────
# Args: <label> <SPAWN_ZONE_MODE>
run_scenario() {
    local label="$1"
    local zone_mode="$2"
    local safe_label
    safe_label=$(echo "$label" | tr ' :/(=)' '_______')
    BENCH_CONTAINER="netserver-fow-${zone_mode}-$$"

    echo ""
    step "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    step "Scenario — $label  (SPAWN_ZONE_MODE=$zone_mode)"
    step "  bots=${BOT_COUNT}  duration=${DURATION}s  WAN real latency"

    if [[ "$zone_mode" == "bimodal" ]]; then
        step "  Zone A: x∈[-400,-200]  Zone B: x∈[200,400]  (distance > 400 >> vision 150)"
        step "  Expected: cross-group entities fully FOW-filtered → ~50% less bandwidth"
    else
        step "  All bots within ±80u of origin → all mutually visible → max bandwidth baseline"
    fi
    step "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

    # Stop the running netserver (frees port 7777)
    step "Stopping existing netserver container..."
    ssh_cmd "docker stop netserver 2>/dev/null || true"
    ok "netserver stopped."

    # Start benchmark container with SPAWN_ZONE_MODE
    step "Starting benchmark container (${BENCH_CONTAINER})..."
    ssh_cmd "docker run -d --name ${BENCH_CONTAINER} \
        -p 7777:7777/udp \
        -e SERVER_PORT=7777 \
        -e SPAWN_ZONE_MODE=${zone_mode} \
        ${SERVER_IMAGE}"
    ok "Container ${BENCH_CONTAINER} started (SPAWN_ZONE_MODE=${zone_mode})."

    sleep 3  # let server bind

    # Start profiler stream
    PROFILER_LOG=$(mktemp)
    ssh -i "$SSH_KEY" -o StrictHostKeyChecking=no -o BatchMode=yes \
        "${VM_USER}@${VM_IP}" \
        "docker logs -f --tail 0 ${BENCH_CONTAINER} 2>&1 | grep --line-buffered '\[PROFILER\]'" \
        > "$PROFILER_LOG" &
    SSH_TAIL_PID=$!

    # Launch bots — staggered 300ms to avoid Azure NSG burst-drop.
    # Pass zone center + roam radius so bots stay anchored to their spawn zone.
    BOT_PIDS=()
    step "Launching ${BOT_COUNT} bots → ${VM_IP}:7777"
    for ((i = 0; i < BOT_COUNT; i++)); do
        if [[ "$zone_mode" == "bimodal" ]]; then
            # Even bots → Zone A (center -300,0); odd bots → Zone B (center +300,0)
            cx=$(( i % 2 == 0 ? -300 : 300 ))
            BOT_ZONE_CENTER_X=$cx BOT_ZONE_CENTER_Y=0 BOT_ROAM_RADIUS=80 \
                SERVER_HOST="$VM_IP" SERVER_PORT=7777 "$BOT_BIN" \
                > /dev/null 2>&1 &
        elif [[ "$zone_mode" == "cluster" ]]; then
            BOT_ZONE_CENTER_X=0 BOT_ZONE_CENTER_Y=0 BOT_ROAM_RADIUS=80 \
                SERVER_HOST="$VM_IP" SERVER_PORT=7777 "$BOT_BIN" \
                > /dev/null 2>&1 &
        else
            SERVER_HOST="$VM_IP" SERVER_PORT=7777 "$BOT_BIN" \
                > /dev/null 2>&1 &
        fi
        BOT_PIDS+=($!)
        sleep 0.3
    done

    # Wait for all bots to connect
    LAUNCH_WAIT=$(awk "BEGIN { printf \"%d\", int($BOT_COUNT * 0.3 + 8) }")
    step "Waiting ${LAUNCH_WAIT}s for bots to connect..."
    sleep "$LAUNCH_WAIT"

    # Collect profiler samples
    step "Collecting profiler data for ${DURATION}s..."
    ELAPSED=0
    SAMPLE_INTERVAL=10
    LAST_SAMPLE=""

    while [[ $ELAPSED -lt $DURATION ]]; do
        SLEEP_TIME=$((SAMPLE_INTERVAL < (DURATION - ELAPSED) ? SAMPLE_INTERVAL : (DURATION - ELAPSED)))
        sleep "$SLEEP_TIME"
        ELAPSED=$((ELAPSED + SLEEP_TIME))

        SAMPLE=$(tail -1 "$PROFILER_LOG" 2>/dev/null || true)
        if [[ -n "$SAMPLE" ]]; then
            LAST_SAMPLE="$SAMPLE"
            CL=$(echo "$SAMPLE" | grep -oP 'Clients: \K[0-9]+' || echo "?")
            TK=$(echo "$SAMPLE" | grep -oP 'Avg Tick: \K[0-9.]+' || echo "?")
            OT=$(echo "$SAMPLE" | grep -oP 'Out: \K[0-9.]+kbps' || echo "?")
            EF=$(echo "$SAMPLE" | grep -oP 'Delta Efficiency: \K[0-9]+%' || echo "?")
            echo -e "  ${CYAN}t=${ELAPSED}s${NC}  Clients=${CL}  Tick=${TK}ms  Out=${OT}  Δeff=${EF}"
        fi
    done

    # Stop profiler stream
    [[ -n "$SSH_TAIL_PID" ]] && kill "$SSH_TAIL_PID" 2>/dev/null || true
    SSH_TAIL_PID=""

    FINAL_PROFILER="$LAST_SAMPLE"

    # Stop bots
    step "Stopping bots..."
    [[ ${#BOT_PIDS[@]} -gt 0 ]] && kill "${BOT_PIDS[@]}" 2>/dev/null || true
    wait "${BOT_PIDS[@]}" 2>/dev/null || true
    BOT_PIDS=()
    ok "Bots stopped."

    # Stop + remove bench container
    step "Stopping benchmark container..."
    ssh_cmd "docker stop ${BENCH_CONTAINER} 2>/dev/null; docker rm ${BENCH_CONTAINER} 2>/dev/null" || true
    BENCH_CONTAINER=""
    ok "Container removed."

    # Restore original netserver
    step "Restoring netserver..."
    ssh_cmd "docker start netserver 2>/dev/null" || true
    ok "netserver restored."

    rm -f "$PROFILER_LOG"
    PROFILER_LOG=""

    # Parse final snapshot
    if [[ -z "$FINAL_PROFILER" ]]; then
        warn "No [PROFILER] line captured for scenario '${label}'."
        R_MODE+=("$label"); R_CLIENTS+=("?/${BOT_COUNT}")
        R_TICK+=("N/A"); R_FULL+=("N/A"); R_OUT+=("N/A"); R_IN+=("N/A"); R_EFF+=("N/A")
        return
    fi

    ok "Final: $FINAL_PROFILER"

    local clients avg_tick full_loop out_kbps in_kbps delta_eff
    clients=$(   echo "$FINAL_PROFILER" | grep -oP 'Clients: \K[0-9]+')
    avg_tick=$(  echo "$FINAL_PROFILER" | grep -oP 'Avg Tick: \K[0-9.]+')
    full_loop=$( echo "$FINAL_PROFILER" | grep -oP 'Full Loop: \K[0-9.]+')
    out_kbps=$(  echo "$FINAL_PROFILER" | grep -oP 'Out: \K[0-9.]+kbps')
    in_kbps=$(   echo "$FINAL_PROFILER" | grep -oP 'In: \K[0-9.]+kbps')
    delta_eff=$( echo "$FINAL_PROFILER" | grep -oP 'Delta Efficiency: \K[0-9]+%')

    R_MODE+=("$label")
    R_CLIENTS+=("${clients:-?}/${BOT_COUNT}")
    R_TICK+=("${avg_tick:-?}ms")
    R_FULL+=("${full_loop:-?}ms")
    R_OUT+=("${out_kbps:-?}")
    R_IN+=("${in_kbps:-?}")
    R_EFF+=("${delta_eff:-?}")

    sleep 5  # grace period between scenarios
}

# ── Run both scenarios ────────────────────────────────────────────────────────

run_scenario "Cluster (FOW=OFF baseline)" "cluster"
run_scenario "Bimodal (FOW=ON groups)"   "bimodal"

# ── Print results table ───────────────────────────────────────────────────────

echo ""
echo -e "${BOLD}${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${BOLD}${CYAN}  FOW BENCHMARK — Azure WAN Results${NC}"
echo -e "${BOLD}${CYAN}  $(date '+%Y-%m-%d %H:%M') | ${BOT_COUNT} bots | ${DURATION}s per scenario | real WAN${NC}"
echo -e "${BOLD}${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo ""
printf "${BOLD}%-30s %-11s %-9s %-9s %-14s %-14s %-14s${NC}\n" \
    "Scenario" "Connected" "Avg Tick" "Full Loop" "Out" "In" "Delta Eff."
echo "──────────────────────────────────────────────────────────────────────────────────────────────────────"
for i in "${!R_MODE[@]}"; do
    printf "%-30s %-11s %-9s %-9s %-14s %-14s %-14s\n" \
        "${R_MODE[$i]}" "${R_CLIENTS[$i]}" "${R_TICK[$i]}" "${R_FULL[$i]}" \
        "${R_OUT[$i]}" "${R_IN[$i]}" "${R_EFF[$i]}"
done
echo "──────────────────────────────────────────────────────────────────────────────────────────────────────"
echo ""

# ── FOW effectiveness ratio ───────────────────────────────────────────────────

if [[ "${R_OUT[0]}" != "N/A" && "${R_OUT[1]}" != "N/A" ]]; then
    out_cluster=$(echo "${R_OUT[0]}" | grep -oP '[0-9.]+')
    out_bimodal=$(echo "${R_OUT[1]}" | grep -oP '[0-9.]+')
    if [[ -n "$out_cluster" && -n "$out_bimodal" && "$out_cluster" != "0" ]]; then
        ratio=$(awk "BEGIN { printf \"%.1f\", ($out_bimodal / $out_cluster) * 100 }")
        reduction=$(awk "BEGIN { printf \"%.1f\", 100 - ($out_bimodal / $out_cluster) * 100 }")
        half_bots=$((BOT_COUNT / 2))
        echo -e "${BOLD}FOW effectiveness (WAN real):${NC}"
        echo -e "  Out_bimodal / Out_cluster = ${ratio}%"
        echo -e "  Bandwidth reduction from FOW filtering = ${BOLD}${reduction}%${NC}"
        echo -e "  Expected: ~50% (each group of ${half_bots} bots only receives its own group;"
        echo -e "  the other ${half_bots} bots are beyond DEFAULT_VISION_RANGE=150u → fully filtered)"
        echo ""
    fi
fi

# ── Save results ──────────────────────────────────────────────────────────────

RESULTS_DIR="$REPO_ROOT/benchmarks/results"
mkdir -p "$RESULTS_DIR"

GIT_HASH=$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo "unknown")
RUN_TS=$(date '+%Y-%m-%d_%Hh%M')
RESULT_FILE="$RESULTS_DIR/fow_azure_${RUN_TS}_${GIT_HASH}.md"

{
    echo "---"
    echo "date: $(date '+%Y-%m-%d %H:%M')"
    echo "commit: $GIT_HASH"
    echo "branch: $(git -C "$REPO_ROOT" rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)"
    echo "platform: Azure VM (${VM_IP}) ← WSL2 bots | real WAN"
    echo "build: Release (Docker ${SERVER_IMAGE})"
    echo "scenario: ${BOT_COUNT} bots | real WAN | ${DURATION}s"
    echo "notes: FOW Interest Management Benchmark — cluster vs bimodal spawn"
    echo "---"
    echo ""
    echo "# FOW Azure Benchmark — $RUN_TS — commit $GIT_HASH"
    echo ""
    echo "**Goal:** Prove VisibilityTracker + SpatialGrid filter cross-group entities"
    echo "when two groups of bots are placed beyond DEFAULT_VISION_RANGE=150u."
    echo ""
    echo "## Spawn Layouts"
    echo ""
    echo "| Mode    | Zone A                        | Zone B                       | Min distance |"
    echo "|---------|-------------------------------|------------------------------|--------------|"
    echo "| cluster | x∈[-80,80] y∈[-80,80]        | same zone                    | 0u           |"
    echo "| bimodal | x∈[-400,-200] y∈[-100,100]   | x∈[200,400] y∈[-100,100]    | ~400u        |"
    echo ""
    echo "Vision range = 150u → bimodal groups are invisible to each other."
    echo ""
    echo "## Results"
    echo ""
    printf "| %-28s | %-9s | %-7s | %-7s | %-12s | %-12s | %-12s |\n" \
        "Scenario" "Connected" "Avg Tick" "Full Loop" "Out" "In" "Delta Eff."
    echo "|-----------------------------|-----------|---------|---------|--------------|--------------|--------------|"
    for i in "${!R_MODE[@]}"; do
        printf "| %-28s | %-9s | %-7s | %-7s | %-12s | %-12s | %-12s |\n" \
            "${R_MODE[$i]}" "${R_CLIENTS[$i]}" "${R_TICK[$i]}" "${R_FULL[$i]}" \
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
            echo "- Bandwidth reduction from FOW = **${rd}%**"
            echo "- Target: ~50%"
        fi
    fi
    echo ""
    echo "## Notes"
    echo ""
    echo "- Server: Docker container with SPAWN_ZONE_MODE env var, same image as production."
    echo "- Bots: WSL2 local → Azure Switzerland North over real WAN (no tc netem)."
    echo "- DEFAULT_VISION_RANGE = 150u; bimodal min cross-group distance ≈ 400u."
    echo "- The original 'netserver' container is restored after each scenario."
} > "$RESULT_FILE"

ok "Results saved: $RESULT_FILE"
ok "Done."
