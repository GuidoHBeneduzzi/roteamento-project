#ifndef ROUTER_H
#define ROUTER_H

#include <netinet/in.h>
#include <time.h>

#define MAX_ROUTES      64
#define MAX_NEIGHBORS   8
#define MAX_LINE        256
#define UDP_PORT        5555
#define INFINITY_METRIC 16   /* mesmo limite do RIP: limita a contagem ao infinito */
#define UPDATE_INTERVAL 5    /* segundos entre broadcasts periodicos */
#define ROUTE_TIMEOUT   18   /* segundos sem confirmacao -> rota expira */

/* Uma entrada da tabela de roteamento (vetor de distancias) */
typedef struct {
    char     destination[32];  /* prefixo, ex "192.168.3.0/24" */
    char     next_hop[16];     /* IP do vizinho ("0.0.0.0" = rede local) */
    int      metric;           /* custo acumulado (0 = rede local) */
    char     iface_out[16];    /* nome do vizinho usado como saida */
    time_t   last_update;      /* timestamp da ultima confirmacao */
    int      valid;            /* 0 = rota invalidada (anunciada com INFINITY) */
} route_entry_t;

/* Um vizinho diretamente conectado */
typedef struct {
    char     name[16];         /* ex: "r2" */
    char     ip[16];           /* IP do vizinho no enlace ponto-a-ponto */
    int      link_cost;        /* custo do enlace (default 1) */
} neighbor_t;

/* Contadores usados para as metricas pedidas no trabalho */
typedef struct {
    long packets_sent;
    long bytes_sent;
    long packets_received;
    long triggered_updates;
    time_t start_time;
} metrics_t;

#endif
