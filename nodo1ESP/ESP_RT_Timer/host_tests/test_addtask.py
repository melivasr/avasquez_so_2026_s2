#!/usr/bin/env python3
"""Prueba por cable de AddTask (0x0A) del nodo ESP32.

Uso (con el monitor de PlatformIO cerrado):
    ~/venvs/ce4303/bin/python host_tests/test_addtask.py
    ~/venvs/ce4303/bin/python host_tests/test_addtask.py --ticks 300
"""
import argparse
import struct
import sys
import time

import test_start_stop as base
from test_start_stop import (CAL_REQ, CAL_RESP, DEPLOY, START, STOP, Node,
                             check, expect_ack)

ADD, SPOR = 0x0A, 0x07
RELEASE, SWITCH, COMPLETE, MISS, IGNORED, TASK_ADDED, TICK_LOST = 1, 2, 3, 4, 5, 6, 7

# (id, costo en ticks C, T, D, criticidad). W = C * ut_por_tick.
BASE = [(1, 1, 4, 4, 1), (2, 5, 10, 10, 1)]
NEW = (3, 1, 20, 20, 2)


def line(t, sn, kind="P"):
    i, c, per, d, k = t
    return f"{i},{kind},{c * sn},{per},{d},{k}"


def deploy_text(tasks, sn):
    return "".join(line(t, sn) + "\n" for t in tasks).encode()


def simulate(tasks, alg, nticks, adds=()):
    """Modelo del calendarizador. adds = [(tick, tarea)]: la tarea entra al
    inicio de ese tick (TASK_ADDED antes de cualquier otro evento del tick)."""
    def new_state(t, start):
        return dict(id=t[0], C=t[1], T=t[2], D=t[3], crit=t[4], active=False,
                    rem=0, dl=0, nxt=start, seq=0)

    st = [new_state(t, 0) for t in tasks]
    events, running = [], 0
    for k in range(nticks):
        for ak, t in adds:
            if ak == k:
                st.append(new_state(t, k))
                events.append((TASK_ADDED, k, t[0], len(st)))
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


def start(node, alg):
    node.events.clear()
    node.reports.clear()
    node.bad_event_frames = node.bad_report_frames = 0
    expect_ack("Start", node.request(START, bytes([alg, 0, 0])), START, 0)


def stop(node):
    expect_ack("Stop", node.request(STOP, timeout=5.0), STOP, 0)
    n = len(node.events)
    node.pump(0.7)
    check("nada después del ACK de Stop", len(node.events) == n,
          f"({len(node.events) - n} eventos tardíos)")
    return list(node.events[:n])


def verify_flow(node, events, tasks, alg, nticks, adds=()):
    check("tramas Event bien formadas", node.bad_event_frames == 0)
    check("hay eventos", len(events) > 0)
    if not events:
        return
    check("sin TICK_LOST", not any(e[0] == TICK_LOST for e in events))
    last = max(e[1] for e in events)
    check(f"el nodo llegó al tick {nticks} (último {last})", last >= nticks)
    got = [e for e in events if e[1] < nticks]
    want = [e for e in simulate(tasks, alg, nticks + 1, adds) if e[1] < nticks]
    diff = first_difference(got, want)
    check(f"flujo idéntico al modelo ({len(want)} eventos)", diff == "", diff)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/cu.usbserial-0001")
    ap.add_argument("--tick", type=int, default=10000)
    ap.add_argument("--ticks", type=int, default=250, help="ticks a comparar con el modelo")
    args = ap.parse_args()
    tick_s = args.tick / 1e6
    run_s = args.ticks * tick_s + 0.6

    node = Node(args.port)
    node.drain()
    print(f"Puerto {args.port}, tick {args.tick} us, {args.ticks} ticks a comparar")

    resp = node.request(CAL_REQ, struct.pack("<I", args.tick),
                        timeout=args.tick * 50 / 1e6 + 5)
    if resp is None or resp[0] != CAL_RESP or len(resp[1]) != 4:
        print("  FALLO calibración", resp)
        return 1
    sn = struct.unpack("<I", resp[1])[0]
    print(f"[0] ut_por_tick = {sn}")
    check("ut_por_tick > 0", sn > 0)

    bad = [
        ("id repetido", line(BASE[0], sn).encode()),
        ("carga vacía", b""),
        ("solo salto de línea", b"\n"),
        ("200 bytes", b"x" * 200),
        ("NUL embebido", b"9,P,1\x00,10,10,1"),
        ("basura", b"\xff\xfe,P,1,1,1"),
        ("id > 65535", b"70000,P,1,10,10,1"),
        ("tipo inválido", b"9,X,1,10,10,1"),
        ("W = 0", b"9,P,0,10,10,1"),
        ("faltan campos", b"9,P,1,10,10"),
    ]

    for alg_name, alg in (("EDF", 1), ("RMS", 0)):
        print(f"\n[1] {alg_name}: AddTask en ejecución (rechazos + una válida)")
        expect_ack("despliegue", node.request(DEPLOY, deploy_text(BASE, sn)), DEPLOY, 0)
        start(node, alg)
        node.pump(0.6)
        for name, payload in bad:
            expect_ack(f"rechazo: {name}", node.request(ADD, payload), ADD, 1)
        t0 = time.monotonic()
        ack = node.request(ADD, line(NEW, sn).encode())
        print(f"        latencia del ACK de AddTask: {(time.monotonic() - t0) * 1000:.0f} ms")
        expect_ack("AddTask válida en ejecución", ack, ADD, 0)
        expect_ack("id repetido tras agregar", node.request(ADD, line(NEW, sn).encode()), ADD, 1)
        node.pump(max(0.0, args.ticks * tick_s - 0.6) + 0.5)
        events = stop(node)

        added = [e for e in events if e[0] == TASK_ADDED]
        check("un solo TASK_ADDED", len(added) == 1, f"({added})")
        if len(added) == 1:
            k = added[0][1]
            check("TASK_ADDED: id 3, ranura 3", added[0][2] == 3 and added[0][3] == 3, f"({added[0]})")
            check(f"TASK_ADDED después del tick 0 (k = {k})", k > 0)
            rel3 = [e for e in events if e[0] == RELEASE and e[2] == 3]
            check("primera liberación de la 3 en el tick de TASK_ADDED",
                  bool(rel3) and rel3[0][1] == k, f"({rel3[:1]})")
            verify_flow(node, events, BASE, alg, args.ticks, adds=[(k, NEW)])

        print(f"\n[1b] {alg_name}: persistencia (Start sin nuevo Deploy)")
        start(node, alg)
        node.pump(run_s)
        events = stop(node)
        rel3 = [e for e in events if e[0] == RELEASE and e[2] == 3]
        check("la 3 sigue en la tabla y se libera en el tick 0",
              bool(rel3) and rel3[0][1] == 0, f"({rel3[:1]})")
        check("sin TASK_ADDED en el reinicio", not any(e[0] == TASK_ADDED for e in events))
        verify_flow(node, events, BASE + [NEW], alg, args.ticks)

    print("\n[2] EDF: AddTask en reposo, luego Start")
    expect_ack("despliegue", node.request(DEPLOY, deploy_text(BASE, sn)), DEPLOY, 0)
    expect_ack("AddTask en reposo (con \\n final)",
               node.request(ADD, (line(NEW, sn) + "\n").encode()), ADD, 0)
    expect_ack("id repetido en reposo", node.request(ADD, line(NEW, sn).encode()), ADD, 1)
    start(node, 1)
    node.pump(run_s)
    events = stop(node)
    check("sin TASK_ADDED (no había ejecución)", not any(e[0] == TASK_ADDED for e in events))
    verify_flow(node, events, BASE + [NEW], 1, args.ticks)

    print("\n[3] Límite de 8 tareas")
    seven = "".join(f"{i},P,{sn},100,100,1\n" for i in range(1, 8)).encode()
    expect_ack("despliegue de 7", node.request(DEPLOY, seven), DEPLOY, 0)
    expect_ack("octava -> 0", node.request(ADD, f"8,P,{sn},100,100,1".encode()), ADD, 0)
    expect_ack("novena -> 1", node.request(ADD, f"9,P,{sn},100,100,1".encode()), ADD, 1)
    expect_ack("octava repetida -> 1", node.request(ADD, f"8,P,{sn},100,100,1".encode()), ADD, 1)
    start(node, 1)
    node.pump(0.5)
    events = stop(node)
    rel0 = {e[2] for e in events if e[0] == RELEASE and e[1] == 0}
    check("las 8 tareas se liberan en el tick 0", rel0 == set(range(1, 9)), f"({sorted(rel0)})")
    check("sin plazos perdidos", not any(e[0] == MISS for e in events))

    print("\n[4] EDF: esporádica agregada en ejecución")
    expect_ack("despliegue", node.request(DEPLOY, deploy_text(BASE, sn)), DEPLOY, 0)
    start(node, 1)
    node.pump(0.5)
    expect_ack("SporadicRelease sin esporádica -> 1", node.request(SPOR), SPOR, 1)
    spor = f"3,S,{2 * sn},50,50,3".encode()
    expect_ack("AddTask esporádica", node.request(ADD, spor), ADD, 0)
    expect_ack("segunda esporádica -> 1", node.request(ADD, f"4,S,{sn},50,50,1".encode()), ADD, 1)
    expect_ack("SporadicRelease ahora -> registrada", node.request(SPOR), SPOR, 0)
    node.pump(0.5)
    events = stop(node)
    added = [e for e in events if e[0] == TASK_ADDED]
    check("TASK_ADDED de la esporádica (id 3, ranura 3)",
          len(added) == 1 and added[0][2] == 3 and added[0][3] == 3, f"({added})")
    rel = [e for e in events if e[0] == RELEASE and e[2] == 3]
    done = [e for e in events if e[0] == COMPLETE and e[2] == 3]
    check("una liberación y un COMPLETE de la esporádica", len(rel) == 1 and len(done) == 1,
          f"({rel} / {done})")
    check("sin plazos perdidos ni ignoradas",
          not any(e[0] in (MISS, IGNORED) for e in events))

    print(f"\nResultado: {base.passed} PASS, {base.failed} FALLO")
    return 0 if base.failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())