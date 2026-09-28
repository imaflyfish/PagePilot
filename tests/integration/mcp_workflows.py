#!/usr/bin/env python3
"""Workflow contracts and observable page effects through actual native MCP."""
import json
import os
from pathlib import Path
import subprocess
import sys

binary = os.environ['PAGEPILOT_BINARY']
port = sys.argv[1]
site = os.environ['PAGEPILOT_FIXTURE_URL']
evidence = Path(os.environ['PAGEPILOT_CHILD_EVIDENCE'])
evidence.mkdir(parents=True, exist_ok=True)
checks = 0


def require(condition, label):
    global checks
    if not condition:
        raise AssertionError(label)
    checks += 1


def request(identity, method, arguments=None):
    return {'jsonrpc': '2.0', 'id': identity, 'method': method, 'params': arguments or {}}


def action(identity, name, arguments=None):
    return request(identity, 'tools/call', {'name': name, 'arguments': arguments or {}})


def step(name, arguments=None, **metadata):
    return {'tool': name, 'args': arguments or {}, **metadata}


def execute(name, version, messages, legacy=False):
    stream = [request('initialize', 'initialize', {
        'protocolVersion': version, 'capabilities': {},
        'clientInfo': {'name': 'owned-workflow-client', 'version': '1'}}),
        {'jsonrpc': '2.0', 'method': 'notifications/initialized'}, *messages]
    completed = subprocess.run([binary, '--port', port, *(['--compat-tools'] if legacy else [])],
                               input=('\n'.join(json.dumps(row, ensure_ascii=False) for row in stream) + '\n').encode(),
                               capture_output=True, timeout=30)
    (evidence / (name + '.stdout.jsonl')).write_bytes(completed.stdout)
    (evidence / (name + '.stderr.jsonl')).write_bytes(completed.stderr)
    require(completed.returncode == 0, name + ': native MCP exits successfully')
    require(not completed.stderr, name + ': diagnostic output remains separate')
    responses = [json.loads(line) for line in completed.stdout.splitlines()]
    require(len(responses) == len(messages) + 1, name + ': exactly one response per request')
    rows = {row['id']: row for row in responses}
    require(rows['initialize']['result']['protocolVersion'] == version, name + ': negotiated version')
    for row in responses[1:]:
        result = row['result']
        if not result.get('isError', False):
            text_value = json.loads(result['content'][0]['text'])
            if version == '2025-11-25':
                require(text_value == result['structuredContent'], name + ': complete text and structured result agree')
            else:
                require('structuredContent' not in result, name + ': legacy text-only result')
    return rows


def value(rows, identity):
    return json.loads(rows[identity]['result']['content'][0]['text'])


canonical = [
    action('tab', 'tab_create', {'url': site + '/page.html'}),
    action('ready', 'page_wait', {'selector': '#person'}),
    action('batch', 'workflow_batch', {'actions': [
        step('element_fill', {'selector': '#person', 'text': 'MCP sequence 中文😀'}),
        step('element_click', {'selector': '#count-button'}),
        step('element_read', {'selector': '#person', 'type': 'value'})]}),
    action('effects', 'page_evaluate', {'script': "({value:document.querySelector('#person').value,clicks:document.querySelector('#clicks').textContent})"}),
    action('init', 'page_evaluate', {'script': 'window.retryCount=0;window.zeroCount=0;true'}),
    action('retry', 'workflow_retry', {'tool': 'page_evaluate', 'args': {'script': '++retryCount>=3'},
                                     'max_retries': 5, 'delay_ms': 0, 'success_check': 'result'}),
    action('zero', 'workflow_retry', {'tool': 'page_evaluate', 'args': {'script': '++zeroCount'}, 'max_retries': 0}),
    action('counters', 'page_evaluate', {'script': '({retryCount,zeroCount})'}),
    action('steps', 'workflow_steps', {'max_step_retries': 0, 'return_intermediate': True, 'steps': [
        step('element_click', {'selector': '#absent', 'timeout': 30}, optional=True),
        step('element_fill', {'selector': '#person', 'text': 'After MCP optional failure'}),
        step('element_read', {'selector': '#person', 'type': 'value'})]}),
    action('child-policy', 'workflow_batch', {'allow_legacy': True, 'actions': [
        step('fill', {'selector': '#person', 'text': 'must not run'})]}),
    action('retained', 'element_read', {'selector': '#person', 'type': 'value'}),
    action('invalid-bool', 'workflow_steps', {'auto_wait': 'false', 'steps': [step('browser_config')]}),
    action('empty', 'workflow_steps', {'steps': []}),
    action('nested', 'workflow_batch', {'actions': [step('workflow_retry', {
        'tool': 'page_evaluate', 'args': {'script': "'nested 中文'"}, 'max_retries': 1})]}),
    action('deadline', 'workflow_retry', {'tool': 'page_evaluate', 'args': {'script': 'false'},
                                        'success_check': 'result', 'delay_ms': 200, 'timeout': 80}),
    action('after-deadline', 'element_read', {'selector': '#person', 'type': 'value'}),
    action('full-last', 'workflow_steps', {'steps': [step('page_evaluate', {'script': "'😀'.repeat(200)"})]}),
    action('close', 'tab_close'),
]
rows = execute('canonical-workflows', '2025-11-25', canonical)
require(value(rows, 'batch')['executed'] == 3, 'canonical batch executes every valid row')
require(value(rows, 'effects')['result'] == {'value': 'MCP sequence 中文😀', 'clicks': '1'}, 'native page independently proves ordered batch effects')
require(value(rows, 'retry')['attempts'] == 3 and value(rows, 'retry')['success'], 'predicate stops at actual third attempt')
require(value(rows, 'zero')['attempts'] == 0 and value(rows, 'counters')['result'] == {'retryCount': 3, 'zeroCount': 0}, 'retry count and zero case preserve exact script execution counts')
result = value(rows, 'steps')
require(result['failed'] == 1 and result['succeeded'] == 2 and result['executed'] == 3, 'optional browser failure stays visible and permits following steps')
require(result['last_result']['value'] == result['steps'][2]['result']['value'] == 'After MCP optional failure', 'full intermediate and last result preserve actual page value')
require(result['page'] == {'url': site + '/page.html', 'title': 'PagePilot fixture'}, 'workflow page summary is live root page state')
require(not value(rows, 'child-policy')['results'][0]['success'] and value(rows, 'retained')['value'] == 'After MCP optional failure', 'JSON cannot enable legacy child dispatch in a canonical-only process')
require(rows['invalid-bool']['result']['isError'] and rows['empty']['result']['isError'], 'invalid workflow arguments become MCP tool errors')
require(value(rows, 'nested')['results'][0]['result']['result']['result'] == 'nested 中文', 'validated nested batch/retry works through MCP')
require(value(rows, 'deadline')['timed_out'] and value(rows, 'deadline')['attempts'] == 1, 'parent deadline prevents a delayed additional attempt')
require(value(rows, 'after-deadline')['value'] == 'After MCP optional failure', 'deadline scope does not leak into following MCP request')
result = value(rows, 'full-last')
require(result['last_result']['result'] == '😀' * 200 and result['steps'][0]['brief'] == '😀' * 150, 'complete last result and UTF-8 brief survive transport')
require(value(rows, 'close')['closed'], 'canonical test tab is closed')

legacy = [
    action('tab', 'new_tab', {'url': site + '/page.html'}),
    action('ready', 'wait', {'selector': '#person'}),
    action('batch', 'batch', {'actions': [step('set_debug', {'enabled': 'false'}),
                                        step('fill', {'selector': '#person', 'text': 'legacy MCP 中文'}), None]}),
    action('read', 'get', {'selector': '#person', 'type': 'value'}),
    action('init', 'eval', {'script': 'window.legacyAttempts=0;true'}),
    action('retry', 'retry', {'tool': 'eval', 'args': {'script': "(()=>{if(++legacyAttempts<2)throw Error('retry me');return legacyAttempts})()"},
                            'max_retries': 3, 'delay_ms': 0}),
    action('steps', 'run_steps', {'max_step_retries': 0, 'return_intermediate': True, 'steps': [
        step('toString', optional=True), step('get', {'selector': '#person', 'type': 'value'})]}),
    action('blocked', 'run_steps', {'steps': [step('retry', {'tool': 'eval', 'args': {'script': '++legacyAttempts'}}, optional=True),
                                           step('eval', {'script': 'legacyAttempts'})], 'max_step_retries': 0}),
    action('close', 'close_tab'),
]
rows = execute('legacy-workflows', '2024-11-05', legacy, legacy=True)
result = value(rows, 'batch')
require(result['executed'] == 3 and [row['success'] for row in result['results']] == [False, True, False], 'legacy malformed rows report independent errors')
require(value(rows, 'read')['value'] == 'legacy MCP 中文', 'valid legacy child runs despite adjacent malformed rows')
require(value(rows, 'retry')['attempts'] == 2 and value(rows, 'retry')['result']['result'] == '2', 'legacy retry reaches actual second native execution with serialized result')
result = value(rows, 'steps')
require(result['failed'] == 1 and result['executed'] == 2 and result['last_result']['value'] == 'legacy MCP 中文', 'legacy optional validation failure preserves subsequent alias dispatch')
result = value(rows, 'blocked')
require(result['failed'] == 1 and result['last_result']['result'] == '2', 'legacy nested-workflow ban prevents hidden script execution')
require(type(value(rows, 'close')['closed']) is int, 'legacy close reports closed tab index')
print(json.dumps({'checks': checks, 'passed': True, 'transport': 'actual native MCP processes'}))
