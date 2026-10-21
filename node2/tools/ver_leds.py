"""Muestra en la terminal lo que hacen los LEDs del nodo 2.

Lee los eventos que manda el nodo

Uso:  python3 ver_leds.py tareas.csv [--puerto /dev/ttyACM0] [--algoritmo EDF|RMS]
                          [--segundos 10] [--tick-us 10000] [--w-en-ticks]
                          [--disparar 1,1.2,3] [--todos] [--cada-tick]

Columnas:
  leds   led1 a led4: la tarea que está corriendo, en binario
         (led1 es el bit 0, la ranura es la posición en el archivo y 0000 es nada)
  led5   plazo perdido: se enciende 200 ms con cada MISS, igual que el LED real
  botón  1 en el tick en que se atendió una pulsación de la esporádica

Opciones:
  --w-en-ticks   la W del archivo está en ticks (se multiplica por s_n)
  --disparar     segundos después de START en que se manda SPORADIC_RELEASE,
                 que es lo mismo que presionar el botón
  --todos        muestra también los ticks con eventos que no cambian los LEDs
  --cada-tick    una línea por tick, en vivo (con --tick-us grande se sigue mejor)

Necesita pyserial (pip install pyserial).
"""

import argparse
import binascii
import struct
import sys
import time

import serial

# Tipos de mensaje
CALIBRATE_REQ, CALIBRATE_RESP, DEPLOY_TASKS, START = 0x01, 0x02, 0x03, 0x04
EVENT, TICK_REPORT, SPORADIC_RELEASE, STOP, ACK = 0x05, 0x06, 0x07, 0x08, 0x09

# Algoritmos (primer byte de START)
RMS, EDF = 0, 1

# Tipos de evento
RELEASE, SWITCH, COMPLETE, MISS, SPORADIC_IGNORED, TASK_ADDED, TICK_LOST, BAD_FRAME = range(1, 9)


def frame(msg_type, payload=b""):
    """Arma una trama: A5 5A, tipo, largo, carga y CRC."""
    body = struct.pack("<BH", msg_type, len(payload)) + payload
    return b"\xA5\x5A" + body + struct.pack("<H", binascii.crc_hqx(body, 0xFFFF))


def start(algorithm, report_ticks):
    """START lleva el algoritmo y cada cuántos ticks se manda TICK_REPORT."""
    return frame(START, struct.pack("<BH", algorithm, report_ticks))


def ack(f):
    """Devuelve (tipo confirmado, estado), o None si no es un ACK."""
    return (f["payload"][0], f["payload"][1]) if f["type"] == ACK else None


def events(f):
    """Saca los registros de un EVENT (en una trama vienen varios)."""
    if f["type"] != EVENT or not f["payload"]:
        return None
    count = f["payload"][0]
    if len(f["payload"]) != 1 + 9 * count:
        return None
    records = []
    for i in range(count):
        ev_type, tick, task_id, arg = struct.unpack_from("<BIHH", f["payload"], 1 + 9 * i)
        records.append({"type": ev_type, "tick": tick, "id": task_id, "arg": arg})
    return records


def tick_report(f):
    """Lee los campos de un TICK_REPORT."""
    if f["type"] != TICK_REPORT or len(f["payload"]) != 22:
        return None
    fields = struct.unpack("<IHHHHHII", f["payload"])
    names = ("tick", "misses", "sporadic_ignored", "ticks_lost",
             "events_dropped", "bad_frames", "max_busy_us", "max_wake_latency_us")
    return dict(zip(names, fields))


def parse(data):
    """Separa las tramas que llegaron y se queda con las que tienen bien el CRC."""
    frames = []
    i = 0
    while i + 7 <= len(data):
        if data[i:i + 2] != b"\xA5\x5A":
            i += 1
            continue
        length = data[i + 3] | data[i + 4] << 8
        raw = data[i:i + 7 + length]
        if len(raw) < 7 + length:
            break
        i += 7 + length
        body = raw[2:-2]
        if binascii.crc_hqx(body, 0xFFFF) == struct.unpack("<H", raw[-2:])[0]:
            frames.append({"type": raw[2], "payload": raw[5:-2], "crc_ok": True})
    return frames


class Node:
    """El puerto serial del nodo."""

    def __init__(self, port):
        self.port = serial.Serial(port, 115200, timeout=0.05)
        time.sleep(1.0)
        self.port.reset_input_buffer()

    def send(self, data, wait=0.3):
        """Manda los bytes y devuelve lo que responda el nodo en wait segundos
        (sin contar EVENT ni TICK_REPORT)."""
        self.port.reset_input_buffer()
        self.port.write(data)
        received = bytearray()
        end = time.time() + wait
        while time.time() < end:
            received += self.port.read(4096)
        return [f for f in parse(bytes(received)) if f["type"] not in (TICK_REPORT, EVENT)]


# Nombre corto de cada evento para la columna de eventos
NAMES = {RELEASE: "RELEASE", SWITCH: "SWITCH", COMPLETE: "COMPLETE", MISS: "MISS",
         SPORADIC_IGNORED: "IGNORADA", TICK_LOST: "TICK_LOST"}


def read_tasks(path):
    """Lee las tareas del archivo; se salta el encabezado y las líneas vacías."""
    lines = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line and line[0].isdigit():
                lines.append(line)
    return lines


class Stream:
    """Va juntando los bytes que llegan y saca las tramas completas."""

    def __init__(self, port):
        self.port = port
        self.data = bytearray()

    def frames(self):
        self.data += self.port.read(4096)
        out = []
        while True:
            i = self.data.find(b"\xA5\x5A")
            if i < 0:
                del self.data[:-1]
                return out
            del self.data[:i]
            if len(self.data) < 7:
                return out
            length = self.data[3] | self.data[4] << 8
            if len(self.data) < 7 + length:
                return out
            raw = bytes(self.data[:7 + length])
            del self.data[:7 + length]
            body = raw[2:-2]
            if binascii.crc_hqx(body, 0xFFFF) == struct.unpack("<H", raw[-2:])[0]:
                out.append({"type": raw[2], "payload": raw[5:-2], "crc_ok": True})


class Leds:
    """Lleva el estado de los LEDs según los eventos de cada tick."""

    def __init__(self, slots, sporadic_id, show_all=False, every_tick=False, miss_ticks=1):
        self.miss_ticks = miss_ticks    # cuántos ticks queda encendido el LED de plazo perdido
        self.miss_until = 0             # sigue encendido mientras el tick sea menor a esto
        self.show_all = show_all or every_tick
        self.every_tick = every_tick
        self.printed_up_to = -1         # hasta qué tick ya se imprimió
        self.slots = slots              # id de la tarea -> ranura (1 a 8)
        self.sporadic_id = sporadic_id
        self.slot = 0
        self.last_line = None
        self.tick = None
        self.button = 0
        self.notes = []

    def add(self, ev):
        # si llegó un evento de otro tick, primero se imprime el anterior
        if self.tick is not None and ev["tick"] != self.tick:
            self.flush()
        self.fill(ev["tick"] - 1)
        self.tick = ev["tick"]

        if ev["type"] == SWITCH:
            self.slot = self.slots.get(ev["id"], 0)
        elif ev["type"] == MISS:
            self.miss_until = ev["tick"] + self.miss_ticks
        if ev["type"] == SPORADIC_IGNORED or (ev["type"] == RELEASE and ev["id"] == self.sporadic_id):
            self.button = 1

        if ev["type"] == SWITCH:
            self.notes.append(f"corre {ev['id'] or 'nada'}")
        elif ev["type"] == TICK_LOST:
            self.notes.append(f"TICK_LOST {ev['arg']}")
        else:
            self.notes.append(f"{NAMES.get(ev['type'], ev['type'])} {ev['id']}#{ev['arg']}")

    def fill(self, up_to):
        """Con --cada-tick imprime los ticks que no tuvieron eventos."""
        if not self.every_tick:
            return
        bits = "".join(str((self.slot >> b) & 1) for b in range(4))
        for t in range(self.printed_up_to + 1, up_to + 1):
            print(f"{t:7d}   {bits}    {self.miss_at(t)}     0", flush=True)
        self.printed_up_to = max(self.printed_up_to, up_to)

    def miss_at(self, tick):
        return 1 if tick < self.miss_until else 0

    def clock(self, tick):
        """Llegó el TICK_REPORT de este tick: sus eventos ya llegaron y se puede imprimir."""
        if self.tick is not None and self.tick <= tick:
            self.flush()
        self.fill(tick)

    def flush(self):
        if self.tick is None:
            return
        self.printed_up_to = max(self.printed_up_to, self.tick)
        bits = "".join(str((self.slot >> b) & 1) for b in range(4))
        miss = self.miss_at(self.tick)
        line = (bits, miss, self.button)
        # se imprime solo si cambió algo o hubo pulsación (con --todos, cada vez que hay eventos)
        if line != self.last_line or self.button or self.show_all:
            print(f"{self.tick:7d}   {bits}    {miss}     {self.button}     {', '.join(self.notes)}",
                  flush=True)
        self.last_line = (bits, miss, 0)
        self.button = 0
        self.notes = []
        self.tick = None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tareas")
    ap.add_argument("--puerto", default="/dev/ttyACM0")
    ap.add_argument("--algoritmo", default="EDF", choices=["EDF", "RMS"])
    ap.add_argument("--segundos", type=float, default=10.0)
    ap.add_argument("--tick-us", type=int, default=10000)
    ap.add_argument("--w-en-ticks", action="store_true")
    ap.add_argument("--disparar", default="")
    ap.add_argument("--todos", action="store_true")
    ap.add_argument("--cada-tick", action="store_true")
    args = ap.parse_args()

    lines = read_tasks(args.tareas)
    if not lines:
        sys.exit("el archivo no tiene tareas")
    triggers = sorted(float(x) for x in args.disparar.split(",") if x.strip())

    node = Node(args.puerto)
    node.send(frame(STOP))

    r = node.send(frame(CALIBRATE_REQ, struct.pack("<I", args.tick_us)), wait=1.0 + 50 * args.tick_us / 1e6)
    cal = [f for f in r if f["type"] == CALIBRATE_RESP]
    if not cal:
        sys.exit(f"la calibración no respondió: {r}")
    s = struct.unpack("<I", cal[0]["payload"])[0]
    print(f"s_n = {s} UT/tick (tick de {args.tick_us} us)")

    # la tarea es su posición en el archivo
    slots, sporadic_id, deploy = {}, 0, []
    for n, line in enumerate(lines, start=1):
        f = line.split(",")
        if args.w_en_ticks:
            f[2] = str(int(f[2]) * s)
        task_id, w = int(f[0]), int(f[2])
        slots[task_id] = n
        if f[1] == "S":
            sporadic_id = task_id
        print(f"  ranura {n} ({n & 1}{n >> 1 & 1}{n >> 2 & 1}{n >> 3 & 1}): tarea {task_id} {f[1]}"
              f" W={w} T={f[3]} D={f[4]} crit={f[5]} -> C={-(-w // s)} ticks")
        deploy.append(",".join(f))

    r = node.send(frame(DEPLOY_TASKS, ("\n".join(deploy) + "\n").encode()))
    if [ack(x) for x in r] != [(DEPLOY_TASKS, 0)]:
        sys.exit(f"DEPLOY_TASKS rechazado: {[ack(x) for x in r]}")

    # el LED de plazo perdido se apaga cuando pasan 200 ms desde el MISS
    leds = Leds(slots, sporadic_id, args.todos, args.cada_tick, -(-200000 // args.tick_us))
    print(f"\n{args.algoritmo}, {args.segundos} s")
    if sporadic_id:
        print(f">>> YA PUEDES PRESIONAR EL BOTÓN (tarea {sporadic_id}); antes de esto se descartan")
    print("   tick   leds  led5  botón   eventos")
    stream = Stream(node.port)
    node.port.reset_input_buffer()
    # con --cada-tick, el TICK_REPORT de cada tick sirve de reloj
    node.port.write(start(EDF if args.algoritmo == "EDF" else RMS, 1 if args.cada_tick else 100))
    t0 = time.time()
    last_report = None

    while time.time() - t0 < args.segundos:
        while triggers and time.time() - t0 >= triggers[0]:
            node.port.write(frame(SPORADIC_RELEASE))
            triggers.pop(0)
        for f in stream.frames():
            if f["type"] == EVENT:
                for ev in events(f) or []:
                    leds.add(ev)
            elif f["type"] == TICK_REPORT:
                last_report = tick_report(f) or last_report
                if last_report:
                    leds.clock(last_report["tick"])
            elif f["type"] == ACK and f["payload"][0] == SPORADIC_RELEASE and f["payload"][1] != 0:
                print(f"   SPORADIC_RELEASE rechazado (estado {f['payload'][1]})")

    # al mandar STOP, los eventos que faltaban llegan antes de su ACK
    node.port.write(frame(STOP))
    end = time.time() + 1.0
    while time.time() < end:
        for f in stream.frames():
            if f["type"] == EVENT:
                for ev in events(f) or []:
                    leds.add(ev)
            elif f["type"] == ACK and f["payload"][0] == STOP:
                end = 0
    leds.flush()

    if last_report:
        print(f"\núltimo TICK_REPORT (tick {last_report['tick']}): plazos perdidos={last_report['misses']}"
              f" esporádicas ignoradas={last_report['sporadic_ignored']} ticks perdidos={last_report['ticks_lost']}"
              f" eventos descartados={last_report['events_dropped']} ocupado máx={last_report['max_busy_us']} us")


if __name__ == "__main__":
    main()
