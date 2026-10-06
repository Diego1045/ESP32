#!/usr/bin/env python3
"""Ventana gráfica de la puerta con animación suave (Pygame).

El Python de Homebrew suele no traer Tkinter; este visor usa pygame-ce.

  pip install pygame-ce pyserial
  python3 tools/lock_gui.py

En Python 3.14 el paquete «pygame» clásico falla al cargar fuentes; usa pygame-ce.

Cierra el monitor de PlatformIO antes de abrir el puerto.
"""

from __future__ import annotations

import argparse
import queue
import sys
import threading

try:
    import pygame
except ImportError:
    print("Falta pygame-ce. Instala con: pip install pygame-ce")
    sys.exit(1)


def _load_fonts() -> tuple:
    """Fuentes del visor; en 3.14 «pygame» rompe font — hace falta pygame-ce."""
    try:
        return (
            pygame.font.SysFont("Helvetica", 26, bold=True),
            pygame.font.SysFont("Helvetica", 32, bold=True),
            pygame.font.SysFont("Helvetica", 18),
            pygame.font.SysFont("Menlo", 14),
            pygame.font.SysFont("Menlo", 13),
        )
    except (NotImplementedError, ImportError) as exc:
        print("No se pudieron cargar las fuentes de pygame:", exc)
        print("En Python 3.14 instala: pip uninstall pygame -y && pip install pygame-ce")
        sys.exit(1)

try:
    import serial
except ImportError:
    print("Falta pyserial. Activa el entorno del proyecto.")
    sys.exit(1)

DEFAULT_PORT = "/dev/cu.usbserial-120"
BAUD = 115200
FPS = 60

BG = (26, 26, 46)
PANEL = (22, 33, 62)
LOG_BG = (15, 15, 26)
TEXT = (200, 200, 216)
GREEN = (123, 237, 159)
RED = (255, 71, 87)
BLUE = (112, 161, 255)
GRAY = (160, 160, 176)
BTN = (45, 58, 92)
BTN_HOVER = (61, 74, 108)


class DoorApp:
    def __init__(self, port: str) -> None:
        pygame.init()
        pygame.display.set_caption("Cerradura ESP32 — Puerta")
        self.screen = pygame.display.set_mode((560, 720))
        self.clock = pygame.time.Clock()
        self.port = port

        (
            self.font_title,
            self.font_deny,
            self.font_body,
            self.font_mono,
            self.font_small,
        ) = _load_fonts()

        self.rx: queue.Queue[str] = queue.Queue()
        self.serial: serial.Serial | None = None
        self.stop = False

        self.opening = 0.0
        self.denied = False
        self.reason = ""
        self.shake_x = 0.0
        self.anim: str | None = None
        self.phase = 0
        self.logs: list[str] = []
        self.status = "Conectando…"
        self.status_color = GRAY

        self.cmd = ""
        self.buttons = [
            ("unlock", "unlock", pygame.Rect(12, 668, 90, 36)),
            ("tap demo", "tap 44CBC871", pygame.Rect(108, 668, 100, 36)),
            ("selftest", "selftest", pygame.Rect(214, 668, 100, 36)),
        ]
        self._start_serial()

    def _start_serial(self) -> None:
        try:
            self.serial = serial.Serial(self.port, BAUD, timeout=0.05)
        except serial.SerialException as exc:
            self.status = "Sin puerto serie"
            self.status_color = RED
            self._append_log(f"Error: {exc}")
            return

        self.status = f"Listo — {self.port}"
        self.status_color = GREEN

        def reader() -> None:
            buf = ""
            while not self.stop and self.serial is not None:
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
        if self.serial is None:
            return
        self.serial.write((text + "\r\n").encode())
        self._append_log("lock> " + text)

    def _append_log(self, line: str) -> None:
        self.logs.append(line)
        self.logs = self.logs[-12:]

    def _handle_line(self, line: str) -> None:
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
            self._append_log(line)

    def _step_anim(self) -> None:
        if self.anim == "open":
            self.opening = min(1.0, self.opening + 0.06)
            self.shake_x = 0.0
            if self.opening >= 1.0:
                self.anim = None
        elif self.anim == "close":
            self.opening = max(0.0, self.opening - 0.08)
            self.denied = False
            self.reason = ""
            if self.opening <= 0.0:
                self.anim = None
        elif self.anim == "deny":
            self.phase += 1
            self.opening = min(0.35, self.phase / 24)
            self.shake_x = 8.0 if self.phase % 2 == 0 else -8.0
            if self.phase > 28:
                self.anim = None
                self.shake_x = 0.0
                self.opening = 0.0

    def _update_status(self) -> None:
        if self.denied and (self.reason or self.phase > 4):
            self.status = ""
            self.status_color = RED
        elif self.opening > 0.9 and not self.denied:
            self.status = "Acceso concedido"
            self.status_color = GREEN
        elif self.opening < 0.05:
            self.status = "Puerta cerrada"
            self.status_color = GRAY
        elif self.anim == "open":
            self.status = "Abriendo…"
            self.status_color = BLUE
        elif self.anim == "close":
            self.status = "Cerrando…"
            self.status_color = GRAY
        elif self.denied:
            self.status = ""
            self.status_color = RED

    def _draw_door(self, surf: pygame.Surface) -> None:
        door_rect = pygame.Rect(40, 48, 480, 400)
        pygame.draw.rect(surf, PANEL, door_rect, border_radius=8)

        w, h = door_rect.width - 80, 320
        cx = door_rect.centerx
        cy = door_rect.centery
        ox = int(self.shake_x)
        fx = cx - w // 2 + ox
        fy = cy - h // 2

        pygame.draw.rect(surf, (61, 61, 74), (fx - 8, fy - 8, w + 16, h + 16), border_radius=4)
        pygame.draw.rect(surf, (13, 13, 18), (fx, fy, w, h))

        gap = int(self.opening * (w * 0.55))
        leaf_w = max(40, w - gap - 12)
        leaf_x = fx + 6 + gap

        if self.opening > 0.05:
            glow = (26, 77, 46) if not self.denied else (77, 26, 26)
            pygame.draw.rect(surf, glow, (fx + 6, fy + 6, w - 12, h - 12))
            if self.opening > 0.4 and not self.denied:
                for i in range(5):
                    pygame.draw.ellipse(
                        surf,
                        (255, 217, 61),
                        (fx + w // 2 - 30 + i * 10, fy + 80, 40, 120),
                    )

        leaf_color = (92, 64, 51) if not self.denied else (107, 48, 48)
        pygame.draw.rect(surf, leaf_color, (leaf_x, fy + 6, leaf_w, h - 12))
        if leaf_w > 24:
            pygame.draw.rect(
                surf,
                (74, 53, 40),
                (leaf_x + 12, fy + 50, leaf_w - 24, h - 130),
            )
        if leaf_w > 50:
            knob_x = leaf_x + leaf_w - 28
            pygame.draw.circle(surf, (201, 162, 39), (knob_x + 8, cy), 10)

        if self.denied and self.phase > 4:
            pygame.draw.line(surf, RED, (fx + 40, fy + 60), (fx + w - 40, fy + h - 60), 8)
            pygame.draw.line(surf, RED, (fx + w - 40, fy + 60), (fx + 40, fy + h - 60), 8)
            deny_s = self.font_deny.render("ACCESO DENEGADO", True, RED)
            surf.blit(deny_s, deny_s.get_rect(center=(cx, fy + h + 36)))

        if self.opening > 0.92 and not self.denied:
            open_s = self.font_title.render("PUERTA ABIERTA", True, GREEN)
            surf.blit(open_s, open_s.get_rect(center=(cx, fy - 28)))

    def _draw_ui(self) -> None:
        self.screen.fill(BG)
        self._draw_door(self.screen)

        y_status = 468
        if self.status:
            st = self.font_title.render(self.status, True, self.status_color)
            self.screen.blit(st, st.get_rect(center=(280, y_status)))

        reason_text = self.reason if self.denied else ""
        if reason_text:
            reason_s = self.font_body.render(reason_text, True, RED)
            self.screen.blit(reason_s, reason_s.get_rect(center=(280, y_status + 32)))

        log_rect = pygame.Rect(12, 500, 536, 108)
        pygame.draw.rect(self.screen, LOG_BG, log_rect, border_radius=4)
        for i, line in enumerate(self.logs):
            ls = self.font_mono.render(line[:72], True, TEXT)
            self.screen.blit(ls, (20, 508 + i * 18))

        pygame.draw.rect(self.screen, LOG_BG, pygame.Rect(12, 620, 536, 36), border_radius=4)
        prompt = f"lock> {self.cmd}_"
        ps = self.font_small.render(prompt, True, TEXT)
        self.screen.blit(ps, (20, 628))

        mx, my = pygame.mouse.get_pos()
        for label, _cmd, rect in self.buttons:
            color = BTN_HOVER if rect.collidepoint(mx, my) else BTN
            pygame.draw.rect(self.screen, color, rect, border_radius=6)
            bs = self.font_small.render(label, True, TEXT)
            self.screen.blit(bs, bs.get_rect(center=rect.center))

        pygame.display.flip()

    def _poll_serial(self) -> None:
        try:
            while True:
                self._handle_line(self.rx.get_nowait())
        except queue.Empty:
            pass

    def _handle_click(self, pos: tuple[int, int]) -> None:
        for _label, cmd, rect in self.buttons:
            if rect.collidepoint(pos):
                self.send(cmd)
                return

    def run(self) -> None:
        running = True
        while running:
            for event in pygame.event.get():
                if event.type == pygame.QUIT:
                    running = False
                elif event.type == pygame.MOUSEBUTTONDOWN and event.button == 1:
                    self._handle_click(event.pos)
                elif event.type == pygame.KEYDOWN:
                    if event.key == pygame.K_ESCAPE:
                        running = False
                    elif event.key == pygame.K_RETURN:
                        text = self.cmd.strip()
                        if text:
                            self.send(text)
                        self.cmd = ""
                    elif event.key == pygame.K_BACKSPACE:
                        self.cmd = self.cmd[:-1]
                    elif event.unicode and event.unicode.isprintable() and len(self.cmd) < 64:
                        self.cmd += event.unicode

            self._poll_serial()
            if self.anim:
                self._step_anim()
            self._update_status()
            self._draw_ui()
            self.clock.tick(FPS)

        self.stop = True
        if self.serial is not None:
            try:
                self.serial.close()
            except Exception:
                pass
        pygame.quit()


def main() -> None:
    parser = argparse.ArgumentParser(description="Visor gráfico de la puerta ESP32")
    parser.add_argument("--port", default=DEFAULT_PORT, help="Puerto serie USB")
    args = parser.parse_args()
    DoorApp(args.port).run()


if __name__ == "__main__":
    main()
