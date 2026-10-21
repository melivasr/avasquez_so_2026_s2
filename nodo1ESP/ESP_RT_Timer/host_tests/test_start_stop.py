#!/usr/bin/env python3
"""Prueba por cable de Calibrar / Deploy / Start / Stop contra el nodo ESP32.

Uso (con el monitor de PlatformIO cerrado):
    ~/venvs/ce4303/bin/python host_tests/test_start_stop.py --alg edf
    ~/venvs/ce4303/bin/python host_tests/test_start_stop.py --alg rms

Las comprobaciones de ACK son automáticas. Lo que pasa en los LEDs lo
observas tú: el script te indica qué mirar y cuándo.
"""
import argparse
import struct
import sys
import time

import serial

CAL_REQ, CAL_RESP, DEPLOY, START, STOP, ACK = 0x01, 0x02, 0x03, 0x04, 0x08, 0x09
EVENT = 0x05


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def build(msg_type: int, payload: bytes = b"") -> bytes:
    body = bytes([msg_type]) + struct.pack("<H", len(payload)) + payload
    return b"\xA5\x5A" + body + struct.pack("<H", crc16(body))


class Node:
    def __init__(self, port: str):
        self.buf = bytearray()
        self.events = []          # (tipo, tick, id, arg) recibidos
        self.bad_event_frames = 0
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = 115200
        self.ser.timeout = 0.05
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()  # abrir el puerto reinicia la placa

    def drain(self, silence: float = 0.5) -> None:
        last = time.monotonic()
        while time.monotonic() - last < silence:
            if self.ser.read(512):
                last = time.monotonic()
        self.buf.clear()

    def _try_parse(self):
        while True:
            i = self.buf.find(b"\xA5\x5A")
            if i < 0:
                if self.buf and self.buf[-1] == 0xA5:
                    del self.buf[:-1]
                else:
                    self.buf.clear()
                return None
            del self.buf[:i]
            if len(self.buf) < 5:
                return None
            length = self.buf[3] | (self.buf[4] << 8)
            if length > 512:
                del self.buf[:2]
                continue
            total = 5 + length + 2
            if len(self.buf) < total:
                return None
            body = bytes(self.buf[2:5 + length])
            rx_crc = self.buf[5 + length] | (self.buf[6 + length] << 8)
            if crc16(body) != rx_crc:
                del self.buf[:2]
                continue
            frame = (self.buf[2], bytes(self.buf[5:5 + length]))
            del self.buf[:total]
            return frame

    def read_frame(self, timeout: float):
        deadline = time.monotonic() + timeout
        while True:
            frame = self._try_parse()
            if frame is not None:
                return frame
            if time.monotonic() >= deadline:
                return None
            data = self.ser.read(256)
            if data:
                self.buf.extend(data)

    def _record_events(self, payload: bytes) -> None:
        if len(payload) >= 1 and len(payload) == 1 + 9 * payload[0]:
            for i in range(payload[0]):
                self.events.append(struct.unpack_from("<BIHH", payload, 1 + 9 * i))
        else:
            self.bad_event_frames += 1

    def request(self, msg_type: int, payload: bytes = b"", timeout: float = 2.0):
        """Envía y devuelve la primera trama que no sea Event (los Event se guardan)."""
        self.ser.write(build(msg_type, payload))
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            frame = self.read_frame(remaining)
            if frame is None:
                return None
            if frame[0] == EVENT:
                self._record_events(frame[1])
                continue
            return frame

    def pump(self, seconds: float) -> None:
        """Lee durante 'seconds' guardando los Event (evita saturar el búfer del SO)."""
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            frame = self.read_frame(min(0.1, max(0.0, deadline - time.monotonic())))
            if frame is not None and frame[0] == EVENT:
                self._record_events(frame[1])


passed = 0
failed = 0


def check(name: str, ok: bool, detail: str = "") -> None:
    global passed, failed
    if ok:
        passed += 1
        print(f"  PASS  {name}")
    else:
        failed += 1
        print(f"  FALLO {name} {detail}")


def expect_ack(name: str, frame, orig: int, status: int) -> None:
    ok = (frame is not None and frame[0] == ACK and len(frame[1]) == 2
          and frame[1][0] == orig and frame[1][1] == status)
    check(name, ok, f"(recibido: {None if frame is None else (hex(frame[0]), frame[1].hex())})")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/cu.usbserial-0001")
    ap.add_argument("--alg", choices=["edf", "rms"], default="edf")
    ap.add_argument("--tick", type=int, default=200000, help="tick en us (la calibración dura 50 ticks)")
    ap.add_argument("--run", type=float, default=20.0, help="segundos observando los LEDs")
    args = ap.parse_args()
    alg = 1 if args.alg == "edf" else 0

    node = Node(args.port)
    node.drain()
    print(f"Puerto {args.port}, algoritmo {args.alg.upper()}, tick {args.tick} us")

    print("\n[1] Estados inválidos antes de calibrar")
    expect_ack("Start sin calibrar -> estado 3", node.request(START, bytes([alg, 0, 0])), START, 3)
    expect_ack("Stop sin ejecución -> estado 3", node.request(STOP), STOP, 3)

    print(f"\n[2] Calibración (tarda ~{args.tick * 50 / 1e6:.1f} s)")
    resp = node.request(CAL_REQ, struct.pack("<I", args.tick), timeout=args.tick * 50 / 1e6 + 5)
    ok = resp is not None and resp[0] == CAL_RESP and len(resp[1]) == 4
    check("CalibrateResp de 4 bytes", ok, f"(recibido: {resp})")
    if not ok:
        return 1
    sn = struct.unpack("<I", resp[1])[0]
    print(f"        ut_por_tick = {sn}")
    check("ut_por_tick > 0", sn > 0)

    print("\n[3] Despliegue")
    bad_id = b"70000,P,10,10,10,1\n"
    two_sporadic = b"1,S,10,10,10,1\n2,S,10,10,10,1\n"
    expect_ack("id > 65535 -> estado 1", node.request(DEPLOY, bad_id), DEPLOY, 1)
    expect_ack("dos esporádicas -> estado 1", node.request(DEPLOY, two_sporadic), DEPLOY, 1)
    tasks = f"1,P,{2 * sn},5,5,1\n2,P,{4 * sn},7,7,1\n".encode()
    expect_ack("despliegue válido -> estado 0", node.request(DEPLOY, tasks), DEPLOY, 0)

    print("\n[4] Start con cargas inválidas")
    expect_ack("Start con carga de 2 bytes -> estado 1", node.request(START, bytes([alg, 0])), START, 1)
    expect_ack("Start con algoritmo 2 -> estado 1", node.request(START, bytes([2, 0, 0])), START, 1)

    print("\n[5] Start")
    expect_ack("Start -> estado 0", node.request(START, bytes([alg, 0, 0])), START, 0)
    print(f"\n  >>> OBSERVA LOS LEDs durante {args.run:.0f} s")
    if args.alg == "rms":
        print("  >>> esperado: ranuras 1,1,2,2,2,1,1 (0,2 s por paso) y luego se enciende GPIO 23 (~1,4 s)")
    else:
        print("  >>> esperado: ranuras 1,1,2,2,2,2,1,1,2... (0,2 s por paso); GPIO 23 apagado")
    node.pump(args.run)

    print("\n[6] Órdenes con el nodo en ejecución")
    expect_ack("Start repetido -> estado 3", node.request(START, bytes([alg, 0, 0])), START, 3)
    expect_ack("Deploy en ejecución -> estado 3", node.request(DEPLOY, tasks), DEPLOY, 3)
    expect_ack("Calibrar en ejecución -> estado 3",
               node.request(CAL_REQ, struct.pack("<I", args.tick)), CAL_REQ, 3)

    print("\n[7] Stop")
    expect_ack("Stop -> estado 0", node.request(STOP), STOP, 0)
    print("  >>> esperado: LEDs de ranura apagados (GPIO 23 conserva su estado)")
    expect_ack("Stop repetido -> estado 3", node.request(STOP), STOP, 3)
    expect_ack("Stop con carga -> estado 1", node.request(STOP, b"\x00"), STOP, 1)

    print("\n[8] Reinicio tras Stop")
    expect_ack("Start tras Stop -> estado 0", node.request(START, bytes([alg, 0, 0])), START, 0)
    print("  >>> esperado: GPIO 23 se apaga al reiniciar; con RMS vuelve a encenderse a ~1,4 s")
    node.pump(6)
    expect_ack("Stop final -> estado 0", node.request(STOP), STOP, 0)

    print(f"\nResultado: {passed} PASS, {failed} FALLO")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())