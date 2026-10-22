#!/usr/bin/env python3
"""R1: estrés a 1 ms y sobrecarga del nodo ESP32 (sin cambios al firmware).

Casos:
  [1] Carga factible a 1 ms: flujo contra el modelo, ocupación máxima, sin pérdidas.
  [2] Sobrecarga (8 tareas con T = D = 1): búfer de eventos lleno, contadores
      saturados, el nodo sigue vivo; latencia real del ACK de Stop.
  [3] Reinicio limpio tras la sobrecarga (también detecta un reinicio de la placa:
      si la calibración se perdió, Start respondería 3).

Uso (con el monitor de PlatformIO cerrado):
    ~/venvs/ce4303/bin/python host_tests/test_stress.py
    ~/venvs/ce4303/bin/python host_tests/test_stress.py --overload-seconds 6
"""
import argparse
import struct
import sys
import time

import test_start_stop as base
from test_start_stop import (CAL_REQ, CAL_RESP, DEPLOY, START, STOP, Node,
                             check, expect_ack)
from test_events import (simulate, first_difference, RELEASE, SWITCH,
                         COMPLETE, MISS, TICK_LOST)

# (id, costo en ticks C, T, D, criticidad). U = 0.4, pocos eventos por tick.
LIGHT = [(1, 2, 10, 10, 1), (2, 4, 20, 20, 1)]
N_REPORT = 100


def deploy_text(tasks, sn):
    return "".join(f"{i},P,{c * sn},{t},{d},{k}\n" for i, c, t, d, k in tasks).encode()


def overload_text(sn):
    # 8 tareas con C = 1 y T = D = 1: cada tick libera 8 y solo una avanza.
    return "".join(f"{i},P,{sn},1,1,1\n" for i in range(1, 9)).encode()


def begin(node, alg, n):
    node.events.clear()
    node.reports.clear()
    node.bad_event_frames = node.bad_report_frames = 0
    ack = node.request(START, bytes([alg, n & 0xFF, n >> 8]))
    t0 = time.monotonic()
    expect_ack(f"Start (N={n})", ack, START, 0)
    return t0


def finish(node):
    t_stop = time.monotonic()
    ack = node.request(STOP, timeout=10.0)
    latency = time.monotonic() - t_stop
    expect_ack("Stop", ack, STOP, 0)
    n_ev, n_rep = len(node.events), len(node.reports)
    node.pump(0.7)
    check("nada después del ACK de Stop",
          len(node.events) == n_ev and len(node.reports) == n_rep,
          f"({len(node.events) - n_ev} eventos y {len(node.reports) - n_rep} reportes tardíos)")
    return list(node.events[:n_ev]), list(node.reports[:n_rep]), latency, t_stop


def show(r):
    print(f"        reporte: tick={r['tick']} plazos={r['misses']} "
          f"ignoradas={r['sporadic_ignored']} ticks_perdidos={r['ticks_lost']} "
          f"descartados={r['events_dropped']} tramas_malas={r['bad_frames']} "
          f"max_ocupado={r['max_busy_us']} us max_latencia={r['max_latency_us']} us")


def pace(events, t0, t_stop, tick_s):
    last = max(e[1] for e in events)
    elapsed = t_stop - t0
    return (elapsed - 0.6 <= last * tick_s <= elapsed + 0.3), last, elapsed


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/cu.usbserial-0001")
    ap.add_argument("--tick", type=int, default=1000, help="tick en us (diseñado para 1000)")
    ap.add_argument("--light-seconds", type=float, default=5.0)
    ap.add_argument("--overload-seconds", type=float, default=12.0)
    args = ap.parse_args()
    tick_s = args.tick / 1e6

    node = Node(args.port)
    node.drain()
    node.ser.timeout = 0.002  # lectura fina: la latencia del ACK no queda en el piso de 50 ms
    print(f"Puerto {args.port}, tick {args.tick} us")

    print("\n[0] Calibración")
    resp = node.request(CAL_REQ, struct.pack("<I", args.tick),
                        timeout=args.tick * 50 / 1e6 + 5)
    if resp is None or resp[0] != CAL_RESP or len(resp[1]) != 4:
        print("  FALLO calibración", resp)
        return 1
    sn = struct.unpack("<I", resp[1])[0]
    print(f"        ut_por_tick = {sn}")
    check("ut_por_tick > 0", sn > 0)

    # ---------------------------------------------------------------- [1]
    print(f"\n[1] Carga factible a {args.tick} us (EDF, N = {N_REPORT}, {args.light_seconds:.0f} s)")
    expect_ack("despliegue", node.request(DEPLOY, deploy_text(LIGHT, sn)), DEPLOY, 0)
    t0 = begin(node, 1, N_REPORT)
    node.pump(args.light_seconds)
    events, reports, lat, t_stop = finish(node)
    print(f"        latencia del ACK de Stop: {lat * 1000:.0f} ms (incluye entregar los eventos pendientes)")

    check("tramas Event y TickReport bien formadas",
          node.bad_event_frames == 0 and node.bad_report_frames == 0)
    check("hay eventos", len(events) > 0)
    check("sin TICK_LOST", not any(e[0] == TICK_LOST for e in events))
    if events:
        ok, last, elapsed = pace(events, t0, t_stop, tick_s)
        check("ritmo real (último tick ~ tiempo transcurrido)", ok,
              f"(último tick {last} = {last * tick_s:.2f} s; transcurrido {elapsed:.2f} s)")
        got = [e for e in events if e[1] < 1000]
        want = [e for e in simulate(LIGHT, 1, 1001) if e[1] < 1000]
        diff = first_difference(got, want)
        check(f"flujo idéntico al modelo en 1000 ticks ({len(want)} eventos)", diff == "", diff)
    check("hay reportes", len(reports) > 0)
    if reports:
        ticks = [r["tick"] for r in reports]
        check(f"reportes cada {N_REPORT} ticks sin huecos",
              all(b - a == N_REPORT for a, b in zip(ticks, ticks[1:])), f"(ticks {ticks[:8]}...)")
        last_r = reports[-1]
        show(last_r)
        ratio = last_r["max_busy_us"] / args.tick
        print(f"        ocupación máxima = {ratio:.0%} del tick")
        check("ocupación máxima < 100 % del tick", last_r["max_busy_us"] < args.tick, f"({ratio:.0%})")
        check("sin plazos perdidos, ticks perdidos ni descartes",
              last_r["misses"] == 0 and last_r["ticks_lost"] == 0 and last_r["events_dropped"] == 0)

    # ---------------------------------------------------------------- [2]
    print(f"\n[2] Sobrecarga: 8 tareas con T = D = 1 (EDF, N = {N_REPORT}, {args.overload_seconds:.0f} s)")
    expect_ack("despliegue", node.request(DEPLOY, overload_text(sn)), DEPLOY, 0)
    t0 = begin(node, 1, N_REPORT)
    node.pump(args.overload_seconds)
    n_rep_run = len(node.reports)
    n_ev_run = len(node.events)
    events, reports, lat, t_stop = finish(node)
    print(f"        eventos recibidos durante la ejecución: {n_ev_run} "
          f"({n_ev_run / args.overload_seconds:.0f} por segundo)")
    print(f"        reportes recibidos ANTES de Stop: {n_rep_run} (esperado por el periodo: "
          f"~{int(args.overload_seconds / (N_REPORT * tick_s))})")
    expected_rep = int(args.overload_seconds / (N_REPORT * tick_s))
    check("el TickReport sigue saliendo durante la sobrecarga",
          n_rep_run >= 0.8 * expected_rep,
          f"(llegaron {n_rep_run}; esperados ~{expected_rep})")
    print(f"        latencia del ACK de Stop: {lat * 1000:.0f} ms (incluye vaciar el búfer)")

    check("tramas Event y TickReport bien formadas",
          node.bad_event_frames == 0 and node.bad_report_frames == 0)
    check("hay eventos", len(events) > 0)
    if events:
        ticks = [e[1] for e in events]
        check("marcas de tiempo no decrecientes", all(a <= b for a, b in zip(ticks, ticks[1:])))
        want_first = [(RELEASE, 0, i, 1) for i in range(1, 9)] + [(SWITCH, 0, 1, 0)]
        check("se conservan los eventos más antiguos (el nuevo se descarta)",
              events[:9] == want_first, f"(primeros: {events[:9]})")
        ok, last, elapsed = pace(events, t0, t_stop, tick_s)
        check("ritmo real (último tick ~ tiempo transcurrido)", ok,
              f"(último tick {last} = {last * tick_s:.2f} s; transcurrido {elapsed:.2f} s)")
    check("llegó al menos un reporte (el de Stop)", len(reports) > 0)
    if reports:
        last_r = reports[-1]
        show(last_r)
        check("ticks_perdidos = 0 bajo sobrecarga", last_r["ticks_lost"] == 0,
              f"(fue {last_r['ticks_lost']})")
        check("eventos_descartados saturado (65535)", last_r["events_dropped"] == 65535,
              f"(fue {last_r['events_dropped']})")
        check("plazos_perdidos saturado (65535)", last_r["misses"] == 65535,
              f"(fue {last_r['misses']})")
        print(f"        ocupación máxima = {last_r['max_busy_us'] / args.tick:.0%} del tick")

    # ---------------------------------------------------------------- [3]
    print("\n[3] Reinicio limpio tras la sobrecarga (también detecta reinicio de la placa)")
    expect_ack("despliegue", node.request(DEPLOY, deploy_text(LIGHT, sn)), DEPLOY, 0)
    begin(node, 1, N_REPORT)
    node.pump(0.8)
    events, reports, lat, _ = finish(node)
    print(f"        latencia del ACK de Stop: {lat * 1000:.0f} ms")
    check("hay reportes", len(reports) > 0)
    if reports:
        first = reports[0]
        show(first)
        check("contadores en cero tras el reinicio",
              first["misses"] == 0 and first["events_dropped"] == 0
              and first["ticks_lost"] == 0 and first["bad_frames"] == 0)
    got = [e for e in events if e[1] < 400]
    want = [e for e in simulate(LIGHT, 1, 401) if e[1] < 400]
    diff = first_difference(got, want)
    check(f"flujo idéntico al modelo en 400 ticks ({len(want)} eventos)", diff == "", diff)

    print(f"\nResultado: {base.passed} PASS, {base.failed} FALLO")
    return 0 if base.failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())