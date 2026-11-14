"""Receives the game's log over Wi-Fi (port/runtime/netlog.c): prints each line and appends it to
captures/netlog.txt. The game sends it when the SD card has sdmc:/3ds/soulsilver-3ds/netlog.txt
holding this computer's address (printed below). At each start-up the game also sends the
previous run's last lines ("prev|") and any new Luma crash dump's registers ("crash|").

    python3 tools/recv_log.py [port]      (default 5555; Ctrl-C to stop)
"""
import os
import socket
import sys
import time


def local_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("10.255.255.255", 1))  # (no packet sent: just picks the LAN interface)
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 5555
    out = os.path.join(os.path.dirname(__file__), "..", "captures", "netlog.txt")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", port))
    print(f"recv_log: listening on UDP {port}; netlog.txt on the SD card should hold: {local_ip()}")
    with open(out, "a", encoding="utf-8") as f:
        f.write(f"\n---- {time.strftime('%Y-%m-%d %H:%M:%S')} ----\n")
        while True:
            data, _ = sock.recvfrom(2048)
            text = data.decode("utf-8", "replace")
            sys.stdout.write(text)
            sys.stdout.flush()
            f.write(text)
            f.flush()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
