#!/usr/bin/env python3
"""Check exported JPEG XS files against the standalone SVT decoder."""
from pathlib import Path
import argparse,os,subprocess,json,shutil
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--decoder',default='SvtJpegxsDecApp')
parser.add_argument('--output',default='build/jpegxs-cli-checks')
args=parser.parse_args()
decoder=shutil.which(args.decoder)
if not decoder: raise SystemExit('Standalone JPEG XS decoder is unavailable: '+args.decoder)
out=Path(args.output);out.mkdir(parents=True,exist_ok=True)
results=[]
for depth in (8,10,12):
    for chroma in ('420','422','444'):
        for variant in ('default','lossless'):
            tag=f'{depth}-{chroma}-{variant}';stream=out/(tag+'.jxs')
            env=dict(os.environ,CODEC_VIS_TEST_EXPORT_RAW='1',CODEC_VIS_TEST_PATTERN='noise',CODEC_VIS_TEST_PARAMS='bpp=18')
            probe=subprocess.run(['./build/test_codec_roundtrips','15',str(depth),chroma,variant,'128','96',str(stream)],env=env,capture_output=True,text=True,timeout=30)
            app=subprocess.run([decoder,'-v','0','-n','1','-i',str(stream),'-o',str(out/(tag+'.standalone.yuv'))],env=dict(os.environ,LD_LIBRARY_PATH='/usr/local/lib:'+os.environ.get('LD_LIBRARY_PATH','')),capture_output=True,text=True,timeout=30)
            same=app.returncode==0 and Path(str(stream)+'.decoded.yuv').read_bytes()==(out/(tag+'.standalone.yuv')).read_bytes()
            results.append(dict(case=tag,probe=probe.returncode,decoder=app.returncode,samples_identical=same))
            (out/(tag+'.log')).write_text(probe.stdout+probe.stderr+'\n--- standalone decoder ---\n'+app.stdout+app.stderr)
            print(tag,'PASS' if probe.returncode==0 and same else 'FAIL',flush=True)
(out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
raise SystemExit(0 if all(r['probe']==0 and r['samples_identical'] for r in results) else 1)
