"""Owned HTTP fixtures with deterministic delayed commits and resources."""
import html
import http.server
import json
import threading
import time
from urllib.parse import parse_qs, urlsplit

_lock = threading.Lock()
_sequence = 0


class FixtureHandler(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        global _sequence
        path = urlsplit(self.path)
        query = parse_qs(path.query)
        if not path.path.startswith('/nav-'):
            return super().do_GET()

        def number(key):
            return min(2000, max(0, int(query.get(key, ['0'])[0])))

        try:
            time.sleep(number('delay') / 1000)
            if path.path == '/nav-redirect':
                target = query.get('to', ['/page.html'])[0]
                if not target.startswith('/') or target.startswith('//'):
                    return self.send_error(400)
                self.send_response(302)
                self.send_header('Location', target)
                self.send_header('Content-Length', '0')
                self.send_header('Cache-Control', 'no-store')
                self.end_headers()
                return
            if path.path == '/nav-empty':
                self.send_response(204)
                self.end_headers()
                return
            if path.path == '/nav-drop':
                self.close_connection = True
                return
            if path.path == '/nav-asset':
                payload = b'<svg xmlns="http://www.w3.org/2000/svg" width="1" height="1"></svg>'
                mime = 'image/svg+xml'
            elif path.path == '/nav-slow.html':
                label = query.get('label', ['Owned navigation'])[0]
                with _lock:
                    _sequence += 1
                    sequence = _sequence
                payload = (f'<!doctype html><meta charset="utf-8"><title>{html.escape(label)}</title>'
                           f'<script>window.navSequence={sequence};window.navLabel={json.dumps(label)};'
                           'window.navLoaded=false;window.navFetched=false;'
                           "addEventListener('load',()=>{navLoaded=true;document.body.dataset.loaded='yes'});"
                           f"fetch('/nav-asset?delay={number('fetch')}')"
                           + (".then(r=>r.text())" if query.get('consume', ['1'])[0] != '0' else '') +
                           ".then(()=>navFetched=true);</script>"
                           f'<body><h1>{html.escape(label)}</h1><input id="nav-input">'
                           f'<img src="/nav-asset?delay={number("asset")}&image=1">'
                           '<button id="nav-button">Owned button</button></body>').encode()
                mime = 'text/html; charset=utf-8'
            else:
                return self.send_error(404)
            self.send_response(200)
            self.send_header('Content-Type', mime)
            self.send_header('Content-Length', str(len(payload)))
            self.send_header('Cache-Control', 'no-store')
            self.end_headers()
            self.wfile.write(payload)
        except (BrokenPipeError, ConnectionResetError):
            # A bounded test intentionally aborts an owned delayed response.
            pass
