#!/usr/bin/env python3
"""R5: corrida larga del nodo ESP32 (EDF, carga sin plazos perdidos, tick de 10 ms).

Comprueba durante toda la corrida: ritmo real y deriva del reloj, sin ticks ni plazos
perdidos, sin descartes, reportes continuos, sin reinicios de la placa y flujo de eventos
idéntico al modelo de principio a fin.

Uso (con el monitor de PlatformIO cerrado). En macOS, 'caffeinate -i' evita que el
equipo se suspenda durante la corrida:
    caffeinate -i ~/venvs/ce4303/bin/python host_tests/test_long_run.py --minutes 1
    caffeinate -i ~/venvs/ce4303/bin/python host_tests/test_long_run.py
    caffeinate -i ~/venvs/ce4303/bin/python host_tests/test_long_run.py --minutes 30
"""
import argparse
import struct
import sys
import time

import test_start_stop as base
from test_start_stop import (CAL_REQ, CAL_RESP, DEPLOY, START, STOP, Node,
                             check, expect_ack)
from test_events import simulate, first_difference, TICK_LOST
from test_invalid_inputs import ResetSpy

BASE = [(1, 1, 4, 4, 1), (2, 5, 10, 10, 1)]   # sin plazos perdidos
N_REPORT = 100


def show(r, elapsed):
    print(f"  [{elapsed:6.0f} s] tick={r['tick']} plazos={r['misses']} "
          f"ticks_perdidos={r['ticks_lost']} descartados={r['events_dropped']} "
          f"max_ocupado={r['max_busy_us']} us max_latencia={r['max_latency_us']} us")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/cu.usbserial-0001")
    ap.add_argument("--tick", type=int, default=10000)
    ap.add_argument("--minutes", type=float, default=10.0)
    ap.add_argument("--progress", type=float, default=60.0, help="segundos entre líneas de avance")
    args = ap.parse_args()
    tick_s = args.tick / 1e6

    node = Node(args.port)
    spy = ResetSpy(node.ser)
    node.drain()
    boot_seen = spy.count > 0
    spy.count = 0

    # Marca de llegada de cada TickReport (para medir la deriva del reloj del nodo).
    stamps = []
    orig_record = node._record_report

    def record(payload):
        orig_record(payload)
        if len(node.reports) > len(stamps):
            stamps.append(time.monotonic())

    node._record_report = record

    print(f"Puerto {args.port}, tick {args.tick} us, {args.minutes:g} min")
    print("Detector de reinicio validado con el arranque inicial: "
          + ("sí" if boot_seen else "NO (me apoyo en la continuidad de los reportes)"))

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
    expect_ack(f"Start (N={N_REPORT})", ack, START, 0)

    duration = args.minutes * 60
    next_progress = args.progress
    print(f"\n[1] Ejecutando {args.minutes:g} min...")
    while time.monotonic() - t0 < duration:
        node.pump(1.0)
        elapsed = time.monotonic() - t0
        if elapsed >= next_progress:
            next_progress += args.progress
            if node.reports:
                show(node.reports[-1], elapsed)

    t_stop = time.monotonic()
    ack = node.request(STOP, timeout=10.0)
    print(f"\n[2] Stop: latencia del ACK {(time.monotonic() - t_stop) * 1000:.0f} ms")
    expect_ack("Stop", ack, STOP, 0)
    n_ev, n_rep = len(node.events), len(node.reports)
    node.pump(0.7)
    check("nada después del ACK de Stop",
          len(node.events) == n_ev and len(node.reports) == n_rep)
    events, reports = list(node.events[:n_ev]), list(node.reports[:n_rep])

    print("\n[3] Análisis")
    check("tramas Event y TickReport bien formadas",
          node.bad_event_frames == 0 and node.bad_report_frames == 0)
    check("sin TICK_LOST", not any(e[0] == TICK_LOST for e in events))
    if boot_seen:
        check("sin reinicios de la placa (texto de arranque del ROM)", spy.count == 0,
              f"({spy.count} apariciones de 'rst:0x')")

    check("hay reportes", len(reports) > 0)
    if reports:
        ticks = [r["tick"] for r in reports]
        check(f"primer reporte en el tick {N_REPORT}", ticks[0] == N_REPORT, f"(fue {ticks[0]})")
        check(f"reportes cada {N_REPORT} ticks sin huecos ({len(reports)} reportes)",
              all(b - a == N_REPORT for a, b in zip(ticks, ticks[1:])),
              f"(primer hueco en el índice "
              f"{next((i for i, (a, b) in enumerate(zip(ticks, ticks[1:])) if b - a != N_REPORT), -1)})")
        first, last = reports[0], reports[-1]
        show(last, t_stop - t0)
        check("sin plazos perdidos, ticks perdidos, descartes ni tramas malas",
              last["misses"] == 0 and last["ticks_lost"] == 0
              and last["events_dropped"] == 0 and last["bad_frames"] == 0)
        ratio = last["max_busy_us"] / args.tick
        print(f"        max_ocupado: primer reporte {first['max_busy_us']} us, "
              f"último {last['max_busy_us']} us ({ratio:.0%} del tick)")
        check("ocupación máxima <= 90 % del tick", ratio <= 0.9, f"({ratio:.0%})")
        check("latencia máxima del despertar < 1 tick", last["max_latency_us"] < args.tick,
              f"({last['max_latency_us']} us)")

        elapsed = t_stop - t0
        check("ritmo real (tick del último reporte ~ tiempo transcurrido)",
              elapsed - N_REPORT * tick_s - 0.3 <= last["tick"] * tick_s <= elapsed + 0.3,
              f"(tick {last['tick']} = {last['tick'] * tick_s:.2f} s; transcurrido {elapsed:.2f} s)")

        if len(reports) >= 4 and len(stamps) == len(reports):
            a, b = 1, len(reports) - 2   # se omiten los extremos (el último puede salir con el Stop)
            node_s = (reports[b]["tick"] - reports[a]["tick"]) * tick_s
            host_s = stamps[b] - stamps[a]
            ppm = (node_s - host_s) / host_s * 1e6
            print(f"        deriva del reloj del nodo respecto al del host: {ppm:+.0f} ppm "
                  f"(nodo {node_s:.3f} s, host {host_s:.3f} s; la llegada tiene jitter de ms)")
            check("deriva del reloj <= 500 ppm", abs(ppm) <= 500, f"({ppm:+.0f} ppm)")

    if events:
        last_tick = max(e[1] for e in events)
        n_cmp = last_tick - 2
        got = [e for e in events if e[1] < n_cmp]
        want = [e for e in simulate(BASE, 1, n_cmp + 1) if e[1] < n_cmp]
        diff = first_difference(got, want)
        check(f"flujo idéntico al modelo en {n_cmp} ticks ({len(want)} eventos)", diff == "", diff)

    print(f"\nResultado: {base.passed} PASS, {base.failed} FALLO")
    return 0 if base.failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())