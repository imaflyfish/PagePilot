#!/usr/bin/env python3
"""Run the bounded local integration matrix; builds are an explicit prior step."""
import argparse
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', required=True)
    parser.add_argument('--binary')
    parser.add_argument('--chrome', help='Chrome executable for the owned browser fixture')
    parser.add_argument('--evidence', required=True)
    parser.add_argument('--thread-subset', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    build = Path(args.build).resolve()
    binary = str(Path(args.binary).resolve()) if args.binary else str(build / 'page-pilot')
    prefix = Path(args.evidence).resolve()
    prefix.parent.mkdir(parents=True, exist_ok=True)
    native = ['wire', 'browser', 'dom', 'keyboard', 'input-target', 'condition',
              'click', 'frame', 'services', 'cookie-context', 'pointer', 'files', 'workflow',
              'navigation', 'recovery', 'target-affinity']
    scripts = ['verify_pngs.py', 'mcp_live.py', 'mcp_files.py', 'mcp_workflows.py',
               'mcp_navigation.py', 'mcp_cancellation.py', 'mcp_compatibility.py']
    if args.thread_subset:
        native = ['input-target', 'condition', 'click', 'recovery', 'target-affinity']
        scripts = ['mcp_cancellation.py']
    # PNG verification consumes the images emitted by pilot-files-tests.
    programs = [str(build / ('pilot-' + name + '-tests')) for name in native]
    programs += [str(root / 'tests/integration' / name) for name in scripts]
    browser_options = ['--chrome', args.chrome] if args.chrome else []
    wire_fault = str(build / 'pilot-wire-fault-tests')
    keyboard_fault = str(build / 'pilot-keyboard-fault-tests')
    commands = [('browser-final', ['with_chrome.py', *browser_options, '--binary', binary, *programs]),
                ('wire-final', ['wire_fixture.py', '--binary', wire_fault]),
                ('fault-final', ['keyboard_fixture.py', '--binary', keyboard_fault])]
    # Say which file is absent now, rather than starting a browser and reporting
    # one failure per program a minute later.
    missing = [p for p in [binary, *programs, wire_fault, keyboard_fault]
               if not Path(p).exists()]
    if missing:
        raise SystemExit('not built: ' + ', '.join(missing))
    failed = []
    for suffix, command in commands:
        receipt = str(prefix) + '-' + suffix
        argv = [sys.executable, str(root / 'tests/integration' / command[0]),
                '--evidence', receipt, *command[1:]]
        with Path(receipt + '-run.txt').open('w') as log:
            completed = subprocess.run(argv, cwd=root, stdout=log, stderr=subprocess.STDOUT)
        if completed.returncode:
            failed.append(suffix)
            print(suffix + ' failed (exit ' + str(completed.returncode) + ')', flush=True)
            # The group's output went to its receipt. A caller that only reads
            # the console, which is how this runs in CI, would otherwise be told
            # that something failed and nothing about what.
            tail = Path(receipt + '-run.txt').read_text(errors='replace').splitlines()
            for line in tail[-40:]:
                print('  | ' + line, flush=True)
            print('  | full output: ' + receipt + '-run.txt', flush=True)
        else:
            print(suffix + ' passed', flush=True)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
