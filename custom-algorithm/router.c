/*
 * router.c - Algoritmo proprio de roteamento (vetor de distancias com
 * split horizon + poison reverse e triggered updates, no estilo RIP,
 * implementado do zero para fins didaticos).
 *
 * Logica de funcionamento:
 *   - Cada roteador conhece suas redes locais (LAN, loopback e enlaces) e
 *     seus vizinhos diretos (arquivo de config). Redes locais tem metrica 0
 *     e NUNCA sao substituidas por rotas aprendidas.
 *   - A cada UPDATE_INTERVAL segundos envia por UDP (porta 5555) o vetor de
 *     distancias completo para cada vizinho. Para rotas aprendidas atraves
 *     daquele proprio vizinho anuncia INFINITY (split horizon with poison
 *     reverse). Rotas invalidadas tambem sao anunciadas com INFINITY, para
 *     que os vizinhos descubram a queda sem esperar o timeout.
 *   - Ao receber um vetor (Bellman-Ford), para cada (destino, metrica):
 *       candidato = metrica + custo_do_enlace
 *       * se vem do next-hop atual: aceita o valor, mesmo que piore
 *         (o next-hop e a fonte da verdade sobre aquela rota);
 *         INFINITY vindo dele invalida a rota;
 *       * se vem de outro vizinho: so troca se for estritamente melhor
 *         (ou se a rota atual estiver invalida);
 *       * INFINITY vindo de outro vizinho e ignorado.
 *     Qualquer mudanca dispara um triggered update imediato.
 *   - Rotas sem confirmacao do next-hop por ROUTE_TIMEOUT segundos expiram
 *     (deteccao implicita de falha de enlace/vizinho).
 *   - Rotas validas sao aplicadas no kernel via `ip route replace`, e
 *     re-sincronizadas periodicamente (o kernel remove rotas quando uma
 *     interface cai; ao voltar, elas sao reinstaladas).
 *   - Metricas (tamanho da tabela, pacotes/bytes enviados, recebidos e
 *     triggered updates) sao gravadas em CSV a cada broadcast periodico.
 *
 * Criterio de selecao de rota: menor custo acumulado (custo de enlace
 * default 1 = numero de saltos, como no RIP), permitindo comparacao
 * direta com RIP sob a mesma metrica. Em empate, mantem a rota atual
 * (evita oscilacao).
 *
 * Uso: ./router <config_file> <log_csv_path>
 *
 * Variaveis de ambiente (para testes fora da topologia):
 *   ROUTER_BIND_IP=a.b.c.d   faz bind do socket nesse IP (default: todos)
 *   ROUTER_NO_KERNEL=1       nao altera a tabela de rotas do kernel
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <time.h>
#include "router.h"

static route_entry_t table[MAX_ROUTES];
static int table_count = 0;
static neighbor_t neighbors[MAX_NEIGHBORS];
static int neighbor_count = 0;
static char my_name[16];
static int sockfd;
static metrics_t metrics;
static pthread_mutex_t table_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t send_lock = PTHREAD_MUTEX_INITIALIZER;
static char log_path[256];
static char table_path[300];
static int no_kernel = 0;
static volatile sig_atomic_t stop_flag = 0;

#define COUNT(field, n) __atomic_fetch_add(&metrics.field, (n), __ATOMIC_RELAXED)
#define READ(field)     __atomic_load_n(&metrics.field, __ATOMIC_RELAXED)

/* ---------- kernel ---------- */
static void kernel_replace(const char *dest, const char *nh) {
    if (no_kernel) return;
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "ip route replace %.31s via %.15s 2>/dev/null", dest, nh);
    if (system(cmd) != 0) { /* vizinho pode estar inalcancavel; tenta de novo no sync */ }
}

static void kernel_delete(const char *dest) {
    if (no_kernel) return;
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "ip route del %.31s 2>/dev/null", dest);
    if (system(cmd) != 0) { /* rota ja nao existia */ }
}

/* ---------- metricas / snapshot da tabela ---------- */
static int valid_routes(void) {
    int n = 0;
    for (int i = 0; i < table_count; i++) if (table[i].valid) n++;
    return n;
}

static void log_metrics(void) {
    pthread_mutex_lock(&table_lock);
    int size = valid_routes();
    FILE *t = fopen(table_path, "w");
    if (t) {
        fprintf(t, "%-20s %-16s %-6s %s\n", "DESTINO", "NEXT-HOP", "METR", "VIA");
        for (int i = 0; i < table_count; i++) {
            if (!table[i].valid) continue;
            fprintf(t, "%-20s %-16s %-6d %s\n", table[i].destination,
                    table[i].metric == 0 ? "direto" : table[i].next_hop,
                    table[i].metric, table[i].metric == 0 ? "-" : table[i].iface_out);
        }
        fclose(t);
    }
    pthread_mutex_unlock(&table_lock);

    FILE *f = fopen(log_path, "a");
    if (!f) return;
    fprintf(f, "%ld,%s,%d,%ld,%ld,%ld,%ld\n", (long)time(NULL), my_name, size,
            READ(packets_sent), READ(bytes_sent), READ(packets_received),
            READ(triggered_updates));
    fclose(f);
}

/* ---------- tabela de roteamento ---------- */
/* Procura por destino, valido ou nao: cada destino ocupa um unico slot,
 * entao rotas que caem e voltam reutilizam o mesmo slot. */
static route_entry_t *find_route(const char *dest) {
    for (int i = 0; i < table_count; i++)
        if (strcmp(table[i].destination, dest) == 0) return &table[i];
    return NULL;
}

enum { K_NONE, K_REPLACE, K_DELETE };

/* Processa um anuncio (dest, metrica ja somada ao custo do enlace) vindo
 * do vizinho nb. Retorna 1 se a tabela mudou. */
static int process_advert(const char *dest, int candidate, neighbor_t *nb) {
    if (candidate > INFINITY_METRIC) candidate = INFINITY_METRIC;
    int changed = 0, kact = K_NONE;
    char kdest[32], knh[16];

    pthread_mutex_lock(&table_lock);
    route_entry_t *r = find_route(dest);
    time_t now = time(NULL);

    if (r && r->valid && r->metric == 0) {
        /* rede local: nunca substituida */
    } else if (r && r->valid && strcmp(r->next_hop, nb->ip) == 0) {
        /* anuncio do next-hop atual: e a fonte da verdade */
        r->last_update = now;
        if (candidate >= INFINITY_METRIC) {
            r->valid = 0;
            r->metric = INFINITY_METRIC;
            changed = 1; kact = K_DELETE;
        } else if (candidate != r->metric) {
            r->metric = candidate;
            changed = 1;          /* mesmo next-hop: kernel nao muda */
        }
    } else if (candidate < INFINITY_METRIC &&
               (!r || !r->valid || candidate < r->metric)) {
        /* rota nova, rota invalida sendo recuperada, ou caminho melhor */
        if (!r) {
            if (table_count >= MAX_ROUTES) { pthread_mutex_unlock(&table_lock); return 0; }
            r = &table[table_count++];
            memset(r, 0, sizeof(*r));
            snprintf(r->destination, sizeof(r->destination), "%s", dest);
        }
        snprintf(r->next_hop, sizeof(r->next_hop), "%s", nb->ip);
        snprintf(r->iface_out, sizeof(r->iface_out), "%s", nb->name);
        r->metric = candidate;
        r->valid = 1;
        r->last_update = now;
        changed = 1; kact = K_REPLACE;
    }
    /* demais casos: anuncio pior (ou INFINITY) de quem nao e o next-hop -> ignora */

    if (kact != K_NONE) {
        snprintf(kdest, sizeof(kdest), "%s", r->destination);
        snprintf(knh, sizeof(knh), "%s", r->next_hop);
    }
    pthread_mutex_unlock(&table_lock);

    if (kact == K_REPLACE) kernel_replace(kdest, knh);
    else if (kact == K_DELETE) kernel_delete(kdest);
    return changed;
}

/* Reinstala no kernel todas as rotas validas aprendidas (idempotente). */
static void kernel_sync(void) {
    if (no_kernel) return;
    char dests[MAX_ROUTES][32], nhs[MAX_ROUTES][16];
    int n = 0;
    pthread_mutex_lock(&table_lock);
    for (int i = 0; i < table_count; i++) {
        if (!table[i].valid || table[i].metric == 0) continue;
        memcpy(dests[n], table[i].destination, 32);
        memcpy(nhs[n], table[i].next_hop, 16);
        n++;
    }
    pthread_mutex_unlock(&table_lock);
    for (int i = 0; i < n; i++) kernel_replace(dests[i], nhs[i]);
}

/* ---------- envio (periodico / triggered) ---------- */
static void send_update_to(neighbor_t *n) {
    char buf[4096];
    int len = 0;
    pthread_mutex_lock(&table_lock);
    for (int i = 0; i < table_count && len < (int)sizeof(buf) - 48; i++) {
        int metric = table[i].metric;
        if (!table[i].valid)
            metric = INFINITY_METRIC;                  /* rota caida */
        else if (strcmp(table[i].next_hop, n->ip) == 0)
            metric = INFINITY_METRIC;                  /* poison reverse */
        len += snprintf(buf + len, sizeof(buf) - len, "%s %d\n",
                        table[i].destination, metric);
    }
    pthread_mutex_unlock(&table_lock);

    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(UDP_PORT);
    inet_pton(AF_INET, n->ip, &dst.sin_addr);
    if (sendto(sockfd, buf, len, 0, (struct sockaddr *)&dst, sizeof(dst)) > 0) {
        COUNT(packets_sent, 1);
        COUNT(bytes_sent, len + 28);                  /* payload + IP + UDP */
    }
}

static void broadcast_update(void) {
    pthread_mutex_lock(&send_lock);
    for (int i = 0; i < neighbor_count; i++) send_update_to(&neighbors[i]);
    pthread_mutex_unlock(&send_lock);
}

static void triggered_update(void) {
    COUNT(triggered_updates, 1);
    broadcast_update();
}

static void *sender_thread(void *arg) {
    (void)arg;
    while (!stop_flag) {
        sleep(UPDATE_INTERVAL);
        broadcast_update();
        kernel_sync();
        log_metrics();
    }
    return NULL;
}

/* ---------- expiracao das rotas (falha implicita) ---------- */
static void *timeout_thread(void *arg) {
    (void)arg;
    while (!stop_flag) {
        sleep(1);
        char expired[MAX_ROUTES][32];
        int n = 0;
        time_t now = time(NULL);
        pthread_mutex_lock(&table_lock);
        for (int i = 0; i < table_count; i++) {
            if (table[i].valid && table[i].metric > 0 &&
                now - table[i].last_update > ROUTE_TIMEOUT) {
                table[i].valid = 0;
                table[i].metric = INFINITY_METRIC;
                memcpy(expired[n++], table[i].destination, 32);
            }
        }
        pthread_mutex_unlock(&table_lock);
        for (int i = 0; i < n; i++) kernel_delete(expired[i]);
        if (n > 0) triggered_update();
    }
    return NULL;
}

/* ---------- recepcao ---------- */
static neighbor_t *neighbor_by_ip(const char *ip) {
    for (int i = 0; i < neighbor_count; i++)
        if (strcmp(neighbors[i].ip, ip) == 0) return &neighbors[i];
    return NULL;
}

static void *receiver_thread(void *arg) {
    (void)arg;
    char buf[4096];
    while (!stop_flag) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        int n = recvfrom(sockfd, buf, sizeof(buf) - 1, 0,
                         (struct sockaddr *)&src, &slen);
        if (n <= 0) continue;
        buf[n] = '\0';

        char ip[16];
        inet_ntop(AF_INET, &src.sin_addr, ip, sizeof(ip));
        neighbor_t *nb = neighbor_by_ip(ip);
        if (!nb) continue;                  /* ignora origem desconhecida */
        COUNT(packets_received, 1);

        int any_change = 0;
        char *save = NULL;
        for (char *line = strtok_r(buf, "\n", &save); line;
             line = strtok_r(NULL, "\n", &save)) {
            char dest[32];
            int metric;
            if (sscanf(line, "%31s %d", dest, &metric) == 2 && metric >= 0)
                any_change |= process_advert(dest, metric + nb->link_cost, nb);
        }
        if (any_change) triggered_update();
    }
    return NULL;
}

/* ---------- configuracao ---------- */
static void load_config(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { perror("config"); exit(1); }
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        char tag[4] = "";
        if (sscanf(line, "%3s", tag) != 1 || tag[0] == '#') continue;
        if (strcmp(tag, "R") == 0) {
            sscanf(line, "R %15s", my_name);
        } else if (strcmp(tag, "L") == 0 && table_count < MAX_ROUTES) {
            route_entry_t *r = &table[table_count++];
            memset(r, 0, sizeof(*r));
            sscanf(line, "L %31s", r->destination);
            strcpy(r->next_hop, "0.0.0.0");
            r->valid = 1;
            r->last_update = time(NULL);
        } else if (strcmp(tag, "N") == 0 && neighbor_count < MAX_NEIGHBORS) {
            neighbor_t *nb = &neighbors[neighbor_count++];
            int cost = 1;
            sscanf(line, "N %15s %15s %d", nb->name, nb->ip, &cost);
            nb->link_cost = cost > 0 ? cost : 1;
        }
    }
    fclose(f);
}

static void on_signal(int sig) { (void)sig; stop_flag = 1; }

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "uso: %s <config_file> <log_csv_path>\n", argv[0]);
        return 1;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    load_config(argv[1]);
    snprintf(log_path, sizeof(log_path), "%s", argv[2]);
    snprintf(table_path, sizeof(table_path), "%s.table", argv[2]);
    no_kernel = getenv("ROUTER_NO_KERNEL") != NULL;

    FILE *f = fopen(log_path, "w");
    if (f) {
        fprintf(f, "timestamp,router,table_size,packets_sent,bytes_sent,packets_received,triggered_updates\n");
        fclose(f);
    }
    metrics.start_time = time(NULL);

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    int one = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in me = {0};
    me.sin_family = AF_INET;
    me.sin_port = htons(UDP_PORT);
    const char *bind_ip = getenv("ROUTER_BIND_IP");
    if (bind_ip) inet_pton(AF_INET, bind_ip, &me.sin_addr);
    else me.sin_addr.s_addr = INADDR_ANY;
    if (bind(sockfd, (struct sockaddr *)&me, sizeof(me)) < 0) { perror("bind"); return 1; }

    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    printf("[%s] algoritmo proprio iniciado: %d redes locais, %d vizinhos\n",
           my_name, table_count, neighbor_count);

    pthread_t t_send, t_recv, t_timeout;
    pthread_create(&t_send, NULL, sender_thread, NULL);
    pthread_create(&t_recv, NULL, receiver_thread, NULL);
    pthread_create(&t_timeout, NULL, timeout_thread, NULL);

    broadcast_update();                                /* anuncio inicial */
    log_metrics();

    while (!stop_flag) sleep(1);

    /* saida limpa: remove do kernel as rotas instaladas por este processo */
    pthread_mutex_lock(&table_lock);
    for (int i = 0; i < table_count; i++)
        if (table[i].valid && table[i].metric > 0) kernel_delete(table[i].destination);
    pthread_mutex_unlock(&table_lock);
    printf("[%s] encerrado\n", my_name);
    return 0;
}
