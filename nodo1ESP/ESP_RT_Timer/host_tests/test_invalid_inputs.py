#!/usr/bin/env python3
"""R4: entradas inválidas. Ninguna debe reiniciar el nodo ni dejarlo inservible.

Casos:
  [1] Barrido de los 256 tipos de mensaje con carga vacía.
  [2] Límites exactos: línea de 95 y 96 caracteres, tabla de 8 y 9 tareas, cargas de 512 B.
  [3] Parser estricto: líneas mal formadas que deben rechazarse (ver predicciones).
  [4] Fuzz aleatorio reproducible (tramas válidas con basura, CRC corrupto, ruido,
      longitud excesiva, tramas truncadas).
  [5] Verificación posterior: el nodo sigue calibrado y ejecuta igual que el modelo.

Detector de reinicio: la placa imprime texto de arranque del ROM ("rst:0x...") en la UART
cuando se reinicia, aunque la consola esté desactivada. Se cuentan esas apariciones y,
como segunda defensa, [5] exige que Start responda 0 (si hubo reinicio, el nodo habría
perdido la calibración y respondería 3).

Uso (con el monitor de PlatformIO cerrado):
    ~/venvs/ce4303/bin/python host_tests/test_invalid_inputs.py
    ~/venvs/ce4303/bin/python host_tests/test_invalid_inputs.py --frames 1000 --seed 7
"""
import argparse
import random
import struct
import sys
import time

import test_start_stop as base
from test_start_stop import (ACK, CAL_REQ, CAL_RESP, DEPLOY, START, STOP, Node,
                             build, check, expect_ack)
from test_events import simulate, first_difference, TICK_LOST

ADD, SPOR = 0x0A, 0x07
BASE = [(1, 1, 4, 4, 1), (2, 5, 10, 10, 1)]
VALID = b"1,P,1,10,10,1\n"
KNOWN = [CAL_REQ, DEPLOY, START, SPOR, STOP, ADD]
LENS = [0, 1, 2, 3, 4, 5, 8, 16, 31, 64, 95, 96, 97, 100, 255, 256, 300,
        500, 510, 511, 512]


class ResetSpy:
    """Cuenta apariciones del texto de arranque del ROM ('rst:0x') en los bytes crudos."""

    def __init__(self, ser):
        self.orig = ser.read
        self.tail = b""
        self.count = 0
        ser.read = self.read

    def read(self, n=1):
        data = self.orig(n)
        if data:
            window = self.tail + data
            self.count += window.count(b"rst:0x")
            self.tail = window[-5:]
        return data


def status_of(node, mtype, payload=b"", timeout=2.0):
    frame = node.request(mtype, payload, timeout=timeout)
    if frame is None or frame[0] != ACK or len(frame[1]) != 2 or frame[1][0] != mtype:
        return None
    return frame[1][1]


def expect(name, node, mtype, payload, want):
    got = status_of(node, mtype, payload)
    check(name, got == want, f"(estado recibido: {got}; esperado {want})")


def line_of_len(length, ident):
    """Línea válida de exactamente 'length' caracteres (rellena W con ceros a la izquierda)."""
    k = length - 12 - len(str(ident))
    assert k >= 0
    line = f"{ident},P,{'0' * k}1,10,10,1"
    assert len(line) == length
    return line


def rand_payload(rng, mtype):
    if mtype in (DEPLOY, ADD) and rng.random() < 0.4:
        text = bytearray(b"7,P,5,10,10,1\n" if mtype == DEPLOY else b"7,P,5,10,10,1")
        for _ in range(rng.randint(0, 3)):
            text[rng.randrange(len(text))] = rng.getrandbits(8)
        return bytes(text)
    n = rng.choice(LENS) if rng.random() < 0.7 else rng.randint(0, 512)
    p = bytearray(rng.getrandbits(8) for _ in range(n))
    if mtype == CAL_REQ and len(p) == 4:
        p[3] |= 0x80  # tick fuera de rango: evita una calibración larga
    return bytes(p)


def rand_frame(rng):
    """Devuelve (bytes a enviar, segundos de espera tras enviarlos)."""
    kind = rng.random()
    t = rng.choice(KNOWN) if rng.random() < 0.6 else rng.randrange(256)
    if kind < 0.60:                      # trama válida, tipo y carga aleatorios
        return build(t, rand_payload(rng, t)), 0.03
    if kind < 0.72:                      # un bit cambiado después de la marca de inicio
        data = bytearray(build(t, rand_payload(rng, t)))
        data[rng.randrange(2, len(data))] ^= 1 << rng.randrange(8)
        return bytes(data), 0.03
    if kind < 0.82:                      # ruido, normalmente sin marca de inicio
        return bytes(rng.getrandbits(8) for _ in range(rng.randint(1, 600))), 0.2
    if kind < 0.90:                      # longitud declarada excesiva
        junk = bytes(rng.getrandbits(8) for _ in range(rng.randint(0, 40)))
        return b"\xA5\x5A" + bytes([t]) + struct.pack("<H", rng.randint(513, 65535)) + junk, 0.2
    data = build(t, rand_payload(rng, t))  # trama truncada: vence el timeout entre bytes
    return data[:rng.randint(1, len(data) - 1)], 0.25


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/cu.usbserial-0001")
    ap.add_argument("--tick", type=int, default=10000)
    ap.add_argument("--frames", type=int, default=400, help="tramas del fuzz aleatorio")
    ap.add_argument("--seed", type=int, default=4303)
    args = ap.parse_args()

    node = Node(args.port)
    spy = ResetSpy(node.ser)
    node.drain()
    boot_seen = spy.count > 0
    spy.count = 0
    print(f"Puerto {args.port}, tick {args.tick} us, semilla {args.seed}")
    print("Detector de reinicio validado con el arranque inicial: "
          + ("sí" if boot_seen else "NO (no se vio 'rst:0x'; me apoyo solo en el Start final)"))

    print("\n[0] Calibración")
    resp = node.request(CAL_REQ, struct.pack("<I", args.tick), timeout=args.tick * 50 / 1e6 + 5)
    if resp is None or resp[0] != CAL_RESP or len(resp[1]) != 4:
        print("  FALLO calibración", resp)
        return 1
    sn = struct.unpack("<I", resp[1])[0]
    print(f"        ut_por_tick = {sn}")
    check("ut_por_tick > 0", sn > 0)

    print("\n[1] Barrido de los 256 tipos con carga vacía (nodo en reposo)")
    expected = {CAL_REQ: 1, DEPLOY: 1, START: 1, STOP: 3, SPOR: 3, ADD: 1}
    differ = []
    for t in range(256):
        got = status_of(node, t, b"", timeout=1.0)
        want = expected.get(t, 2)
        if got != want:
            differ.append((hex(t), got, want))
    check("los 256 tipos responden con el ACK esperado", not differ,
          f"({len(differ)} difieren, (tipo, recibido, esperado): {differ[:6]})")

    print("\n[2] Límites exactos")
    expect("Deploy con línea de 95 -> 0", node, DEPLOY, (line_of_len(95, 1) + "\n").encode(), 0)
    expect("Deploy con línea de 96 -> 1", node, DEPLOY, (line_of_len(96, 1) + "\n").encode(), 1)
    expect("AddTask con línea de 95 -> 0", node, ADD, line_of_len(95, 2).encode(), 0)
    expect("AddTask con línea de 95 + \\n -> 0", node, ADD, (line_of_len(95, 3) + "\n").encode(), 0)
    expect("AddTask con línea de 96 -> 1", node, ADD, line_of_len(96, 4).encode(), 1)
    expect("AddTask con línea de 96 + \\n -> 1", node, ADD, (line_of_len(96, 5) + "\n").encode(), 1)
    eight = "".join(f"{i},P,1,10,10,1\n" for i in range(1, 8)) + "8,S,1,10,10,1\n"
    nine = "".join(f"{i},P,1,10,10,1\n" for i in range(1, 10))
    expect("Deploy de 8 tareas (7 P + 1 S) -> 0", node, DEPLOY, eight.encode(), 0)
    expect("Deploy de 9 tareas -> 1", node, DEPLOY, nine.encode(), 1)
    expect("Deploy de 512 B de '\\n' -> 1", node, DEPLOY, b"\n" * 512, 1)
    expect("Deploy de 512 B de 'a' -> 1", node, DEPLOY, b"a" * 512, 1)
    expect("AddTask de 512 B de 'a' -> 1", node, ADD, b"a" * 512, 1)

    print("\n[3] Parser estricto (predicción: FALLO en los 5 primeros con el firmware actual)")
    cases = [
        ("AddTask: coma doble", ADD, b"9,P,,1,10,10,1"),
        ("AddTask: coma final", ADD, b"9,P,1,10,10,1,"),
        ("AddTask: coma inicial", ADD, b",9,P,1,10,10,1"),
        ("Deploy: coma doble", DEPLOY, b"9,P,,1,10,10,1\n"),
        ("Deploy: NUL embebido", DEPLOY, b"9,P,1,10,10,1\x00basura\n"),
        ("AddTask: espacio inicial", ADD, b" 9,P,1,10,10,1"),
        ("AddTask: CR final", ADD, b"9,P,1,10,10,1\r"),
        ("AddTask: signo +", ADD, b"+9,P,1,10,10,1"),
        ("AddTask: campo extra", ADD, b"9,P,1,10,10,1,5"),
        ("AddTask: tipo en minúscula", ADD, b"9,p,1,10,10,1"),
        ("AddTask: número de 11 dígitos", ADD, b"9,P,99999999999,10,10,1"),
    ]
    for name, mtype, payload in cases:
        status_of(node, DEPLOY, VALID)  # tabla de referencia (una tarea)
        expect(name + " -> 1", node, mtype, payload, 1)

    print(f"\n[4] Fuzz aleatorio: {args.frames} entradas, semilla {args.seed}")
    rng = random.Random(args.seed)
    status_of(node, DEPLOY, VALID)
    for i in range(args.frames):
        data, wait = rand_frame(rng)
        node.ser.write(data)
        node.pump(wait)
        if i % 40 == 39:                 # vuelve a reposo si algún Start aleatorio fue válido
            node.pump(0.2)
            status_of(node, STOP, b"", timeout=0.6)
            node.events.clear()
            node.reports.clear()
    node.pump(0.5)                       # deja vencer una trama a medias
    st = status_of(node, STOP, b"", timeout=3.0)
    check("tras el fuzz, Stop responde (0 o 3)", st in (0, 3), f"(estado: {st})")
    expect("Stop repetido -> 3 (el nodo quedó en reposo)", node, STOP, b"", 3)
    if boot_seen:
        check("sin reinicios de la placa durante la prueba (texto de arranque del ROM)",
              spy.count == 0, f"({spy.count} apariciones de 'rst:0x')")
    else:
        print("        (detector de arranque sin validar: no cuento reinicios por texto)")

    print("\n[5] Verificación posterior")
    text = "".join(f"{i},P,{c * sn},{t},{d},{k}\n" for i, c, t, d, k in BASE).encode()
    expect("Deploy válido", node, DEPLOY, text, 0)
    node.events.clear()
    node.reports.clear()
    node.bad_event_frames = 0
    expect("Start -> 0 (la calibración sigue en memoria: no hubo reinicio)",
           node, START, bytes([1, 0, 0]), 0)
    node.pump(3.2)
    expect("Stop", node, STOP, b"", 0)
    n_ev = len(node.events)
    node.pump(0.5)
    events = list(node.events[:n_ev])
    check("tramas Event bien formadas", node.bad_event_frames == 0)
    check("sin TICK_LOST", not any(e[0] == TICK_LOST for e in events))
    got = [e for e in events if e[1] < 300]
    want = [e for e in simulate(BASE, 1, 301) if e[1] < 300]
    diff = first_difference(got, want)
    check(f"flujo idéntico al modelo en 300 ticks ({len(want)} eventos)", diff == "", diff)

    print(f"\nResultado: {base.passed} PASS, {base.failed} FALLO")
    return 0 if base.failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())