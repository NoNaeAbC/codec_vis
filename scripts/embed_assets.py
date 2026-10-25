#!/usr/bin/env python3
"""Embed raw assets with GNU objcopy; emit only linker declarations as C++."""
import argparse
from pathlib import Path
import re
import shutil
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--word-size', type=int, choices=(1, 4), default=1)
parser.add_argument('--objcopy', default='objcopy')
parser.add_argument('--target-machine', required=True)
parser.add_argument('assets', nargs='+', help='C++ identifier=source path')
args = parser.parse_args()
if args.target_machine.startswith('x86_64'):
    target, architecture = 'elf64-x86-64', 'i386:x86-64'
elif args.target_machine.startswith('aarch64'):
    target, architecture = 'elf64-littleaarch64', 'aarch64'
else:
    parser.error('Unsupported ELF architecture: ' + args.target_machine)
folder = args.output.resolve().parent
folder.mkdir(parents=True, exist_ok=True)
stack = folder / 'empty-stack-note'
stack.write_bytes(b'')
lines = ['// Generated linker declarations. Asset bytes live in objcopy ELF objects.',
         '#pragma once', '#include <span>', '#include <cstdint>']
names = []
for asset in args.assets:
    name, filename = asset.split('=', 1)
    if not re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]*', name):
        parser.error('Invalid C++ identifier: ' + name)
    source = Path(filename)
    if not source.stat().st_size or source.stat().st_size % args.word_size:
        parser.error('Empty or misaligned asset: ' + filename)
    binary = 'asset_' + name + '.bin'
    symbol = '_binary_asset_' + name + '_bin_'
    public = 'codec_vis_asset_' + name + '_'
    shutil.copyfile(source, folder / binary)
    subprocess.run([
        args.objcopy, '--input-target=binary', '--output-target=' + target,
        '--binary-architecture=' + architecture,
        '--set-section-alignment', '.data=16',
        '--rename-section', '.data=.rodata.codec_vis.' + name + ',alloc,load,readonly,data,contents',
        '--redefine-sym', symbol + 'start=' + public + 'start',
        '--redefine-sym', symbol + 'end=' + public + 'end',
        '--strip-symbol', symbol + 'size',
        '--add-section', '.note.GNU-stack=' + str(stack),
        '--set-section-flags', '.note.GNU-stack=readonly',
        binary, 'embedded_' + name + '.o',
    ], cwd=folder, check=True)
    lines.extend(['extern "C" {',
                  'alignas(16) extern const unsigned char ' + public + 'start[];',
                  'extern const unsigned char ' + public + 'end[];', '}'])
    names.append((name, public, source.stat().st_size))
lines.append('namespace codec_gui::embedded {')
for name, public, size in names:
    kind = 'uint8_t' if args.word_size == 1 else 'uint32_t'
    lines.append(f'inline const std::span<const {kind}> {name}' + '{' +
                 f'reinterpret_cast<const {kind}*>({public}start), '
                 f'{size} / sizeof({kind})' + '};')
lines.append('}')
text = '\n'.join(lines) + '\n'
args.output.write_text(text)
