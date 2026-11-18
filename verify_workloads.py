# SPDX-License-Identifier: GPL-3.0-only
"""Run the agreed acceptance matrix explicitly, not during ordinary unit tests."""
from concurrent.futures import ThreadPoolExecutor,as_completed
from pathlib import Path
import hashlib,json,subprocess,time,tempfile
import server
from native_engine import ALGORITHMS
from run_manager import memory_bytes
ROOT=Path(__file__).resolve().parent

def run_matrix(output,control=False):
    exe=ROOT/'engine/engine.exe';binary_hash=hashlib.sha256(exe.read_bytes()).hexdigest()
    output=Path(output);output.mkdir(parents=True,exist_ok=True)
    def run(rate,algorithm,duration):
        folder=output/f'{duration}s-{rate}Mbps-{algorithm}';folder.mkdir(exist_ok=True)
        result_file=folder/'validated.json'
        if result_file.exists():return json.loads(result_file.read_text())
        c=server.read_ini_config('PaperS1Mcsr');c.update(algorithm=algorithm,seed=0,simTimeLimit=duration,routeRecording='changes')
        c.update(controlMode='packet' if control else 'time',csibEnabled=control,processingMode='neutral')
        if control:c.update(csibPeriod=5,controlResyncInterval=10,csibVote=True)
        c['stations'][18]['sendInterval']=1380*8/(rate*1e6)
        input_file=folder/'engine-input.json';input_file.write_text(json.dumps(c,allow_nan=False))
        start=time.monotonic();peak=0
        with (folder/'run.log').open('wb') as log:
            process=subprocess.Popen([str(exe),str(input_file),str(folder)],stdout=log,stderr=subprocess.STDOUT,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
            try:
                while process.poll() is None:
                    time.sleep(.1);peak=max(peak,memory_bytes(process))
                    if peak>4*1024**3:raise RuntimeError('Exceeded 4GiB memory acceptance limit')
            finally:
                if process.poll() is None:process.kill();process.wait()
        if process.returncode:raise RuntimeError((folder/'run.log').read_text(errors='replace'))
        assert hashlib.sha256(exe.read_bytes()).hexdigest()==binary_hash,'Engine changed during validation'
        summary=json.loads((folder/'summary.json').read_text())
        assert summary['attempted']==summary['sent']+summary['noUplink']
        assert summary['attempted']==summary['received']+summary['dropped']+summary['inFlight']
        assert summary['received']>0 and summary['maxQueuePackets']<=500
        wire=summary.get('control',{});assert wire.get('inFlight',0)>=0
        if control:
            assert wire['wireBytes']>0
            assert wire['hopAccepted']==wire['arrived']+wire['dropped']+wire['inFlight']
            if algorithm.startswith('mcsr-'):
                assert wire['generatedByKind'].get('csib',0)>0 and wire['generatedByKind'].get('summary',0)>0
                assert any(row.get('frameAttempts',0)>row.get('messageHopAttempts',0) for row in wire['byKind'].values())
        physical_hash=hashlib.sha256((folder/'physical-events.json').read_bytes()).hexdigest()
        common_hash=hashlib.sha256((folder/'common-srd.json').read_bytes()).hexdigest()
        assert summary['metricVersion']==4
        result={'commonSrdHash':common_hash,'physicalEventsHash':physical_hash,'duration':duration,'rateMbps':rate,'algorithm':algorithm,'binarySHA256':binary_hash,'elapsedSeconds':time.monotonic()-start,'peakMemoryBytes':peak,'summary':summary}
        result_file.write_text(json.dumps(result,indent=2));return result
    smoke=[(r,a,10) for r in range(10,61,5) for a in ALGORITHMS]
    full=[(r,a,600) for r in (10,35,60) for a in ALGORITHMS]
    cases=[(10,a,20) for a in ALGORITHMS] if control else smoke+full
    with ThreadPoolExecutor(max_workers=2) as pool:
        for future in as_completed([pool.submit(run,*case) for case in cases]):
            result=future.result();print(result['duration'],result['rateMbps'],result['algorithm'],'OK',round(result['elapsedSeconds'],2),'s',round(result['peakMemoryBytes']/1024**2,1),'MiB',flush=True)
    records=[json.loads(p.read_text()) for p in output.glob('*/validated.json')]
    for duration in ((20,) if control else (10,600)):
        for rate in ((10,) if control else (range(10,61,5) if duration==10 else (10,35,60))):
            hashes={r['summary']['trafficHash'] for r in records if r['duration']==duration and r['rateMbps']==rate}
            assert len(hashes)==1,(duration,rate,hashes)
            common={r['commonSrdHash'] for r in records if r['duration']==duration and r['rateMbps']==rate};assert len(common)==1
            physical={r['physicalEventsHash'] for r in records if r['duration']==duration and r['rateMbps']==rate};assert len(physical)==1
    print(('CONTROL' if control else 'DATA')+' ACCEPTANCE PASSED:',len(cases),'runs;',output,flush=True)

def main():
    import sys
    with tempfile.TemporaryDirectory(prefix='leo-control-acceptance-' if '--control' in sys.argv else 'leo-data-acceptance-') as output:run_matrix(output,control='--control' in sys.argv)

if __name__=='__main__':main()
