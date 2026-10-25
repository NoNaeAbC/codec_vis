#!/usr/bin/env python3
"""Link all application libraries as archives, except host graphics/session APIs.

Using -l:libNAME.a rather than a global -Bstatic keeps glibc and driver loading
normal. Missing archives fail the build instead of silently shipping codec .so's.
"""
import shlex
import subprocess
import sys

packages = sys.argv[1:]
flags = shlex.split(subprocess.check_output(['pkg-config', '--static', '--libs', *packages], text=True))
dynamic = {'vulkan', 'va', 'va-drm', 'drm'}
result = ['-Wl,--start-group']
for flag in flags:
    if flag in {'-lstdc++', '-lgcc', '-lgcc_s'}:
        continue  # The compiler supplies it via -static-libstdc++.
    if flag == '-lnsl':
        # GStreamer probes Solaris networking libraries even on Linux, where
        # its socketpair/getifaddrs calls are supplied by libc.
        continue
    if flag == '-fopenmp':
        flag = '-l:libgomp.a'
    if flag == '-latomic_asneeded':
        flag = '-latomic'
    if flag.startswith('-l') and not flag.startswith('-l:') and flag[2:] not in dynamic:
        flag = '-l:lib' + flag[2:] + '.a'
    result.append(flag)
result.extend(['-Wl,--end-group', '-static-libstdc++', '-static-libgcc'])
print(shlex.join(result))
