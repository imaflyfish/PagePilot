#!/usr/bin/env python3
"""Native MCP upload and full-size screenshot roundtrips with restricted roots."""
import base64
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from PIL import Image

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

noise = """(()=>{
  document.body.style.margin='0';document.body.innerHTML='<canvas id=noise width=512 height=512 style="display:block"></canvas>';
  const canvas=document.querySelector('#noise'),context=canvas.getContext('2d'),image=context.createImageData(512,512);
  let seed=98;
  for(let i=0;i<image.data.length;i+=4){seed=(1664525*seed+1013904223)>>>0;image.data[i]=seed&255;image.data[i+1]=(seed>>>8)&255;image.data[i+2]=(seed>>>16)&255;image.data[i+3]=255;}
  context.putImageData(image,0,0);return true;
})()"""

def run(name, messages, roots, compatibility=False):
    command = [binary, '--port', port]
    if compatibility:
        command.append('--compat-tools')
    for root in roots:
        command.extend(['--allow-root', str(root)])
    completed = subprocess.run(command, input=('\n'.join(json.dumps(message, ensure_ascii=False) for message in messages) + '\n').encode(), capture_output=True, timeout=30)
    (evidence / (name + '.stdout.jsonl')).write_bytes(completed.stdout)
    (evidence / (name + '.stderr.jsonl')).write_bytes(completed.stderr)
    require(completed.returncode == 0, name + ': native child exit')
    require(not completed.stderr, name + ': protocol output remains isolated')
    responses = [json.loads(line) for line in completed.stdout.splitlines()]
    require(len(responses) == len(messages) - 1, name + ': one response per request')
    return {row['id']: row for row in responses}

def initialize(version):
    return [request('initialize', 'initialize', {'protocolVersion': version, 'capabilities': {}, 'clientInfo': {'name': 'owned-file-client', 'version': '1'}}),
            {'jsonrpc': '2.0', 'method': 'notifications/initialized'}]

def verify_noise(result, name):
    encoded = result['screenshot']
    require(len(encoded) > 500000, name + ': screenshot exceeds preview/truncation thresholds')
    decoded = base64.b64decode(encoded, validate=True)
    require(len(decoded) == result['size'], name + ': complete decoded PNG byte count')
    with Image.open(io.BytesIO(decoded)) as image:
        image.load()
        require(image.size == (512, 512) and image.format == 'PNG', name + ': independent PNG decode')
        seed = (1664525 * 98 + 1013904223) & 0xffffffff
        require(image.convert('RGB').getpixel((0, 0)) == (seed & 255, (seed >> 8) & 255, (seed >> 16) & 255), name + ': independent fixture pixel')
    (evidence / (name + '.png')).write_bytes(decoded)

with tempfile.TemporaryDirectory(prefix='pagepilot-mcp-files-') as directory:
    base = Path(directory)
    allowed, outside = base / 'allowed', base / 'outside'
    allowed.mkdir(); outside.mkdir()
    source = allowed / '中文😀.txt'; source.write_text('Actual MCP upload 中文😀')
    forbidden = outside / 'sentinel.txt'; forbidden.write_text('private test sentinel')
    messages = initialize('2025-11-25') + [
        action('tab', 'tab_create'),
        action('navigate', 'page_navigate', {'url': site + '/page.html'}),
        action('upload', 'form_upload', {'selector': '#files', 'files': str(source)}),
        action('read', 'page_evaluate', {'script': "document.querySelector('#files').files[0].text()"}),
        action('denied', 'form_upload', {'selector': '#files', 'files': str(forbidden)}),
        action('retained', 'page_evaluate', {'script': "document.querySelector('#files').files[0].name"}),
        action('paint', 'page_evaluate', {'script': noise}),
        action('capture', 'page_capture', {'selector': '#noise'}),
        action('save', 'page_capture', {'selector': '#noise', 'path': str(evidence / 'atomic.png')}),
        action('denied-output', 'page_capture', {'path': str(outside / 'blocked.png')}),
        action('close', 'tab_close'),
    ]
    rows = run('canonical-files', messages, [allowed, evidence])
    def value(identity):
        return rows[identity]['result']['structuredContent']
    require(value('upload')['uploaded'] == 1 and value('read')['result'] == 'Actual MCP upload 中文😀', 'MCP upload produces real readable browser file')
    require(rows['denied']['result']['isError'] and value('retained')['result'] == '中文😀.txt', 'MCP root refusal preserves current file selection')
    require(not rows['capture']['result']['isError'], 'MCP native capture succeeds')
    verify_noise(value('capture'), 'canonical-noise')
    text_result = json.loads(rows['capture']['result']['content'][0]['text'])
    require(text_result['screenshot'] == value('capture')['screenshot'], 'structured and text MCP results contain identical complete image')
    require(Path(value('save')['saved']).read_bytes() == (evidence / 'canonical-noise.png').read_bytes(), 'native atomic screenshot output matches full inline image')
    require(rows['denied-output']['result']['isError'] and not (outside / 'blocked.png').exists(), 'MCP output root refusal creates no outside file')
    require(value('close')['closed'], 'owned MCP file-test tab closed')
    legacy = initialize('2024-11-05') + [
        action('tab', 'new_tab', {'url': site + '/page.html'}),
        action('upload', 'upload_file', {'selector': '#files', 'files': str(source)}),
        action('read', 'eval', {'script': "document.querySelector('#files').files[0].text()"}),
        action('paint', 'eval', {'script': noise}),
        action('capture', 'screenshot', {'selector': '#noise'}),
        action('close', 'close_tab'),
    ]
    rows = run('legacy-files', legacy, [allowed, evidence], compatibility=True)
    require(all(not rows[key]['result'].get('isError', False) for key in ('tab', 'upload', 'read', 'paint', 'capture', 'close')), 'legacy names complete through actual native MCP process')
    legacy_image = rows['capture']['result']
    require('structuredContent' not in legacy_image, 'MCP 2024 response uses legacy text content')
    verify_noise(json.loads(legacy_image['content'][0]['text']), 'legacy-noise')
print(json.dumps({'checks': checks, 'passed': True, 'transport': 'actual native MCP processes', 'decoder': 'Pillow'}))
