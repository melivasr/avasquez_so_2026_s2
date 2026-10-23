#!/usr/bin/env python3
"""PTY + fuentes reales de Pico; solo sustituimos USB, reloj y GPIO.

No requiere pyserial, placas ni PlatformIO. No valida ESP-IDF ni el Pico SDK.
"""
import binascii
import csv
import os
from pathlib import Path
import select
import signal
import struct
import subprocess
import tempfile
import termios
import threading
import time
import tty
import unittest

ROOT = Path(__file__).resolve().parents[1]


def frame(kind, payload=b""):
    body = struct.pack("<BH", kind, len(payload)) + payload
    return b"\xa5\x5a" + body + struct.pack("<H", binascii.crc_hqx(body, 0xFFFF))


class NativeNode:
    def __init__(self):
        master, self.fd = os.openpty()
        tty.setraw(self.fd)
        os.set_blocking(self.fd, False)
        self.path = os.ttyname(self.fd)
        self.process = subprocess.Popen([ROOT / "build/native_pico"], stdin=master,
                                        stdout=master, stderr=subprocess.PIPE)
        os.close(master)
        self.buffer = bytearray()
        self.frames = []

    def read(self, timeout=0.05):
        if select.select([self.fd], [], [], timeout)[0]:
            self.buffer.extend(os.read(self.fd, 65536))
        while len(self.buffer) >= 7:
            self.assert_magic()
            length = struct.unpack_from("<H", self.buffer, 3)[0]
            if len(self.buffer) < 7 + length:
                break
            raw = bytes(self.buffer[:7 + length])
            del self.buffer[:7 + length]
            assert binascii.crc_hqx(raw[2:-2], 0xFFFF) == struct.unpack("<H", raw[-2:])[0]
            self.frames.append((raw[2], raw[5:-2]))

    def assert_magic(self):
        assert self.buffer[:2] == b"\xa5\x5a", bytes(self.buffer)

    def exchange(self, kind, payload=b"", timeout=3):
        os.write(self.fd, frame(kind, payload))
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.read()
            for i, (reply_kind, data) in enumerate(self.frames):
                if (reply_kind == 9 and data[0] == kind) or (kind == 1 and reply_kind == 2):
                    self.frames.pop(i)
                    return reply_kind, data
        raise AssertionError(f"sin respuesta tipo {kind}; proceso={self.process.poll()}")

    def collect(self, seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.read(min(0.02, max(0, deadline - time.monotonic())))

    def events(self):
        result = []
        for kind, payload in self.frames:
            if kind == 5:
                assert len(payload) == 1 + payload[0] * 9
                result.extend(struct.unpack_from("<BIHH", payload, 1 + i * 9)
                              for i in range(payload[0]))
        return result

    def close(self):
        self.process.terminate()
        try:
            self.process.communicate(timeout=3)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.communicate()
        os.close(self.fd)


class HostSession:
    def __init__(self, directory, nodes, algorithm="EDF", tick_us=10000, terminal=False):
        self.directory = Path(directory)
        tasks = self.directory / "input.csv"
        tasks.write_text("1,P,1,10,10,2\n2,S,1,100,100,-1\n3,S,1,100,100,9\n")
        config = self.directory / "host.ini"
        self.output = self.directory / "report"
        config.write_text(f"algorithm={algorithm}\nheuristic=First-Fit\ntick_us={tick_us}\n"
                          f"esp_port={nodes[0].path}\npico_port={nodes[1].path}\n"
                          f"report_ticks=5\nmissed_reports=40\ntimeout_ms=100\n"
                          f"tasks={tasks}\noutput={self.output}\n")
        self.input_master = self.input_slave = None
        if terminal:
            self.input_master, self.input_slave = os.openpty()
            self.saved_terminal = termios.tcgetattr(self.input_slave)
        self.process = subprocess.Popen([ROOT / "build/host", config],
                                        stdin=self.input_slave if terminal else subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                        text=True, bufsize=1)
        self.lines = []
        self.condition = threading.Condition()

        def reader():
            for line in self.process.stdout:
                with self.condition:
                    self.lines.append(line)
                    self.condition.notify_all()

        self.reader = threading.Thread(target=reader, daemon=True)
        self.reader.start()

    def wait(self, text, timeout=10):
        deadline = time.monotonic() + timeout
        with self.condition:
            while text not in "".join(self.lines):
                if self.process.poll() is not None or time.monotonic() >= deadline:
                    raise AssertionError(f"no llegó {text!r}: {''.join(self.lines)}; "
                                         f"salida={self.process.poll()}")
                self.condition.wait(timeout=min(0.1, deadline - time.monotonic()))

    def command(self, text):
        if self.input_master is not None:
            os.write(self.input_master, (text + "\n").encode())
        else:
            self.process.stdin.write(text + "\n")
            self.process.stdin.flush()

    def finish(self, stop=True):
        if stop:
            self.command("q")
        elif stop is False:
            self.process.send_signal(signal.SIGTERM)
        self.process.wait(timeout=10)
        self.reader.join(timeout=2)
        stderr = self.process.stderr.read()
        if self.process.stdin:
            self.process.stdin.close()
        self.process.stdout.close()
        self.process.stderr.close()
        assert self.process.returncode == 0, (self.lines, stderr)
        return stderr

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait()
        for fd in (self.input_master, self.input_slave):
            if fd is not None:
                os.close(fd)
        self.input_master = self.input_slave = None

    def rows(self, name):
        with (self.output / name).open() as handle:
            return list(csv.DictReader(handle))


class Integration(unittest.TestCase):
    def test_heartbeat_under_event_saturation(self):
        node = NativeNode()
        self.addCleanup(node.close)
        self.assertEqual(node.exchange(1, struct.pack("<I", 1000))[0], 2)
        tasks = "".join(f"{i},P,1,1,1,1\n" for i in range(1, 9)).encode()
        self.assertEqual(node.exchange(3, tasks), (9, b"\x03\x00"))
        self.assertEqual(node.exchange(4, b"\x01\x05\x00"), (9, b"\x04\x00"))
        node.collect(0.4)
        reports = [struct.unpack("<IHHHHHII", data) for kind, data in node.frames if kind == 6]
        self.assertGreater(len(reports), 3)
        self.assertGreater(reports[-1][0], 50)
        self.assertGreater(reports[-1][4], 0, "la prueba debe llenar la cola local")
        self.assertEqual(node.exchange(8), (9, b"\x08\x00"))

    def test_native_commands(self):
        node = NativeNode()
        self.addCleanup(node.close)
        self.assertEqual(node.exchange(4, b"\x01\x01\x00"), (9, b"\x04\x03"))
        self.assertEqual(node.exchange(1, struct.pack("<I", 1)), (9, b"\x01\x01"))
        kind, data = node.exchange(1, struct.pack("<I", 10000))
        self.assertEqual(kind, 2)
        self.assertGreater(struct.unpack("<I", data)[0], 0)
        # Tabla vacía borra una sesión anterior y aun así mantiene heartbeat.
        self.assertEqual(node.exchange(3), (9, b"\x03\x00"))
        self.assertEqual(node.exchange(4, b"\x01\x02\x00"), (9, b"\x04\x00"))
        node.collect(0.08)
        self.assertTrue(any(kind == 6 and len(data) == 22 for kind, data in node.frames))
        self.assertEqual(node.exchange(10, b"65535,P,1,10,10,-2147483648\n"), (9, b"\x0a\x00"))
        node.collect(0.08)
        added = [event for event in node.events() if event[0] == 6]
        self.assertEqual(len(added), 1)
        self.assertGreater(added[0][1], 0)
        self.assertEqual(node.exchange(10, b"65535,P,1,10,10,1"), (9, b"\x0a\x01"))
        self.assertEqual(node.exchange(10, b"4,S,1,100,100,-1"), (9, b"\x0a\x00"))
        self.assertEqual(node.exchange(7), (9, b"\x07\x00"))
        node.collect(0.04)
        self.assertEqual(node.exchange(7), (9, b"\x07\x00"))
        node.collect(0.04)
        self.assertTrue(any(e[0] == 5 and e[2] == 4 and e[3] == 0 for e in node.events()))
        self.assertEqual(node.exchange(10, b"5,S,1,10,10,1"), (9, b"\x0a\x01"))
        self.assertEqual(node.exchange(10, b"6,P,1,10,10,1\0extra"), (9, b"\x0a\x01"))
        corrupt = bytearray(frame(10, b"7,P,1,10,10,1"))
        corrupt[-1] ^= 1
        os.write(node.fd, corrupt)
        node.collect(0.05)
        reports = [struct.unpack("<IHHHHHII", data) for kind, data in node.frames if kind == 6]
        self.assertGreater(reports[-1][5], 0)
        self.assertEqual(node.exchange(8), (9, b"\x08\x00"))
        node.frames.clear()
        node.collect(0.05)
        self.assertFalse(node.frames, "telemetría posterior al ACK de STOP")

    def test_host_modes_and_migration(self):
        for algorithm in ("RMS", "EDF"):
            with self.subTest(algorithm=algorithm), tempfile.TemporaryDirectory() as directory:
                nodes = [NativeNode(), NativeNode()]
                session = None
                try:
                    session = HostSession(directory, nodes, algorithm)
                    session.wait("Comandos:")
                    placement = next(line for line in session.lines if "Tarea 1: ADMITIDA" in line)
                    failed = 1 if "[Pico]" in placement else 0
                    survivor = 1 - failed
                    session.command("TrIgGeR 2")
                    time.sleep(0.04)
                    session.command("trigger 2")
                    session.command("trigger 3")
                    session.command("AdD 4,p,1,20,20,-5")
                    session.wait("DESPLEGADA y confirmada sin reiniciar tick")
                    time.sleep(0.25)
                    session.command("FaIl PiCo" if failed else "FAIL ESP")
                    session.wait("REUBICADA y confirmada")
                    time.sleep(0.3)
                    stderr = session.finish()
                    self.assertNotIn("ESP32 caído" if survivor == 0 else "Pico caído", stderr)
                    rows = session.rows("events.csv")
                    self.assertTrue(any(r["kind"] == "4" for r in rows))
                    self.assertTrue(any(r["kind"] == "3" and r["code"] == "6" and
                                        r["node"] == str(survivor) and r["id"] == "1" and int(r["tick"]) > 0
                                        for r in rows))
                    self.assertTrue(any("esporádica no migra" in r["detail"] for r in rows))
                    self.assertEqual(sum("START confirmado" in r["detail"] for r in rows), 2)
                    stats = session.rows("tasks.csv")
                    for node, task in ((str(failed), "1"), (str(survivor), "1"), (str(survivor), "4")):
                        st = next(r for r in stats if r["node"] == node and r["id"] == task)
                        self.assertGreater(int(st["completed"]), 0)
                        # Todas estas tareas hacen una sola UT: cada COMPLETE
                        # prueba exactamente un tick; SWITCH no es una duración.
                        self.assertEqual(st["run_ticks"], st["completed"])
                    st = next(r for r in stats if r["id"] == "2")
                    self.assertEqual(int(st["sporadic_ignored"]), 1)
                    self.assertTrue(session.rows("gantt.csv"))
                finally:
                    if session:
                        session.close()
                    for node in nodes:
                        node.close()

    def test_terminal_editing_and_case(self):
        with tempfile.TemporaryDirectory() as directory:
            nodes = [NativeNode(), NativeNode()]
            session = None
            try:
                session = HostSession(directory, nodes, terminal=True)
                session.wait("Comandos:")
                active = termios.tcgetattr(session.input_slave)
                self.assertFalse(active[3] & (termios.ICANON | termios.ECHO))
                # Ambos códigos de Backspace corrigen el CSV antes de enviarlo.
                session.command("\x7f\x08AdD 40,p,1,20,20,-5x\x7f\x085")
                session.wait("Tarea 40: DESPLEGADA y confirmada sin reiniciar tick")
                session.command("StAtUx\x7fS")
                session.wait("U=")
                # Ctrl+U permite recuperarse incluso de una línea excesiva.
                session.command("z" * 300 + "\x15AdD 41,P,1,20,20,2")
                session.wait("Tarea 41: DESPLEGADA y confirmada sin reiniciar tick")
                # Q sigue siendo inmediato: no requiere Enter.
                os.write(session.input_master, b"Q")
                stderr = session.finish(stop=None)
                self.assertEqual(termios.tcgetattr(session.input_slave), session.saved_terminal)
                output = "".join(session.lines)
                self.assertIn("\b \b", output)
                self.assertNotIn("^?", output)
                self.assertNotIn("Tarea inválida", stderr)
                rows = session.rows("events.csv")
                self.assertEqual(sum("STOP: confirmado" in row["detail"] for row in rows), 2)
                self.assertTrue(any(row["id"] == "40" and row["kind"] == "6" and
                                    row["code"] == "1" for row in rows))
            finally:
                if session:
                    session.close()
                for node in nodes:
                    node.close()

    def test_calibration_timeout_and_autonomy(self):
        with tempfile.TemporaryDirectory() as directory:
            nodes = [NativeNode(), NativeNode()]
            session = None
            try:
                # 50 * 50 ms = 2.5 s, supera timeout_ms=100 del INI.
                session = HostSession(directory, nodes, tick_us=50000)
                session.wait("Comandos:")
                session.finish(stop=False)
                rows = session.rows("events.csv")
                self.assertEqual(sum("START confirmado" in r["detail"] for r in rows), 2)
                # SIGTERM del host no debe enviar STOP ni detener el nodo.
                for node in nodes:
                    node.collect(0.35)
                    self.assertTrue(any(kind == 6 for kind, _ in node.frames))
                    self.assertEqual(node.exchange(8), (9, b"\x08\x00"))
            finally:
                if session:
                    session.close()
                for node in nodes:
                    node.close()

    def test_invalid_config_before_serial(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            tasks = path / "tasks.csv"
            tasks.write_text("1,P,1,10,10,1\n")
            config = path / "host.ini"
            base = f"esp_port=/dev/invalid1\npico_port=/dev/invalid2\ntasks={tasks}\n"
            config.write_text(base + "report_ticks=65536\n")
            result = subprocess.run([ROOT / "build/host", config], capture_output=True, timeout=3)
            self.assertEqual(result.returncode, 2)
            config.write_text(base)
            tasks.write_text("65536,P,1,10,10,1\n")
            result = subprocess.run([ROOT / "build/host", config], capture_output=True, timeout=3)
            self.assertEqual(result.returncode, 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
