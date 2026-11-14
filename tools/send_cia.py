"""Installs the CIA on a 3DS over Wi-Fi, with FBI's Remote Install (no SD card shuffling).

On the 3DS: FBI -> Remote Install -> Receive URLs over the network. It shows its IP address.
Then, on this computer (same Wi-Fi):

    python3 tools/send_cia.py <3DS IP> [file.cia]     (default: build-3ds/soulsilver.cia)

This serves the file over HTTP from this computer and sends its URL to FBI (port 5000, the
protocol of FBI's own servefiles.py: a 4-byte big-endian length, then the URLs, one per line);
FBI downloads and installs it, then closes the connection, and this exits.
"""
import functools
import http.server
import os
import socket
import struct
import sys
import threading
import urllib.parse

FBI_PORT = 5000


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    ds_ip = sys.argv[1]
    path = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), "..", "build-3ds", "soulsilver.cia"))
    if not os.path.isfile(path):
        sys.exit(f"send_cia: no file at {path} (build and package first: port/package.sh)")

    # this computer's address as the 3DS sees it (the interface that routes to it)
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    probe.connect((ds_ip, FBI_PORT))
    host = probe.getsockname()[0]
    probe.close()

    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=os.path.dirname(path))
    server = http.server.ThreadingHTTPServer((host, 0), handler)
    port = server.server_address[1]
    threading.Thread(target=server.serve_forever, daemon=True).start()
    url = f"http://{host}:{port}/{urllib.parse.quote(os.path.basename(path))}"
    print(f"send_cia: serving {path} ({os.path.getsize(path) // 1024} KB) at {url}")

    with socket.create_connection((ds_ip, FBI_PORT), timeout=10) as sock:
        payload = url.encode("ascii")
        sock.sendall(struct.pack("!L", len(payload)) + payload)
        print("send_cia: sent to FBI; confirm on the 3DS if it asks, then wait for the install")
        sock.settimeout(None)
        while True:  # (FBI closes the connection when it's done)
            try:
                if not sock.recv(1):
                    break
            except OSError:
                break
    server.shutdown()
    print("send_cia: done")


if __name__ == "__main__":
    main()
