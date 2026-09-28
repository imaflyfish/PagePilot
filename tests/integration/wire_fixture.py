#!/usr/bin/env python3
"""Owned stdlib HTTP/WebSocket peer for real native transport fault tests."""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import socket
import socketserver
import struct
import subprocess
import threading
import time


def exact(stream, size):
    chunks = []
    while size:
        chunk = stream.recv(size)
        if not chunk:
            raise EOFError()
        chunks.append(chunk); size -= len(chunk)
    return b''.join(chunks)


def frame(stream):
    first, second = exact(stream, 2)
    size = second & 127
    if size == 126: size = struct.unpack('!H', exact(stream, 2))[0]
    if size == 127: size = struct.unpack('!Q', exact(stream, 8))[0]
    if size > 20 * 1024 * 1024: raise AssertionError('unexpected oversized client frame')
    mask = exact(stream, 4) if second & 128 else None
    payload = exact(stream, size)
    if mask: payload = bytes(value ^ mask[i % 4] for i, value in enumerate(payload))
    if not second & 128: raise AssertionError('native client must mask WebSocket frames')
    return first & 15, payload


def packet(payload, opcode=1, final=True):
    if not isinstance(payload, bytes): payload = json.dumps(payload, ensure_ascii=False).encode()
    length = len(payload)
    header = bytes([(128 if final else 0) | opcode])
    if length < 126: header += bytes([length])
    elif length < 65536: header += b'\x7e' + struct.pack('!H', length)
    else: header += b'\x7f' + struct.pack('!Q', length)
    return header + payload


class Peer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = False

    def __init__(self, mode):
        self.mode, self.records, self.errors = mode, [], []
        self.stopping = threading.Event()
        super().__init__(('127.0.0.1', 0), Handler)


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        stream, server = self.request, self.server
        stream.settimeout(6)
        try:
            header = bytearray()
            while not header.endswith(b'\r\n\r\n'):
                header += exact(stream, 1)
                if len(header) > 16384: raise AssertionError('oversized client HTTP header')
            lines = bytes(header).decode().split('\r\n')
            path = lines[0].split(' ')[1]
            headers = dict(line.split(': ', 1) for line in lines[1:] if ': ' in line)
            mode = server.mode
            server.records.append({'path': path})
            if path == '/json/version':
                if mode in ['discovery-stall', 'shared-deadline']:
                    server.stopping.wait(0.6 if mode == 'discovery-stall' else 0.17)
                port = server.server_address[1]
                descriptor = {'Browser': 'PagePilot-owned-peer', 'webSocketDebuggerUrl': f'ws://127.0.0.1:{port}/owned'}
                overrides = {'discovery-remote-host': 'ws://example.invalid:9222/owned',
                             'discovery-wrong-port': f'ws://127.0.0.1:{port+1}/owned',
                             'discovery-control-path': f'ws://127.0.0.1:{port}/owned\r\nX: bad',
                             'discovery-nul-path': f'ws://127.0.0.1:{port}/owned\x00bad',
                             'discovery-fragment': f'ws://127.0.0.1:{port}/owned#fragment',
                             'discovery-wrong-scheme': f'wss://127.0.0.1:{port}/owned',
                             'discovery-wrong-type': 98}
                if mode in overrides: descriptor['webSocketDebuggerUrl'] = overrides[mode]
                if mode == 'discovery-missing-url': del descriptor['webSocketDebuggerUrl']
                body = json.dumps(descriptor).encode()
                if mode == 'discovery-invalid-json': body = b'{'
                if mode == 'discovery-duplicate': body = b'{"webSocketDebuggerUrl":"a","webSocketDebuggerUrl":"b"}'
                if mode == 'discovery-oversize': body = b'x' * (1024 * 1024 + 1)
                if mode == 'discovery-truncated':
                    stream.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n{}'); return
                status = b'503 Owned failure' if mode == 'discovery-status' else b'200 OK'
                response = b'HTTP/1.1 ' + status + b'\r\nContent-Length: ' + str(len(body)).encode() + b'\r\nConnection: close\r\n\r\n' + body
                if mode == 'fragmented':
                    for at in range(0, len(response), 17): stream.sendall(response[at:at+17])
                    server.records.append({'fragmented_http': True})
                else: stream.sendall(response)
                return
            if mode.startswith('discovery-'):
                raise AssertionError('invalid discovery descriptor reached WebSocket peer')
            if mode in ['handshake-stall', 'shared-deadline']:
                server.stopping.wait(0.6 if mode == 'handshake-stall' else 0.17)
            if mode == 'handshake-status':
                stream.sendall(b'HTTP/1.1 403 Owned refusal\r\nContent-Length: 0\r\n\r\n'); return
            key = next(v for k, v in headers.items() if k.lower() == 'sec-websocket-key')
            accept = base64.b64encode(hashlib.sha1((key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest())
            if mode == 'handshake-bad-accept': accept = b'wrong'
            stream.sendall(b'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ' + accept + b'\r\n\r\n')
            pending, delayed = [], None
            while not server.stopping.is_set():
                opcode, payload = frame(stream)
                if opcode == 10:
                    server.records.append({'pong': payload.decode()}); continue
                if opcode == 8: return
                if opcode != 1: raise AssertionError('expected client text request')
                request = json.loads(payload)
                server.records.append({'request': request})
                if mode == 'request-limit' and request['method'] == 'Owned.tooLarge':
                    raise AssertionError('oversized request reached peer')
                reply = {'id': request['id'], 'result': request.get('params', {})}
                if 'sessionId' in request: reply['sessionId'] = request['sessionId']
                if mode in ['multiplex', 'close-pending']:
                    pending.append(reply)
                    if len(pending) != 8: continue
                    if mode == 'close-pending': return
                    stream.sendall(packet({'id': 99999, 'result': {'late': True}}) + b''.join(packet(row) for row in reversed(pending)))
                    continue
                if mode == 'fragmented' and request['id'] == 1:
                    event = {'method': 'Owned.event', 'params': {'text': '事件😀'}, 'sessionId': 'session-a'}
                    stream.sendall(packet(b'owned-ping', 9))
                    for value in [event, reply]:
                        data = json.dumps(value, ensure_ascii=False).encode()
                        # Deliberately split a UTF-8 scalar across continuation
                        # frames, with a control frame between its first bytes.
                        cut = next(i for i, value in enumerate(data) if value >= 128) + 1
                        stream.sendall(packet(data[:cut], 1, False) + packet(b'owned-mid', 9) +
                                       packet(data[cut:cut+1], 0, False) + packet(data[cut+1:], 0))
                    continue
                if mode == 'protocol-error' and request['method'] == 'Owned.error':
                    reply = {'id': request['id'], 'error': {'code': -32601, 'message': 'Owned unsupported method'}}
                if mode in ['late-reply', 'progress-error', 'cancel-call']:
                    if request['method'] == 'Owned.wait': delayed = reply; continue
                    if delayed is not None:
                        stream.sendall(packet(delayed)); delayed = None
                if mode == 'peer-close':
                    stream.sendall(packet(struct.pack('!H', 1000), 8)); return
                if mode == 'peer-eof': return
                if mode == 'event-count-limit':
                    stream.sendall(b''.join(packet({'method': 'Owned.event', 'params': {'i': i}}) for i in range(10001)))
                    server.stopping.wait(5); return
                if mode == 'event-byte-limit':
                    stream.sendall(packet({'method': 'Owned.event', 'params': {'data': 'x' * (8 * 1024 * 1024)}}))
                    server.stopping.wait(5); return
                if mode == 'websocket-size-limit':
                    stream.sendall(b'\x81\x7f' + struct.pack('!Q', 32 * 1024 * 1024 + 1))
                    server.stopping.wait(5); return
                if mode == 'binary-json': stream.sendall(packet(reply, 2)); continue
                if mode == 'invalid-json': stream.sendall(packet(b'{')); continue
                if mode == 'duplicate-json': stream.sendall(packet(b'{"id":1,"id":1,"result":{}}')); continue
                malformed = {
                    'bad-fractional-id': {'id': request['id'] + 0.5, 'result': {'poison': True}},
                    'bad-string-id': {'id': str(request['id']), 'result': {}},
                    'bad-bool-id': {'id': True, 'result': {}},
                    'bad-negative-id': {'id': -1, 'result': {}},
                    'bad-zero-id': {'id': 0, 'result': {}},
                    'bad-large-id': {'id': 18446744073709551615, 'result': {}},
                    'bad-both-result-error': {'id': request['id'], 'result': {}, 'error': {'code': -1, 'message': 'bad'}},
                    'bad-missing-result': {'id': request['id']},
                    'bad-array-result': {'id': request['id'], 'result': []},
                    'bad-fractional-error': {'id': request['id'], 'error': {'code': -1.5, 'message': 'bad'}},
                    'bad-large-error': {'id': request['id'], 'error': {'code': 9223372036854775807, 'message': 'bad'}},
                    'bad-string-error': {'id': request['id'], 'error': 'bad'},
                    'bad-message-error': {'id': request['id'], 'error': {'code': -1, 'message': 98}},
                    'bad-event-method': {'method': 98},
                    'bad-event-params': {'method': 'Owned.event', 'params': []},
                    'bad-event-result': {'method': 'Owned.event', 'result': {}},
                    'bad-response-method': {'id': request['id'], 'method': 'Owned.event', 'result': {}},
                    'bad-session-type': {'id': request['id'], 'result': {}, 'sessionId': 98},
                    'bad-session-mismatch': {'id': request['id'], 'result': {}, 'sessionId': 'wrong-session'},
                    'bad-empty-envelope': {}, 'bad-array-envelope': [], 'bad-null-envelope': None,
                }
                if mode in malformed: reply = malformed[mode]
                stream.sendall(packet(reply))
        except (EOFError, BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass
        except Exception as error:
            server.errors.append(repr(error))


MODES = ['discovery-status', 'discovery-invalid-json', 'discovery-duplicate', 'discovery-missing-url',
         'discovery-wrong-type', 'discovery-remote-host', 'discovery-wrong-port', 'discovery-control-path',
         'discovery-nul-path', 'discovery-fragment', 'discovery-wrong-scheme', 'discovery-oversize',
         'discovery-truncated', 'discovery-stall', 'handshake-status', 'handshake-bad-accept',
         'handshake-stall', 'shared-deadline', 'fragmented', 'multiplex', 'protocol-error', 'late-reply',
         'progress-error', 'cancel-call', 'request-limit', 'peer-close', 'peer-eof', 'close-pending',
         'event-count-limit', 'event-byte-limit', 'websocket-size-limit', 'binary-json', 'invalid-json',
         'duplicate-json', 'bad-fractional-id', 'bad-string-id', 'bad-bool-id', 'bad-negative-id',
         'bad-zero-id', 'bad-large-id', 'bad-both-result-error', 'bad-missing-result', 'bad-array-result',
         'bad-fractional-error', 'bad-large-error', 'bad-string-error', 'bad-message-error', 'bad-event-method',
         'bad-event-params', 'bad-event-result', 'bad-response-method', 'bad-session-type',
         'bad-session-mismatch', 'bad-empty-envelope', 'bad-array-envelope', 'bad-null-envelope']


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', required=True)
    parser.add_argument('--evidence', required=True)
    parser.add_argument('--mode', choices=MODES, action='append')
    args = parser.parse_args()
    destination = Path(args.evidence).resolve(); destination.mkdir(parents=True, exist_ok=True)
    results, checks = [], 0
    for mode in args.mode or MODES:
        peer = Peer(mode)
        running = threading.Thread(target=peer.serve_forever, kwargs={'poll_interval': 0.01})
        running.start()
        try:
            completed = subprocess.run([str(Path(args.binary).resolve()), str(peer.server_address[1]), mode], capture_output=True, timeout=15)
            (destination / (mode + '.stdout')).write_bytes(completed.stdout)
            (destination / (mode + '.stderr')).write_bytes(completed.stderr)
        finally:
            peer.stopping.set(); peer.shutdown(); peer.server_close(); running.join(timeout=2)
        row = {'mode': mode, 'exit_code': completed.returncode, 'server_thread_stopped': not running.is_alive(),
               'peer_errors': peer.errors, 'records': peer.records}
        (destination / (mode + '.peer.json')).write_text(json.dumps(row, ensure_ascii=False, indent=2) + '\n')
        results.append(row)
        (destination / 'receipt.json').write_text(json.dumps({'modes': results, 'passed': False, 'checks': checks}, indent=2) + '\n')
        if completed.returncode or completed.stderr or peer.errors or running.is_alive():
            raise RuntimeError(mode + ': ' + completed.stderr.decode() + repr(peer.errors))
        result = json.loads(completed.stdout)
        if mode == 'fragmented':
            if not all(any(r.get('pong') == marker for r in peer.records) for marker in ['owned-ping', 'owned-mid']):
                raise AssertionError('peer did not observe both masked pong replies')
        checks += result['checks']
    (destination / 'receipt.json').write_text(json.dumps({'modes': results, 'passed': True, 'checks': checks}, indent=2) + '\n')
    print(json.dumps({'passed': True, 'modes': len(results), 'native_checks': checks}))


if __name__ == '__main__':
    main()
