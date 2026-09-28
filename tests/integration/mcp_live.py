#!/usr/bin/env python3
"""Real native MCP child process against the owned Chrome and local fixture."""
import json
import os
from pathlib import Path
import subprocess
import sys

binary = os.environ.get('PAGEPILOT_BINARY', str(Path(__file__).resolve().parents[2] / 'build/debug/page-pilot'))
port = sys.argv[1]
url = os.environ['PAGEPILOT_FIXTURE_URL'] + '/page.html'
checks = 0


def require(value, label):
    global checks
    if not value:
        raise AssertionError(label)
    checks += 1


def request(identity, method, parameters=None):
    return {'jsonrpc': '2.0', 'id': identity, 'method': method, 'params': parameters or {}}


def record_child(name, completed):
    destination = os.environ.get('PAGEPILOT_CHILD_EVIDENCE')
    if destination:
        directory = Path(destination)
        directory.mkdir(parents=True, exist_ok=True)
        (directory / (name + '.stdout.jsonl')).write_bytes(completed.stdout)
        (directory / (name + '.stderr.jsonl')).write_bytes(completed.stderr)


messages = [
    request(1, 'initialize', {'protocolVersion': '2025-11-25', 'capabilities': {}, 'clientInfo': {'name': 'owned-client', 'version': '1'}}),
    {'jsonrpc': '2.0', 'method': 'notifications/initialized'},
    request(2, 'tools/list'),
    request(3, 'tools/call', {'name': 'tab_create', 'arguments': {'url': 'about:blank'}}),
    request(4, 'tools/call', {'name': 'page_navigate', 'arguments': {'url': url}}),
    request('中文-id', 'tools/call', {'name': 'page_evaluate', 'arguments': {'script': "'中文😀'"}}),
    request(6, 'tools/call', {'name': 'browser_debug', 'arguments': {'enabled': 'false'}}),
    request('unknown-tool', 'tools/call', {'name': 'unknown_tool'}),
    request('null-arguments', 'tools/call', {'name': 'browser_settings', 'arguments': None}),
    request('array-arguments', 'tools/call', {'name': 'browser_settings', 'arguments': []}),
    request(10, 'tools/call', {'name': 'browser_debug', 'arguments': {'enabled': True}}),
    request(11, 'tools/call', {'name': 'element_fill', 'arguments': {'selector': '#person', 'text': 'MCP中文😀'}}),
    request(12, 'tools/call', {'name': 'element_read', 'arguments': {'selector': '#person', 'type': 'value'}}),
    request(13, 'tools/call', {'name': 'element_click', 'arguments': {'selector': '#count-button'}}),
    request(14, 'tools/call', {'name': 'element_read', 'arguments': {'selector': '#clicks', 'type': 'text'}}),
    request(15, 'tools/call', {'name': 'page_assert', 'arguments': {'type': 'visible', 'selector': '#person'}}),
    request(16, 'tools/call', {'name': 'browser_debug', 'arguments': {'enabled': False}}),
    request(7, 'tools/call', {'name': 'tab_close'}),
    request(8, 'ping'),
    request(9, 'missing'),
]
encoded = '\n'.join(json.dumps(message, ensure_ascii=False) for message in messages)
# An unterminated final valid message must still be processed on EOF.
completed = subprocess.run([binary, '--port', port], input=encoded.encode(), capture_output=True, timeout=30)
record_child('canonical', completed)
require(completed.returncode == 0, completed.stderr.decode(errors='replace'))
responses = [json.loads(line) for line in completed.stdout.splitlines()]
require(len(responses) == 19, 'notification gets no response and EOF does not drop last request')
by_id = {response['id']: response for response in responses}
require(by_id[1]['result']['protocolVersion'] == '2025-11-25', 'protocol negotiation')
require(len(by_id[2]['result']['tools']) == 54, 'canonical catalog via stdio')
require(by_id[3]['result']['structuredContent']['created'], 'real tab creation via MCP')
require(by_id[4]['result']['structuredContent']['title'] == 'PagePilot fixture', 'real navigation through stdio')
require(by_id['中文-id']['result']['structuredContent']['result'] == '中文😀', 'UTF-8 request IDs and results')
require(by_id[6]['result']['isError'], 'schema error reaches MCP as tool error')
for identity in ['unknown-tool', 'null-arguments', 'array-arguments']:
    require(by_id[identity]['error']['code'] == -32602 and 'result' not in by_id[identity],
            identity + ' receives a protocol error without disrupting later calls')
require(by_id[10]['result']['structuredContent']['debug'], 'diagnostics enabled through MCP')
require(by_id[11]['result']['structuredContent']['currentValue'] == 'MCP中文😀', 'native fill through MCP')
require(by_id[12]['result']['structuredContent']['value'] == 'MCP中文😀', 'independent read verifies MCP fill')
require(by_id[13]['result']['structuredContent']['clicked'], 'native click through MCP')
require(by_id[14]['result']['structuredContent']['text'] == '1', 'page effect verifies MCP click')
require(by_id[15]['result']['structuredContent']['passed'], 'DOM assertion through MCP')
require(not by_id[16]['result']['structuredContent']['debug'], 'diagnostics disabled through MCP')
diagnostics = [json.loads(line) for line in completed.stderr.splitlines()]
require(any(row.get('operation') == 'fill' for row in diagnostics) and b'MCP' not in completed.stderr,
        'diagnostics use stderr and omit page arguments and values')
require(by_id[7]['result']['structuredContent']['closed'], 'created target closed')
require(by_id[8]['result'] == {}, 'ping result')
require(by_id[9]['error']['code'] == -32601, 'unknown method error')
legacy = [request(1, 'initialize', {'protocolVersion': '2024-11-05', 'capabilities': {}, 'clientInfo': {'name': 'legacy', 'version': '1'}}),
          {'jsonrpc': '2.0', 'method': 'notifications/initialized'}, request(2, 'tools/list'),
          request(3, 'tools/call', {'name': 'get_url'})]
stream = '\n'.join(json.dumps(value) for value in legacy).encode() + b'\n{invalid}\n' + json.dumps(request(4, 'ping')).encode()
completed = subprocess.run([binary, '--port', port, '--compat-tools'], input=stream, capture_output=True, timeout=30)
record_child('compatibility', completed)
require(completed.returncode == 0, 'legacy process exit')
responses = [json.loads(line) for line in completed.stdout.splitlines()]
by_id = {response['id']: response for response in responses}
require(len(by_id[2]['result']['tools']) == 75, 'legacy catalog covers all old tools')
require(not by_id[3]['result']['isError'] and 'structuredContent' not in by_id[3]['result'], 'legacy alias response contract')
require(by_id[None]['id'] is None and by_id[None]['error']['code'] == -32700, 'malformed JSON response')
require(by_id[4]['id'] == 4 and by_id[4]['result'] == {}, 'valid frame after parse failure and EOF')
frame_messages = [
    request(1, 'initialize', {'protocolVersion': '2025-11-25', 'capabilities': {}, 'clientInfo': {'name': 'owned-frame-client', 'version': '1'}}),
    {'jsonrpc': '2.0', 'method': 'notifications/initialized'},
]
frame_actions = [
    ('tab_create', {}),
    ('page_navigate', {'url': os.environ['PAGEPILOT_FIXTURE_URL'] + '/frame-host.html'}),
    ('page_wait', {'type': 'function', 'expression': 'readyFrames.length>=6', 'timeout': 5000}),
    ('frame_enter', {'selector': '#crossFrame'}),
    ('element_fill', {'selector': '#frame-input', 'text': 'MCP frame 中文😀'}),
    ('page_evaluate', {'script': 'frameMarker'}),
    ('element_click', {'selector': '#frame-button'}),
    ('element_read', {'selector': '#frame-clicks', 'type': 'text'}),
    ('frame_enter', {'selector': '#child-frame'}),
    ('page_evaluate', {'script': 'frameMarker'}),
    ('frame_leave', {}),
    ('frame_reset', {}),
    ('frame_list', {}),
    ('tab_close', {}),
]
for identity, (name, arguments) in enumerate(frame_actions, 2):
    frame_messages.append(request(identity, 'tools/call', {'name': name, 'arguments': arguments}))
completed = subprocess.run([binary, '--port', port], input=('\n'.join(json.dumps(row, ensure_ascii=False) for row in frame_messages) + '\n').encode(), capture_output=True, timeout=30)
record_child('frames', completed)
require(completed.returncode == 0, 'frame MCP process exit')
require(not completed.stderr, 'ordinary frame MCP process has no diagnostic contamination')
responses = [json.loads(line) for line in completed.stdout.splitlines()]
require(len(responses) == len(frame_actions) + 1, 'all frame requests receive one response')
by_id = {row['id']: row for row in responses}
require(all(not by_id[identity]['result'].get('isError', False) for identity in range(2, 16)), 'all native frame tools complete through MCP')
require(by_id[5]['result']['structuredContent']['separateSession'], 'MCP enters actual OOP frame')
require(by_id[6]['result']['structuredContent']['currentValue'] == 'MCP frame 中文😀', 'MCP fills OOP frame input')
require(by_id[7]['result']['structuredContent']['result'] == 'localhost|0', 'MCP evaluates OOP default world')
require(by_id[9]['result']['structuredContent']['text'] == '1', 'MCP click changes actual child document')
require(by_id[11]['result']['structuredContent']['result'] == '127.0.0.1|1', 'MCP enters nested cross-process frame')
require(by_id[12]['result']['structuredContent']['depth'] == 1, 'MCP leaves one frame')
require(by_id[13]['result']['structuredContent']['exited'] == 1, 'MCP resets remaining frame scope')
require(len(by_id[14]['result']['structuredContent']['frames']) == 7, 'MCP enumerates complete owned frame tree')
require(by_id[15]['result']['structuredContent']['closed'], 'MCP closes owned frame-test tab')
service_actions = [
    ('tab_create', {}),
    ('page_navigate', {'url': url}),
    ('element_fill', {'selector': '#person', 'text': 'MCP chosen', 'field': '#notes'}),
    ('element_read', {'selector': '#person', 'type': 'value'}),
    ('page_dialog', {'text': 'MCP prompt 中文😀'}),
    ('element_click', {'selector': '#dialog-button'}),
    ('element_read', {'selector': '#dialog-result', 'type': 'text'}),
    ('page_evaluate', {'script': "confirm('unarmed')"}),
    ('page_storage', {'action': 'set', 'key': '__proto__', 'value': 'MCP literal'}),
    ('page_storage', {'key': '__proto__'}),
    ('browser_cookies', {'action': 'set', 'name': 'mcp-cookie', 'value': 'owned', 'domain': '127.0.0.1'}),
    ('browser_cookies', {'name': 'mcp-cookie'}),
    ('browser_cookies', {'action': 'delete', 'name': 'mcp-cookie'}),
    ('browser_cookies', {'name': 'mcp-cookie'}),
    ('pointer_action', {'action': 'move', 'x': 10, 'y': 15}),
    ('pointer_drag', {'from_selector': '#drag', 'to_selector': '#drop'}),
    ('element_read', {'selector': '#drop', 'type': 'text'}),
    ('element_highlight', {'selector': '#person', 'color': 'purple', 'duration': 1000}),
    ('element_read', {'selector': '#person', 'type': 'styles', 'properties': ['outline-color']}),
    ('page_snapshot', {}),
    ('tab_close', {}),
]
service_messages = [request(1, 'initialize', {'protocolVersion': '2025-11-25', 'capabilities': {}, 'clientInfo': {'name': 'owned-services-client', 'version': '1'}}),
                    {'jsonrpc': '2.0', 'method': 'notifications/initialized'}]
for identity, (name, arguments) in enumerate(service_actions, 2):
    service_messages.append(request(identity, 'tools/call', {'name': name, 'arguments': arguments}))
completed = subprocess.run([binary, '--port', port], input=('\n'.join(json.dumps(row, ensure_ascii=False) for row in service_messages) + '\n').encode(), capture_output=True, timeout=30)
record_child('services', completed)
require(completed.returncode == 0, 'services MCP process exit')
require(not completed.stderr, 'services MCP stdout/stderr separation')
responses = [json.loads(line) for line in completed.stdout.splitlines()]
require(len(responses) == len(service_actions) + 1, 'one response per services request')
by_id = {row['id']: row for row in responses}
require(all(not by_id[identity]['result'].get('isError', False) for identity in range(2, 23)), 'all services requests complete through actual MCP child')
def service_result(identity):
    return by_id[identity]['result']['structuredContent']
require(service_result(5)['value'] == 'MCP chosen', 'MCP unknown adapter field cannot redirect fill')
require(service_result(8)['text'] == 'MCP prompt 中文😀', 'MCP modal handling produces page-visible prompt result')
require(service_result(9)['result'] is False, 'MCP one-shot dialog rule consumed')
require(service_result(11)['__proto__'] == 'MCP literal', 'MCP storage keeps prototype-like literal key')
require(service_result(13)['cookies'][0]['value'] == 'owned', 'MCP cookie is read back from browser')
require(service_result(15)['cookies'] == [], 'MCP cookie delete effect')
require(service_result(16)['moved'] == {'x': 10, 'y': 15}, 'MCP pointer move coordinates')
require(service_result(18)['text'] == 'owned-item', 'MCP native drag produces real drop effect')
require(service_result(20)['styles']['outline-color'] == 'rgb(128, 0, 128)', 'MCP highlight produces computed style effect')
require(service_result(21)['format'] == 'ax-yaml' and 'Count clicks' in service_result(21)['snapshot'], 'MCP snapshot contains actual accessible content')
require(service_result(22)['closed'], 'MCP services tab cleanup')
print(json.dumps({'checks': checks, 'passed': True, 'transport': 'actual native stdio child'}))
