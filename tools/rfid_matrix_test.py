#!/usr/bin/env python3
"""Matriz SPI swap x velocidad: flashea, selftest, scans, resume."""
import re
import subprocess
import sys
import time
from collections import Counter
from pathlib import Path

try:
    import serial
except ImportError:
    print("pip install pyserial")
    sys.exit(1)

ROOT = Path(__file__).resolve().parents[1]
PROFILE = ROOT / "main/hardware_profile.h"
MAIN = ROOT / "main/main.c"
PORT = "/dev/cu.usbserial-120"
MATRIX = [
    (0, 100_000),
    (0, 500_000),
    (1, 100_000),
    (1, 500_000),
    (1, 1_000_000),
]


def patch(swap: int, hz: int) -> None:
    prof = PROFILE.read_text()
    prof = re.sub(
        r"#define HW_SPI_SWAP_MOSI_MISO \d",
        f"#define HW_SPI_SWAP_MOSI_MISO {swap}",
        prof,
    )
    PROFILE.write_text(prof)
    main = MAIN.read_text()
    main = re.sub(
        r"\.clock_speed_hz = \d+,",
        f".clock_speed_hz = {hz},",
        main,
    )
    MAIN.write_text(main)


def flash() -> bool:
    r = subprocess.run(
        ["pio", "run", "-t", "upload"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    return r.returncode == 0


def serial_session(scans: int = 18) -> dict:
    s = serial.Serial(PORT, 115200, timeout=0.2)
    s.dtr = False
    s.rts = True
    time.sleep(0.08)
    s.rts = False
    time.sleep(3.2)
    boot = b""
    for _ in range(20):
        boot += s.read(8192)
        time.sleep(0.08)
    boot_t = boot.decode("utf-8", errors="replace")
    fifo_ok = "fifo_rw=OK" in boot_t
    if "prueba FIFO fallo" in boot_t or "Buffers content" in boot_t:
        fifo_ok = False

    s.write(b"selftest\r\n")
    time.sleep(0.9)
    st = s.read(4000).decode("utf-8", errors="replace")
    if "fifo_rw=OK" in st:
        fifo_ok = True

    s.write(b"diag reset\r\n")
    time.sleep(0.3)
    s.read(500)

    scan_errs = Counter()
    uid_samples = []
    for _ in range(scans):
        s.write(b"scan\r\n")
        time.sleep(1.0)
        o = s.read(5000).decode("utf-8", errors="replace")
        if "scan manual" in o or "UID=" in o:
            scan_errs["UID_OK"] += 1
            uid_samples.append(o[-200:])
        else:
            m = re.search(r"fallo \(0x([0-9A-Fa-f]+)\)", o)
            scan_errs[m.group(1) if m else "?"] += 1

    s.write(b"diag\r\n")
    time.sleep(0.8)
    diag = s.read(4000).decode("utf-8", errors="replace")
    s.close()

    fw_m = re.search(r"version reg = 0x([0-9A-Fa-f]+)", st + boot_t)
    fw = fw_m.group(1) if fw_m else "??"
    m_ok = re.search(r"ok=(\d+)", diag)
    m_col = re.search(r"colision=(\d+)", diag)
    m_to = re.search(r"timeout=(\d+)", diag)
    m_sel = re.search(r"SELECT\s+: ok=(\d+)", diag)

    return {
        "fw": fw,
        "fifo_ok": fifo_ok,
        "selftest": st[-600:],
        "scans": dict(scan_errs),
        "uid_samples": uid_samples[:2],
        "poll_ok": int(m_ok.group(1)) if m_ok else -1,
        "poll_col": int(m_col.group(1)) if m_col else -1,
        "poll_to": int(m_to.group(1)) if m_to else -1,
        "select_ok": int(m_sel.group(1)) if m_sel else -1,
    }


def main() -> None:
    results = []
    for swap, hz in MATRIX:
        label = f"swap={swap} hz={hz}"
        print(f"\n######## {label} ########")
        patch(swap, hz)
        if not flash():
            print("FLASH FAIL", label)
            results.append((label, {"error": "flash"}))
            continue
        time.sleep(1)
        try:
            r = serial_session()
        except Exception as e:
            r = {"error": str(e)}
        r["label"] = label
        results.append((label, r))
        print(
            f"  fw=0x{r.get('fw','?')} fifo={r.get('fifo_ok')} "
            f"scans={r.get('scans')} poll_ok={r.get('poll_ok')} select_ok={r.get('select_ok')}"
        )

    # restore best default
    patch(1, 500_000)

    out = ROOT / ".docs/diagnostico-rfid-matriz.md"
    lines = ["# Matriz diagnóstico RC522\n", f"Puerto: `{PORT}`\n"]
    for label, r in results:
        lines.append(f"\n## {label}\n")
        if "error" in r:
            lines.append(f"- Error: {r['error']}\n")
            continue
        lines.append(f"- FW: 0x{r['fw']}\n")
        lines.append(f"- FIFO test (selftest): {r['fifo_ok']}\n")
        lines.append(f"- Scans manuales: `{r['scans']}`\n")
        lines.append(
            f"- Sondeo auto: ok={r['poll_ok']} col={r['poll_col']} timeout={r['poll_to']} select_ok={r['select_ok']}\n"
        )
        if r.get("uid_samples"):
            lines.append(f"- Ejemplo UID: `{r['uid_samples'][0][:120]}`\n")

    lines.append("\n## Conclusión automática\n")
    best = max(
        (r for _, r in results if "scans" in r),
        key=lambda x: x.get("scans", {}).get("UID_OK", 0),
        default=None,
    )
    if best:
        lines.append(
            f"- Mejor tasa UID en scans: `{best.get('label')}` → {best.get('scans')}\n"
        )
    out.write_text("".join(lines))
    print(f"\nInforme: {out}")


if __name__ == "__main__":
    main()
