#!/bin/bash
# simulate_link_failure.sh <rA> <rB> <segundos_down> [events.csv]
#
# Derruba o enlace entre rA e rB (as duas pontas) por N segundos e restaura.
# A interface e localizada pelo IP do enlace, pois o Docker nao garante a
# ordem eth0..ethN. Registra os instantes em events.csv (se informado).
#
# Exemplo: ./scripts/simulate_link_failure.sh r1 r5 30
#   (derruba R1-R5, que e o caminho direto LAN1 -> LAN5)

A=${1:?informe o roteador A, ex: r1}
B=${2:?informe o roteador B, ex: r5}
DOWN=${3:-30}
EVENTS=${4:-}

# sub-rede de cada enlace (ordem dos roteadores nao importa)
case "$(echo "$A $B" | tr ' ' '\n' | sort | tr -d '\n')" in
  r1r2) NET=10.0.12. ;; r2r3) NET=10.0.23. ;; r3r4) NET=10.0.34. ;;
  r4r5) NET=10.0.45. ;; r1r5) NET=10.0.51. ;; r1r3) NET=10.0.13. ;;
  r2r4) NET=10.0.24. ;;
  *) echo "nao existe enlace entre $A e $B"; exit 1 ;;
esac

iface() { docker exec "$1" ip -o -4 addr show | awk -v p="$NET" 'index($4,p)==1 {print $2}'; }
IA=$(iface "$A"); IB=$(iface "$B")
[ -z "$IA" ] || [ -z "$IB" ] && { echo "interface do enlace $NET nao encontrada"; exit 1; }

log() { echo "[$(date +%T)] $2 enlace $A-$B ($A:$IA, $B:$IB)"; [ -n "$EVENTS" ] && echo "$1,$2,$A-$B" >> "$EVENTS"; true; }

docker exec "$A" ip link set "$IA" down; docker exec "$B" ip link set "$IB" down
log "$(date +%s)" down
sleep "$DOWN"
docker exec "$A" ip link set "$IA" up;   docker exec "$B" ip link set "$IB" up
log "$(date +%s)" up
