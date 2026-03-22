#!/usr/bin/env bash
# NetworkMiddleware — P-4.3 Benchmark Script
#
# Applies tc netem network degradation, launches the Docker stack at the
# requested scale, waits for the duration, collects server logs, then
# restores the network.
#
# Usage:
#   TC_IFACE=lo DELAY=50ms LOSS=5% BOT_COUNT=10 DURATION=60 ./scripts/run_benchmarks.sh
#
# Defaults:
#   TC_IFACE=lo      (correct for network_mode: host on Linux / WSL2)
#   DELAY=50ms
#   LOSS=5%
#   BOT_COUNT=10
#   DURATION=60      (seconds)
#
# IMPORTANT — execution environment:
#   This script requires a Linux host or WSL2 with real tc/netem support.
#   Docker Desktop on Windows silently ignores network_mode: host and
#   tc qdisc on 'lo' will not affect container traffic.
#   Run on a native Linux machine or WSL2 for scientifically valid results.
#
# Scenarios (from P-4.3 handoff):
#   Scenario A — Clean Lab:   BOT_COUNT=10  DELAY=0ms   LOSS=0%   (no tc needed)
#   Scenario B — Real World:  BOT_COUNT=10  DELAY=50ms  LOSS=5%
#   Scenario C — Stress/Zerg: BOT_COUNT=50  DELAY=100ms LOSS=2%

set -euo pipefail

TC_IFACE=${TC_IFACE:-lo}
DELAY=${DELAY:-50ms}
LOSS=${LOSS:-5%}
BOT_COUNT=${BOT_COUNT:-10}
DURATION=${DURATION:-60}

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
LOG_FILE="bench_${BOT_COUNT}bots_${DELAY}_${LOSS}_${TIMESTAMP}.log"

echo "=== NetworkMiddleware P-4.3 Benchmark ==="
echo "  Interface : $TC_IFACE"
echo "  Delay     : $DELAY"
echo "  Loss      : $LOSS"
echo "  Bots      : $BOT_COUNT"
echo "  Duration  : ${DURATION}s"
echo "  Log file  : $LOG_FILE"
echo ""

# ── Network degradation (idempotent) ──────────────────────────────────────────
# Delete any existing qdisc first so the script is safe to re-run.
sudo tc qdisc del dev "$TC_IFACE" root 2>/dev/null || true

if [[ "$DELAY" != "0ms" || "$LOSS" != "0%" ]]; then
    sudo tc qdisc add dev "$TC_IFACE" root netem delay "$DELAY" loss "$LOSS"
    echo "[tc] Degradation active on $TC_IFACE: delay=$DELAY loss=$LOSS"
else
    echo "[tc] Clean-lab mode — no degradation applied"
fi

# ── Launch Docker stack ───────────────────────────────────────────────────────
docker-compose up --build --scale "bot=${BOT_COUNT}" -d
echo "[docker] Stack started with $BOT_COUNT bots"

# ── Run for DURATION seconds ──────────────────────────────────────────────────
echo "[bench] Running for ${DURATION}s — watching server profiler output..."
sleep "$DURATION"

# ── Collect logs ──────────────────────────────────────────────────────────────
echo "[bench] Collecting server logs → $LOG_FILE"
docker-compose logs server > "$LOG_FILE" 2>&1
echo "[bench] Done. Profiler lines:"
grep "\[PROFILER\]" "$LOG_FILE" || echo "  (no profiler output yet — increase DURATION)"

# ── Stop containers ───────────────────────────────────────────────────────────
docker-compose down
echo "[docker] Stack stopped"

# ── Restore network ───────────────────────────────────────────────────────────
sudo tc qdisc del dev "$TC_IFACE" root 2>/dev/null || true
echo "[tc] Network restored on $TC_IFACE"

echo ""
echo "=== Benchmark complete ==="
echo "Results: $LOG_FILE"
