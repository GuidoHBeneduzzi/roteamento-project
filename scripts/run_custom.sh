#!/bin/bash
# run_custom.sh start|stop|show [roteador]
# Controla o algoritmo proprio nos containers (cenario PROTO=custom).
cd "$(dirname "$0")/.."
ROUTERS="r1 r2 r3 r4 r5"
mkdir -p custom-algorithm/logs
case "$1" in
  start)
    for r in $ROUTERS; do
      docker exec -d "$r" sh -c "/opt/custom/router /opt/custom/configs/$r.conf /opt/custom/logs/$r.csv > /opt/custom/logs/$r.out 2>&1"
    done
    echo "algoritmo iniciado em: $ROUTERS (logs em custom-algorithm/logs/)" ;;
  stop)
    for r in $ROUTERS; do docker exec "$r" pkill -f /opt/custom/router 2>/dev/null; done
    echo "algoritmo parado" ;;
  show)
    for r in ${2:-$ROUTERS}; do echo "== $r"; cat "custom-algorithm/logs/$r.csv.table" 2>/dev/null; done ;;
  *) echo "uso: $0 start|stop|show [roteador]"; exit 1 ;;
esac
