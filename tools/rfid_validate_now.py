#!/usr/bin/env python3
"""Validación rápida post-cableado: flashea, selftest, scans, sondeo."""
import re
import subprocess
import sys
import time
from collections import Counter
from pathlib import Path

import serial

ROOT = Path(__file__).resolve().parents[1]
PROFILE = ROOT / "main/hardware_profile.h"
MAIN = ROOT / "main/main.c"
PORT = "/dev/cu.usbserial-120"


def patch(swap: int, hz: int) -> None:
    prof = PROFILE.read_text()
    prof = re.sub(
        r"#define HW_SPI_SWAP_MOSI_MISO \d",
        f"#define HW_SPI_SWAP_MOSI_MISO {swap}",
        prof,
    )
    PROFILE.write_text(prof)
    main = MAIN.read_text()
    main = re.sub(r"\.clock_speed_hz = \d+,", f".clock_speed_hz = {hz},", main)
    MAIN.write_text(main)


def flash() -> bool:
    return (
        subprocess.run(["pio", "run", "-t", "upload"], cwd=ROOT, capture_output=True).returncode
        == 0
    )


def run_case(swap: int, hz: int, scans: int = 25) -> dict:
    patch(swap, hz)
    if not flash():
        return {"error": "flash_fail"}
    time.sleep(1.5)
    s = serial.Serial(PORT, 115200, timeout=0.2)
    s.dtr = False
    s.rts = True
    time.sleep(0.1)
    s.rts = False
    time.sleep(3.5)

    boot = b""
    for _ in range(25):
        boot += s.read(8192)
        time.sleep(0.06)
    boot_t = boot.decode("utf-8", errors="replace")

    s.write(b"diag reset\r\n")
    time.sleep(0.4)
    s.read(800)
    s.write(b"selftest\r\n")
    time.sleep(1.2)
    st = s.read(5000).decode("utf-8", errors="replace")

    fw_m = re.search(r"version=0x([0-9A-Fa-f]+)", st)
    fifo_ok = "fifo_rw=OK" in st
    ant_on = "antena_RF=ON" in st

    errs = Counter()
    uid_lines = []
    for _ in range(scans):
        s.write(b"scan\r\n")
        time.sleep(1.05)
        o = s.read(6000).decode("utf-8", errors="replace")
        if "scan manual" in o:
            errs["UID"] += 1
            m = re.search(r"UID=([0-9A-Fa-f ]+)", o)
            if m:
                uid_lines.append(m.group(1).strip())
        elif "SELECT fallo" in o:
            m = re.search(r"SELECT fallo \(0x([0-9A-Fa-f]+)\)", o)
            errs["SEL_" + (m.group(1) if m else "?")] += 1
        else:
            m = re.search(r"REQA/WUPA fallo \(0x([0-9A-Fa-f]+)\)", o)
            errs["REQ_" + (m.group(1) if m else "?")] += 1

    s.write(b"diag\r\n")
    time.sleep(0.9)
    diag = s.read(4000).decode("utf-8", errors="replace")

    s.write(b"format on\r\n")
    time.sleep(0.3)
    listen = ""
    t0 = time.time()
    while time.time() - t0 < 12:
        listen += s.read(8192).decode("utf-8", errors="replace")
        time.sleep(0.04)

    s.close()

    def diag_int(name):
        m = re.search(rf"{name}=(\d+)", diag)
        return int(m.group(1)) if m else 0

    rfid_hits = [
        ln
        for ln in listen.splitlines()
        if "[RFID]" in ln and any(k in ln for k in ("UID", "lectura", "CONCEDIDO", "provision", "DENIED"))
    ]

    return {
        "swap": swap,
        "hz": hz,
        "fw": fw_m.group(1) if fw_m else "??",
        "fifo_ok": fifo_ok,
        "ant_on": ant_on,
        "scans": dict(errs),
        "uids": list(dict.fromkeys(uid_lines)),
        "poll_ok": diag_int("ok"),
        "poll_col": diag_int("colision"),
        "poll_to": diag_int("timeout"),
        "select_ok": diag_int(r"SELECT\s+: ok"),
        "boot_fifo_warn": "FIFO" in boot_t or "rw test" in boot_t,
        "rfid_listen": rfid_hits[:8],
        "selftest_tail": st[-400:],
    }


def main():
    cases = [(0, 100_000), (0, 500_000), (1, 100_000), (1, 500_000)]
    results = []
    for swap, hz in cases:
        label = f"swap={swap} hz={hz}"
        print(f"\n=== {label} ===", flush=True)
        r = run_case(swap, hz)
        r["label"] = label
        results.append(r)
        if "error" in r:
            print("  FLASH FAIL")
            continue
        print(
            f"  fw=0x{r['fw']} fifo={r['fifo_ok']} ant={r['ant_on']} "
            f"scans={r['scans']} poll_ok={r['poll_ok']} sel_ok={r['select_ok']} uids={r['uids']}"
        )

    best = max(
        (r for r in results if "scans" in r),
        key=lambda x: (x["scans"].get("UID", 0), x["fifo_ok"], x["fw"] == "92"),
        default=None,
    )
    if best:
        patch(best["swap"], best["hz"])
        flash()
        print(f"\n>>> Config guardada: swap={best['swap']} hz={best['hz']}")

    out = ROOT / ".docs/diagnostico-rfid-ultimo.md"
    lines = ["# Última validación RFID\n\n"]
    for r in results:
        lines.append(f"## {r.get('label','?')}\n\n")
        if r.get("error"):
            lines.append(f"- {r['error']}\n\n")
            continue
        lines.append(f"- FW: 0x{r['fw']}, FIFO OK: {r['fifo_ok']}, Antena: {r['ant_on']}\n")
        lines.append(f"- Scans: `{r['scans']}`\n")
        lines.append(f"- UIDs: {r['uids']}\n")
        lines.append(f"- Poll: ok={r['poll_ok']} col={r['poll_col']} to={r['poll_to']} select_ok={r['select_ok']}\n")
        if r.get("rfid_listen"):
            lines.append("- Escucha format on:\n")
            for ln in r["rfid_listen"]:
                lines.append(f"  - {ln.strip()}\n")
        lines.append("\n")
    if best:
        lines.append(f"## Config elegida\n\nswap={best['swap']}, {best['hz']} Hz\n")
    out.write_text("".join(lines))
    print(f"Informe: {out}")


if __name__ == "__main__":
    main()
