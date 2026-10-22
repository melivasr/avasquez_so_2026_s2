#!/usr/bin/env python3
"""Prueba por cable de la tarea esporádica del nodo ESP32.

Parte automática (SporadicRelease enviado por el host, equivale al botón):
    ~/venvs/ce4303/bin/python host_tests/test_sporadic.py
Parte manual con el botón real (GPIO 27):
    ~/venvs/ce4303/bin/python host_tests/test_sporadic.py --button
"""
import argparse
import struct
import sys

import test_start_stop as base
from test_start_stop import (CAL_REQ, CAL_RESP, DEPLOY, START, STOP, Node,
                             check, expect_ack)

SPOR = 0x07
RELEASE, COMPLETE, MISS, IGNORED = 1, 3, 4, 5
SPOR_ID, SPOR_T, SPOR_D = 3, 50, 50


def tasks_text(sn, with_sporadic=True):
    text = f"1,P,{sn},4,4,1\n"                              # periódica: C=1, T=D=4
    if with_sporadic:
        text += f"{SPOR_ID},S,{2 * sn},{SPOR_T},{SPOR_D},3\n"  # esporádica: C=2, T=D=50
    return text.encode()


def start(node, text, alg, n=10):
    node.events.clear()
    node.reports.clear()
    node.bad_event_frames = node.bad_report_frames = 0
    expect_ack("despliegue", node.request(DEPLOY, text), DEPLOY, 0)
    expect_ack("Start", node.request(START, bytes([alg, n & 0xFF, n >> 8])), START, 0)


def stop(node):
    expect_ack("Stop", node.request(STOP, timeout=5.0), STOP, 0)
    n_ev, n_rep = len(node.events), len(node.reports)
    node.pump(0.7)
    check("nada después del ACK de Stop",
          len(node.events) == n_ev and len(node.reports) == n_rep)


def sporadic_events(events):
    rel = [e for e in events if e[0] == RELEASE and e[2] == SPOR_ID]
    ign = [e for e in events if e[0] == IGNORED]
    done = [e for e in events if e[0] == COMPLETE and e[2] == SPOR_ID]
    return rel, ign, done


def analyze(events, reports, expect_releases, expect_ignored, max_latency=6):
    """Comprobaciones de consistencia sobre los eventos de la esporádica."""
    rel, ign, done = sporadic_events(events)
    check(f"{expect_releases} liberaciones de la esporádica", len(rel) == expect_releases,
          f"(fueron {len(rel)}: {rel})")
    check(f"{expect_ignored} pulsaciones ignoradas", len(ign) == expect_ignored,
          f"(fueron {len(ign)}: {ign})")
    check("liberaciones separadas al menos T ticks",
          all(b[1] - a[1] >= SPOR_T for a, b in zip(rel, rel[1:])),
          f"(ticks {[r[1] for r in rel]})")
    check("números de trabajo consecutivos", [r[3] for r in rel] == list(range(1, len(rel) + 1)))
    check("cada ignorada cae dentro del intervalo mínimo tras una liberación y lleva motivo 0",
          all(i[2] == SPOR_ID and i[3] == 0 and
              any(0 <= i[1] - r[1] < SPOR_T for r in rel) for i in ign))
    check("cada liberación tiene su COMPLETE", len(done) == len(rel))
    lat = [d[1] - r[1] for r, d in zip(rel, done)]
    if lat:
        print(f"        latencia de respuesta (ticks, liberación -> fin): {lat}")
    check(f"latencia de respuesta <= {max_latency} ticks", all(x <= max_latency for x in lat))
    check("sin plazos perdidos", not any(e[0] == MISS for e in events))
    check("TickReport.esporadicas_ignoradas = ignoradas recibidas hasta ese tick",
          bool(reports) and all(
              r["sporadic_ignored"] == sum(1 for i in ign if i[1] <= r["tick"]) for r in reports))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/cu.usbserial-0001")
    ap.add_argument("--tick", type=int, default=10000)
    ap.add_argument("--button", action="store_true", help="agrega la prueba manual con el botón")
    ap.add_argument("--button-seconds", type=float, default=20.0)
    ap.add_argument("--only-button", action="store_true",
                    help="omite la parte automática y hace solo la prueba del botón (implica --button)")
    args = ap.parse_args()
    if args.only_button:
        args.button = True

    node = Node(args.port)
    node.drain()
    print(f"Puerto {args.port}, tick {args.tick} us (T de la esporádica = {SPOR_T} ticks)")

    resp = node.request(CAL_REQ, struct.pack("<I", args.tick), timeout=args.tick * 50 / 1e6 + 5)
    if resp is None or resp[0] != CAL_RESP or len(resp[1]) != 4:
        print("  FALLO calibración", resp)
        return 1
    sn = struct.unpack("<I", resp[1])[0]
    print(f"[0] ut_por_tick = {sn}")

    if not args.only_button:
        print("\n[1] SporadicRelease sin ejecución")
        expect_ack("sin ejecución -> estado 3", node.request(SPOR), SPOR, 3)

        for alg_name, alg in (("EDF", 1), ("RMS", 0)):
            print(f"\n[2] {alg_name}: periódica + esporádica, pulsaciones del host")
            start(node, tasks_text(sn), alg)
            node.pump(0.5)
            rel0, _, _ = sporadic_events(node.events)
            check("la esporádica no se libera sola", not rel0, f"({rel0})")
            expect_ack("pulsación 1 -> registrada", node.request(SPOR), SPOR, 0)
            node.pump(0.1)
            expect_ack("pulsación 2 (dentro del intervalo mínimo) -> registrada", node.request(SPOR), SPOR, 0)
            node.pump(0.7)
            expect_ack("pulsación 3 (pasado el intervalo) -> registrada", node.request(SPOR), SPOR, 0)
            node.pump(0.4)
            expect_ack("carga no vacía -> estado 1", node.request(SPOR, b"\x00"), SPOR, 1)
            stop(node)
            analyze(node.events, node.reports, expect_releases=2, expect_ignored=1)

        print("\n[3] Sin tarea esporádica en la tabla")
        start(node, tasks_text(sn, with_sporadic=False), 1)
        expect_ack("SporadicRelease sin esporádica -> estado 1", node.request(SPOR), SPOR, 1)
        node.pump(0.3)
        stop(node)
        check("el rechazo no genera eventos de esporádica",
              not any(e[0] in (IGNORED,) or (e[0] == RELEASE and e[2] == SPOR_ID) for e in node.events))

    if args.button:
        print("\n[4] Botón real (GPIO 27)")
        start(node, tasks_text(sn), 1)
        print(f"  >>> Durante {args.button_seconds:.0f} s presiona el botón varias veces:")
        print("  >>>   unas 3 pulsaciones lentas (más de 1 s entre ellas)")
        print("  >>>   y unas 3 RÁPIDAS: tres pulsaciones seguidas en menos de 0,3 s.")
        print(f"  >>> (T = {SPOR_T} ticks = {SPOR_T * args.tick / 1e6:.1f} s: las rápidas después de la primera deben ignorarse)")
        print("  >>> Cuenta cuántas veces lo presionaste. Observa el LED de ranura 2 en cada liberación.")
        node.pump(args.button_seconds)
        stop(node)
        rel, ign, _ = sporadic_events(node.events)
        print(f"        liberaciones en los ticks {[r[1] for r in rel]}")
        print(f"        ignoradas    en los ticks {[i[1] for i in ign]}")
        try:
            pressed = int(input("  ¿Cuántas veces presionaste el botón? ").strip())
        except (ValueError, EOFError):
            pressed = None
        if pressed is not None:
            check("liberaciones + ignoradas = pulsaciones", len(rel) + len(ign) == pressed,
                  f"({len(rel)} + {len(ign)} = {len(rel) + len(ign)}; presionaste {pressed}. "
                  "Más que lo presionado = rebotes contados; menos = pulsaciones perdidas)")
        analyze(node.events, node.reports,
                expect_releases=len(rel), expect_ignored=len(ign))

    print(f"\nResultado: {base.passed} PASS, {base.failed} FALLO")
    return 0 if base.failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())