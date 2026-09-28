#!/usr/bin/env python3
"""Check an installed executable without a browser or source-tree imports."""
import argparse
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', required=True)
    parser.add_argument('--evidence', required=True)
    args = parser.parse_args()
    binary = str(Path(args.binary).resolve())
    records = []

    def run(arguments, data='', overrides=None):
        environment = {**os.environ, 'CDP_PORT': '9222', 'PAGEPILOT_PORT': '9222',
                       **(overrides or {})}
        environment = {key: value for key, value in environment.items() if value is not None}
        result = subprocess.run([binary, *arguments], input=data, text=True,
                                capture_output=True, timeout=10, cwd='/', env=environment)
        records.append({'arguments': arguments, 'exit_code': result.returncode,
                        'port_environment': {key: environment.get(key) for key in
                                             ['CDP_PORT', 'PAGEPILOT_PORT']},
                        'stdout': result.stdout, 'stderr': result.stderr})
        return result

    checks = 0

    def check(condition):
        nonlocal checks
        if not condition:
            raise RuntimeError('CLI verification failed: ' + json.dumps(records[-1]))
        checks += 1

    result = run(['--version'])
    check(result.returncode == 0 and result.stdout == 'PagePilot 1.0.0\n' and not result.stderr)
    result = run(['--help'])
    check(result.returncode == 0 and '--allow-root' in result.stdout and not result.stderr)
    for option, count in [('--catalog', 54), ('--compat-catalog', 75)]:
        result = run([option])
        names = [row['name'] for row in json.loads(result.stdout)]
        check(result.returncode == 0 and len(names) == count and len(set(names)) == count and not result.stderr)
    for options in [['--port'], ['--port', '0'], ['--port', '65536'],
                    ['--port', '-1'], ['--port', '1x'], ['--allow-root'], ['--unknown']]:
        result = run(options)
        check(result.returncode == 2 and not result.stdout and result.stderr.startswith('page-pilot:'))
    result = run([], overrides={'PAGEPILOT_PORT': 'invalid'})
    check(result.returncode == 2 and not result.stdout and 'invalid DevTools port' in result.stderr)
    result = run([])
    check(result.returncode == 0 and not result.stdout and not result.stderr)
    for version in ['2024-11-05', '2025-11-25']:
        messages = [{'jsonrpc': '2.0', 'id': 1, 'method': 'initialize', 'params': {'protocolVersion': version, 'capabilities': {}, 'clientInfo': {'name': 'installed-cli-check', 'version': '1.0'}}},
                    {'jsonrpc': '2.0', 'method': 'notifications/initialized'},
                    {'jsonrpc': '2.0', 'id': 2, 'method': 'tools/list'}]
        result = run([], ''.join(json.dumps(message) + '\n' for message in messages))
        replies = [json.loads(line) for line in result.stdout.splitlines()]
        check(result.returncode == 0 and not result.stderr and len(replies) == 2)
        check(replies[0]['result']['protocolVersion'] == version and
              replies[0]['result']['serverInfo']['version'] == '1.0.0' and
              len(replies[1]['result']['tools']) == 54)
    messages = [
        {'jsonrpc': '2.0', 'id': 1, 'method': 'initialize', 'params': {
            'protocolVersion': '2025-11-25', 'capabilities': {},
            'clientInfo': {'name': 'port-precedence-check', 'version': '1.0'}}},
        {'jsonrpc': '2.0', 'method': 'notifications/initialized'},
        {'jsonrpc': '2.0', 'id': 2, 'method': 'tools/call',
         'params': {'name': 'browser_settings'}}]
    for arguments, overrides, expected in [
        ([], {'CDP_PORT': 'invalid', 'PAGEPILOT_PORT': '9231'}, 9231),
        (['--port', '9232'], {'CDP_PORT': 'invalid', 'PAGEPILOT_PORT': 'invalid'}, 9232),
        (['--port', '9233'], {'CDP_PORT': '9224', 'PAGEPILOT_PORT': ''}, 9233),
        ([], {'CDP_PORT': '9234', 'PAGEPILOT_PORT': None}, 9234),
        ([], {'CDP_PORT': None, 'PAGEPILOT_PORT': None}, 9222),
    ]:
        result = run(arguments, ''.join(json.dumps(message) + '\n' for message in messages), overrides)
        replies = [json.loads(line) for line in result.stdout.splitlines()]
        check(result.returncode == 0 and not result.stderr and len(replies) == 2 and
              replies[-1]['result']['structuredContent']['cdpPort'] == expected)
    for overrides in [{'CDP_PORT': 'invalid', 'PAGEPILOT_PORT': None},
                      {'CDP_PORT': '9222', 'PAGEPILOT_PORT': ''}]:
        result = run([], overrides=overrides)
        check(result.returncode == 2 and not result.stdout and 'invalid DevTools port' in result.stderr)
    for option in ['--help', '--version', '--catalog', '--compat-catalog']:
        result = run([option], overrides={'CDP_PORT': 'invalid', 'PAGEPILOT_PORT': 'invalid'})
        check(result.returncode == 0 and result.stdout and not result.stderr)
    evidence = Path(args.evidence)
    evidence.parent.mkdir(parents=True, exist_ok=True)
    evidence.write_text(json.dumps({'passed': True, 'checks': checks, 'binary': binary,
                                    'records': records}, indent=2) + '\n')
    print(json.dumps({'passed': True, 'checks': checks}))


if __name__ == '__main__':
    main()
