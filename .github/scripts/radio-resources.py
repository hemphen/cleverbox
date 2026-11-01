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
    sources = ('radio_transport.cpp', 'idf_hal.h', 'board.h')
    paths = [f'firmware/components/radio_transport/{name}' for name in sources]
    # Struct layouts used by the owner also come from protocol/store/time headers.
    # Mixing base source with candidate headers can fail compilation or silently
    # measure the wrong layout. Keep each component's original include tree.
    public_headers = subprocess.check_output([
        'git', '-C', str(root), 'ls-tree', '-r', '--name-only', base,
        'firmware/components'], text=True).splitlines()
    public_headers = [p for p in public_headers if len(Path(p).parts) >= 5 and
                      Path(p).parts[3] == 'include' and p.endswith('.h')]
    include_dirs = sorted({str(temp.joinpath(*Path(p).parts[:4]))
                           for p in public_headers})
    for name in paths + public_headers:
        target = temp / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(subprocess.check_output([
            'git', '-C', str(root), 'show', f'{base}:{name}']))
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
        command.append(str(temp / 'firmware/components/radio_transport/radio_transport.cpp') if arg == entry['file'] else arg)
        i += 1
    # All base project headers must take precedence over candidate layouts.
    compiler_index = next(i for i, a in enumerate(command) if a.endswith('riscv32-esp-elf-g++'))
    command[compiler_index + 1:compiler_index + 1] = ['-I' + p for p in include_dirs]
    subprocess.run(command, cwd=entry['directory'], check=True)
    before, after = ram(output), ram(original_object)
    print(f'Radio object RAM versus {base} (same compiler/SDK/configuration):')
    for key in before:
        print(f'  {key}: base={before[key]} candidate={after[key]} delta={after[key]-before[key]:+d} bytes')
    print('Object sections only; runtime heap/stack and final linker padding are excluded.')
