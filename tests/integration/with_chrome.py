#!/usr/bin/env python3
"""Run native integration programs against an owned, temporary Chrome profile."""
import argparse
import json
import functools
import http.server
import threading
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
from fixture_server import FixtureHandler


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--chrome', default='/Applications/Google Chrome.app/Contents/MacOS/Google Chrome')
    parser.add_argument('--evidence', required=True)
    parser.add_argument('--binary')
    parser.add_argument('programs', nargs='+')
    args = parser.parse_args()
    evidence = Path(args.evidence).resolve()
    evidence.mkdir(parents=True, exist_ok=True)
    results = []
    handler = functools.partial(FixtureHandler, directory=str(Path(__file__).resolve().parents[1] / 'fixtures'))
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), handler)
    serving = threading.Thread(target=server.serve_forever, daemon=True)
    serving.start()
    fixture_url = 'http://127.0.0.1:' + str(server.server_port)
    profile = None
    browser = None
    try:
        with tempfile.TemporaryDirectory(prefix='pagepilot-profile-') as directory:
            profile = Path(directory)
            with (evidence / 'chrome-stderr.log').open('wb') as log:
                browser = subprocess.Popen([args.chrome, '--headless=new', '--remote-debugging-port=0',
                    '--user-data-dir=' + str(profile), '--no-first-run', '--no-default-browser-check',
                    '--disable-background-networking', '--disable-component-update', '--disable-sync',
                    '--metrics-recording-only', '--password-store=basic', '--use-mock-keychain', 'about:blank'],
                    stdout=log, stderr=log, start_new_session=True)
                try:
                    deadline = time.monotonic() + 20
                    marker = profile / 'DevToolsActivePort'
                    while not marker.exists():
                        if browser.poll() is not None:
                            raise RuntimeError('owned Chrome exited before DevTools became ready')
                        if time.monotonic() > deadline:
                            raise TimeoutError('owned Chrome did not expose DevTools within 20 seconds')
                        time.sleep(0.05)
                    port = int(marker.read_text().splitlines()[0])
                    for program in args.programs:
                        if browser.poll() is not None:
                            raise RuntimeError('owned Chrome exited before the next integration program')
                        command = [str(Path(program).resolve()), str(port)]
                        result = {'program': command[0], 'exit_code': None}
                        try:
                            with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    start_new_session=True, env={**os.environ, "PAGEPILOT_FIXTURE_URL": fixture_url, "PAGEPILOT_CHILD_EVIDENCE": str(evidence / (Path(program).name + '-children')), **({"PAGEPILOT_BINARY": str(Path(args.binary).resolve())} if args.binary else {})}) as process:
                                try:
                                    stdout, stderr = process.communicate(timeout=60)
                                except subprocess.TimeoutExpired:
                                    result['timed_out'] = True
                                    # Stop this program's helper processes too before
                                    # running another independent program.
                                    try:
                                        os.killpg(process.pid, signal.SIGKILL)
                                    except ProcessLookupError:
                                        pass
                                    stdout, stderr = process.communicate()
                                result['exit_code'] = process.returncode
                        except OSError as error:
                            stdout, stderr = b'', str(error).encode()
                            result['launch_error'] = str(error)
                        (evidence / (Path(program).name + '.stdout')).write_bytes(stdout)
                        (evidence / (Path(program).name + '.stderr')).write_bytes(stderr)
                        result['browser_alive_after'] = browser.poll() is None
                        results.append(result)
                        if not result['browser_alive_after']:
                            raise RuntimeError('owned Chrome exited during an integration program')
                finally:
                    if browser.poll() is None:
                        os.killpg(browser.pid, signal.SIGTERM)
                        try:
                            browser.wait(timeout=10)
                        except subprocess.TimeoutExpired:
                            os.killpg(browser.pid, signal.SIGKILL)
                            browser.wait(timeout=5)
    finally:
        server.shutdown()
        server.server_close()
        serving.join(timeout=5)
        (evidence / 'owned-browser-run.json').write_text(json.dumps({
            'programs': results, 'owned_browser_pid': browser.pid if browser else None,
            'owned_browser_exit': browser.returncode if browser else None,
            'temporary_profile_removed_after_exit': profile is not None and not profile.exists(),
            'http_server_thread_stopped': not serving.is_alive()}, indent=2) + '\n')
    failed = [Path(result['program']).name for result in results
              if result['exit_code'] != 0 or result.get('timed_out')]
    print(json.dumps({'passed': not failed, 'programs': len(results),
                      'failed_programs': failed, 'profile_cleaned': not profile.exists()}))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
