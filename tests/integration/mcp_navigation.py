#!/usr/bin/env python3
"""Navigation completion and parent workflow deadlines over native MCP stdio."""
import json
import os
from pathlib import Path
import subprocess
import sys

binary = os.environ['PAGEPILOT_BINARY']
site = os.environ['PAGEPILOT_FIXTURE_URL']
evidence = Path(os.environ['PAGEPILOT_CHILD_EVIDENCE'])
evidence.mkdir(parents=True, exist_ok=True)
checks = 0


def require(condition, label):
    global checks
    if not condition:
        raise AssertionError(label)
    checks += 1


def action(identity, name, arguments=None):
    return {'jsonrpc': '2.0', 'id': identity, 'method': 'tools/call',
            'params': {'name': name, 'arguments': arguments or {}}}


messages = [
    {'jsonrpc': '2.0', 'id': 'initialize', 'method': 'initialize', 'params': {
        'protocolVersion': '2025-11-25', 'capabilities': {},
        'clientInfo': {'name': 'owned-navigation-client', 'version': '1'}}},
    {'jsonrpc': '2.0', 'method': 'notifications/initialized'},
    action('tab', 'tab_create'),
    action('slow', 'page_navigate', {'url': site + '/nav-slow.html?delay=150&asset=200&label=MCPNavigation'}),
    action('before', 'page_evaluate', {'script': '({sequence:navSequence,loaded:navLoaded})'}),
    action('reload', 'page_reload'),
    action('after', 'page_evaluate', {'script': '({sequence:navSequence,loaded:navLoaded})'}),
    action('redirect', 'page_navigate', {'url': site + '/nav-redirect?delay=100&to=%2Fnav-slow.html%3Fasset%3D100%26label%3DMCPRedirect'}),
    action('back', 'page_back'),
    action('back-state', 'page_evaluate', {'script': '({label:navLabel,loaded:navLoaded})'}),
    action('forward', 'page_forward'),
    action('forward-state', 'page_evaluate', {'script': '({label:navLabel,loaded:navLoaded})'}),
    action('deadline', 'workflow_steps', {'timeout': 100, 'max_step_retries': 0, 'steps': [
        {'tool': 'page_navigate', 'args': {'url': site + '/nav-slow.html?delay=700&label=LateMCP'}, 'optional': True},
        {'tool': 'page_evaluate', 'args': {'script': "window.mustNotRun='wrong'"}}]}),
    action('stop', 'page_stop'),
    action('recover', 'page_navigate', {'url': site + '/nav-slow.html?label=MCPRecovered'}),
    action('recovered-state', 'page_evaluate', {'script': '({label:navLabel,loaded:navLoaded,untouched:typeof mustNotRun===\'undefined\'})'}),
    action('close', 'tab_close'),
]
completed = subprocess.run([binary, '--port', sys.argv[1]],
                           input=('\n'.join(json.dumps(row) for row in messages) + '\n').encode(),
                           capture_output=True, timeout=30)
(evidence / 'navigation.stdout.jsonl').write_bytes(completed.stdout)
(evidence / 'navigation.stderr.jsonl').write_bytes(completed.stderr)
require(completed.returncode == 0, 'native MCP process exit')
require(not completed.stderr, 'protocol output remains isolated')
responses = [json.loads(line) for line in completed.stdout.splitlines()]
require(len(responses) == len(messages) - 1, 'one response per request without notification response')
rows = {row['id']: row for row in responses}
require(all(not row['result'].get('isError', False) for row in responses), 'native navigation calls return complete tool results')


def value(identity):
    return rows[identity]['result']['structuredContent']


require(value('slow')['title'] == 'MCPNavigation' and value('before')['result']['loaded'], 'initial slow navigation waits for page resources')
require(value('reload')['reloaded'] and value('after')['result']['loaded'], 'MCP reload completes a loaded page')
require(value('before')['result']['sequence'] != value('after')['result']['sequence'], 'reload does not return the old server document')
require(value('redirect')['redirected'] and value('redirect')['title'] == 'MCPRedirect', 'redirect result describes final committed page')
require(value('back')['moved'] and value('back-state')['result'] == {'label': 'MCPNavigation', 'loaded': True}, 'back history result proves actual destination')
require(value('forward')['moved'] and value('forward-state')['result'] == {'label': 'MCPRedirect', 'loaded': True}, 'forward history result proves actual destination')
result = value('deadline')
require(result['timed_out'] and result['executed'] == 1 and result['failed'] == 1, 'workflow deadline prevents subsequent children after slow navigation')
require(result['steps'][0]['attempts'] == 1, 'parent deadline does not cause implicit navigation retry')
require(value('recover')['title'] == 'MCPRecovered' and value('recovered-state')['result'] == {'label': 'MCPRecovered', 'loaded': True, 'untouched': True}, 'following MCP request recovers after explicit stop and re-navigation')
require(value('close')['closed'], 'owned test tab closed')
print(json.dumps({'checks': checks, 'passed': True, 'transport': 'actual native MCP process'}))
