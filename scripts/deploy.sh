#!/usr/bin/env bash
# deploy.sh — Gestión del NetServer en Oracle Cloud (o cualquier host Linux)
#
# PRIMER USO (autenticación con GHCR):
#   GH_TOKEN=<tu_PAT> bash scripts/deploy.sh
#
# USO NORMAL (actualizar a la última versión):
#   bash scripts/deploy.sh
#
# COMANDOS:
#   bash scripts/deploy.sh            # pull + restart
#   bash scripts/deploy.sh logs       # ver logs en tiempo real
#   bash scripts/deploy.sh stop       # parar el servidor
#   bash scripts/deploy.sh status     # ver estado del contenedor
#
# SETUP INICIAL EN ORACLE CLOUD (ejecutar UNA sola vez):
#   sudo apt-get install -y docker.io
#   sudo usermod -aG docker $USER && newgrp docker
#   # Abrir puerto UDP en el firewall del OS:
#   sudo iptables -I INPUT 6 -p udp --dport 7777 -j ACCEPT
#   sudo netfilter-persistent save
#   # También abrir en la consola de Oracle: VCN → Security Lists → Ingress Rule
#   #   Source: 0.0.0.0/0 | Protocol: UDP | Dest Port: 7777

set -euo pipefail

REGISTRY="ghcr.io"
OWNER="alexrubio14"
IMAGE="${REGISTRY}/${OWNER}/netserver:latest"
CONTAINER="netserver"
PORT=7777

# ─── Subcomandos ─────────────────────────────────────────────────────────────
case "${1:-deploy}" in
  logs)
    docker logs -f "$CONTAINER"
    exit 0
    ;;
  stop)
    docker stop "$CONTAINER" && echo "Servidor parado."
    exit 0
    ;;
  status)
    docker ps --filter "name=${CONTAINER}" --format "table {{.Names}}\t{{.Status}}\t{{.Ports}}"
    exit 0
    ;;
  deploy) ;;  # continúa abajo
  *)
    echo "Uso: $0 [deploy|logs|stop|status]"
    exit 1
    ;;
esac

# ─── Autenticación (solo si se pasa GH_TOKEN) ────────────────────────────────
if [[ -n "${GH_TOKEN:-}" ]]; then
  echo "$GH_TOKEN" | docker login "$REGISTRY" -u "$OWNER" --password-stdin
  echo "✓ Autenticado en GHCR"
fi

# ─── Pull de la última imagen ────────────────────────────────────────────────
echo "Descargando ${IMAGE}..."
docker pull "$IMAGE"

# ─── Reemplazar contenedor en ejecución ─────────────────────────────────────
docker stop "$CONTAINER" 2>/dev/null && echo "Contenedor anterior parado." || true
docker rm   "$CONTAINER" 2>/dev/null || true

# --network host: el contenedor ve la IP pública directamente (mejor para UDP).
# --restart unless-stopped: se reinicia automáticamente tras reboot de la VM.
docker run -d \
  --name "$CONTAINER" \
  --network host \
  --restart unless-stopped \
  "$IMAGE"

echo ""
echo "✓ NetServer desplegado en UDP :${PORT}"
echo ""
echo "--- Últimas líneas de log ---"
sleep 1
docker logs --tail 20 "$CONTAINER"
echo ""
echo "Para ver logs en tiempo real: bash scripts/deploy.sh logs"
