#!/usr/bin/env python3
"""Serve this directory so the phone can load the page over Wi-Fi.

    python3 contrib/rows_sidecar/serve.py          # port 8080
    python3 contrib/rows_sidecar/serve.py 9000

Prints the URLs to type on the phone. Nothing is cached, so a reload on the
phone always picks up an edit.
"""

from __future__ import annotations

import http.server
import socket
import sys
from pathlib import Path

HERE = Path(__file__).parent


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw):
        super().__init__(*a, directory=str(HERE), **kw)

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, fmt, *args):
        sys.stderr.write("  %s\n" % (fmt % args))


def local_addresses() -> list[str]:
    addrs = set()
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("192.0.2.1", 1))  # never sends; just picks the default route
        addrs.add(s.getsockname()[0])
        s.close()
    except OSError:
        pass
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            addrs.add(info[4][0])
    except socket.gaierror:
        pass
    return sorted(a for a in addrs if not a.startswith("127."))


def main() -> int:
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    server = http.server.ThreadingHTTPServer(("0.0.0.0", port), Handler)
    print(f"serving {HERE} on port {port}")
    for addr in local_addresses():
        print(f"  http://{addr}:{port}/")
    print(f"  http://localhost:{port}/   (this machine)")
    print("ctrl-c to stop")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
