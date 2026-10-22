#!/usr/bin/env python3
"""Prueba por cable de TickReport (0x06) del nodo ESP32.

Uso (con el monitor de PlatformIO cerrado):
    ~/venvs/ce4303/bin/python host_tests/test_tickreport.py
"""
import argparse
import struct
import sys
import time

import test_start_stop as base
from test_start_stop import (CAL_REQ, CAL_RESP, DEPLOY, START, STOP, Node,
                             build, check, expect_ack)

MISS = 4
SET_A = [(1, 2, 5, 5, 1), (2, 4, 7, 7, 1)]   # carga solo EDF
SET_B = [(1, 1, 4, 4, 1), (2, 5, 10, 10, 1)]  # sin plazos perdidos


def deploy_text(tasks, sn):
    return "".join(f"{i},P,{c * sn},{t},{d},{k}\n" for i, c, t, d, k in tasks).encode()


def start(node, tasks, sn, alg, n):
    node.events.clear()
    node.reports.clear()
    node.bad_event_frames = node.bad_report_frames = 0
    expect_ack("despliegue", node.request(DEPLOY, deploy_text(tasks, sn)), DEPLOY, 0)
    expect_ack(f"Start (N={n})", node.request(START, bytes([alg, n & 0xFF, n >> 8])), START, 0)


def stop(node):
    expect_ack("Stop", node.request(STOP, timeout=5.0), STOP, 0)
    n_ev, n_rep = len(node.events), len(node.reports)
    node.pump(0.7)
    check("nada después del ACK de Stop",
          len(node.events) == n_ev and len(node.reports) == n_rep,
          f"({len(node.events) - n_ev} eventos y {len(node.reports) - n_rep} reportes tardíos)")


def basic_checks(reports, n, tick_us, seconds, malformed):
    check("tramas TickReport de 22 bytes (ninguna malformada)", malformed == 0,
          f"({malformed} malformadas)")
    check("hay reportes", len(reports) > 0)
    if not reports:
        return
    ticks = [r["tick"] for r in reports]
    check(f"primer reporte en el tick {n}", ticks[0] == n, f"(fue {ticks[0]})")
    check(f"reportes cada {n} ticks sin huecos",
          all(b - a == n for a, b in zip(ticks, ticks[1:])), f"(ticks {ticks[:8]}...)")
    last_s = ticks[-1] * tick_us / 1e6
    check("último reporte consistente con la duración",
          seconds - 0.1 - n * tick_us / 1e6 <= last_s <= seconds + 0.4,
          f"(último reporte en {last_s:.2f} s; duración de la pausa {seconds} s)")
    for key in ("misses", "ticks_lost", "events_dropped", "bad_frames", "max_busy_us", "max_latency_us"):
        vals = [r[key] for r in reports]
        check(f"{key} no decrece", all(a <= b for a, b in zip(vals, vals[1:])))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/cu.usbserial-0001")
    ap.add_argument("--tick", type=int, default=10000)
    args = ap.parse_args()
    tick_s = args.tick / 1e6

    node = Node(args.port)
    node.drain()
    print(f"Puerto {args.port}, tick {args.tick} us")

    resp = node.request(CAL_REQ, struct.pack("<I", args.tick), timeout=args.tick * 50 / 1e6 + 5)
    if resp is None or resp[0] != CAL_RESP or len(resp[1]) != 4:
        print("  FALLO calibración", resp)
        return 1
    sn = struct.unpack("<I", resp[1])[0]
    print(f"[0] ut_por_tick = {sn}")

    print("\n[1] N = 20, conjunto B (sin plazos perdidos), EDF, 3 s")
    start(node, SET_B, sn, 1, 20)
    node.pump(3.0)
    stop(node)
    rep = list(node.reports)
    check("sin tramas TickReport malformadas", node.bad_report_frames == 0)
    basic_checks(rep, 20, args.tick, 3.0, node.bad_report_frames)
    if rep:
        last = rep[-1]
        ratio = last["max_busy_us"] / args.tick
        print(f"        max_ocupado = {last['max_busy_us']} us = {ratio:.0%} del tick (esperado cerca de 80 %)")
        print(f"        max_latencia = {last['max_latency_us']} us")
        check("ocupación máxima <= 90 % del tick", ratio <= 0.9, f"({ratio:.0%})")
        check("latencia del despertar < 1 tick", last["max_latency_us"] < args.tick)
        check("sin plazos perdidos, ticks perdidos ni descartes",
              last["misses"] == 0 and last["ticks_lost"] == 0 and last["events_dropped"] == 0)
        check("sporadic_ignored = 0 (aún no hay esporádica)", last["sporadic_ignored"] == 0)

    print("\n[2] N = 0 (desactivado), 2 s")
    start(node, SET_B, sn, 1, 0)
    node.pump(2.0)
    stop(node)
    check("sin reportes con N = 0", len(node.reports) == 0, f"({len(node.reports)} reportes)")
    check("los eventos siguen llegando", len(node.events) > 0)

    print("\n[3] N = 20 con tramas corruptas durante la ejecución")
    start(node, SET_B, sn, 1, 20)
    node.pump(0.5)
    good = build(DEPLOY, b"1,P,1,1,1,1\n")
    for _ in range(5):                       # CRC incorrecto
        bad = bytearray(good)
        bad[-1] ^= 0x01
        node.ser.write(bytes(bad))
        node.pump(0.05)
    node.ser.write(good[:-3])                # trama truncada: vence el timeout entre bytes
    node.pump(0.4)
    expect_ack("el nodo sigue respondiendo (Deploy en ejecución -> 3)",
               node.request(DEPLOY, b"1,P,1,1,1,1\n"), DEPLOY, 3)
    node.pump(0.6)
    stop(node)
    if node.reports:
        last = node.reports[-1]
        check("bad_frames = 6 (5 con CRC malo + 1 truncada)", last["bad_frames"] == 6,
              f"(fue {last['bad_frames']})")
        check("la ejecución no se alteró (sin plazos perdidos ni ticks perdidos)",
              last["misses"] == 0 and last["ticks_lost"] == 0)
    else:
        check("hay reportes", False)

    print("\n[4] N = 10, RMS con carga solo EDF (hay plazos perdidos), 3 s")
    start(node, SET_A, sn, 0, 10)
    node.pump(3.0)
    stop(node)
    basic_checks(list(node.reports), 10, args.tick, 3.0, node.bad_report_frames)
    miss_ticks = [e[1] for e in node.events if e[0] == MISS]
    mismatches = [r for r in node.reports
                  if r["misses"] != sum(1 for t in miss_ticks if t <= r["tick"])]
    check("plazos perdidos del reporte = MISS recibidos hasta ese tick",
          not mismatches and bool(miss_ticks),
          f"({len(mismatches)} reportes difieren; MISS recibidos: {len(miss_ticks)})")

    print("\n[5] N = 1 (un reporte por tick), 2 s")
    start(node, SET_B, sn, 1, 1)
    node.pump(2.0)
    stop(node)
    basic_checks(list(node.reports), 1, args.tick, 2.0, node.bad_report_frames)
    if node.reports:
        last = node.reports[-1]
        check("N = 1 sin descartes ni ticks perdidos",
              last["events_dropped"] == 0 and last["ticks_lost"] == 0)

    print(f"\nResultado: {base.passed} PASS, {base.failed} FALLO")
    return 0 if base.failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())