"""复位开发板并从开机第一行开始抓串口日志。

用途:ESP32 开机自检的输出只在启动瞬间打印一次,如果先打开串口再上电就
会全部错过。这个脚本先打开串口、再拉 RTS 复位芯片,所以能抓到完整的引导日志。

用法:
    python capture_boot.py --port COM6 --seconds 15 --out boot.log
    python capture_boot.py --port COM6 --seconds 15 --no-reset   # 只抓,不复位
"""

from __future__ import annotations

import argparse
import sys
import time

import serial


def reset_classic(port: serial.Serial) -> None:
    """常见 USB-TTL 复位电路:DTR/RTS 经三极管控制 EN 与 IO0。"""
    port.dtr = False
    port.rts = True      # EN 拉低 = 复位
    time.sleep(0.15)
    port.rts = False     # 释放 = 运行
    time.sleep(0.05)


def reset_inverted(port: serial.Serial) -> None:
    """部分板子 DTR/RTS 极性相反。"""
    port.dtr = True
    port.rts = False
    time.sleep(0.15)
    port.rts = True
    time.sleep(0.05)
    port.rts = False
    port.dtr = False


def capture(port: serial.Serial, seconds: float) -> list[str]:
    lines: list[str] = []
    deadline = time.time() + seconds
    pending = b""
    while time.time() < deadline:
        chunk = port.read(port.in_waiting or 1)
        if not chunk:
            continue
        pending += chunk
        while b"\n" in pending:
            raw, pending = pending.split(b"\n", 1)
            lines.append(raw.rstrip(b"\r").decode("utf-8", "replace"))
    if pending:
        lines.append(pending.decode("utf-8", "replace"))
    return lines


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM6")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--seconds", type=float, default=15.0)
    ap.add_argument("--out", default="")
    ap.add_argument("--no-reset", action="store_true")
    args = ap.parse_args()

    with serial.Serial(args.port, args.baud, timeout=0.2) as port:
        if not args.no_reset:
            reset_classic(port)
        lines = capture(port, args.seconds)

        # 复位极性不对时几乎抓不到东西,换一种极性再试一次。
        if not args.no_reset and len(lines) < 3:
            print("[capture] 第一次复位没抓到输出,换极性重试", file=sys.stderr)
            reset_inverted(port)
            lines = capture(port, args.seconds)

    if args.out:
        with open(args.out, "w", encoding="utf-8") as handle:
            handle.write("\n".join(lines) + "\n")
        print(f"[capture] {len(lines)} 行 -> {args.out}", file=sys.stderr)
    else:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        print("\n".join(lines))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
