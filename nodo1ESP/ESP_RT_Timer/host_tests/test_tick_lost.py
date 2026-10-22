#!/usr/bin/env python3
"""R2: TICK_LOST con inyección de fallos.

SOLO funciona con el firmware del entorno esp32dev_fault (compilado con
-DRT_FAULT_INJECT), que bloquea la tarea calendarizadora tras procesar los ticks 150
y 580. Con el firmware de producción este script debe FALLAR (control negativo): no hay
TICK_LOST.

Qué comprueba:
  - Dos TICK_LOST exactos: (tick 153, 2 ticks perdidos) y (tick 620, 39 ticks perdidos).
  - El flujo de eventos (con el salto de tiempo lógico) es idéntico al modelo.
  - Los TickReport salen en los ticks esperados (sin ráfagas tras el salto) y el
    contador ticks_perdidos acumulado es el esperado.
  - El nodo conserva el ritmo real y no vuelve a perder ticks.

Uso (con el monitor de PlatformIO cerrado):
    ~/venvs/ce4303/bin/python host_tests/test_tick_lost.py
"""
import argparse
import struct
import sys
import time

import test_start_stop as base
from test_start_stop import (CAL_REQ, CAL_RESP, DEPLOY, START, STOP, Node,
                             check, expect_ack)
from test_events import first_difference, RELEASE, SWITCH, COMPLETE, MISS, TICK_LOST

BASE = [(1, 1, 4, 4, 1), (2, 5, 10, 10, 1)]   # (id, C, T, D, criticidad)
N_REPORT = 100
# (tick tras el que se bloquea el nodo, ticks que se pierden). Deben coincidir con el firmware.
STALLS = [(150, 2), (580, 39)]


def simulate(tasks, alg, nticks, stalls):
    """Modelo del calendarizador con saltos: los ticks bloqueados no se procesan y,
    al reanudar, se emite TICK_LOST (tick = el de reanudación, arg = ticks perdidos)."""
    resume = {k + lost + 1: lost for k, lost in stalls}
    skipped = {t for k, lost in stalls for t in range(k + 1, k + 1 + lost)}
    st = [dict(id=t[0], C=t[1], T=t[2], D=t[3], crit=t[4], active=False,
               rem=0, dl=0, nxt=0, seq=0) for t in tasks]
    events, running = [], 0
    for k in range(nticks):
        if k in skipped:
            continue
        if k in resume:
            events.append((TICK_LOST, k, 0, resume[k]))
        for t in st:
            if t["active"] and t["dl"] <= k:
                events.append((MISS, k, t["id"], t["seq"] & 0xFFFF))
                t["active"], t["rem"] = False, 0
        for t in st:
            if t["nxt"] <= k:
                if t["active"]:
                    events.append((MISS, k, t["id"], t["seq"] & 0xFFFF))
                t["seq"] += 1
                t["active"], t["rem"], t["dl"] = True, t["C"], k + t["D"]
                while t["nxt"] <= k:
                    t["nxt"] += t["T"]
                events.append((RELEASE, k, t["id"], t["seq"] & 0xFFFF))
        ready = [t for t in st if t["active"]]
        key = (lambda t: (t["T"], -t["crit"], t["id"])) if alg == 0 else \
              (lambda t: (t["dl"], -t["crit"], t["id"]))
        chosen = min(ready, key=key) if ready else None
        new_id = chosen["id"] if chosen else 0
        if new_id != running:
            events.append((SWITCH, k, new_id, running))
            running = new_id
        if chosen:
            chosen["rem"] -= 1
            if chosen["rem"] == 0:
                chosen["active"] = False
                events.append((COMPLETE, k + 1, chosen["id"], chosen["seq"] & 0xFFFF))
    return events


def expected_report_ticks(nticks, n, stalls):
    """Réplica de tick_report_due(): un reporte al llegar a 'next'; tras un salto sale uno solo."""
    skipped = {t for k, lost in stalls for t in range(k + 1, k + 1 + lost)}
    out, nxt = [], n
    for k in range(nticks):
        if k in skipped:
            continue
        if k >= nxt:
            out.append(k)
            nxt = (k // n + 1) * n
    return out


def lost_until(tick, stalls):
    return sum(lost for k, lost in stalls if k + lost + 1 <= tick)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/cu.usbserial-0001")
    ap.add_argument("--tick", type=int, default=10000)
    ap.add_argument("--ticks", type=int, default=1000, help="ticks a comparar con el modelo")
    args = ap.parse_args()
    tick_s = args.tick / 1e6
    run_s = args.ticks * tick_s + 0.5 + 0.6   # + bloqueos (~0,43 s a 10 ms) + margen

    node = Node(args.port)
    node.drain()
    print(f"Puerto {args.port}, tick {args.tick} us, {args.ticks} ticks, bloqueos {STALLS}")

    resp = node.request(CAL_REQ, struct.pack("<I", args.tick), timeout=args.tick * 50 / 1e6 + 5)
    if resp is None or resp[0] != CAL_RESP or len(resp[1]) != 4:
        print("  FALLO calibración", resp)
        return 1
    sn = struct.unpack("<I", resp[1])[0]
    print(f"[0] ut_por_tick = {sn}")
    check("ut_por_tick > 0", sn > 0)

    text = "".join(f"{i},P,{c * sn},{t},{d},{k}\n" for i, c, t, d, k in BASE).encode()
    expect_ack("despliegue", node.request(DEPLOY, text), DEPLOY, 0)
    node.events.clear()
    node.reports.clear()
    node.bad_event_frames = node.bad_report_frames = 0
    ack = node.request(START, bytes([1, N_REPORT & 0xFF, N_REPORT >> 8]))
    t0 = time.monotonic()
    expect_ack(f"Start (EDF, N={N_REPORT})", ack, START, 0)
    node.pump(run_s)
    t_stop = time.monotonic()
    expect_ack("Stop", node.request(STOP, timeout=5.0), STOP, 0)
    n_ev, n_rep = len(node.events), len(node.reports)
    node.pump(0.7)
    check("nada después del ACK de Stop",
          len(node.events) == n_ev and len(node.reports) == n_rep)
    events, reports = list(node.events[:n_ev]), list(node.reports[:n_rep])

    print("\n[1] TICK_LOST")
    lost_events = [e for e in events if e[0] == TICK_LOST]
    want_lost = [(TICK_LOST, k + lost + 1, 0, lost) for k, lost in STALLS]
    print(f"        recibidos: {lost_events}")
    check("exactamente los TICK_LOST esperados", lost_events == want_lost,
          f"(esperados {want_lost}; si no llegó ninguno, el firmware no es el de esp32dev_fault)")
    check("tramas Event y TickReport bien formadas",
          node.bad_event_frames == 0 and node.bad_report_frames == 0)

    print("\n[2] Flujo contra el modelo con saltos")
    got = [e for e in events if e[1] < args.ticks]
    want = [e for e in simulate(BASE, 1, args.ticks + 1, STALLS) if e[1] < args.ticks]
    diff = first_difference(got, want)
    check(f"flujo idéntico al modelo ({len(want)} eventos)", diff == "", diff)
    near = [e for e in events if e[0] == MISS and e[1] in {k + lost + 1 for k, lost in STALLS}]
    print(f"        plazos perdidos al reanudar (por el salto de tiempo): {near}")

    print("\n[3] TickReport")
    check("hay reportes", len(reports) > 0)
    if reports:
        ticks = [r["tick"] for r in reports]
        want_ticks = expected_report_ticks(max(ticks) + 1, N_REPORT, STALLS)
        print(f"        ticks de los reportes alrededor de los saltos: "
              f"{[t for t in ticks if 100 <= t <= 700]}")
        check("reportes en los ticks esperados (un solo reporte tras el salto, sin ráfagas)",
              ticks == want_ticks, f"(recibidos {ticks[:12]}...; esperados {want_ticks[:12]}...)")
        bad_lost = [(r["tick"], r["ticks_lost"], lost_until(r["tick"], STALLS)) for r in reports
                    if r["ticks_lost"] != lost_until(r["tick"], STALLS)]
        check("ticks_perdidos acumulado correcto en cada reporte", not bad_lost,
              f"((tick, recibido, esperado): {bad_lost[:4]})")
        miss_ticks = [e[1] for e in events if e[0] == MISS]
        bad_miss = [r["tick"] for r in reports
                    if r["misses"] != sum(1 for t in miss_ticks if t <= r["tick"])]
        check("plazos_perdidos del reporte = MISS recibidos hasta ese tick", not bad_miss,
              f"(difieren en {bad_miss[:4]})")
        last = reports[-1]
        print(f"        último reporte: tick={last['tick']} plazos={last['misses']} "
              f"ticks_perdidos={last['ticks_lost']} descartados={last['events_dropped']} "
              f"max_ocupado={last['max_busy_us']} us max_latencia={last['max_latency_us']} us")
        check("sin descartes de eventos", last["events_dropped"] == 0)
        elapsed = t_stop - t0
        check("ritmo real tras los saltos (tick del último reporte ~ tiempo transcurrido)",
              elapsed - N_REPORT * tick_s - 0.3 <= last["tick"] * tick_s <= elapsed + 0.3,
              f"(tick {last['tick']} = {last['tick'] * tick_s:.2f} s; transcurrido {elapsed:.2f} s)")
        check("el bloqueo no cuenta en max_ocupado (< 90 % del tick)",
              last["max_busy_us"] < 0.9 * args.tick, f"({last['max_busy_us']} us)")

    print(f"\nResultado: {base.passed} PASS, {base.failed} FALLO")
    return 0 if base.failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())