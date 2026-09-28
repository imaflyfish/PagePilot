#!/usr/bin/env python3
"""Inject keyboard reply loss/errors into an owned CDP peer; never controls Chrome."""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import socketserver
import subprocess
import threading
from wire_fixture import exact, frame, packet

MODES = ['timeout', 'cancel', 'keydown-error', 'keyup-error', 'session-gone', 'cleanup-timeout']


class Peer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = False

    def __init__(self, mode):
        self.mode, self.records, self.keys, self.errors = mode, [], [], []
        self.attachments = 0
        self.triggered = False
        self.waiting = threading.Event()
        super().__init__(('127.0.0.1', 0), Handler)


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        stream, server = self.request, self.server
        stream.settimeout(6)
        try:
            header = bytearray()
            while not header.endswith(b'\r\n\r\n'):
                header += exact(stream, 1)
                if len(header) > 16384: raise AssertionError('unexpected HTTP header size')
            lines = bytes(header).decode().split('\r\n')
            if lines[0].split(' ')[1] == '/json/version':
                body = json.dumps({'webSocketDebuggerUrl': f'ws://127.0.0.1:{server.server_address[1]}/owned'}).encode()
                stream.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: ' + str(len(body)).encode() + b'\r\nConnection: close\r\n\r\n' + body)
                return
            headers = dict(line.split(': ', 1) for line in lines[1:] if ': ' in line)
            key = next(v for k, v in headers.items() if k.lower() == 'sec-websocket-key')
            accept = base64.b64encode(hashlib.sha1((key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest())
            stream.sendall(b'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ' + accept + b'\r\n\r\n')
            delayed = None
            while True:
                opcode, payload = frame(stream)
                if opcode == 8: return
                if opcode != 1: raise AssertionError('expected a text request')
                request = json.loads(payload)
                server.records.append(request)
                method, params = request['method'], request.get('params', {})
                reply = {'id': request['id'], 'result': {}}
                if 'sessionId' in request: reply['sessionId'] = request['sessionId']
                if method == 'Target.getTargets':
                    reply['result'] = {'targetInfos': [{'targetId': 'owned-page', 'type': 'page', 'url': 'about:blank', 'title': 'Owned'}]}
                elif method == 'Target.attachToTarget':
                    server.attachments += 1
                    reply['result'] = {'sessionId': f'owned-session-{server.attachments}'}
                elif method == 'Page.getFrameTree':
                    reply['result'] = {'frameTree': {'frame': {'id': 'owned-frame', 'loaderId': 'owned-loader'}}}
                elif method == 'Owned.waiting':
                    reply['result'] = {'waiting': server.waiting.wait(3)}
                elif method == 'Owned.records':
                    reply['result'] = {'keys': server.keys, 'attachments': server.attachments}
                elif method == 'Input.dispatchKeyEvent':
                    server.keys.append(request)
                    if delayed:
                        stream.sendall(packet(delayed)); delayed = None
                    trigger = params['type'] == 'keyUp' and params['code'] == 'KeyA' if server.mode == 'keyup-error' else params['type'] == 'rawKeyDown' and params['code'] == 'ShiftLeft'
                    if trigger and not server.triggered:
                        server.triggered = True; server.waiting.set()
                        if server.mode in ['timeout', 'cancel', 'cleanup-timeout']:
                            delayed = reply; continue
                        reply.pop('result')
                        reply['error'] = {'code': -32001 if server.mode == 'session-gone' else -32000, 'message': 'Owned key reply failure'}
                    elif server.mode == 'cleanup-timeout' and params['type'] == 'keyUp' and params['code'] == 'ShiftLeft':
                        delayed = reply; continue
                    elif server.mode == 'session-gone' and server.triggered and request.get('sessionId') == 'owned-session-1':
                        reply.pop('result')
                        reply['error'] = {'code': -32001, 'message': 'Owned session no longer exists'}
                stream.sendall(packet(reply))
        except (EOFError, BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass
        except Exception as error:
            server.errors.append(repr(error))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', required=True)
    parser.add_argument('--evidence', required=True)
    args = parser.parse_args()
    destination = Path(args.evidence).resolve(); destination.mkdir(parents=True, exist_ok=True)
    results, checks = [], 0
    for mode in MODES:
        peer = Peer(mode)
        running = threading.Thread(target=peer.serve_forever, kwargs={'poll_interval': 0.01})
        running.start()
        try:
            completed = subprocess.run([str(Path(args.binary).resolve()), str(peer.server_address[1]), mode], capture_output=True, timeout=15)
            (destination / (mode + '.stdout')).write_bytes(completed.stdout)
            (destination / (mode + '.stderr')).write_bytes(completed.stderr)
        finally:
            peer.shutdown(); peer.server_close(); running.join(timeout=2)
        row = {'mode': mode, 'exit_code': completed.returncode, 'server_thread_stopped': not running.is_alive(),
               'peer_errors': peer.errors, 'records': peer.records}
        (destination / (mode + '.peer.json')).write_text(json.dumps(row, indent=2) + '\n')
        results.append(row)
        (destination / 'receipt.json').write_text(json.dumps({'modes': results, 'passed': False, 'checks': checks}, indent=2) + '\n')
        if completed.returncode or completed.stderr or peer.errors or running.is_alive():
            raise RuntimeError(mode + ': ' + completed.stderr.decode() + repr(peer.errors))
        checks += json.loads(completed.stdout)['checks']
    (destination / 'receipt.json').write_text(json.dumps({'modes': results, 'passed': True, 'checks': checks}, indent=2) + '\n')
    print(json.dumps({'passed': True, 'modes': len(results), 'native_checks': checks}))


if __name__ == '__main__':
    main()
