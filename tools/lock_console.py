#!/usr/bin/env python3
"""Consola de la cerradura con animación real en la terminal.

El monitor de PlatformIO solo añade líneas, así que cada fotograma salía
como otra pantalla. Este visor redibuja la puerta en el mismo sitio.

  python3 tools/lock_console.py

Escribe comandos (unlock, tap 44CBC871, help) y Enter.
Salir: Ctrl-C.
"""

from __future__ import annotations

import curses
import queue
import sys
import threading
import time

try:
    import serial
except ImportError:
    print("Falta pyserial. Activa el entorno del proyecto.")
    sys.exit(1)

PORT = "/dev/cu.usbserial-120"
BAUD = 115200
DOOR_ROWS = 14


def door_lines(opening: float, denied: bool, shake: int, reason: str) -> list[str]:
    """opening 0 = cerrada, 1 = abierta. shake desplaza el marco."""
    opening = max(0.0, min(1.0, opening))
    pad = " " * (shake if denied else 0)
    leaf = int(round((1.0 - opening) * 10))
    inner = []
    for row in range(6):
        if denied and opening > 0.65:
            art = ["\\     /", " \\   / ", "  \\ /  ", "   X   ", "  / \\  ", " /   \\ "][row]
            inner.append(f"|   {art:^11}   |")
        elif leaf >= 8:
            knob = "    o    " if row == 3 else "         "
            panel = " ______ " if row in (1, 5) else f"|{knob}|" if 2 <= row <= 4 else "        "
            inner.append(f"|    {panel:^10}    |")
        else:
            gap = " " * (10 - leaf)
            slab = "/" * max(leaf, 0)
            light = " " if opening < 0.35 else "*" if row in (1, 4) else "·"
            body = (gap + light + slab).ljust(11)[:11]
            inner.append(f"|   {body}   |")
    if denied and opening > 0.65:
        title = "!! ACCESO DENEGADO !!"
        sub = (reason or "")[:28]
    elif opening > 0.92:
        title = ">> PUERTA ABIERTA <<"
        sub = ""
    elif opening < 0.08:
        title = "puerta cerrada"
        sub = ""
    else:
        title = "abriendo..." if not denied else "..."
        sub = ""
    lines = [
        "",
        pad + "   _____________________",
        pad + "  |                     |",
        *[pad + "  " + ln for ln in inner],
        pad + "  |_____________________|",
        f"  {title:^23}",
        f"  {sub:^23}",
    ]
    return lines[:DOOR_ROWS]


class Viewer:
    def __init__(self, stdscr: curses.window, port: str) -> None:
        self.scr = stdscr
        self.port = port
        self.logs: list[str] = []
        self.rx: queue.Queue[str] = queue.Queue()
        self.opening = 0.0
        self.denied = False
        self.reason = ""
        self.shake = 0
        self.anim: str | None = None
        self.phase = 0
        self.serial = None
        self.stop = False

    def start_serial(self) -> None:
        self.serial = serial.Serial(self.port, BAUD, timeout=0.05)

        def reader() -> None:
            buf = ""
            while not self.stop:
                try:
                    chunk = self.serial.read(256)
                except Exception:
                    break
                if not chunk:
                    continue
                buf += chunk.decode("utf-8", errors="replace")
                while "\n" in buf:
                    line, buf = buf.split("\n", 1)
                    self.rx.put(line.replace("\r", ""))

        threading.Thread(target=reader, daemon=True).start()

    def send(self, text: str) -> None:
        if self.serial is not None:
            self.serial.write((text + "\r\n").encode())

    def handle_line(self, line: str) -> None:
        if line.startswith("@@DOOR@@"):
            payload = line[len("@@DOOR@@") :].strip()
            kind, _, extra = payload.partition("|")
            if kind == "OPEN":
                self.anim = "open"
                self.phase = 0
                self.denied = False
                self.reason = ""
            elif kind == "CLOSE":
                self.anim = "close"
                self.phase = 0
            elif kind == "DENY":
                self.anim = "deny"
                self.phase = 0
                self.denied = True
                self.reason = extra
            return
        if line.strip():
            self.logs.append(line)
            self.logs = self.logs[-200:]

    def step_anim(self) -> None:
        if self.anim == "open":
            self.opening = min(1.0, self.opening + 0.08)
            self.shake = 0
            if self.opening >= 1.0:
                self.anim = None
        elif self.anim == "close":
            self.opening = max(0.0, self.opening - 0.1)
            self.denied = False
            if self.opening <= 0.0:
                self.anim = None
        elif self.anim == "deny":
            self.phase += 1
            self.opening = min(1.0, self.phase / 8)
            self.shake = 2 if self.phase % 2 == 0 else 0
            if self.phase > 16:
                self.anim = None
                self.shake = 0

    def draw(self, typed: str) -> None:
        self.scr.erase()
        h, w = self.scr.getmaxyx()
        lines = door_lines(self.opening, self.denied, self.shake, self.reason)
        color = curses.color_pair(2 if self.denied else 1)
        for i, text in enumerate(lines):
            if i >= h - 2:
                break
            self.scr.addnstr(i, 0, text, w - 1, color | curses.A_BOLD)
        log_top = DOOR_ROWS
        room = max(1, h - log_top - 2)
        for i, text in enumerate(self.logs[-room:]):
            self.scr.addnstr(log_top + i, 0, text, w - 1, curses.color_pair(3))
        prompt = "lock> " + typed
        self.scr.addnstr(h - 1, 0, prompt[: w - 1], w - 1, curses.color_pair(3))
        self.scr.move(h - 1, min(len(prompt), w - 1))
        self.scr.refresh()

    def run(self) -> None:
        curses.curs_set(1)
        curses.start_color()
        curses.use_default_colors()
        curses.init_pair(1, curses.COLOR_GREEN, -1)
        curses.init_pair(2, curses.COLOR_RED, -1)
        curses.init_pair(3, curses.COLOR_WHITE, -1)
        self.scr.nodelay(True)
        self.scr.timeout(40)
        self.start_serial()
        typed = ""
        while True:
            try:
                while True:
                    self.handle_line(self.rx.get_nowait())
            except queue.Empty:
                pass
            if self.anim:
                self.step_anim()
            ch = self.scr.getch()
            if ch in (3, 4):  # Ctrl-C / Ctrl-D
                break
            if ch in (10, 13) and typed:
                self.send(typed)
                self.logs.append("lock> " + typed)
                typed = ""
            elif ch in (curses.KEY_BACKSPACE, 127, 8):
                typed = typed[:-1]
            elif 32 <= ch < 127:
                typed += chr(ch)
            self.draw(typed)
        self.stop = True
        if self.serial is not None:
            self.serial.close()


def main() -> None:
    if not sys.stdout.isatty():
        print("Ábrelo en la terminal de Cursor, no en un monitor que solo acumule texto:")
        print("  python3 tools/lock_console.py")
        sys.exit(1)
    try:
        curses.wrapper(lambda scr: Viewer(scr, PORT).run())
    except serial.SerialException as exc:
        print(f"No se pudo abrir {PORT}: {exc}")
        print("Cierra el monitor de PlatformIO y vuelve a lanzar este visor.")
        sys.exit(1)


if __name__ == "__main__":
    main()
