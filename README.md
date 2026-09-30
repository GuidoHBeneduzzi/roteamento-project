# Trabalho I — Roteamento IP (5 roteadores: RIP × OSPF × algoritmo próprio)

Ambiente experimental em **Docker + FRRouting 9.1** (fork mantido do
Quagga, uma das plataformas sugeridas no enunciado) com 5 roteadores em
anel e 2 diagonais, cada um com sua LAN. Sobre a mesma topologia são
comparados **RIP**, **OSPF** e um **algoritmo próprio** de vetor de
distâncias em C. As três soluções **nunca rodam ao mesmo tempo**.

- Topologia física e lógica: [`topology/topology.md`](topology/topology.md)
- Análise comparativa: [`docs/comparacao.md`](docs/comparacao.md)
- Vídeo de demonstração: **(adicionar link)**

## Estrutura

```
docker-compose.yml         5 roteadores + client1 (LAN1) e client5 (LAN5)
configs/{rip,ospf,custom}/ frr.conf + daemons de cada roteador por cenário
custom-algorithm/          algoritmo próprio (router.c, router.h, configs/)
scripts/
  build_custom.sh          compila o algoritmo (binário estático p/ os containers)
  run_custom.sh            start | stop | show do algoritmo nos roteadores
  simulate_link_failure.sh derruba/restaura um enlace (ex.: r1 r5 30)
  collect_metrics.sh       coleta métricas (+ falha de enlace opcional)
  plot_metrics.py          gera os gráficos comparativos
docs/comparacao.md         análise escrita
```

## Requisitos

Docker com Compose v2 (Linux, ou Docker Desktop no Windows/macOS) e
Python 3 com matplotlib. O algoritmo próprio é compilado dentro de um
container, então não é preciso ter GCC no host.

## 1. Cenário RIP

```bash
PROTO=rip docker compose up -d
docker exec r1 vtysh -c "show ip route"          # rotas R (RIP)
docker exec client1 ping -c 3 192.168.5.10       # LAN1 -> LAN5
docker exec client1 traceroute 192.168.5.10      # caminho: r1 -> r5
docker compose down
```

## 2. Cenário OSPF

```bash
PROTO=ospf docker compose up -d
docker exec r1 vtysh -c "show ip ospf neighbor"  # 3 vizinhos em Full
docker exec r1 vtysh -c "show ip route"          # rotas O (OSPF)
docker compose down
```

## 3. Cenário do algoritmo próprio

```bash
./scripts/build_custom.sh                        # uma vez
PROTO=custom docker compose up -d                # FRR sem protocolo algum
./scripts/run_custom.sh start
./scripts/run_custom.sh show r1                  # tabela do algoritmo
docker exec r1 ip route                          # rotas instaladas no kernel
docker exec client1 ping -c 3 192.168.5.10
./scripts/run_custom.sh stop
docker compose down
```

### Como funciona o algoritmo próprio

Vetor de distâncias com **split horizon + poison reverse** e **triggered
updates**. Cada roteador envia seu vetor completo por UDP (porta 5555) a
cada 5 s. As regras de seleção de rota são:

- redes locais (métrica 0) nunca são substituídas;
- um anúncio do **next-hop atual** é sempre aceito, mesmo que piore, e
  métrica 16 vinda dele invalida a rota;
- um anúncio de **outro vizinho** só é aceito se for estritamente melhor;
  em empate, mantém a rota atual para evitar oscilação;
- rotas caídas são anunciadas com métrica 16, para propagar a falha rápido;
- rotas sem confirmação por 18 s expiram (falha implícita).

As rotas vão para o kernel via `ip route replace` e são ressincronizadas
a cada 5 s. A justificativa completa das decisões de projeto está no
comentário no topo de [`custom-algorithm/router.c`](custom-algorithm/router.c).

Teste isolado, sem Docker (5 instâncias em 127.0.0.1–5):

```bash
cd custom-algorithm && make
for i in 1 2 3 4 5; do
  sed -E 's/^N r([0-9]) [0-9.]+ /N r\1 127.0.0.\1 /' configs/r$i.conf > /tmp/r$i.conf
  ROUTER_NO_KERNEL=1 ROUTER_BIND_IP=127.0.0.$i ./router /tmp/r$i.conf /tmp/r$i.csv &
done
sleep 8; cat /tmp/r1.csv.table
```

## 4. Coleta de métricas e falha de enlace

Com um cenário no ar (e, no caso `custom`, com o algoritmo iniciado), rode:

```bash
./scripts/collect_metrics.sh rip 120 --fail r1 r5 40 30
```

Esse comando coleta por 120 s e derruba o enlace R1-R5 aos 40 s, por 30 s.
Troque `rip` por `ospf` ou `custom` conforme o cenário. Os resultados vão
para `results/<proto>/`:

- `rX.pcap`: pacotes de controle de cada roteador;
- `tables.csv`: tamanho da tabela de roteamento;
- `rtt.csv`: delay LAN1 → LAN5;
- `events.csv`: instantes em que o enlace caiu e voltou.

A captura usa a imagem `nicolaka/netshoot`, que é baixada automaticamente.

A falha também pode ser disparada sozinha:
`./scripts/simulate_link_failure.sh r1 r5 30`.

## 5. Gráficos

```bash
python3 scripts/plot_metrics.py        # lê results/, grava em graficos/
```

Gera os seguintes arquivos em `graficos/`:

- `tamanho_tabela.png`
- `pacotes_controle.png`
- `banda_controle.png` (taxa de transmissão do controle)
- `delay.png`
- `resumo.png` e `resumo.csv` (inclui o tempo sem conectividade após a falha)

## Solução de problemas

- **Ver as interfaces:** `docker exec r1 ip -br addr`. O Docker não garante
  a ordem `ethX`, por isso as configurações não dependem dos nomes.
- **Sem rota:** confira `docker exec r1 sysctl net.ipv4.ip_forward`, que
  deve ser 1, e se a rota default do Docker foi removida
  (`docker exec r1 ip route`).
- **Trocar de cenário:** sempre rode `docker compose down` antes de subir
  outro `PROTO`.
