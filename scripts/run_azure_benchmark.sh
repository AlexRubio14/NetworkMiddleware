#!/usr/bin/env bash
# run_azure_benchmark.sh — Remote benchmark against the live Azure VM
#
# Launches N bots from local WSL2, lets them run for DURATION seconds,
# samples the server profiler every 10s, and saves results to
# benchmarks/results/azure_<timestamp>_<hash>.md
#
# Usage:
#   bash scripts/run_azure_benchmark.sh                      # 10 bots, 60s
#   bash scripts/run_azure_benchmark.sh --bots 50            # 50 bots, 60s
#   bash scripts/run_azure_benchmark.sh --bots 50 --duration 120
#
# Env vars:
#   AZURE_VM_IP       — public IP of the Azure VM       (default: 20.208.47.230)
#   AZURE_VM_USER     — SSH username                    (default: alexrp14)
#   AZURE_SSH_KEY     — path to private key             (default: ~/.ssh/azure_key)
#   BOT_BIN           — path to HeadlessBot binary      (auto-detected)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# ── Config ────────────────────────────────────────────────────────────────────

VM_IP="${AZURE_VM_IP:-20.208.47.230}"
VM_USER="${AZURE_VM_USER:-alexrp14}"
SSH_KEY="${AZURE_SSH_KEY:-$HOME/.ssh/azure_key}"
BOT_COUNT=10
DURATION=60

# Auto-detect HeadlessBot binary
if [[ -n "${BOT_BIN:-}" && -f "$BOT_BIN" ]]; then
    :
elif [[ -f "$REPO_ROOT/cmake-build-wsl/HeadlessBot/HeadlessBot" ]]; then
    BOT_BIN="$REPO_ROOT/cmake-build-wsl/HeadlessBot/HeadlessBot"
elif [[ -f "$REPO_ROOT/cmake-build-debug/HeadlessBot/HeadlessBot" ]]; then
    BOT_BIN="$REPO_ROOT/cmake-build-debug/HeadlessBot/HeadlessBot"
else
    echo "HeadlessBot binary not found. Build with:"
    echo "  cmake -S . -B cmake-build-wsl && cmake --build cmake-build-wsl"
    exit 1
fi

# ── Parse args ────────────────────────────────────────────────────────────────

while [[ $# -gt 0 ]]; do
    case "$1" in
        --bots)     BOT_COUNT="$2"; shift 2 ;;
        --duration) DURATION="$2";  shift 2 ;;
        *) echo "Unknown arg: $1"; exit 1 ;;
    esac
done

# ── SSH ControlMaster — one handshake, all calls reuse the socket ─────────────

SSH_CTL="/tmp/azure_bench_ctl_$$"
SSH_OPTS="-i $SSH_KEY -o StrictHostKeyChecking=no -o ConnectTimeout=10 -o BatchMode=yes \
    -o ControlMaster=auto -o ControlPath=$SSH_CTL -o ControlPersist=300"

ssh_cmd() { ssh $SSH_OPTS "${VM_USER}@${VM_IP}" "$@"; }

# ── Colors ────────────────────────────────────────────────────────────────────

CYAN='\033[0;36m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'
BOLD='\033[1m'; NC='\033[0m'
step() { echo -e "${CYAN}[bench]${NC} $*"; }
ok()   { echo -e "${GREEN}[  OK  ]${NC} $*"; }
warn() { echo -e "${YELLOW}[ WARN ]${NC} $*"; }

# ── Cleanup trap ──────────────────────────────────────────────────────────────

BOT_PIDS=()
LOG_DIR=""
PROFILER_LOG=""
SSH_TAIL_PID=""
cleanup() {
    [[ -n "$SSH_TAIL_PID" ]] && kill "$SSH_TAIL_PID" 2>/dev/null || true
    if [[ ${#BOT_PIDS[@]} -gt 0 ]]; then
        kill "${BOT_PIDS[@]}" 2>/dev/null || true
        wait "${BOT_PIDS[@]}" 2>/dev/null || true
    fi
    [[ -n "$LOG_DIR" ]] && rm -rf "$LOG_DIR" || true
    [[ -n "$PROFILER_LOG" ]] && rm -f "$PROFILER_LOG" || true
    ssh -O stop -o ControlPath="$SSH_CTL" "${VM_USER}@${VM_IP}" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# ── Header ────────────────────────────────────────────────────────────────────

echo ""
echo -e "${BOLD}${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${BOLD}${CYAN}  Azure Remote Benchmark — NetworkMiddleware${NC}"
echo -e "${BOLD}${CYAN}  Target: ${VM_IP}:7777 | Bots: ${BOT_COUNT} | Duration: ${DURATION}s${NC}"
echo -e "${BOLD}${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo ""

# ── Open master connection + verify server ────────────────────────────────────

step "Opening SSH connection to ${VM_IP}..."
if ! ssh_cmd 'docker inspect --format "{{.State.Status}}" netserver 2>/dev/null' | grep -q "running"; then
    echo -e "${RED}[FAIL]${NC} Server container 'netserver' is not running on ${VM_IP}"
    echo "       Start it with: ssh ${VM_USER}@${VM_IP} 'docker start netserver'"
    exit 1
fi
ok "SSH connected. Server is running."

# ── Restart server to clear any stale sessions ────────────────────────────────

step "Killing any leftover HeadlessBot processes from previous runs..."
pkill -x HeadlessBot 2>/dev/null || true
ok "Stale bots cleared"

step "Restarting server to clear stale sessions..."
ssh_cmd 'docker restart netserver' &>/dev/null
sleep 3  # wait for server to bind
ok "Server restarted"

# ── Start profiler stream — one SSH, writes PROFILER lines to local file ──────
# docker logs output goes to stderr, so 2>&1 is required before the pipe.

PROFILER_LOG=$(mktemp)
# Use a dedicated SSH connection for the stream (not ControlMaster) so it
# doesn't compete with the sample/control calls.
ssh -i "$SSH_KEY" -o StrictHostKeyChecking=no -o BatchMode=yes \
    "${VM_USER}@${VM_IP}" \
    "docker logs -f --tail 0 netserver 2>&1 | grep --line-buffered '\[PROFILER\]'" \
    > "$PROFILER_LOG" &
SSH_TAIL_PID=$!

# ── Helper: get latest PROFILER line from local stream (no SSH call) ──────────

get_profiler() {
    tail -1 "$PROFILER_LOG" 2>/dev/null || true
}

# ── Launch bots ───────────────────────────────────────────────────────────────

LOG_DIR=$(mktemp -d)
step "Launching ${BOT_COUNT} bots → ${VM_IP}:7777"

for ((i = 0; i < BOT_COUNT; i++)); do
    SERVER_HOST="$VM_IP" SERVER_PORT=7777 "$BOT_BIN" \
        > "$LOG_DIR/bot_${i}.log" 2>&1 &
    BOT_PIDS+=($!)
    sleep 0.3   # stagger 300ms between bots — Azure NSG drops bursts that look like DDoS
done

# Wait for bots to connect + profiler stream to have live data.
# With 300ms stagger, N bots take N*0.3s to launch; add 8s on top for handshakes to complete.
LAUNCH_WAIT=$(awk "BEGIN { printf \"%d\", int($BOT_COUNT * 0.3 + 8) }")
step "Waiting ${LAUNCH_WAIT}s for bots to connect..."
sleep "$LAUNCH_WAIT"

# ── Collect profiler during run ───────────────────────────────────────────────

step "Collecting profiler data for ${DURATION}s..."

SAMPLES=()
ELAPSED=0
SAMPLE_INTERVAL=10

while [[ $ELAPSED -lt $DURATION ]]; do
    SLEEP_TIME=$((SAMPLE_INTERVAL < (DURATION - ELAPSED) ? SAMPLE_INTERVAL : (DURATION - ELAPSED)))
    sleep "$SLEEP_TIME"
    ELAPSED=$((ELAPSED + SLEEP_TIME))

    SAMPLE=$(get_profiler)
    if [[ -n "$SAMPLE" ]]; then
        SAMPLES+=("${ELAPSED}s: $SAMPLE")
        CLIENTS=$(echo "$SAMPLE" | grep -oP 'Clients: \K[0-9]+' || echo "?")
        TICK=$(echo "$SAMPLE"   | grep -oP 'Avg Tick: \K[0-9.]+' || echo "?")
        OUT=$(echo "$SAMPLE"    | grep -oP 'Out: \K[0-9.]+kbps' || echo "?")
        EFF=$(echo "$SAMPLE"    | grep -oP 'Delta Efficiency: \K[0-9]+%' || echo "?")
        echo -e "  ${CYAN}t=${ELAPSED}s${NC}  Clients=${CLIENTS}  Tick=${TICK}ms  Out=${OUT}  Δeff=${EFF}"
    fi
done

# ── Final snapshot (while bots still connected) ───────────────────────────────

step "Collecting final profiler snapshot..."
FINAL_PROFILER=$(get_profiler)

# ── Stop profiler stream ──────────────────────────────────────────────────────

[[ -n "$SSH_TAIL_PID" ]] && kill "$SSH_TAIL_PID" 2>/dev/null || true
SSH_TAIL_PID=""

# ── Stop bots ─────────────────────────────────────────────────────────────────

step "Stopping bots..."
kill "${BOT_PIDS[@]}" 2>/dev/null || true
wait "${BOT_PIDS[@]}" 2>/dev/null || true
BOT_PIDS=()
ok "Bots stopped"

if [[ -n "$FINAL_PROFILER" ]]; then
    ok "Final: $FINAL_PROFILER"
else
    warn "No profiler line found"
    FINAL_PROFILER="(no data)"
fi

# ── Parse final snapshot ──────────────────────────────────────────────────────

CLIENTS=$(echo "$FINAL_PROFILER"  | grep -oP 'Clients: \K[0-9]+'       || echo "?")
CONNECTED="${CLIENTS}"
AVG_TICK=$(echo "$FINAL_PROFILER" | grep -oP 'Avg Tick: \K[0-9.]+'     || echo "?")
FULL_LOOP=$(echo "$FINAL_PROFILER"| grep -oP 'Full Loop: \K[0-9.]+'    || echo "?")
OUT_KBPS=$(echo "$FINAL_PROFILER" | grep -oP 'Out: \K[0-9.]+kbps'     || echo "?")
IN_KBPS=$(echo "$FINAL_PROFILER"  | grep -oP 'In: \K[0-9.]+kbps'      || echo "?")
RETRIES=$(echo "$FINAL_PROFILER"  | grep -oP 'Retries: \K[0-9]+'       || echo "?")
CRC_ERR=$(echo "$FINAL_PROFILER"  | grep -oP 'CRC Err: \K[0-9]+'       || echo "?")
DELTA_EFF=$(echo "$FINAL_PROFILER"| grep -oP 'Delta Efficiency: \K[0-9]+%' || echo "?")

BUDGET="?"
if [[ "$AVG_TICK" != "?" ]]; then
    BUDGET=$(awk "BEGIN { printf \"%.1f%%\", ($AVG_TICK / 10.0) * 100 }")
fi

# LagComp HITs — count from local profiler log (approximation via sample count)
HIT_COUNT="(see server logs)"

# ── Print results ─────────────────────────────────────────────────────────────

echo ""
echo -e "${BOLD}${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${BOLD}${CYAN}  RESULTS${NC}"
echo -e "${BOLD}${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo ""
printf "  ${BOLD}%-18s${NC} %s\n" "Connected bots:"   "${CONNECTED}/${BOT_COUNT}"
printf "  ${BOLD}%-18s${NC} %s\n" "Avg Tick:"         "${AVG_TICK}ms"
printf "  ${BOLD}%-18s${NC} %s\n" "Full Loop:"        "${FULL_LOOP}ms"
printf "  ${BOLD}%-18s${NC} %s\n" "Tick Budget:"      "${BUDGET} (target <10%)"
printf "  ${BOLD}%-18s${NC} %s\n" "Outbound:"         "${OUT_KBPS}"
printf "  ${BOLD}%-18s${NC} %s\n" "Inbound:"          "${IN_KBPS}"
printf "  ${BOLD}%-18s${NC} %s\n" "Retries:"          "${RETRIES}"
printf "  ${BOLD}%-18s${NC} %s\n" "CRC Errors:"       "${CRC_ERR}"
printf "  ${BOLD}%-18s${NC} %s\n" "Delta Efficiency:" "${DELTA_EFF}"
printf "  ${BOLD}%-18s${NC} %s\n" "LagComp HITs:"     "${HIT_COUNT}"
echo ""

# ── Save results ──────────────────────────────────────────────────────────────

RESULTS_DIR="$REPO_ROOT/benchmarks/results"
mkdir -p "$RESULTS_DIR"

GIT_HASH=$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo "unknown")
RUN_TS=$(date '+%Y-%m-%d_%Hh%M')
RESULT_FILE="$RESULTS_DIR/azure_${RUN_TS}_${GIT_HASH}.md"

{
    echo "---"
    echo "date: $(date '+%Y-%m-%d %H:%M')"
    echo "commit: $GIT_HASH"
    echo "branch: $(git -C "$REPO_ROOT" rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)"
    echo "platform: Azure VM (${VM_IP}) ← WSL2 bots"
    echo "build: Release (Docker ghcr.io/alexrubio14/netserver:latest)"
    echo "scenario: ${BOT_COUNT} bots | real WAN latency | ${DURATION}s"
    echo "notes: Auto-generated by run_azure_benchmark.sh"
    echo "---"
    echo ""
    echo "# Azure Benchmark — ${RUN_TS} — commit ${GIT_HASH}"
    echo ""
    echo "**Scenario:** ${BOT_COUNT} bots | real WAN | ${DURATION}s | server: ${VM_IP}:7777"
    echo ""
    echo "## Results"
    echo ""
    echo "| Metric | Value |"
    echo "|--------|-------|"
    echo "| Connected bots | ${CONNECTED}/${BOT_COUNT} |"
    echo "| Avg Tick | ${AVG_TICK}ms |"
    echo "| Full Loop | ${FULL_LOOP}ms |"
    echo "| Tick Budget | ${BUDGET} |"
    echo "| Outbound | ${OUT_KBPS} |"
    echo "| Inbound | ${IN_KBPS} |"
    echo "| Retries | ${RETRIES} |"
    echo "| CRC Errors | ${CRC_ERR} |"
    echo "| Delta Efficiency | ${DELTA_EFF} |"
    echo "| LagComp HITs | ${HIT_COUNT} |"
    echo ""
    echo "## Profiler samples"
    echo ""
    echo '```'
    for s in "${SAMPLES[@]:-}"; do
        echo "$s"
    done
    echo ""
    echo "Final: $FINAL_PROFILER"
    echo '```'
} > "$RESULT_FILE"

ok "Results saved: $RESULT_FILE"
echo ""
