#!/usr/bin/env python3
"""R3: el nodo sigue ejecutando cuando el host se detiene o desaparece.

Modo automático: el host deja de leer durante unos segundos (puerto abierto, sin
Stop), vuelve a leer y se comprueba que el nodo avanzó al ritmo real.
    ~/venvs/ce4303/bin/python host_tests/test_host_gone.py

Modo manual: el script inicia la ejecución y termina SIN enviar Stop (el puerto
se cierra). Observas los LEDs para ver si el nodo sigue ejecutando.
    ~/venvs/ce4303/bin/python host_tests/test_host_gone.py --vanish
"""
import argparse
import struct
import sys
import time

import test_start_stop as base
from test_start_stop import (CAL_REQ, CAL_RESP, DEPLOY, START, STOP, Node,
                             check, expect_ack)
from test_events import simulate, first_difference

BASE = [(1, 1, 4, 4, 1), (2, 5, 10, 10, 1)]   # sin plazos perdidos (EDF)
N_REPORT = 100


def deploy_text(tasks, sn):
    return "".join(f"{i},P,{c * sn},{t},{d},{k}\n" for i, c, t, d, k in tasks).encode()


def calibrate(node, tick_us):
    resp = node.request(CAL_REQ, struct.pack("<I", tick_us),
                        timeout=tick_us * 50 / 1e6 + 5)
    if resp is None or resp[0] != CAL_RESP or len(resp[1]) != 4:
        return 0
    return struct.unpack("<I", resp[1])[0]


def show(r):
    print(f"        reporte: tick={r['tick']} plazos={r['misses']} "
          f"ticks_perdidos={r['ticks_lost']} descartados={r['events_dropped']} "
          f"max_ocupado={r['max_busy_us']} us max_latencia={r['max_latency_us']} us")


def run_silent(node, args, tick_us):
    tick_s = tick_us / 1e6
    sn = calibrate(node, tick_us)
    print(f"[0] ut_por_tick = {sn}")
    check("calibración", sn > 0)
    if sn == 0:
        return

    print("\n[1] Ejecución normal durante 1 s")
    expect_ack("despliegue", node.request(DEPLOY, deploy_text(BASE, sn)), DEPLOY, 0)
    node.events.clear()
    node.reports.clear()
    node.bad_event_frames = node.bad_report_frames = 0
    ack = node.request(START, bytes([1, N_REPORT & 0xFF, N_REPORT >> 8]))
    t0 = time.monotonic()
    expect_ack("Start", ack, START, 0)
    node.pump(1.0)
    n_rep_before = len(node.reports)
    print(f"        {len(node.events)} eventos y {n_rep_before} reportes recibidos")

    print(f"\n[2] El host deja de leer durante {args.silent:.0f} s (puerto abierto, sin Stop)")
    time.sleep(args.silent)
    tick_resume = int((time.monotonic() - t0) / tick_s)

    print(f"\n[3] El host vuelve a leer durante {args.after:.0f} s (el nodo debería ir por el tick ~{tick_resume})")
    node.pump(args.after)

    print("\n[4] Stop")
    t_stop = time.monotonic()
    ack = node.request(STOP, timeout=10.0)
    print(f"        latencia del ACK de Stop: {(time.monotonic() - t_stop) * 1000:.0f} ms")
    expect_ack("Stop (el nodo sigue respondiendo)", ack, STOP, 0)
    n_ev, n_rep = len(node.events), len(node.reports)
    node.pump(0.7)
    check("nada después del ACK de Stop",
          len(node.events) == n_ev and len(node.reports) == n_rep)
    events, reports = list(node.events[:n_ev]), list(node.reports[:n_rep])

    check("llegaron reportes después de la pausa", len(reports) > n_rep_before,
          f"(antes {n_rep_before}, total {len(reports)})")
    if reports:
        last = reports[-1]
        show(last)
        elapsed = t_stop - t0
        check("el nodo avanzó al ritmo real durante la pausa (tick del último reporte)",
              elapsed - N_REPORT * tick_s - 0.3 <= last["tick"] * tick_s <= elapsed + 0.3,
              f"(último reporte en el tick {last['tick']} = {last['tick'] * tick_s:.2f} s; "
              f"transcurrido {elapsed:.2f} s)")
        check("sin plazos perdidos ni ticks perdidos",
              last["misses"] == 0 and last["ticks_lost"] == 0)
        print(f"        eventos descartados por el nodo: {last['events_dropped']} (informativo)")

    if events:
        tick_last = max(e[1] for e in events)
        lo, hi = tick_resume + 100, tick_last - 10
        if hi > lo:
            got = [e for e in events if lo <= e[1] < hi]
            want = [e for e in simulate(BASE, 1, hi + 1) if lo <= e[1] < hi]
            diff = first_difference(got, want)
            check(f"flujo en vivo tras la pausa idéntico al modelo (ticks {lo}..{hi}, {len(want)} eventos)",
                  diff == "", diff)
        else:
            print("        (rango en vivo demasiado corto para comparar con el modelo)")


def run_vanish(node, args, tick_us):
    sn = calibrate(node, tick_us)
    print(f"[0] ut_por_tick = {sn}")
    check("calibración", sn > 0)
    if sn == 0:
        return
    tasks = f"1,P,{2 * sn},5,5,1\n2,P,{4 * sn},7,7,1\n".encode()   # carga solo EDF
    expect_ack("despliegue", node.request(DEPLOY, tasks), DEPLOY, 0)
    ack = node.request(START, bytes([0, 10, 0]))   # RMS: pierde plazos en el tick 7
    expect_ack("Start (RMS)", ack, START, 0)
    node.pump(3.0)
    print("\n  >>> Este script termina AHORA sin enviar Stop (el puerto se cierra).")
    print("  >>> OBSERVA LOS LEDs durante 60 s:")
    print("  >>>   el nodo sigue ejecutando: las ranuras siguen alternando (0,2 s por paso)")
    print("  >>>   y GPIO 23 (plazo perdido) queda encendido.")
    print("  >>>   si TODOS los LEDs se apagan, la placa se reinició (probablemente por el cierre del puerto).")
    print("  >>> Anota: ¿siguió alternando? ¿hubo reinicio? ¿a los cuántos segundos?")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/cu.usbserial-0001")
    ap.add_argument("--tick", type=int, default=None,
                    help="tick en us (por defecto 10000 en automático, 200000 con --vanish)")
    ap.add_argument("--silent", type=float, default=8.0, help="segundos sin leer")
    ap.add_argument("--after", type=float, default=4.0, help="segundos leyendo tras la pausa")
    ap.add_argument("--vanish", action="store_true",
                    help="modo manual: el script termina sin Stop")
    args = ap.parse_args()
    tick_us = args.tick if args.tick else (200000 if args.vanish else 10000)

    node = Node(args.port)
    node.drain()
    print(f"Puerto {args.port}, tick {tick_us} us")

    if args.vanish:
        run_vanish(node, args, tick_us)
    else:
        run_silent(node, args, tick_us)

    print(f"\nResultado: {base.passed} PASS, {base.failed} FALLO")
    return 0 if base.failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())