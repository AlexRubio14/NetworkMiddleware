# DL-T.2 — CI/CD Pipeline & Azure VM Deploy

**Date:** 2026-03-26
**Branch:** `azure-auto-deploy` → PR #27 → `main`
**Tests:** 231 / 231 (sin cambios — infraestructura pura)

---

## El punto de partida

El proyecto ya tenía un pipeline de CI (tests + build en Windows/Linux) pero el despliegue era manual: había que entrar a la VM, hacer pull de la imagen Docker y reiniciar el contenedor a mano. Esto hacía que cada merge a `main` requiriera una acción humana adicional y que el servidor de Azure pudiera quedar desactualizado indefinidamente si se olvidaba.

El objetivo era cerrar ese gap: que cada push a `main` provocara automáticamente el redeploy en la VM de Azure, sin intervención manual.

---

## Infraestructura de la VM (setup inicial)

### Diagnóstico / Contexto

La VM elegida es una **Azure B1s** (1 vCPU, 1 GiB RAM, Ubuntu 22.04) — el tier más barato de Azure Student, suficiente para correr el servidor autoritativo en demos y benchmarks. El servidor escucha en **UDP 7777**, lo que requería abrir ese puerto tanto en el **Network Security Group (NSG)** de Azure como verificar el firewall del OS (`ufw`).

Por defecto Azure asigna IPs dinámicas: al apagar y encender la VM la IP cambia, lo que rompería los secretos de GitHub Actions en cada reinicio.

### Solución / Decisión

1. **NSG Inbound rule:** `UDP 7777`, source `*`, acción `Allow` — permite tráfico de clientes reales al servidor.
2. **IP estática:** La IP pública `20.208.47.230` se fijó como estática en la configuración de Azure. Coste marginal; evita tener que actualizar `AZURE_VM_IP` en cada ciclo de encendido/apagado.
3. **Docker:** instalado con `apt-get install docker.io`. El usuario `alexrp14` añadido al grupo `docker` para evitar `sudo` en cada comando.
4. **ufw:** `ufw allow 7777/udp` ejecutado aunque el servicio estaba inactivo — el NSG de Azure actúa como primera línea de defensa.

---

## Autenticación GHCR en la VM

### Diagnóstico / Contexto

La imagen del servidor se publica en **GHCR** (GitHub Container Registry) como paquete privado bajo `ghcr.io/alexrubio14/netserver:latest`. Docker no puede hacer `pull` sin autenticarse primero, y las credenciales deben persistir en la VM para que los deploys automáticos funcionen sin intervención.

### Solución / Decisión

Se generó un **GitHub PAT** con permiso `read:packages` y se autenticó con:

```bash
echo "<PAT>" | docker login ghcr.io -u alexrubio14 --password-stdin
```

Las credenciales quedan almacenadas en `~/.docker/config.json` de la VM. El job de GitHub Actions no necesita reautenticar en cada deploy porque reutiliza esas credenciales a través de la sesión SSH.

---

## Job `deploy-azure` en el CD pipeline

### Diagnóstico / Contexto

El CD existente (`cd.yml`) ya tenía cuatro jobs: `build-windows`, `build-linux`, `release` y `docker-publish`. Este último publicaba la imagen en GHCR pero el ciclo se cortaba ahí — nada actualizaba la VM.

### Solución / Decisión

Se añadió el job `deploy-azure` con dependencia `needs: docker-publish`, garantizando que solo se ejecuta si la imagen nueva está disponible en GHCR:

```yaml
deploy-azure:
  name: Deploy to Azure VM
  needs: docker-publish
  runs-on: ubuntu-latest
  steps:
    - name: Deploy via SSH
      uses: appleboy/ssh-action@v1.0.3
      with:
        host: ${{ secrets.AZURE_VM_IP }}
        username: ${{ secrets.AZURE_VM_USER }}
        key: ${{ secrets.AZURE_SSH_KEY }}
        script: |
          docker pull ghcr.io/alexrubio14/netserver:latest
          docker stop netserver || true
          docker rm   netserver || true
          docker run -d \
            --name netserver \
            --network host \
            --restart unless-stopped \
            ghcr.io/alexrubio14/netserver:latest
          docker logs --tail 20 netserver
```

El flag `--restart unless-stopped` garantiza que el contenedor se reinicia automáticamente si la VM se reinicia, sin necesidad de un nuevo deploy manual.

Los tres secretos necesarios se añadieron en **GitHub → Settings → Secrets and variables → Actions**:

| Secreto | Contenido |
|---|---|
| `AZURE_SSH_KEY` | Contenido completo del `.pem` de Azure |
| `AZURE_VM_IP` | `20.208.47.230` |
| `AZURE_VM_USER` | `alexrp14` |

---

## Script `deploy.sh` (deploys manuales)

Se añade `scripts/deploy.sh` para casos en los que se necesite hacer un redeploy manual desde la VM sin esperar al pipeline (hotfix rápido, debug, etc.):

```bash
bash ~/deploy.sh            # pull + restart
bash ~/deploy.sh logs       # ver logs en tiempo real
bash ~/deploy.sh stop       # parar el servidor
bash ~/deploy.sh status     # estado del contenedor
```

---

## Flujo completo post-merge

```
PR merged → main
    └─▶ build-windows (SFML static .exe)
    └─▶ build-linux (AppImage)
         └─▶ release (GitHub Release con ambos artefactos)
    └─▶ docker-publish (imagen amd64+arm64 → GHCR)
              └─▶ deploy-azure (SSH → VM → pull + restart)
```

---

## Resultados

| | Antes | Después |
|---|---|---|
| **Deploy tras merge a main** | Manual (SSH + comandos) | Automático (< 2 min tras CI) |
| **IP de la VM** | Dinámica (cambia al reiniciar) | Estática (`20.208.47.230`) |
| **Reinicio tras reboot VM** | Manual | Automático (`--restart unless-stopped`) |
| **Tests** | 231 / 231 | 231 / 231 |

---

## Lección

El salto de "imagen publicada" a "servidor actualizado" es pequeño en código pero grande en fiabilidad operativa. Tener el deploy como parte del pipeline elimina la clase entera de bugs "funciona en CI pero el servidor sigue en la versión anterior". La IP estática es un requisito no negociable para que los secretos de GitHub Actions sean estables a largo plazo — el coste marginal en créditos Azure es despreciable comparado con el coste de depurar un pipeline roto por un cambio de IP silencioso.
