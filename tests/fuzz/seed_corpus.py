#!/usr/bin/env python3
"""Generate reproducible seeds; fuzz mutations belong outside the source tree."""
import hashlib
import json
from pathlib import Path
import sys

root = Path(sys.argv[1]); root.mkdir(parents=True, exist_ok=True)
seeds = []
for message in [
    {'id': 1, 'result': {}}, {'id': 1.5, 'result': {}}, {'id': True, 'result': {}},
    {'id': 9007199254740991, 'result': {'text': '中文😀'}}, {'id': 9007199254740992, 'result': {}},
    {'id': 1, 'error': {'code': -32601, 'message': 'owned'}},
    {'id': 1, 'result': {}, 'sessionId': 'owned'},
    {'method': 'Owned.event', 'params': {'array': [None, True, 98]}},
    {'id': 1, 'result': {}, 'error': {}}, {'method': None}, [], None,
]: seeds.append((0, json.dumps(message, ensure_ascii=False).encode()))
for name, args in [
    ('element_fill', {'selector': '#owned', 'text': '中文😀'}),
    ('page_wait', {'timeout': ' 10 ', 'ms': 0}),
    ('form_upload', {'selector': '#files', 'files': ['owned.txt']}),
    ('workflow_steps', {'steps': [{'tool': 'page_evaluate', 'args': {'script': '42'}}]}),
    ('browser_debug', {'enabled': 'false'}), ('tab_activate', {'index': 1.5}),
    ('form_fill', {'fields': {'__proto__': 'literal', 'nested': {'a': [1]}}}),
]: seeds.append((1, json.dumps({'name': name, 'args': args}, ensure_ascii=False).encode()))
for body in [b'{}\n{}\r\n', b'\r\n \t\n{}', b'x' * 4096 + b'\n', b'x' * 4097,
             '中文😀\r\nnext'.encode(), b'\x00\n', b'\r', b'']:
    seeds.append((6, body))
initialize = {'jsonrpc': '2.0', 'id': 'init', 'method': 'initialize', 'params': {
    'protocolVersion': '2024-11-05', 'capabilities': {}, 'clientInfo': {}}}
for message in [initialize, [initialize, {'jsonrpc': '2.0', 'method': 'notifications/initialized'},
                             {'jsonrpc': '2.0', 'id': 7, 'method': 'tools/list'}],
                {'jsonrpc': '2.0', 'id': None, 'method': 'ping'},
                {'jsonrpc': '2.0', 'id': 'call', 'method': 'tools/call', 'params': {
                    'name': 'get_text', 'arguments': {'selector': 'h1'}}},
                {'jsonrpc': '2.0', 'id': 18446744073709551615, 'method': 'ping'},
                {'jsonrpc': '2.0', 'method': 'notifications/cancelled', 'params': {'requestId': 7}},
                {'jsonrpc': '2.0', 'id': 7, 'method': 'tools/list', 'params': {'cursor': 'bad'}},
                {'jsonrpc': '2.0', 'id': 'call', 'method': 'tools/call', 'params': {
                    'name': 'workflow_batch', 'arguments': {'actions': [{'tool': 'get_text'}]}}}]:
    for mode in [3, 15]: seeds.append((mode, json.dumps(message, ensure_ascii=False).encode()))
for body in [b'{"id":1,"id":2}', b'[' * 65 + b'0' + b']' * 65, b'"\xff"', b'{']:
    seeds.append((0, body))
for mode, body in seeds:
    data = bytes([mode]) + body
    (root / hashlib.sha256(data).hexdigest()).write_bytes(data)
print(json.dumps({'seed_inputs': len(seeds), 'output': str(root.resolve())}))
