#!/usr/bin/env bash
# azure_integration_test.sh — Integration tests against the live Azure VM
#
# Test suite:
#   T1. Handshake — bot connects and reaches Established state
#   T2. Input loop — bot sends inputs for 5s without disconnect
#   T3. Multi-bot  — 5 bots connect simultaneously, all reach Established
#   T4. LagComp    — 2+ bots connected → server emits [LagComp] HIT lines
#   T5. Reconnect  — bot connects, disconnects cleanly, reconnects successfully
#
# Usage:
#   bash scripts/azure_integration_test.sh
#   AZURE_VM_IP=1.2.3.4 bash scripts/azure_integration_test.sh
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

SSH_OPTS="-i $SSH_KEY -o StrictHostKeyChecking=no -o ConnectTimeout=10 -o BatchMode=yes"

# ── Colors ────────────────────────────────────────────────────────────────────

GREEN='\033[0;32m'; RED='\033[0;31m'; CYAN='\033[0;36m'; YELLOW='\033[1;33m'
BOLD='\033[1m'; NC='\033[0m'
PASS=0; FAIL=0

pass() { echo -e "  ${GREEN}[PASS]${NC} $*"; ((PASS++)); }
fail() { echo -e "  ${RED}[FAIL]${NC} $*"; ((FAIL++)); }
info() { echo -e "  ${CYAN}      ${NC} $*"; }

run_test() { echo -e "${BOLD}${CYAN}── $* ${NC}"; }

# ── Helper: launch bot, wait for pattern in output ───────────────────────────
# Returns: 0 if pattern found within timeout, 1 otherwise
# Usage: bot_expect <timeout_s> <grep_pattern> [log_var_name]

bot_expect() {
    local timeout_s="$1"
    local pattern="$2"
    local log_file
    log_file=$(mktemp)
    SERVER_HOST="$VM_IP" SERVER_PORT=7777 timeout "$timeout_s" "$BOT_BIN" \
        > "$log_file" 2>&1 || true
    if grep -q "$pattern" "$log_file"; then
        echo "$log_file"
        return 0
    else
        echo "$log_file"
        return 1
    fi
}

# ── Header ────────────────────────────────────────────────────────────────────

echo ""
echo -e "${BOLD}${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${BOLD}${CYAN}  Azure Integration Tests — NetworkMiddleware${NC}"
echo -e "${BOLD}${CYAN}  Target: ${VM_USER}@${VM_IP}:7777${NC}"
echo -e "${BOLD}${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo ""

# ── T1: Handshake ─────────────────────────────────────────────────────────────

run_test "T1: Handshake"
LOG=$(mktemp)
SERVER_HOST="$VM_IP" SERVER_PORT=7777 timeout 7 "$BOT_BIN" > "$LOG" 2>&1 || true

if grep -q "Established" "$LOG"; then
    NID=$(grep -oP 'NetworkID=\K[0-9]+' "$LOG" | head -1 || echo "?")
    pass "Bot reached Established (NetworkID=${NID})"
else
    fail "Bot did not reach Established state"
    info "Last log lines:"
    tail -5 "$LOG" | while IFS= read -r line; do info "  $line"; done
fi
rm -f "$LOG"
echo ""

# ── T2: Input loop stability (5 seconds) ─────────────────────────────────────

run_test "T2: Input loop — 5s stability"
LOG=$(mktemp)
SERVER_HOST="$VM_IP" SERVER_PORT=7777 timeout 8 "$BOT_BIN" > "$LOG" 2>&1 || true

if grep -q "Established" "$LOG" && ! grep -q "disconnected\|timeout\|failed" "$LOG"; then
    pass "Bot ran for 5s without error"
elif grep -q "Established" "$LOG"; then
    ERRORS=$(grep -c "disconnected\|timeout\|failed" "$LOG" || true)
    fail "Bot connected but encountered ${ERRORS} error(s)"
    tail -5 "$LOG" | while IFS= read -r line; do info "  $line"; done
else
    fail "Bot did not connect"
fi
rm -f "$LOG"
echo ""

# ── T3: Multi-bot (5 bots simultaneous) ──────────────────────────────────────

run_test "T3: Multi-bot — 5 simultaneous connections"
LOGS=()
PIDS=()
for i in $(seq 1 5); do
    L=$(mktemp)
    LOGS+=("$L")
    SERVER_HOST="$VM_IP" SERVER_PORT=7777 timeout 8 "$BOT_BIN" > "$L" 2>&1 &
    PIDS+=($!)
    sleep 0.3   # stagger handshakes — Azure NSG rate-limits connection bursts
done

# Wait for all bots
for pid in "${PIDS[@]}"; do
    wait "$pid" 2>/dev/null || true
done

CONNECTED=0
for log in "${LOGS[@]}"; do
    grep -q "Established" "$log" && ((CONNECTED++)) || true
done

if [[ $CONNECTED -eq 5 ]]; then
    pass "All 5 bots reached Established"
elif [[ $CONNECTED -ge 3 ]]; then
    fail "${CONNECTED}/5 bots connected (partial — check server capacity or NSG)"
else
    fail "${CONNECTED}/5 bots connected"
fi
info "Connected: ${CONNECTED}/5"

for log in "${LOGS[@]}"; do rm -f "$log"; done
echo ""

# ── T4: LagComp — requires 2+ bots shooting ──────────────────────────────────

run_test "T4: LagComp HIT detection"

# Get server log line count before test
LOG_LINES_BEFORE=$(ssh $SSH_OPTS "${VM_USER}@${VM_IP}" \
    "docker logs netserver 2>/dev/null | wc -l" || echo "0")

# Launch 4 bots for 10s (chaos mode generates shots between bots)
BOT_LOGS=()
BOT_PIDS=()
for i in $(seq 1 4); do
    L=$(mktemp)
    BOT_LOGS+=("$L")
    SERVER_HOST="$VM_IP" SERVER_PORT=7777 timeout 12 "$BOT_BIN" > "$L" 2>&1 &
    BOT_PIDS+=($!)
    sleep 0.3   # stagger handshakes
done

sleep 10

# Stop bots
for pid in "${BOT_PIDS[@]}"; do
    kill "$pid" 2>/dev/null || true
done
wait "${BOT_PIDS[@]}" 2>/dev/null || true

# Check server logs for LagComp HITs during this window
HIT_COUNT=$(ssh $SSH_OPTS "${VM_USER}@${VM_IP}" \
    "docker logs --tail 500 netserver 2>/dev/null | grep '\[LagComp\] HIT' | wc -l" || echo "0")

if [[ "$HIT_COUNT" -gt 0 ]]; then
    pass "LagComp HIT detected (${HIT_COUNT} hits in server log)"
    SAMPLE=$(ssh $SSH_OPTS "${VM_USER}@${VM_IP}" \
        "docker logs --tail 500 netserver 2>/dev/null | grep '\[LagComp\] HIT' | tail -2" || true)
    info "$SAMPLE"
else
    fail "No LagComp HITs detected (bots may not have been in range or shots didn't fire)"
fi

for log in "${BOT_LOGS[@]}"; do rm -f "$log"; done
echo ""

# ── T5: Clean disconnect + reconnect ─────────────────────────────────────────

run_test "T5: Clean disconnect and reconnect"

# First connection
LOG1=$(mktemp)
SERVER_HOST="$VM_IP" SERVER_PORT=7777 timeout 5 "$BOT_BIN" > "$LOG1" 2>&1 || true
FIRST_OK=false
grep -q "Established" "$LOG1" && FIRST_OK=true

# Second connection (reconnect)
LOG2=$(mktemp)
SERVER_HOST="$VM_IP" SERVER_PORT=7777 timeout 5 "$BOT_BIN" > "$LOG2" 2>&1 || true
SECOND_OK=false
grep -q "Established" "$LOG2" && SECOND_OK=true

if $FIRST_OK && $SECOND_OK; then
    pass "Connect → disconnect → reconnect successful"
elif $FIRST_OK && ! $SECOND_OK; then
    fail "First connection OK but reconnect failed"
else
    fail "First connection failed"
fi

rm -f "$LOG1" "$LOG2"
echo ""

# ── Summary ───────────────────────────────────────────────────────────────────

TOTAL=$((PASS + FAIL))
echo -e "${BOLD}${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
if [[ $FAIL -eq 0 ]]; then
    echo -e "${GREEN}${BOLD}  PASSED ${PASS}/${TOTAL} — All integration tests OK${NC}"
    EXIT_CODE=0
else
    echo -e "${RED}${BOLD}  FAILED ${FAIL}/${TOTAL} — ${PASS} passed, ${FAIL} failed${NC}"
    EXIT_CODE=1
fi
echo -e "${BOLD}${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo ""
exit $EXIT_CODE
