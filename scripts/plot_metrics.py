#!/usr/bin/env python3
"""
plot_metrics.py — graficos comparativos (item 7 da avaliacao).

Le results/<proto>/ gerado por collect_metrics.sh para cada cenario
(rip, ospf, custom) e gera em --outdir:

    tamanho_tabela.png    tamanho medio da tabela de rotas (kernel) no tempo
    pacotes_controle.png  pacotes de controle/s enviados (soma dos 5 roteadores)
    banda_controle.png    taxa de transmissao do controle (kbit/s)
    delay.png             RTT LAN1 -> LAN5, com a janela de falha destacada
    resumo.png / resumo.csv  medias e tempo sem conectividade apos a falha

Uso: python3 scripts/plot_metrics.py [--results results] [--outdir graficos]
So depende de matplotlib (os .pcap sao lidos com a biblioteca padrao).
"""
import argparse
import csv
import math
import os
import struct
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

NAMES = {"rip": "RIP", "ospf": "OSPF", "custom": "Algoritmo proprio"}
BIN = 5  # segundos por ponto nos graficos de controle


def read_pcap_outgoing(path):
    """Retorna [(timestamp, bytes_ip)] dos pacotes ENVIADOS pelo roteador.
    Suporta captura em '-i any' (Linux cooked SLL e SLL2)."""
    out = []
    with open(path, "rb") as f:
        gh = f.read(24)
        if len(gh) < 24:
            return out
        magic = gh[:4]
        if magic in (b"\xd4\xc3\xb2\xa1", b"\x4d\x3c\xb2\xa1"):
            end = "<"
        elif magic in (b"\xa1\xb2\xc3\xd4", b"\xa1\xb2\x3c\x4d"):
            end = ">"
        else:
            raise ValueError(f"{path}: formato pcap desconhecido")
        frac = 1e-9 if magic in (b"\x4d\x3c\xb2\xa1", b"\xa1\xb2\x3c\x4d") else 1e-6
        linktype = struct.unpack(end + "I", gh[20:24])[0] & 0xFFFF
        while True:
            rh = f.read(16)
            if len(rh) < 16:
                break
            sec, sub, incl, _orig = struct.unpack(end + "IIII", rh)
            data = f.read(incl)
            if linktype == 113:            # LINUX_SLL
                pkttype, hdr = struct.unpack(">H", data[0:2])[0], 16
            elif linktype == 276:          # LINUX_SLL2
                pkttype, hdr = data[10], 20
            else:                          # Ethernet ou outro: conta tudo
                pkttype, hdr = 4, 14
            if pkttype != 4 or len(data) < hdr + 4:   # 4 = PACKET_OUTGOING
                continue
            ip_len = struct.unpack(">H", data[hdr + 2:hdr + 4])[0]
            out.append((sec + sub * frac, ip_len))
    return out


def read_csv(path):
    if not os.path.exists(path):
        return []
    with open(path) as f:
        return list(csv.DictReader(f))


def load(results, proto):
    d = os.path.join(results, proto)
    if not os.path.isdir(d):
        return None
    tables = read_csv(os.path.join(d, "tables.csv"))
    rtt = read_csv(os.path.join(d, "rtt.csv"))
    events = read_csv(os.path.join(d, "events.csv"))
    stamps = [int(r["timestamp"]) for r in tables] + [int(r["timestamp"]) for r in rtt]
    if not stamps:
        return None
    t0, t1 = min(stamps), max(stamps)

    # tabela: media entre roteadores por instante
    by_ts = defaultdict(list)
    for r in tables:
        by_ts[int(r["timestamp"])].append(int(r["table_size"]))
    tab_t = sorted(by_ts)
    tab = ([t - t0 for t in tab_t], [sum(by_ts[t]) / len(by_ts[t]) for t in tab_t])

    # controle: soma dos 5 roteadores em janelas de BIN segundos
    pk, by = defaultdict(int), defaultdict(int)
    for fn in sorted(os.listdir(d)):
        if fn.endswith(".pcap"):
            for ts, n in read_pcap_outgoing(os.path.join(d, fn)):
                b = int((ts - t0) // BIN)
                pk[b] += 1
                by[b] += n
    nb = max(1, int((t1 - t0) // BIN))
    ctrl_t = [b * BIN for b in range(nb)]
    pps = [pk[b] / BIN for b in range(nb)]
    kbps = [by[b] * 8 / BIN / 1000 for b in range(nb)]

    rtt_t = [int(r["timestamp"]) - t0 for r in rtt]
    rtt_v = [float(r["rtt_ms"]) if r["rtt_ms"] not in ("", "NaN") else math.nan for r in rtt]

    ev = [(int(e["timestamp"]) - t0, e["event"]) for e in events]
    down = next((t for t, e in ev if e == "down"), None)
    up = next((t for t, e in ev if e == "up"), None)

    # tempo sem conectividade: pings perdidos a partir da falha x intervalo medio
    outage = None
    if down is not None and len(rtt_t) > 1:
        outage, start = 0.0, None
        for t, v in zip(rtt_t, rtt_v):
            if t < down:
                continue
            if math.isnan(v) and start is None:
                start = t
            elif not math.isnan(v) and start is not None:
                outage += t - start
                start = None
        if start is not None:
            outage += rtt_t[-1] - start

    valid = [v for v in rtt_v if not math.isnan(v)]
    return {
        "tab": tab, "ctrl_t": ctrl_t, "pps": pps, "kbps": kbps,
        "rtt": (rtt_t, rtt_v), "down": down, "up": up,
        "summary": {
            "tabela_media": sum(tab[1]) / len(tab[1]) if tab[1] else 0,
            "pacotes_s": sum(pps) / len(pps),
            "kbit_s": sum(kbps) / len(kbps),
            "rtt_medio_ms": sum(valid) / len(valid) if valid else math.nan,
            "sem_conectividade_s": outage if outage is not None else math.nan,
        },
    }


def line_plot(series, key, ylabel, title, path):
    fig, ax = plt.subplots(figsize=(8, 4.5))
    for name, s in series.items():
        x, y = s[key] if key in ("tab", "rtt") else (s["ctrl_t"], s[key])
        ax.plot(x, y, marker="o", markersize=3, label=name)
    first = next(iter(series.values()))
    if first["down"] is not None:
        ax.axvspan(first["down"], first["up"] if first["up"] is not None else first["down"] + 1,
                   color="red", alpha=0.08, label="enlace em falha")
    ax.set_xlabel("Tempo desde o inicio da coleta (s)")
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    ax.grid(True, alpha=0.3)
    ax.legend()
    fig.savefig(path, dpi=150, bbox_inches="tight")
    plt.close(fig)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", default="results")
    ap.add_argument("--outdir", default="graficos")
    args = ap.parse_args()
    os.makedirs(args.outdir, exist_ok=True)

    series = {}
    for proto, name in NAMES.items():
        s = load(args.results, proto)
        if s:
            series[name] = s
    if not series:
        raise SystemExit(f"nenhum resultado em {args.results}/(rip|ospf|custom)")

    o = args.outdir
    line_plot(series, "tab", "Rotas na tabela (media dos 5 roteadores)",
              "Tamanho da tabela de roteamento", os.path.join(o, "tamanho_tabela.png"))
    line_plot(series, "pps", "Pacotes de controle/s (rede toda)",
              "Pacotes de roteamento enviados", os.path.join(o, "pacotes_controle.png"))
    line_plot(series, "kbps", "kbit/s (rede toda)",
              "Taxa de transmissao usada pelo roteamento", os.path.join(o, "banda_controle.png"))
    line_plot(series, "rtt", "RTT (ms)", "Delay LAN1 -> LAN5", os.path.join(o, "delay.png"))

    keys = ["tabela_media", "pacotes_s", "kbit_s", "rtt_medio_ms", "sem_conectividade_s"]
    labels = ["Tabela (rotas)", "Pacotes/s", "kbit/s", "RTT medio (ms)", "Sem conectividade (s)"]
    with open(os.path.join(o, "resumo.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["solucao"] + keys)
        for name, s in series.items():
            w.writerow([name] + [f"{s['summary'][k]:.2f}" for k in keys])

    fig, axes = plt.subplots(1, len(keys), figsize=(16, 3.8))
    for ax, k, lab in zip(axes, keys, labels):
        vals = [series[n]["summary"][k] for n in series]
        bars = ax.bar(range(len(series)), [0 if math.isnan(v) else v for v in vals],
                      color=["C0", "C1", "C2"][:len(series)])
        ax.set_xticks(range(len(series)))
        ax.set_xticklabels([n.replace(" ", "\n") for n in series], fontsize=8)
        ax.set_title(lab, fontsize=10)
        for b, v in zip(bars, vals):
            ax.text(b.get_x() + b.get_width() / 2, b.get_height(),
                    "n/d" if math.isnan(v) else f"{v:.1f}", ha="center", va="bottom", fontsize=8)
    fig.tight_layout()
    fig.savefig(os.path.join(o, "resumo.png"), dpi=150, bbox_inches="tight")
    plt.close(fig)

    print(f"Graficos salvos em {o}/")
    with open(os.path.join(o, "resumo.csv")) as f:
        print(f.read())


if __name__ == "__main__":
    main()
