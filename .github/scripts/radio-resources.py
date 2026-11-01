#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compare radio object static RAM with exact base sources and the same IDF flags.

No second firmware build. Image/total linker sizes come from idf.py build/size.
This excludes runtime heap, stack use and linker padding between components.
"""
import json
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
base = sys.argv[1]
entries = json.loads((root / 'firmware/build/compile_commands.json').read_text())
entry = next(e for e in entries if e['file'].endswith('/radio_transport/radio_transport.cpp'))
args = entry.get('arguments') or shlex.split(entry['command'])
compiler = next(Path(a) for a in args if a.endswith('riscv32-esp-elf-g++'))
size = compiler.with_name('riscv32-esp-elf-size')
original_object = Path(entry['directory']) / args[args.index('-o') + 1]


def ram(path):
    text = subprocess.check_output([str(size), '-A', str(path)], text=True)
    totals = {'data': 0, 'bss': 0}
    for line in text.splitlines():
        fields = line.split()
        if len(fields) < 3:
            continue
        for key, prefixes in [('data', ('.data', '.sdata')), ('bss', ('.bss', '.sbss'))]:
            if fields[0].startswith(prefixes):
                totals[key] += int(fields[1])
    return totals


with tempfile.TemporaryDirectory(prefix='radio-resource-base-') as temp:
    temp = Path(temp)
    for name in ('radio_transport.cpp', 'idf_hal.h', 'board.h', 'include/radio_transport.h'):
        target = temp / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(subprocess.check_output([
            'git', '-C', str(root), 'show', f'{base}:firmware/components/radio_transport/{name}']))
    output = temp / 'radio.o'
    command = []
    i = 0
    while i < len(args):
        arg = args[i]
        if arg in ('-MF', '-MT', '-MQ'):
            i += 2
            continue
        if arg in ('-MD', '-MMD', '-MP'):
            i += 1
            continue
        if arg == '-o':
            command.extend(['-o', str(output)])
            i += 2
            continue
        command.append(str(temp / 'radio_transport.cpp') if arg == entry['file'] else arg)
        i += 1
    # The base transport's public header must take precedence over the candidate.
    command.insert(command.index('-c'), '-I' + str(temp / 'include'))
    subprocess.run(command, cwd=entry['directory'], check=True)
    before, after = ram(output), ram(original_object)
    print(f'Radio object RAM versus {base} (same compiler/SDK/configuration):')
    for key in before:
        print(f'  {key}: base={before[key]} candidate={after[key]} delta={after[key]-before[key]:+d} bytes')
    print('Object sections only; runtime heap/stack and final linker padding are excluded.')
