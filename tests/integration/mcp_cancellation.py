#!/usr/bin/env python3
"""Interactive cancellation of actual native MCP/browser operations."""
import http.server
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import threading
import time

binary = os.environ['PAGEPILOT_BINARY']
site = os.environ['PAGEPILOT_FIXTURE_URL']
evidence = Path(os.environ['PAGEPILOT_CHILD_EVIDENCE'])
evidence.mkdir(parents=True, exist_ok=True)
checks = 0
clients = []


def require(condition, label):
    global checks
    if not condition:
        raise AssertionError(label)
    checks += 1


class Client:
    def __init__(self, name, port=sys.argv[1], legacy=False):
        self.name, self.serial, self.pending, self.rows = name, 0, {}, []
        self.output, self.errors, self.replies = [], [], queue.Queue()
        self.process = subprocess.Popen([binary, '--port', str(port), *(['--compat-tools'] if legacy else [])],
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.diagnostics = threading.Thread(target=lambda: self.errors.append(self.process.stderr.read()), daemon=True)
        self.reader.start(); self.diagnostics.start(); clients.append(self)
        self.send({'jsonrpc': '2.0', 'id': 'initialize', 'method': 'initialize', 'params': {
            'protocolVersion': '2024-11-05' if legacy else '2025-11-25', 'capabilities': {},
            'clientInfo': {'name': 'owned-cancellation-client', 'version': '1'}}})
        self.response('initialize')
        self.send({'jsonrpc': '2.0', 'method': 'notifications/initialized'})

    def read(self):
        for line in iter(self.process.stdout.readline, b''):
            self.output.append(line)
            try:
                row = json.loads(line); self.rows.append(row); self.replies.put(row)
            except Exception as error:
                self.replies.put(error)

    def send(self, message, fragmented=False):
        payload = (json.dumps(message, ensure_ascii=False) + ('\r\n' if fragmented else '\n')).encode()
        if fragmented:
            for start in range(0, len(payload), 7):
                self.process.stdin.write(payload[start:start + 7]); self.process.stdin.flush()
        else:
            self.process.stdin.write(payload); self.process.stdin.flush()

    def start(self, name, args=None, identity=None):
        self.serial += 1
        identity = identity if identity is not None else 'call-' + str(self.serial)
        self.send({'jsonrpc': '2.0', 'id': identity, 'method': 'tools/call',
                   'params': {'name': name, 'arguments': args or {}}})
        return identity

    def response(self, identity, timeout=3):
        deadline = time.monotonic() + timeout
        while identity not in self.pending:
            row = self.replies.get(timeout=max(0.001, deadline - time.monotonic()))
            if isinstance(row, Exception): raise row
            self.pending[row['id']] = row
        return self.pending.pop(identity)

    def call(self, name, args=None, timeout=3):
        row = self.response(self.start(name, args), timeout)
        if 'error' in row or row['result'].get('isError', False): raise AssertionError(row)
        return json.loads(row['result']['content'][0]['text'])

    def evaluate(self, script):
        return self.call('page_evaluate', {'script': script})['result']

    def cancel(self, identity, fragmented=False):
        self.send({'jsonrpc': '2.0', 'method': 'notifications/cancelled',
                   'params': {'requestId': identity, 'reason': 'owned test 中文😀'}}, fragmented)

    def stop(self):
        if self.process.stdin and not self.process.stdin.closed: self.process.stdin.close()
        try: self.process.wait(timeout=5)
        except subprocess.TimeoutExpired: self.process.kill(); self.process.wait(timeout=5)
        self.reader.join(timeout=2); self.diagnostics.join(timeout=2)
        (evidence / (self.name + '.stdout.jsonl')).write_bytes(b''.join(self.output))
        (evidence / (self.name + '.stderr.txt')).write_bytes(b''.join(self.errors))


def observe(client, expression):
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        if client.evaluate(expression): return
        time.sleep(0.01)
    raise AssertionError('Browser did not reach: ' + expression)


def step(tool, args=None, **fields):
    return {'tool': tool, 'args': args or {}, **fields}


cancelled = []
try:
    controller = Client('controller')
    created = controller.call('tab_create', {'url': site + '/page.html'})
    controller.call('page_wait', {'selector': '#person'})
    target = created['target']
    observer = Client('observer')
    tabs = observer.call('tab_list')['tabs']
    observer.call('tab_activate', {'index': next(row['index'] for row in tabs if row['id'] == target)})
    require(observer.evaluate('document.title') == 'PagePilot fixture', 'independent MCP observer uses the same owned target')

    pending = controller.start('page_evaluate', {'script': 'window.promiseStarted=true;new Promise(resolve=>window.releasePromise=resolve)'}, identity=801)
    observe(observer, 'window.promiseStarted===true')
    controller.cancel('801')
    malformed = {'jsonrpc': '2.0', 'method': 'notifications/cancelled', 'params': {'requestId': 801, 'reason': False}}
    controller.send(malformed); controller.cancel('unknown')
    queued = controller.start('element_fill', {'selector': '#person', 'text': 'must never run'}, identity='queued-fill')
    controller.cancel(queued)
    time.sleep(0.06)
    require(not any(row['id'] == 801 for row in controller.rows), 'wrong-type and malformed cancellation leave the pending script active')
    begin = time.monotonic(); controller.cancel(pending, fragmented=True); cancelled.extend([pending, queued])
    require(controller.call('element_fill', {'selector': '#person', 'text': 'After cancelled promise'})['currentValue'] == 'After cancelled promise', 'following native input works before cancelled page promise resolves')
    require(time.monotonic() - begin < 1.5, 'fragmented cancellation interrupts an actual pending CDP response promptly')
    require(observer.evaluate("document.querySelector('#person').value") == 'After cancelled promise', 'queued cancelled mutation did not run')
    observer.evaluate("releasePromise('late reply');true")
    require(controller.evaluate('6*7') == 42, 'late CDP reply cannot corrupt the next result')

    controller.evaluate('window.batchStarted=false;window.stepStarted=false;window.retryAttempts=0;true')
    pending = controller.start('workflow_batch', {'actions': [
        step('page_evaluate', {'script': 'batchStarted=true'}), step('page_wait', {'ms': 30000}),
        step('element_fill', {'selector': '#person', 'text': 'wrong batch tail'})]})
    observe(observer, 'batchStarted'); controller.cancel(pending); cancelled.append(pending)
    require(controller.call('element_read', {'selector': '#person', 'type': 'value'})['value'] == 'After cancelled promise', 'cancelled batch never executes its tail')
    pending = controller.start('workflow_steps', {'max_step_retries': 10, 'steps': [
        step('page_evaluate', {'script': 'stepStarted=true'}), step('page_wait', {'ms': 30000}, optional=True, timeout=60000),
        step('element_fill', {'selector': '#person', 'text': 'wrong optional tail'})]})
    observe(observer, 'stepStarted'); controller.cancel(pending); cancelled.append(pending)
    require(controller.call('element_read', {'selector': '#person', 'type': 'value'})['value'] == 'After cancelled promise', 'optional step cannot swallow cancellation or trigger retry')
    pending = controller.start('workflow_retry', {'tool': 'page_evaluate', 'args': {'script': "++retryAttempts;throw Error('retry fixture')"}, 'max_retries': 10, 'delay_ms': 2000})
    observe(observer, 'retryAttempts===1'); controller.cancel(pending); cancelled.append(pending)
    require(controller.evaluate('retryAttempts') == 1, 'cancellation during retry delay prevents a second script execution')

    point = controller.evaluate("(()=>{window.downSeen=0;window.upSeen=0;document.addEventListener('mousedown',()=>++downSeen);document.addEventListener('mouseup',()=>++upSeen);const r=document.querySelector('#person').getBoundingClientRect();return {x:r.x+r.width/2,y:r.y+r.height/2}})()")
    pending = controller.start('workflow_batch', {'actions': [
        step('pointer_action', {'action': 'move', **point}), step('pointer_action', {'action': 'down'}),
        step('page_wait', {'ms': 30000}), step('element_fill', {'selector': '#person', 'text': 'wrong pointer tail'})]})
    observe(observer, 'downSeen===1'); controller.cancel(pending); cancelled.append(pending)
    require(controller.evaluate('upSeen') == 1, 'cancelled workflow releases its held native mouse button')
    require(controller.call('element_read', {'selector': '#person', 'type': 'value'})['value'] == 'After cancelled promise', 'pointer cleanup does not execute cancelled tail')
    controller.call('element_click', {'selector': '#count-button'})
    require(observer.evaluate("document.querySelector('#clicks').textContent") == '1', 'ordinary native click still works after held-button cleanup')

    controller.call('page_navigate', {'url': site + '/frame-host.html'})
    controller.call('page_wait', {'type': 'function', 'expression': 'readyFrames.length>=6', 'timeout': 5000})
    controller.call('frame_enter', {'selector': '#crossFrame'})
    observer.call('frame_enter', {'selector': '#crossFrame'})
    pending = controller.start('page_evaluate', {'script': 'window.framePending=true;new Promise(resolve=>window.releaseFrame=resolve)'})
    observe(observer, 'window.framePending===true'); controller.cancel(pending); cancelled.append(pending)
    require(controller.evaluate('frameMarker') == 'localhost|0', 'cancellation preserves selected OOP frame scope for the next request')
    observer.evaluate("releaseFrame('late OOP');true")
    controller.call('element_fill', {'selector': '#frame-input', 'text': 'After OOP cancellation'})
    require(observer.evaluate("document.querySelector('#frame-input').value") == 'After OOP cancellation', 'real OOP input still reaches intended child document')
    controller.call('frame_reset'); observer.call('frame_reset')
    controller.call('tab_close')
    for identity in cancelled: controller.cancel(identity)
    controller.call('browser_settings')
    controller.stop(); observer.stop()
    require(all(row['id'] not in cancelled for row in controller.rows), 'no cancelled request produces a tool response, including after late CDP replies')
    require(controller.process.returncode == observer.process.returncode == 0, 'both owned native clients exit normally')
    require(not any(controller.errors) and not any(observer.errors), 'cancellation has no stderr protocol contamination')

    legacy = Client('legacy', legacy=True)
    legacy.call('new_tab', {'url': site + '/page.html'}); legacy.call('wait', {'selector': '#person'})
    pending = legacy.start('wait', {'ms': 30000}, identity='legacy-wait')
    time.sleep(0.05); legacy.cancel(pending)
    require(legacy.call('fill', {'selector': '#person', 'text': 'Legacy cancellation'})['currentValue'] == 'Legacy cancellation', 'legacy tools and MCP 2024 support cancellation and recovery')
    legacy.call('close_tab'); legacy.stop()
    require(all(row['id'] != pending for row in legacy.rows) and legacy.process.returncode == 0, 'legacy cancelled request has no late response')

    for phase in ['discovery', 'handshake']:
        entered, release = threading.Event(), threading.Event()
        class DelayedEndpoint(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args): pass
            def do_GET(self):
                if self.path == '/json/version' and phase == 'handshake':
                    body = json.dumps({'webSocketDebuggerUrl': f'ws://127.0.0.1:{self.server.server_port}/owned'}).encode()
                    self.send_response(200); self.send_header('Content-Length', str(len(body))); self.end_headers(); self.wfile.write(body)
                else:
                    entered.set(); release.wait(timeout=5)
                    self.close_connection = True
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), DelayedEndpoint)
        serving = threading.Thread(target=server.serve_forever, daemon=True); serving.start()
        try:
            client = Client('cold-' + phase, port=server.server_port)
            pending = client.start('browser_status')
            require(entered.wait(timeout=3), phase + ': actual native connection reached delayed endpoint')
            begin = time.monotonic(); client.cancel(pending)
            require('timeouts' in client.call('browser_settings') and time.monotonic() - begin < 1.5, phase + ': cancellation frees cold connection wait promptly')
            client.stop()
            require(client.process.returncode == 0 and all(row['id'] != pending for row in client.rows), phase + ': cancelled connection exits without response or worker leak')
        finally:
            release.set(); server.shutdown(); server.server_close(); serving.join(timeout=2)
finally:
    for client in clients: client.stop()

print(json.dumps({'checks': checks, 'passed': True, 'transport': 'interactive native MCP processes with independent browser observer'}))
