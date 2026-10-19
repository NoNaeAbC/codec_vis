#!/usr/bin/env python3
"""Regress large HEVC still-image NAL decoding and compare with FFmpeg."""
import filecmp
import os
from pathlib import Path
import resource
import subprocess

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
root = Path('build/hevc-large-nal').resolve()
root.mkdir(parents=True, exist_ok=True)
stream = root / 'lossless-12bit-444.h265'
env = dict(os.environ, CODEC_VIS_TEST_PATTERN='noise',
           CODEC_VIS_TEST_EXPORT_RAW='1', CODEC_VIS_TEST_CHECK_COLOR='1')
with (root / 'roundtrip.log').open('w') as log:
    subprocess.run(['build/test_codec_roundtrips', '1', '12', '444', 'lossless',
                    '4096', '2160', str(stream)], env=env, stdout=log,
                   stderr=subprocess.STDOUT, check=True, timeout=180)

# Verify that this fixture actually exercises the old default 16 MiB limit.
data = stream.read_bytes()
starts = []
offset = 0
while (offset := data.find(b'\x00\x00\x01', offset)) != -1:
    starts.append(offset)
    offset += 3
largest = max(b - a - 3 for a, b in zip(starts, starts[1:] + [len(data)]))
assert largest > 16 * 1024 * 1024, f'Fixture NAL is too small: {largest}'
del data
reference = root / 'ffmpeg.yuv'
subprocess.run(['ffmpeg', '-v', 'error', '-xerror', '-i', str(stream),
                '-frames:v', '1', '-pix_fmt', 'yuv444p12le', '-f', 'rawvideo',
                '-y', str(reference)], check=True, timeout=180)
assert filecmp.cmp(str(stream) + '.decoded.yuv', reference, shallow=False), \
    'libde265 and FFmpeg decoded samples differ'
print(f'PASS: {largest}-byte NAL; lossless samples and FFmpeg decode agree exactly')
