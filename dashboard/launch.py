#!/usr/bin/env python3
"""Launch the GBF dashboard in the default browser."""
import subprocess, webbrowser, pathlib, http.server, threading, time

PORT = 8731
ROOT = pathlib.Path(__file__).parent

class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw):
        super().__init__(*a, directory=str(ROOT), **kw)
    def log_message(self, *_): pass

def serve():
    with http.server.HTTPServer(("", PORT), Handler) as s:
        s.serve_forever()

t = threading.Thread(target=serve, daemon=True)
t.start()
time.sleep(0.3)
webbrowser.open(f"http://localhost:{PORT}/index.html")
print(f"Dashboard running at http://localhost:{PORT}/index.html")
print("Press Ctrl+C to stop.")
try:
    t.join()
except KeyboardInterrupt:
    print("\nStopped.")
