# Comparação: RIP vs OSPF vs Algoritmo Próprio

> As seções 3 a 6 devem ser preenchidas com os números de
> `graficos/resumo.csv` e com os gráficos gerados em `graficos/`.

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

## 3. Comportamento em mudança de topologia
Medido: RIP **[__] s**, OSPF **[__] s**, próprio **[__] s** sem conectividade (`graficos/delay.png`).

Comportamento esperado, para comparar com o medido:
- **OSPF**: a queda da interface gera novo LSA imediatamente e todos recalculam o SPF, então a perda deve ser de poucos segundos ou nenhuma.
- **RIP**: R1 perde a rota direta na hora e passa a usar o anúncio de 3 saltos já recebido de R2/R3. O triggered update do FRR propaga a mudança, então a perda também deve ser curta. Split horizon evita a contagem ao infinito nessa malha.
- **Próprio**: R1 remove a rota do kernel na queda da interface, mas a tabela do algoritmo só invalida a rota após o timeout (até 18 s). Durante esse tempo, os pacotes para LAN5 são descartados. É a maior perda esperada, e é consequência direta da decisão de não ter hello nem detecção por interface.

## 4. Consumo de recursos de controle
Medido: RIP **[__] pkt/s / [__] kbit/s**, OSPF **[__] / [__]**, próprio **[__] / [__]**.

Esperado: em regime, o OSPF envia só hellos a cada 10 s, com LSAs apenas em mudanças ou no refresh de 30 min, então deve ter o menor custo. RIP e o algoritmo próprio enviam a tabela inteira a cada 5 s para cada vizinho. O próprio tende a usar mais bytes, porque o formato é texto e inclui rotas caídas e envenenadas. Todos os três mostram picos na falha, com LSAs e triggered updates.

## 5. Tamanho da tabela de roteamento
Medido: média de **[__]** rotas por roteador.

Esperado: as três soluções aprendem os mesmos 17 prefixos (5 LANs, 7 enlaces e 5 loopbacks). Diferenças pequenas vêm de detalhes, como o ECMP do OSPF, e da queda temporária de rotas durante a falha.

## 6. Delay
Medido: RTT médio **[__] ms** (RIP), **[__] ms** (OSPF), **[__] ms** (próprio).

O protocolo não altera o encaminhamento em si, que é sempre feito pelo kernel. Ele só decide o caminho. Portanto, o RTT deve ser equivalente nos três casos, subindo levemente enquanto R1-R5 está fora (3 saltos em vez de 1). Em containers, as diferenças ficam na casa de décimos de ms.

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
