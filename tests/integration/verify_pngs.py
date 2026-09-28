#!/usr/bin/env python3
"""Independently decode actual native captures and check fixture pixels."""
import json
import os
from pathlib import Path
from PIL import Image

directory = Path(os.environ['PAGEPILOT_CHILD_EVIDENCE']).parent / 'pilot-files-tests-children'
checks = 0

def require(condition, label):
    global checks
    if not condition:
        raise AssertionError(label)
    checks += 1

def read(name):
    path = directory / name
    metadata = json.loads((directory / (name + '.json')).read_text())
    with Image.open(path) as candidate:
        candidate.verify()
    with Image.open(path) as candidate:
        require(candidate.format == 'PNG' and candidate.size == (metadata['width'], metadata['height']), name + ': independent decode and dimensions')
        require(path.stat().st_size == metadata['size'], name + ': complete byte count')
        return candidate.convert('RGB')

viewport = read('viewport.png')
require(viewport.getpixel((60, 100)) == (230, 40, 50) and viewport.getpixel((160, 100)) == (30, 170, 70), 'viewport has exact fixture swatch pixels')
full = read('full.png')
require(full.size == (800, 1600) and full.getpixel((100, 1350)) == (30, 80, 220), 'full capture contains below-viewport content')
element = read('element.png')
require(element.size == (200, 120) and element.getpixel((20, 20)) == (230, 40, 50) and element.getpixel((150, 20)) == (30, 170, 70), 'element crop has correct content and origin')
scrolled = read('scrolled.png')
require(scrolled.getpixel((20, 20)) == (250, 220, 20), 'viewport capture honors actual scroll position')
far = read('far.png')
require(far.size == (160, 90) and far.getpixel((20, 20)) == (30, 80, 220), 'offscreen element crop has exact pixels')
saved = read('saved.png')
require(saved.tobytes() == element.tobytes(), 'atomic file output decodes identically to inline element capture')
retina = read('retina.png')
require(retina.size == (1600, 1200) and retina.getpixel((120, 200)) == (230, 40, 50), 'device-scale image contains correct physical pixels')
frame = read('oop-element.png')
require(frame.getpixel((frame.width // 2, frame.height // 2)) == (30, 80, 220), 'transformed OOP crop contains intended child element')
print(json.dumps({'checks': checks, 'passed': True, 'decoder': 'Pillow', 'images': 8}))
