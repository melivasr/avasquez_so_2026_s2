#!/usr/bin/env python3
"""Prueba por cable del flujo de eventos (Event 0x05) del nodo ESP32.

Compara los eventos recibidos contra un modelo independiente del
calendarizador escrito en Python, para RMS y EDF y dos conjuntos de tareas.

Uso (con el monitor de PlatformIO cerrado):
    ~/venvs/ce4303/bin/python host_tests/test_events.py
    ~/venvs/ce4303/bin/python host_tests/test_events.py --ticks 500
"""
import argparse
import struct
import sys
import time

import test_start_stop as base
from test_start_stop import (CAL_REQ, CAL_RESP, DEPLOY, START, STOP, Node,
                             check, expect_ack)

RELEASE, SWITCH, COMPLETE, MISS, TICK_LOST = 1, 2, 3, 4, 7

# (id, costo en ticks C, T, D, criticidad). W = C * ut_por_tick.
SETS = {
    "A (carga solo EDF)": [(1, 2, 5, 5, 1), (2, 4, 7, 7, 1)],
    "B (apropiacion)": [(1, 1, 4, 4, 1), (2, 5, 10, 10, 1)],
}


def simulate(tasks, alg, nticks):
    """Modelo del calendarizador: devuelve [(tipo, tick, id, arg)]."""
    st = [dict(id=t[0], C=t[1], T=t[2], D=t[3], crit=t[4], active=False,
               rem=0, dl=0, nxt=0, seq=0) for t in tasks]
    events, running = [], 0
    for k in range(nticks):
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


def first_difference(got, want):
    for i, (g, w) in enumerate(zip(got, want)):
        if g != w:
            return f"índice {i}: recibido {g}, esperado {w}"
    if len(got) != len(want):
        return f"longitud: recibidos {len(got)}, esperados {len(want)}"
    return ""


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/cu.usbserial-0001")
    ap.add_argument("--tick", type=int, default=10000, help="tick en us")
    ap.add_argument("--ticks", type=int, default=300, help="ticks a comparar con el modelo")
    args = ap.parse_args()
    tick_s = args.tick / 1e6
    run = args.ticks * tick_s + 1.0

    node = Node(args.port)
    node.drain()
    print(f"Puerto {args.port}, tick {args.tick} us, {args.ticks} ticks a comparar")

    print("\n[0] Calibración")
    resp = node.request(CAL_REQ, struct.pack("<I", args.tick),
                        timeout=args.tick * 50 / 1e6 + 5)
    if resp is None or resp[0] != CAL_RESP or len(resp[1]) != 4:
        print("  FALLO calibración", resp)
        return 1
    sn = struct.unpack("<I", resp[1])[0]
    print(f"        ut_por_tick = {sn}")
    check("ut_por_tick > 0", sn > 0)

    for set_name, tasks in SETS.items():
        for alg_name, alg in (("EDF", 1), ("RMS", 0)):
            print(f"\n[{alg_name}] conjunto {set_name}")
            text = "".join(f"{i},P,{c * sn},{t},{d},{k}\n" for i, c, t, d, k in tasks)
            expect_ack("despliegue", node.request(DEPLOY, text.encode()), DEPLOY, 0)

            node.events.clear()
            node.bad_event_frames = 0
            ack = node.request(START, bytes([alg, 0, 0]))
            t_start = time.monotonic()
            expect_ack("Start", ack, START, 0)
            node.pump(run)

            t_stop = time.monotonic()
            expect_ack("Stop", node.request(STOP, timeout=5.0), STOP, 0)
            n_at_ack = len(node.events)
            node.pump(0.7)
            check("ningún Event después del ACK de Stop", len(node.events) == n_at_ack,
                  f"({len(node.events) - n_at_ack} eventos tardíos)")

            events = node.events[:n_at_ack]
            check("tramas Event bien formadas", node.bad_event_frames == 0,
                  f"({node.bad_event_frames} malformadas)")
            check("hay eventos", len(events) > 0)
            if not events:
                continue

            ticks = [e[1] for e in events]
            check("marcas de tiempo no decrecientes",
                  all(a <= b for a, b in zip(ticks, ticks[1:])))
            check("sin TICK_LOST", not any(e[0] == TICK_LOST for e in events))
            last = max(ticks)
            check(f"el nodo llegó al tick {args.ticks} (último {last})", last >= args.ticks)

            elapsed = t_stop - t_start
            check("ritmo real (último tick ~ tiempo transcurrido)",
                  elapsed - 0.6 <= last * tick_s <= elapsed + 0.2,
                  f"(último tick {last} = {last * tick_s:.2f} s; transcurrido {elapsed:.2f} s)")

            got = [e for e in events if e[1] < args.ticks]
            want = [e for e in simulate(tasks, alg, args.ticks + 1) if e[1] < args.ticks]
            diff = first_difference(got, want)
            check(f"flujo idéntico al modelo ({len(want)} eventos)", diff == "", diff)

            misses = [e for e in events if e[0] == MISS]
            if set_name.startswith("A") and alg == 1:
                check("EDF/A: sin plazos perdidos", not misses)
            if set_name.startswith("A") and alg == 0:
                check("RMS/A: primer plazo perdido de la tarea 2 en el tick 7",
                      bool(misses) and misses[0][1:3] == (7, 2))
            if set_name.startswith("B"):
                check(f"{alg_name}/B: sin plazos perdidos", not misses)

    print(f"\nResultado: {base.passed} PASS, {base.failed} FALLO")
    return 0 if base.failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())