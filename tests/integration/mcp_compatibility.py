#!/usr/bin/env python3
"""Execute all 75 legacy names and verify effects using an owned Chrome."""
import base64
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading

site = os.environ['PAGEPILOT_FIXTURE_URL']
evidence = Path(os.environ['PAGEPILOT_CHILD_EVIDENCE'])
evidence.mkdir(parents=True, exist_ok=True)
checks, labels, used, records = 0, [], set(), []
catalog = json.loads((Path(__file__).resolve().parents[2] / 'resources/tool_catalog.json').read_text())
expected = {row['legacy'] for row in catalog}


def require(condition, label):
    global checks
    if not condition:
        raise AssertionError(label)
    checks += 1
    labels.append(label)


class Client:
    def __init__(self, directory):
        self.serial, self.output, self.responses = 0, [], queue.Queue()
        self.diagnostics = (evidence / 'compatibility.stderr.txt').open('wb')
        self.process = subprocess.Popen([os.environ['PAGEPILOT_BINARY'], '--port', sys.argv[1],
                                         '--compat-tools', '--allow-root', str(directory)],
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.diagnostics)
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.reader.start()
        result = self.request('initialize', {'protocolVersion': '2024-11-05', 'capabilities': {},
                                            'clientInfo': {'name': 'owned-compatibility-client', 'version': '1'}})
        require(result['protocolVersion'] == '2024-11-05', 'legacy protocol negotiation')
        self.send({'jsonrpc': '2.0', 'method': 'notifications/initialized'})

    def read(self):
        for line in iter(self.process.stdout.readline, b''):
            self.output.append(line)
            self.responses.put(json.loads(line))

    def send(self, message):
        self.process.stdin.write((json.dumps(message, ensure_ascii=False) + '\n').encode())
        self.process.stdin.flush()

    def request(self, method, params):
        self.serial += 1
        self.send({'jsonrpc': '2.0', 'id': self.serial, 'method': method, 'params': params})
        response = self.responses.get(timeout=12)
        if response.get('id') != self.serial or 'error' in response:
            raise AssertionError(response)
        return response['result']

    def call(self, name, args=None, error=None):
        args = args or {}
        response = self.request('tools/call', {'name': name, 'arguments': args})
        records.append({'name': name, 'arguments': args, 'isError': response.get('isError', False)})
        if name in expected:
            used.add(name)
        if error is not None:
            require(response.get('isError') and error in response['content'][0]['text'],
                    name + ': expected script failure is reported')
            return None
        if response.get('isError') or 'structuredContent' in response:
            raise AssertionError((name, response))
        return json.loads(response['content'][0]['text'])

    def raw(self, expression):
        return self.call('page_evaluate', {'script': expression})['result']

    def close(self):
        self.process.stdin.close()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=5)
        self.reader.join(timeout=2)
        self.diagnostics.close()
        (evidence / 'compatibility.stdout.jsonl').write_bytes(b''.join(self.output))


client = None
try:
    with tempfile.TemporaryDirectory(prefix='pagepilot-compatibility-') as directory:
        root = Path(directory)
        client = Client(root)
        call, raw = client.call, client.raw
        advertised = client.request('tools/list', {})['tools']
        require(len(advertised) == 75 and {row['name'] for row in advertised} == expected,
                'catalog advertises every legacy primary and alias exactly once')
        require(call('status')['connected'], 'status connects to the owned Chrome')
        require(call('health_check')['healthy'], 'health_check confirms page and browser')
        initial = call('list_tabs')['count']
        created = call('new_tab', {'url': site + '/nav-redirect?to=/nav-slow.html%3Fasset%3D180'})
        require(created['index'] == initial and created['url'] == site + '/nav-slow.html?asset=180'
                and raw('navLoaded'), 'new_tab waits for redirected document load and returns its final URL and index')
        owner = created['index']
        extra = call('new_tab', {'url': site + '/page.html'})
        require(extra['index'] == owner + 1, 'new_tab appends one unique tab')
        require(call('switch_tab', {'index': owner})['switched'] == owner,
                'switch_tab selects the requested index')
        listing = call('list_tabs')
        require(sum(row['active'] for row in listing['tabs']) == 1 and listing['tabs'][owner]['active'],
                'list_tabs has one active target without duplicate page entries')
        closed = call('close_tab')
        require(type(closed['closed']) is int and closed['closed'] == extra['index']
                and closed['remaining'] == initial + 1 and raw('navLoaded'),
                'close_tab defaults to the last tab while preserving another active tab')
        config = call('get_config')
        require(config['timeouts'] == {'fast': 3000, 'default': 5000, 'long': 10000}, 'get_config exposes default timeouts')
        require(call('set_config', {'fast_timeout': 4000, 'long_timeout': 12000})['current']['long'] == 12000,
                'set_config updates action budgets')
        require(call('set_debug', {'enabled': False}) == {'debug': False}, 'set_debug accepts a strict boolean')
        nav = call('navigate', {'url': site + '/page.html'})
        require(nav['finalUrl'] == site + '/page.html' and nav['hasDialog'] is False,
                'navigate retains URL, title and visible-dialog result')
        call('navigate', {'url': site + '/page.html#owned-history'})
        require(call('go_back')['url'] == site + '/page.html', 'go_back reaches the previous same-document entry')
        require(call('go_forward')['url'] == site + '/page.html#owned-history', 'go_forward reaches the next same-document entry')
        call('reload')
        require(raw("document.querySelector('#person').value") == 'initial', 'reload replaces document state')
        require(call('get_page', {'type': 'source'})['source'].startswith('<!DOCTYPE html>'), 'get_page source preserves document doctype')
        require(call('stop_loading') == {'stopped': True}, 'stop_loading executes in the selected document')

        vectors = json.loads((Path(__file__).resolve().parents[1] / 'fixtures/legacy-values.json').read_text())['vectors']
        for index, vector in enumerate(vectors):
            if 'error_contains' in vector:
                call('eval', {'script': vector['script']}, error=vector['error_contains'])
            else:
                result = call('eval', {'script': vector['script']})['result']
                require(result == vector['serialized'], 'eval baseline value ' + str(index) + ': ' + vector['script'])
        require(raw('once') == 1, 'failed legacy script executes exactly once')
        require(raw('6*7') == 42, 'canonical evaluation retains raw JSON inside a compatibility process')
        require(call('type', {'selector': '#person', 'text': 'Typed 中文😀'})['currentValue'] == 'Typed 中文😀',
                'type inputs complete Unicode text')
        require(call('fill', {'selector': '#person', 'text': 'Filled 中文'})['currentValue'] == 'Filled 中文',
                'fill replaces previous text')
        call('fill_form', {'fields': {'#person': 'Form field', '#notes': 'Form notes'}})
        require(raw("[document.querySelector('#person').value,document.querySelector('#notes').value]")
                == ['Form field', 'Form notes'], 'fill_form updates both independently observed fields')
        call('click', {'selector': '#count-button'})
        require(call('get', {'selector': '#clicks', 'type': 'text'})['text'] == '1', 'click and get observe a native click effect')
        require(call('check', {'selector': '#person', 'state': 'editable'})['editable'], 'check reports editable state')
        require(call('assert', {'type': 'count', 'selector': '#person', 'expected': 1})['passed'], 'assert validates actual element count')
        require(call('wait', {'ms': 1})['waited'] == 1, 'wait completes a bounded delay')
        call('scroll', {'to': 'bottom'})
        require(raw('scrollY>1000'), 'scroll changes the document position')
        call('scroll', {'to': 'top'})
        require(call('count', {'selector': 'select option'})['count'] == 3, 'count sees all options')
        require(call('find', {'selector': 'select option', 'limit': 2})['found'] == 2, 'find obeys its result limit')
        require(call('get_text', {'selector': '#count-button'})['text'] == 'Count clicks', 'get_text alias selects text output')
        require(call('get_html', {'selector': '#count-button', 'outer': True})['html'].startswith('<button'), 'get_html alias preserves outer selection')
        require(call('get_attribute', {'selector': '#person', 'attribute': 'placeholder'})['placeholder'] == 'Your name',
                'get_attribute alias returns the named attribute')
        require(call('exists', {'selector': '#person'})['exists'], 'exists alias reports present element')
        require(call('check', {'text': 'setTimeout('})['exists'] is False,
                'text lookup excludes script source in ancestor aggregate text')
        require(call('is_visible', {'selector': '#hidden'})['visible'] is False, 'is_visible alias distinguishes hidden element')
        call('wait_for', {'selector': '#person', 'state': 'visible'})
        require(raw("document.querySelector('#person').getBoundingClientRect().width>0"), 'wait_for alias reaches a visible element')
        call('wait_for_text', {'text': 'Loaded later'})
        require(raw("document.querySelector('#delayed').textContent") == 'Loaded later', 'wait_for_text observes delayed content')

        call('focus', {'selector': '#person'})
        require(raw("document.activeElement.id") == 'person', 'focus targets the requested control')
        call('hotkey', {'keys': 'ControlOrMeta+A'})
        call('press_key', {'key': 'Backspace'})
        require(raw("document.querySelector('#person').value") == '', 'hotkey and press_key produce native selection and deletion')
        call('blur', {'selector': '#person'})
        require(raw("document.activeElement.id") != 'person', 'blur removes focus from the requested control')
        call('mouse', {'action': 'move', 'x': 0, 'y': 0})
        raw("window.hoverSeen=0;document.querySelector('#count-button').addEventListener('mouseover',()=>++hoverSeen);true")
        call('hover', {'selector': '#count-button'})
        require(raw('hoverSeen>0'), 'hover delivers a native mouse event')
        point = raw("(()=>{const r=document.querySelector('#count-button').getBoundingClientRect();return {x:r.x+r.width/2,y:r.y+r.height/2}})()")
        call('mouse', {'action': 'click', **point})
        require(raw("document.querySelector('#clicks').textContent") == '2', 'mouse performs coordinate click')
        call('move_mouse', point); call('mouse_down'); call('mouse_up')
        require(raw("document.querySelector('#clicks').textContent") == '3', 'move_mouse, mouse_down and mouse_up aliases compose one click')
        call('drag', {'from_selector': '#drag', 'to_selector': '#drop'})
        require(raw("document.querySelector('#drop').textContent") == 'owned-item', 'drag transfers the actual HTML drag payload')
        call('select', {'selector': '#color', 'text': 'Green'})
        require(raw("document.querySelector('#color').value") == 'g', 'select chooses by label')
        call('select_option', {'selector': '#color', 'value': 'b'})
        require(raw("document.querySelector('#color').value") == 'b', 'select_option alias chooses by value')
        call('checkbox', {'selector': '#check', 'checked': True})
        require(raw("document.querySelector('#check').checked"), 'checkbox reaches checked state')
        call('set_checked', {'selector': '#check', 'checked': False})
        require(raw("document.querySelector('#check').checked") is False, 'set_checked alias reaches unchecked state')
        call('toggle_checkbox', {'selector': '#check'})
        require(raw("document.querySelector('#check').checked"), 'toggle_checkbox alias changes state')
        source = root / 'owned-中文.txt'; source.write_text('Upload 内容😀')
        call('upload_file', {'selector': '#files', 'files': str(source)})
        require(raw("document.querySelector('#files').files[0].text()") == 'Upload 内容😀', 'upload_file exposes exact file bytes to the page')
        image = call('screenshot', {'selector': '#count-button'})
        pixels = base64.b64decode(image['screenshot'], validate=True)
        require(pixels.startswith(b'\x89PNG\r\n\x1a\n') and len(pixels) == image['size'], 'screenshot returns complete PNG bytes and size')
        call('dialog', {'action': 'accept', 'text': 'Legacy prompt'})
        call('click', {'selector': '#dialog-button'})
        require(raw("document.querySelector('#dialog-result').textContent") == 'Legacy prompt', 'dialog supplies the armed prompt response')
        call('highlight', {'selector': '#count-button', 'duration': 2000, 'color': 'red'})
        require(raw("getComputedStyle(document.querySelector('#count-button')).outlineColor") == 'rgb(255, 0, 0)',
                'highlight applies the requested visible outline')

        call('cookies', {'action': 'set', 'name': 'primary_owned', 'value': 'a', 'domain': '127.0.0.1', 'path': '/'})
        call('set_cookie', {'name': 'alias_owned', 'value': 'b', 'domain': '127.0.0.1'})
        cookies = {row['name']: row['value'] for row in call('get_cookies')['cookies']}
        require(cookies.get('primary_owned') == 'a' and cookies.get('alias_owned') == 'b', 'cookies, set_cookie and get_cookies share the browser cookie store')
        call('clear_cookies')
        require(call('get_cookies')['cookies'] == [], 'clear_cookies clears the owned browser context')
        # Earlier programs share this owned server origin. Start this case with
        # an explicit empty store instead of assuming a brand-new origin.
        call('clear_storage')
        call('storage', {'action': 'set', 'key': 'primary_owned', 'value': 'a'})
        call('set_storage', {'key': 'alias_owned', 'value': '中文'})
        require(call('get_storage')['storage'] == {'primary_owned': 'a', 'alias_owned': '中文'},
                'storage, set_storage and get_storage operate on actual local storage')
        call('clear_storage')
        require(raw('localStorage.length') == 0, 'clear_storage removes the actual stored values')
        snapshot = call('snapshot')
        require(snapshot['format'] == 'ax-yaml' and 'Count clicks' in snapshot['snapshot'], 'snapshot declares its native accessibility format')
        call('console_logs', {'clear': True})
        raw("console.log('owned',42,{x:1});console.warn('second');true")
        logs = call('console_logs', {'limit': 1, 'clear': True})
        require(logs['total'] == 2 and len(logs['logs']) == 1 and logs['logs'][0]['text'] == 'second',
                'console_logs returns selected rows and total before clear')
        raw("console.log('owned',42,{x:1});true")
        require(call('console_logs')['logs'][0]['text'] == 'owned 42 {x: 1}', 'console_logs preserves legacy object preview text')
        call('console_logs', {'clear': True})
        raw("console.log('',[1,2],Array(3).concat(9));true")
        require(call('console_logs')['logs'][0]['text'] == ' [1, 2] [empty x 3, 9]',
                'console_logs preserves empty arguments and sparse array previews')
        call('console_logs', {'clear': True})
        raw("fetch('/missing-owned-compatibility.txt').then(r=>r.text())")
        require(any(row.get('source') == 'network' and '404' in row['text'] for row in call('console_logs')['logs']),
                'console_logs includes browser resource failures from the Log domain')

        raw("(()=>{const f=document.createElement('iframe');f.id='compat-frame';f.srcdoc='<title>Child title</title><p>Child content</p>';document.body.append(f);return true})()")
        call('wait', {'type': 'function', 'expression': "document.querySelector('#compat-frame').contentDocument?.title==='Child title'"})
        inventory = call('list_frames')
        require(len(inventory['frames']) == 2 and inventory['current_depth'] == 0, 'list_frames includes root and child')
        require(call('enter_frame', {'selector': '#compat-frame'})['depth'] == 1, 'enter_frame selects child scope')
        require(raw('document.title') == 'Child title', 'canonical evaluation observes selected child document')
        require(call('get_url')['url'] == site + '/page.html#owned-history', 'get_url alias retains root-page scope while inside a frame')
        require(call('get_title')['title'] == 'PagePilot fixture', 'get_title alias retains root-page title while inside a frame')
        info = call('get_page')
        require(info['inFrame'] and info['viewport'] is None and info['title'] == 'PagePilot fixture', 'get_page retains legacy root metadata and null CDP viewport')
        require(call('get_page', {'type': 'text'})['text'] == 'Child content', 'get_page text reads the selected frame')
        require(call('get_page', {'type': 'source'})['source'].startswith('<html>'), 'get_page source reads the selected frame')
        require(call('exit_frame')['depth'] == 0 and raw('document.title') == 'PagePilot fixture', 'exit_frame restores parent scope')
        require(call('exit_frame')['reason'] == 'Not in iframe', 'exit_frame reports empty stack without failure')
        call('enter_frame', {'selector': '#compat-frame'})
        require(call('exit_all_frames')['exited'] == 1 and raw('document.title') == 'PagePilot fixture', 'exit_all_frames reports and clears selected scopes')
        sequence = call('batch', {'actions': [{'tool': 'fill', 'args': {'selector': '#person', 'text': 'Batch'}},
                                              {'tool': 'get', 'args': {'selector': '#person', 'type': 'value'}}]})
        require(sequence['executed'] == 2 and sequence['results'][1]['result']['value'] == 'Batch', 'batch dispatches validated legacy children in order')
        retried = call('retry', {'tool': 'eval', 'args': {'script': '42'}, 'max_retries': 1})
        require(retried['success'] and retried['result']['result'] == '42', 'retry preserves nested legacy evaluation format')
        steps = call('run_steps', {'steps': [{'tool': 'get_text', 'args': {'selector': '#count-button'}}]})
        require(steps['succeeded'] == 1 and steps['last_result']['text'] == 'Count clicks', 'run_steps supports alias children')
        stats = call('request_stats')
        require(stats['total'] > 75 and 'eval' in stats['byTool'], 'request_stats records actual dispatches by operation')
        cleaned = call('cleanup')
        require(cleaned['before']['consoleLogs'] >= 1 and cleaned['before']['requestStats'] > 75
                and cleaned['after'] == {'consoleLogs': 0, 'requestStats': 0}, 'cleanup preserves legacy before/after fields')
        require(call('console_logs')['total'] == 0, 'cleanup actually empties the log buffer')
        require(call('reconnect')['reconnected'], 'reconnect creates a fresh CDP attachment')
        listing = call('list_tabs')['tabs']
        require(len(listing) == initial + 1, 'reconnect preserves existing browser tabs')
        call('switch_tab', {'index': next(row['index'] for row in listing if row['id'] == created['target'])})
        require(raw("document.querySelector('#person').value") == 'Batch', 'reconnect preserves owned page state')
        call('close_tab', {'index': next(row['index'] for row in listing if row['id'] == created['target'])})
        require(call('list_tabs')['count'] == initial, 'test closes only its created tabs')
        require(used == expected, 'all 75 advertised legacy names executed: missing ' + str(sorted(expected - used)))
finally:
    if client is not None:
        client.close()
    (evidence / 'coverage.json').write_text(json.dumps({'executed_names': sorted(used), 'checks': checks,
                                                      'labels': labels, 'calls': records}, ensure_ascii=False, indent=2) + '\n')

require(client.process.returncode == 0, 'native compatibility process exits successfully')
require(not (evidence / 'compatibility.stderr.txt').read_bytes(), 'compatibility process has no diagnostic errors')
(evidence / 'coverage.json').write_text(json.dumps({'executed_names': sorted(used), 'checks': checks,
                                                  'labels': labels, 'calls': records, 'passed': True},
                                                 ensure_ascii=False, indent=2) + '\n')
print(json.dumps({'checks': checks, 'legacy_names': len(used), 'baseline_value_vectors': len(vectors), 'passed': True}))
