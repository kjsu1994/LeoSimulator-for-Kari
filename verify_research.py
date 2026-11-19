# SPDX-License-Identifier: GPL-3.0-only
"""Create durable, source-bound verification evidence. No user runs are modified."""
from datetime import datetime,timezone
from pathlib import Path
import argparse,json,platform,subprocess,sys,tempfile,time,uuid
from research import file_hash,source_hashes
ROOT=Path(__file__).resolve().parent

def verify(output,build=True,browser=True):
    output=Path(output);output.mkdir(parents=True,exist_ok=False)
    report={'schema':1,'createdAt':datetime.now(timezone.utc).isoformat(),'status':'running','platform':platform.platform(),'python':sys.version,'scope':[],'checks':[]}
    def save():
        (output/'report.json').write_text(json.dumps(report,ensure_ascii=False,allow_nan=False,indent=2),encoding='utf8')
    def command(name,args,timeout=600):
        print('CHECK:',name,flush=True)
        start=time.monotonic();log=output/(name+'.log')
        with log.open('wb') as f:result=subprocess.run(args,cwd=ROOT,stdout=f,stderr=subprocess.STDOUT,timeout=timeout,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        report['checks'].append({'name':name,'status':'passed' if result.returncode==0 else 'failed','seconds':time.monotonic()-start,'log':log.name,'sha256':file_hash(log)});save()
        if result.returncode:raise RuntimeError(f'{name} failed; see {log}')
    try:
        if build:command('build',[sys.executable,'engine/build.py'],timeout=900)
        report['engineSHA256']=file_hash(ROOT/'engine/engine.exe');report['sources']=source_hashes(ROOT)
        info=json.loads((ROOT/'engine/build-info.json').read_text(encoding='utf8'))
        assert info['sha256']==report['engineSHA256'],'Engine metadata mismatch'
        actual={p.relative_to(ROOT/'engine').as_posix():file_hash(p) for p in (ROOT/'engine').rglob('*') if p.is_file() and p.suffix in ('.h','.cpp','.cc')}
        assert actual==info['sources'],'Native sources do not match build'
        for manifest,base in [(ROOT/'engine/vendor/routing-origin.json',ROOT/'engine/vendor/routing'),(ROOT/'engine/vendor-origin.json',ROOT/'engine/vendor/Global/Utilities/libnorad')]:
            hashes=json.loads(manifest.read_text(encoding='utf8'))
            for name,expected in hashes.items():assert file_hash(base/name)==expected,('Vendor changed',name)
        report['scope'].append('vendor-integrity')
        names=['test_algorithms','test_control']+[p.stem for p in sorted((ROOT/'engine/tests/reference').glob('*.cpp'))]
        for name in names:command(name,[str(ROOT/'engine'/(name+'.exe'))])
        with tempfile.TemporaryDirectory(prefix='leo-kernel-') as tmp:command('test_kernel',[str(ROOT/'engine/test_kernel.exe'),tmp])
        command('independent-oracle',[sys.executable,'verify_oracle.py'])
        report['scope']+=['fixed-graph-core','analytic-packet-kernel','reference-core']
        command('repetitions',[sys.executable,'verify_repetitions.py',str(output/'repetitions'),'--final'])
        report['scope'].append('repetition-workflow-12-satellites')
        command('python-regression',[sys.executable,'-m','unittest','discover','-p','test_*.py'])
        command('csv',['node','test_csv.js']);command('replay',['node','test_replay.js'])
        if browser:command('browser',['node','test_browser.js'])
        report['scope']+=['integration','result-integrity']+(['browser'] if browser else [])
        assert source_hashes(ROOT)==report['sources'],'Sources changed during verification'
        assert file_hash(ROOT/'engine/engine.exe')==report['engineSHA256'],'Engine changed during verification'
        report['status']='passed'
    except Exception as exc:report.update(status='failed',error=str(exc));raise
    finally:save()
    print('VERIFIED:',output,flush=True)
    return report

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path);parser.add_argument('--skip-build',action='store_true');parser.add_argument('--skip-browser',action='store_true');args=parser.parse_args()
    output=args.output or ROOT/'data/validation'/('verification-'+datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')+'-'+uuid.uuid4().hex[:6])
    verify(output,build=not args.skip_build,browser=not args.skip_browser)

if __name__=='__main__':main()
