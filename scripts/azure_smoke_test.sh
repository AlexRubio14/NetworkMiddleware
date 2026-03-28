#!/usr/bin/env bash
# azure_smoke_test.sh — Verify Azure VM is up and NetServer is responding
#
# Checks:
#   1. SSH reachable
#   2. Docker container 'netserver' is running
#   3. Server emits PROFILER lines (tick loop alive)
#   4. UDP port 7777 accepts a bot connection (1 bot, handshake only)
#
# Usage:
#   bash scripts/azure_smoke_test.sh
#   AZURE_VM_IP=1.2.3.4 bash scripts/azure_smoke_test.sh
#
# Env vars (all optional — fall back to defaults):
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

# Auto-detect HeadlessBot binary
if [[ -n "${BOT_BIN:-}" && -f "$BOT_BIN" ]]; then
    :
elif [[ -f "$REPO_ROOT/cmake-build-wsl/HeadlessBot/HeadlessBot" ]]; then
    BOT_BIN="$REPO_ROOT/cmake-build-wsl/HeadlessBot/HeadlessBot"
elif [[ -f "$REPO_ROOT/cmake-build-debug/HeadlessBot/HeadlessBot" ]]; then
    BOT_BIN="$REPO_ROOT/cmake-build-debug/HeadlessBot/HeadlessBot"
else
    BOT_BIN=""
fi

SSH_OPTS="-i $SSH_KEY -o StrictHostKeyChecking=no -o ConnectTimeout=10 -o BatchMode=yes"

# ── Colors ────────────────────────────────────────────────────────────────────

GREEN='\033[0;32m'; RED='\033[0;31m'; CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'
PASS=0; FAIL=0

pass() { echo -e "${GREEN}[PASS]${NC} $*"; ((PASS++)); }
fail() { echo -e "${RED}[FAIL]${NC} $*"; ((FAIL++)); }
step() { echo -e "${CYAN}[smoke]${NC} $*"; }

# ── Header ────────────────────────────────────────────────────────────────────

echo -e "${BOLD}${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${BOLD}${CYAN}  Azure Smoke Test — NetworkMiddleware${NC}"
echo -e "${BOLD}${CYAN}  Target: ${VM_USER}@${VM_IP}${NC}"
echo -e "${BOLD}${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo ""

# ── Check 1: SSH reachable ────────────────────────────────────────────────────

step "1/4 SSH connectivity..."
if ssh $SSH_OPTS "${VM_USER}@${VM_IP}" 'echo ok' &>/dev/null; then
    pass "SSH reachable (${VM_IP})"
else
    fail "SSH unreachable — check VM is running and key is correct"
    echo ""
    echo -e "${RED}Cannot continue without SSH. Aborting.${NC}"
    exit 1
fi

# ── Check 2: Docker container running ────────────────────────────────────────

step "2/4 Docker container status..."
CONTAINER_STATUS=$(ssh $SSH_OPTS "${VM_USER}@${VM_IP}" \
    "docker inspect --format '{{.State.Status}}' netserver 2>/dev/null || echo 'missing'")

if [[ "$CONTAINER_STATUS" == "running" ]]; then
    pass "Container 'netserver' is running"
else
    fail "Container 'netserver' status: ${CONTAINER_STATUS}"
fi

# ── Check 3: Profiler alive (server tick loop) ────────────────────────────────

step "3/4 Server tick loop (PROFILER lines)..."
LAST_PROFILER=$(ssh $SSH_OPTS "${VM_USER}@${VM_IP}" \
    "docker logs --tail 50 netserver 2>/dev/null | grep '\[PROFILER\]' | tail -1 || true")

if [[ -n "$LAST_PROFILER" ]]; then
    pass "Tick loop alive"
    echo "       $LAST_PROFILER"
else
    fail "No PROFILER lines found in last 50 log lines"
fi

# ── Check 4: Bot handshake ────────────────────────────────────────────────────

step "4/4 Bot handshake (UDP port 7777)..."

if [[ -z "$BOT_BIN" ]]; then
    echo -e "${CYAN}[smoke]${NC}   HeadlessBot not found — skipping handshake check"
    echo "         Build with: cmake -S . -B cmake-build-wsl && cmake --build cmake-build-wsl"
    ((FAIL++))
else
    # Launch 1 bot, give it 6s to complete handshake, capture output
    BOT_LOG=$(mktemp)
    SERVER_HOST="$VM_IP" SERVER_PORT=7777 timeout 6 "$BOT_BIN" > "$BOT_LOG" 2>&1 || true

    if grep -q "Established" "$BOT_LOG"; then
        NETWORK_ID=$(grep -oP 'NetworkID=\K[0-9]+' "$BOT_LOG" | head -1 || echo "?")
        pass "Handshake established (NetworkID=${NETWORK_ID})"
    elif grep -q "Handshake timeout" "$BOT_LOG"; then
        fail "Handshake timeout — UDP 7777 may be blocked (check Azure NSG)"
    elif grep -q "Connection denied" "$BOT_LOG"; then
        fail "Connection denied by server"
    else
        fail "Handshake did not complete — see /tmp for bot log"
        cp "$BOT_LOG" /tmp/azure_smoke_bot.log
        echo "         Log: /tmp/azure_smoke_bot.log"
    fi
    rm -f "$BOT_LOG"
fi

# ── Summary ───────────────────────────────────────────────────────────────────

echo ""
echo -e "${BOLD}${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
TOTAL=$((PASS + FAIL))
if [[ $FAIL -eq 0 ]]; then
    echo -e "${GREEN}${BOLD}  PASSED ${PASS}/${TOTAL} checks — Azure VM healthy${NC}"
    EXIT_CODE=0
else
    echo -e "${RED}${BOLD}  FAILED ${FAIL}/${TOTAL} checks${NC}"
    EXIT_CODE=1
fi
echo -e "${BOLD}${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo ""
exit $EXIT_CODE
