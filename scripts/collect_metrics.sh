#!/bin/bash
# collect_metrics.sh <rip|ospf|custom> <duracao_s> [--fail rA rB inicio_s duracao_s]
#
# Mesma metodologia para as tres solucoes (comparacao justa):
#   - pacotes/bytes de controle: tcpdump em cada roteador (container auxiliar
#     nicolaka/netshoot no namespace de rede do roteador), filtro do protocolo;
#   - tamanho da tabela: `ip -4 route` (tabela do kernel) a cada 2s;
#   - delay/conectividade: ping client1 (LAN1) -> client5 (LAN5) a cada 1s;
#   - falha de enlace opcional, com instantes registrados em events.csv.
#
# Saida: results/<proto>/{tables.csv,rtt.csv,events.csv,rX.pcap}
# Ex.:  ./scripts/collect_metrics.sh rip 120 --fail r1 r5 40 30

cd "$(dirname "$0")/.."
PROTO=${1:?rip|ospf|custom}; DURATION=${2:-120}
FAIL=""; [ "$3" = "--fail" ] && FAIL="$4 $5" && FAIL_AT=${6:-40} && FAIL_DUR=${7:-30}
ROUTERS="r1 r2 r3 r4 r5"
OUT="results/$PROTO"; rm -rf "$OUT"; mkdir -p "$OUT"

case "$PROTO" in
  rip)    FILTER="udp port 520" ;;
  ospf)   FILTER="ip proto 89" ;;
  custom) FILTER="udp port 5555" ;;
  *) echo "protocolo invalido"; exit 1 ;;
esac

echo "timestamp,router,table_size" > "$OUT/tables.csv"
echo "timestamp,rtt_ms"            > "$OUT/rtt.csv"
echo "timestamp,event,link"        > "$OUT/events.csv"

cleanup() {
  kill $PING_PID $TABLE_PID $FAIL_PID 2>/dev/null
  for r in $ROUTERS; do docker stop "cap_$r" >/dev/null 2>&1; done
}
trap cleanup EXIT

for r in $ROUTERS; do
  docker rm -f "cap_$r" >/dev/null 2>&1
  docker run -d --rm --name "cap_$r" --net "container:$r" -v "$PWD/$OUT":/cap \
    nicolaka/netshoot tcpdump -i any -nn -U -w "/cap/$r.pcap" "$FILTER" >/dev/null
done

( while true; do
    TS=$(date +%s)
    RTT=$(docker exec client1 ping -c 1 -W 1 192.168.5.10 2>/dev/null | sed -n 's/.*time=\([0-9.]*\).*/\1/p')
    echo "$TS,${RTT:-NaN}" >> "$OUT/rtt.csv"; sleep 1
  done ) & PING_PID=$!

( while true; do
    TS=$(date +%s)
    for r in $ROUTERS; do
      echo "$TS,$r,$(docker exec "$r" ip -4 route show 2>/dev/null | wc -l)" >> "$OUT/tables.csv"
    done; sleep 2
  done ) & TABLE_PID=$!

if [ -n "$FAIL" ]; then
  ( sleep "$FAIL_AT"; ./scripts/simulate_link_failure.sh $FAIL "$FAIL_DUR" "$OUT/events.csv" ) & FAIL_PID=$!
fi

echo "coletando $PROTO por ${DURATION}s -> $OUT"
sleep "$DURATION"
cleanup; trap - EXIT
echo "concluido: $OUT"
