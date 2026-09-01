#!/usr/bin/env python3
"""Local demo and test harness for Oma Tab.

Serves demo/index.html on 127.0.0.1 and exposes a small read-mostly API:

  GET  /api/status              -> `omatab status --json`
  GET  /api/events?after=OFFSET -> telemetry events appended since OFFSET
  POST /api/action/NAME         -> a fixed allowlist of `omatab` commands

Only loopback is bound. The page is a playground for typing with the input
method plus a live view of what the addon recorded for each keystroke.
"""
import argparse
import http.server
import json
import os
import pathlib
import subprocess
import sys
import urllib.parse
import webbrowser

ROOT = pathlib.Path(__file__).resolve().parent
STATE_HOME = os.environ.get("XDG_STATE_HOME") or os.path.expanduser("~/.local/state")
LOG_PATH = os.environ.get("OMATAB_LOG_PATH") or os.path.join(STATE_HOME, "omatab", "events.jsonl")

ACTIONS = {
    "enable": ["enable"],
    "disable": ["disable"],
    "ocr-on": ["ocr", "on"],
    "ocr-off": ["ocr", "off"],
    "telemetry-on": ["telemetry", "on"],
    "telemetry-off": ["telemetry", "off"],
}

# Fields worth sending to the page; the raw request/response bodies and the
# full OCR text stay out of the browser.
EVENT_FIELDS = {
    "model_result": ["timestamp", "outcome", "suggestion", "latency_ms", "textbox_prefix",
                     "window_class", "ocr_age_ms", "request_id", "error"],
    "accept_word": ["timestamp", "accepted_piece", "remaining_suggestion", "original_suggestion", "request_id"],
    "accept_full": ["timestamp", "accepted_piece", "original_suggestion", "request_id"],
    "dismiss": ["timestamp", "original_suggestion", "request_id"],
    "typed_over": ["timestamp", "typed", "matched", "remaining_suggestion", "request_id"],
    "config_applied": ["timestamp", "full_accept_key", "screen_context"],
    "service_start": ["timestamp", "full_accept_key", "screen_context", "telemetry_enabled"],
    "disabled": ["timestamp"],
}


def run_omatab(args):
    try:
        completed = subprocess.run(["omatab", *args], capture_output=True, text=True, timeout=20)
    except FileNotFoundError:
        return 503, {"error": "omatab is not installed or not on PATH"}
    except subprocess.TimeoutExpired:
        return 504, {"error": "omatab timed out"}
    if completed.returncode != 0:
        return 500, {"error": completed.stderr.strip() or f"omatab exited {completed.returncode}"}
    if not completed.stdout.strip():
        return 200, {"ok": True}
    try:
        return 200, json.loads(completed.stdout)
    except json.JSONDecodeError:
        return 200, {"ok": True, "output": completed.stdout.strip()}


def read_events(after):
    try:
        size = os.path.getsize(LOG_PATH)
    except OSError:
        return {"offset": 0, "exists": False, "events": []}
    if after is None or after > size:
        # First call, or the log rotated: start from the end so the page only
        # shows what happens from now on.
        return {"offset": size, "exists": True, "events": []}
    events = []
    with open(LOG_PATH, "rb") as handle:
        handle.seek(after)
        chunk = handle.read()
    # Only consume complete lines; a partial trailing line is read next time.
    last_newline = chunk.rfind(b"\n")
    if last_newline < 0:
        return {"offset": after, "exists": True, "events": []}
    consumed = last_newline + 1
    for line in chunk[:consumed].splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        kind = event.get("type")
        fields = EVENT_FIELDS.get(kind)
        if not fields:
            continue
        trimmed = {"type": kind}
        for key in fields:
            if key in event:
                value = event[key]
                if key == "textbox_prefix" and isinstance(value, str):
                    value = value[-60:]
                trimmed[key] = value
        events.append(trimmed)
    return {"offset": after + consumed, "exists": True, "events": events}


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(ROOT), **kwargs)

    def log_message(self, *_):
        pass

    def send_json(self, status, payload):
        body = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        if url.path == "/api/status":
            status, payload = run_omatab(["status", "--json"])
            return self.send_json(status, payload)
        if url.path == "/api/events":
            query = urllib.parse.parse_qs(url.query)
            after = query.get("after", [None])[0]
            try:
                after = int(after) if after is not None else None
            except ValueError:
                after = None
            return self.send_json(200, read_events(after))
        if url.path == "/":
            self.path = "/index.html"
        return super().do_GET()

    def do_POST(self):
        url = urllib.parse.urlparse(self.path)
        prefix = "/api/action/"
        if not url.path.startswith(prefix):
            return self.send_json(404, {"error": "unknown endpoint"})
        name = url.path[len(prefix):]
        if name not in ACTIONS:
            return self.send_json(400, {"error": f"unknown action {name}"})
        status, payload = run_omatab(ACTIONS[name])
        if status != 200:
            return self.send_json(status, payload)
        status, payload = run_omatab(["status", "--json"])
        return self.send_json(status, payload)


def main():
    parser = argparse.ArgumentParser(description="Serve the Oma Tab demo page.")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--open", action="store_true", help="open the page in the default browser")
    args = parser.parse_args()

    server = None
    port = args.port
    for candidate in range(port, port + 20):
        try:
            server = http.server.ThreadingHTTPServer(("127.0.0.1", candidate), Handler)
            port = candidate
            break
        except OSError:
            continue
    if server is None:
        print("Could not bind a loopback port for the demo", file=sys.stderr)
        return 1

    url = f"http://127.0.0.1:{port}/"
    print(f"Oma Tab demo: {url}  (Ctrl+C to stop)")
    if args.open:
        webbrowser.open(url)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
