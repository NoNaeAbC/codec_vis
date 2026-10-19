#!/usr/bin/env python3
"""Run native codec probes sequentially; isolate crashes and retain all outcomes.

This script orchestrates tests only. Pixel validation runs in the C++ probe.
"""
import argparse
import csv
import json
import os
import resource
from pathlib import Path
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--binary', default='build/test_codec_roundtrips')
parser.add_argument('--output', default='build/roundtrip-report')
parser.add_argument('--backend', type=int, action='append')
parser.add_argument('--timeout', type=int, default=60)
parser.add_argument('--allow-unavailable', action='store_true', help='Allow an incomplete matrix when a dependency or GPU is missing')
args = parser.parse_args()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
binary = str(Path(args.binary).resolve())
out = Path(args.output).resolve()
out.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, TMPDIR=str(out))
listed = subprocess.run([binary, '--list'], capture_output=True, text=True, env=env, check=True)
(out/'discovery.log').write_text(listed.stderr)
cases = []
for line in listed.stdout.splitlines():
    fields = line.split('\t')
    if len(fields) < 7 or not fields[0].isdigit():
        continue
    backend, name, available, depths, chromas, scc, error = fields[:7]
    backend = int(backend)
    if args.backend and backend not in args.backend:
        continue
    if available != '1':
        cases.append((backend, name, 8, '420', 'default', 96, 64))
        continue
    for depth in map(int, depths.split(',')):
        for chroma in chromas.split(','):
            cases.append((backend, name, depth, chroma, 'default', 640 if backend in (4,5) or backend >= 10000 else 96, 480 if backend in (4,5) or backend >= 10000 else 64))
            variants = []
            if backend in (1,10,11,14,15): variants += ['lossless']
            if backend == 6 and chroma == '420': variants += ['alf']
            if backend == 8: variants += ['near'] + (['hp1','hp2','hp3'] if depth in (8,16) else [])
            if backend == 9: variants += ['progressive','arithmetic']
            if backend == 12: variants += ['frequency','tiles']
            if scc == '1' and chroma != '422': variants += ['scc']
            for variant in variants:
                cases.append((backend,name,depth,chroma,variant,640 if backend in (4,5) or backend >= 10000 else 96,480 if backend in (4,5) or backend >= 10000 else 64))
        if 8 <= backend <= 13 and depth != 16:
            cases.append((backend,name,depth,'400','default',96,64))
            if backend in (10,11): cases.append((backend,name,depth,'400','lossless',96,64))
    # Odd-size padding and a square input exercise different codec geometry.
    depth = int(depths.split(',')[0])
    chroma = chromas.split(',')[0]
    if backend not in (4,5) and backend < 10000:
        cases.extend([(backend,name,depth,chroma,'default',127,65),
                      (backend,name,depth,chroma,'default',128,128)])

results=[]
with (out/'results.tsv').open('w') as table:
    writer=csv.writer(table, delimiter='\t')
    writer.writerow(['backend','name','depth','chroma','variant','width','height','status','detail'])
    for index, (backend,name,depth,chroma,variant,w,h) in enumerate(cases):
        tag=f'{backend}-{depth}-{chroma}-{variant}-{w}x{h}'
        command=[binary,str(backend),str(depth),chroma,variant,str(w),str(h)]
        # Preserve video bitstreams for independent decoder diagnosis.
        if backend in (1,2,4,6,7):
            extension = {1: '.h265', 2: '.vvc', 4: '.h265', 6: '.vvc', 7: '.ivf'}[backend]
            command.append(str(out/(tag+extension)))
        try:
            run=subprocess.run(command, env=env, capture_output=True, text=True, timeout=args.timeout)
            log=run.stdout+'\n--- stderr ---\n'+run.stderr
            status='PASS' if run.returncode == 0 else ('UNAVAILABLE' if run.returncode == 77 else 'FAIL')
            lines=[s for s in run.stdout.splitlines() if s.startswith(('FAIL\t','PASS\t','UNAVAILABLE\t'))]
            detail=lines[-1] if lines else f'exit={run.returncode}; last stage: '+run.stdout[-300:]
        except subprocess.TimeoutExpired as e:
            status='TIMEOUT'; detail=f'exceeded {args.timeout}s'
            log=(e.stdout or b'').decode(errors='replace')+'\n'+(e.stderr or b'').decode(errors='replace')
        (out/(tag+'.log')).write_text(log)
        detail=detail.replace('\t',' ').replace('\n',' ')
        result=dict(backend=backend,name=name,depth=depth,chroma=chroma,variant=variant,width=w,height=h,status=status,detail=detail)
        results.append(result)
        writer.writerow(result.values());table.flush()
        print(f'{index+1}/{len(cases)} {tag}: {status} {detail[:180]}', flush=True)
(out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
counts={s:sum(r['status']==s for r in results) for s in ['PASS','FAIL','UNAVAILABLE','TIMEOUT']}
print(json.dumps(counts), flush=True)
sys.exit(1 if counts['FAIL'] or counts['TIMEOUT'] or (counts['UNAVAILABLE'] and not args.allow_unavailable) else 0)
