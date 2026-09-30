# Comparação: RIP vs OSPF vs Algoritmo Próprio

## Metodologia

As três soluções rodam sobre a mesma topologia, **uma por vez**, e são
medidas pelo mesmo script (`scripts/collect_metrics.sh`):

- **Pacotes e taxa de controle**: `tcpdump` em cada roteador, contando
  apenas pacotes **enviados** (evita contar cada pacote duas vezes).
  Filtros: RIP `udp 520`, OSPF `ip proto 89`, próprio `udp 5555`.
- **Tamanho da tabela**: número de rotas na tabela do kernel (`ip route`),
  igual para as três soluções.
- **Delay**: ping client1 (LAN1) → client5 (LAN5) a cada 1 s.
- **Mudança de topologia**: queda do enlace R1-R5 aos 40 s por 30 s.
  O tempo sem conectividade é estimado pelos pings perdidos.

Parâmetros de tempo (influenciam diretamente os resultados):

| Solução  | Updates/Hello      | Detecção de falha do vizinho           |
|----------|--------------------|----------------------------------------|
| RIP      | 5 s (padrão: 30 s) | timeout 15 s, garbage 20 s             |
| OSPF     | hello 10 s (padrão)| dead 40 s (padrão)                     |
| Próprio  | 5 s + triggered    | timeout 18 s                           |

Como a falha é simulada derrubando a interface nas duas pontas, RIP e OSPF
detectam a queda **imediatamente** pelo evento de interface. O algoritmo
próprio, por decisão de projeto, só detecta por timeout. Esta diferença
deve aparecer no tempo sem conectividade e precisa ser discutida.

## 1. Princípio de funcionamento
- **RIP**: vetor de distâncias; cada roteador envia sua tabela aos vizinhos periodicamente (Bellman-Ford distribuído).
- **OSPF**: estado de enlace; cada roteador inunda LSAs, todos montam o mesmo mapa da rede e calculam a árvore de menor caminho (Dijkstra).
- **Algoritmo próprio**: vetor de distâncias com split horizon + poison reverse, triggered updates, anúncio explícito de rotas caídas (métrica 16) e expiração por timeout. Detalhes em `custom-algorithm/router.c`.

## 2. Forma de seleção de rotas
- **RIP**: menor número de saltos (máx. 15).
- **OSPF**: menor custo acumulado (custo = referência / banda da interface). Nesta topologia todos os enlaces têm o mesmo custo, então o resultado equivale a saltos. Suporta ECMP.
- **Algoritmo próprio**: menor custo acumulado, com custo de enlace configurável (default 1 = saltos). Anúncio do next-hop atual é sempre aceito, mesmo que piore. Anúncio de outro vizinho só é aceito se for estritamente melhor. Em empate, mantém a rota atual (evita oscilação). Redes locais nunca são substituídas.

## Resultados medidos

| Solução | Tabela (rotas) | Pacotes/s | kbit/s | RTT médio | Sem conectividade na falha |
|---|---|---|---|---|---|
| RIP | 15,73 | 4,06 | 8,70 | 0,17 ms | 2 s |
| OSPF | 21,30 | 3,92 | 2,77 | 0,16 ms | 0 s |
| Algoritmo próprio | 15,66 | 2,79 | 6,76 | 0,13 ms | 21 s |

Valores somados sobre os 5 roteadores (pacotes e banda) ou médios por roteador (tabela), em 120 s de coleta, com queda de R1-R5 aos 40 s por 30 s. Gráficos em `graficos/`.

## 3. Comportamento em mudança de topologia
- **OSPF (0 s)**: a queda da interface gera um novo LSA na hora e todos recalculam o SPF. O desvio para R1 → R3 → R4 → R5 ocorreu sem perda de pings. Porém, **na subida da rede** o OSPF foi o mais lento: foram necessários de 60 a 90 s até os vizinhos chegarem a *Full*, porque, nas redes broadcast do Docker, a eleição de DR/BDR espera o *dead interval* (40 s).
- **RIP (2 s)**: R1 perde a rota direta com a interface, mas o ripd do FRR só guarda a melhor rota. Por isso ele precisa esperar o próximo anúncio de R2/R3 (a cada 5 s) para aprender o caminho alternativo.
- **Algoritmo próprio (21 s)**: o kernel remove a rota junto com a interface, mas o algoritmo só invalida a rota por timeout (18 s sem anúncio de R5) e depois aceita o anúncio alternativo. Isso confirma o custo da decisão de projeto de não ter hello nem detecção por interface. A reconvergência em si funcionou (LAN5 via R3 com métrica 3).

## 4. Consumo de recursos de controle
- **OSPF, 2,77 kbit/s**: tem a menor banda, porque em regime só envia hellos pequenos a cada 10 s; LSAs só aparecem na falha. Em pacotes (3,92/s) fica próximo do RIP, porque envia hellos também nas LANs.
- **RIP, 8,70 kbit/s**: tem a maior banda, porque envia a tabela inteira a cada 5 s em todas as interfaces, inclusive nas LANs.
- **Algoritmo próprio, 2,79 pacotes/s e 6,76 kbit/s**: envia o menor número de pacotes, porque manda o vetor só para os vizinhos (unicast), nunca nas LANs. Os pacotes são grandes, porque o formato é texto.

## 5. Tamanho da tabela de roteamento
RIP (15,7) e o algoritmo próprio (15,7) têm o esperado: 16 rotas por roteador na tabela principal do kernel, com uma pequena queda durante a falha. O OSPF aparece com 21,3 porque instala **ECMP** (caminhos múltiplos de mesmo custo). Por exemplo, R1 alcança a LAN4 por R2 e por R3 ao mesmo tempo, e cada próximo salto conta como uma linha. Portanto, o número maior reflete redundância, não mais destinos.

## 6. Delay
O RTT médio ficou entre 0,13 e 0,17 ms nas três soluções. O protocolo só decide o caminho; o encaminhamento é feito sempre pelo kernel. Durante a falha o caminho passa de 2 para 4 saltos, mas, em containers na mesma máquina, a diferença fica na casa de microssegundos.

## 7. Escalabilidade e complexidade
- **RIP**: configuração mínima, mas limite de 15 saltos, convergência lenta e envio da tabela inteira a cada update. Adequado a redes pequenas.
- **OSPF**: exige mais configuração (áreas, router-id) e mais CPU/memória (LSDB + SPF), mas converge rápido e escala com áreas hierárquicas.
- **Algoritmo próprio**: cerca de 350 linhas de C, protocolo em texto (fácil de depurar). Herda as limitações de vetor de distâncias e não tem autenticação nem detecção ativa de vizinho (hello).

## 8. Adequação a diferentes cenários
- **Rede pequena e estável** (até algumas dezenas de roteadores, poucas mudanças), como um escritório ou um laboratório: RIP resolve com configuração mínima, e o algoritmo próprio teria desempenho parecido.
- **Rede média ou grande, ou com mudanças frequentes**, como um campus, um provedor ou um data center: OSPF, pela convergência rápida, pelo custo baseado em banda, pelo ECMP e pela hierarquia de áreas, que limita o flooding. O custo é mais configuração e mais memória/CPU por roteador.
- **Enlaces com bandas diferentes**: OSPF, ou o algoritmo próprio com custos por enlace no arquivo de config. RIP escolheria um caminho curto e lento no lugar de um mais longo e rápido.
- **Ambiente didático ou experimental**: o algoritmo próprio, porque o protocolo em texto facilita a observação (tcpdump) e a modificação da lógica. Não é adequado para produção, por não ter autenticação, não ter hello e detectar falhas lentamente.
- **Entre sistemas autônomos**: nenhum dos três. Esse é o papel do BGP.
