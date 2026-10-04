#!/usr/bin/env python3
"""Serves dist/ with the cross-origin isolation headers Boxedwine's audio path wants."""
import http.server
import sys
from functools import partial


class Handler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        super().end_headers()


port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
directory = sys.argv[2] if len(sys.argv) > 2 else "dist"
http.server.ThreadingHTTPServer(("127.0.0.1", port), partial(Handler, directory=directory)).serve_forever()
