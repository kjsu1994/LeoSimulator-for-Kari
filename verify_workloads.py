# SPDX-License-Identifier: GPL-3.0-only
"""Run the agreed acceptance matrix explicitly, not during ordinary unit tests."""
from concurrent.futures import ThreadPoolExecutor,as_completed
from pathlib import Path
import argparse,hashlib,json,math,subprocess,time
from datetime import datetime,timezone
from research import digest,file_hash,source_hashes
import server
from native_engine import ALGORITHMS
from run_manager import memory_bytes
ROOT=Path(__file__).resolve().parent

def cached_result(folder,signature):
    try:
        cached=json.loads((folder/'validated.json').read_text())
        if not isinstance(cached,dict):return None
        expected={'summary.json','physical-events.json','common-srd.json','engine-input.json'}
        if cached.get('signature')==signature and expected<=set(cached.get('outputs',{})) and all(file_hash(folder/name)==sha for name,sha in cached['outputs'].items()):return cached
    except (OSError,ValueError):pass
    return None

def workload_config(rate,algorithm,duration,control=False,long_control=False):
    c=server.read_ini_config('PaperS1Mcsr')
    c.update(algorithm=algorithm,seed=0,simTimeLimit=duration,routeRecording='changes',packetTrace=False,warmupSeconds=0,drainSeconds=0,
             controlMode='packet' if control else 'time',csibEnabled=control,processingMode='neutral')
    if control:c.update(csibPeriod=180 if long_control else 5,controlResyncInterval=30 if long_control else 10,csibVote=True)
    if long_control:c.update(numSatellites=132,planes=12)
    for station in c['stations']:
        if station['enabled']:station['endTime']=duration
    c['stations'][18].update(packetSize=1380,sendInterval=1380*8/(rate*1e6))
    return c

def attempt_bounds(config):
    def tick(value):return math.floor(value*1e11+.5)
    low=high=0
    for station in config['stations']:
        if not station['enabled']:continue
        end=min(tick(config['simTimeLimit']),tick(station['endTime']));step=tick(station['sendInterval'])
        starts=[station['startTime']]*2 if 'startTime' in station else station.get('startRange',[1,5])
        counts=[max(0,(end-tick(start)+step-1)//step) for start in starts]
        low+=min(counts);high+=max(counts)
    return [low,high]

def run_matrix(output,control=False,long_control=False):
    exe=ROOT/'engine/engine.exe';binary_hash=hashlib.sha256(exe.read_bytes()).hexdigest()
    output=Path(output);output.mkdir(parents=True,exist_ok=True)
    validators=digest(source_hashes(ROOT))
    def run(rate,algorithm,duration):
        folder=output/f'{duration}s-{rate}Mbps-{algorithm}';folder.mkdir(exist_ok=True)
        result_file=folder/'validated.json'
        c=workload_config(rate,algorithm,duration,control,long_control)
        signature=digest({'input':c,'engine':binary_hash,'validators':validators})
        cached=cached_result(folder,signature)
        if cached is not None:return cached
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
        expected=attempt_bounds(c)
        assert expected[0]<=summary['attempted']<=expected[1],('Traffic did not span configured interval',summary['attempted'],expected)
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
                if long_control:
                    with (folder/'network-events.jsonl').open(encoding='utf8') as stream:
                        generated=set();last_applied=0
                        for line in stream:
                            event=json.loads(line)
                            if event['event']=='csib-generated':generated.add(int(event['time']/180))
                            if event['event']=='csib-applied':last_applied=event['time']
                    assert {1,2,3}<=generated and last_applied>=540,'Missing sustained CSIB cycles'

        physical_hash=hashlib.sha256((folder/'physical-events.json').read_bytes()).hexdigest()
        common_hash=hashlib.sha256((folder/'common-srd.json').read_bytes()).hexdigest()
        assert summary['metricVersion']==4
        assert summary['modelVersion']==c['modelVersion'],'Engine model version mismatch'
        output_files=['summary.json','physical-events.json','common-srd.json','engine-input.json']
        if long_control:output_files.append('network-events.jsonl')
        result={'expectedAttempts':expected,'signature':signature,'outputs':{name:file_hash(folder/name) for name in output_files},'commonSrdHash':common_hash,'physicalEventsHash':physical_hash,'duration':duration,'rateMbps':rate,'algorithm':algorithm,'binarySHA256':binary_hash,'elapsedSeconds':time.monotonic()-start,'peakMemoryBytes':peak,'summary':summary}
        result_file.write_text(json.dumps(result,indent=2));return result
    smoke=[(r,a,10) for r in range(10,61,5) for a in ALGORITHMS]
    full=[(r,a,600) for r in (10,35,60) for a in ALGORITHMS]
    cases=[(10,a,600 if long_control else 20) for a in ALGORITHMS] if control else smoke+full
    with ThreadPoolExecutor(max_workers=2) as pool:
        for future in as_completed([pool.submit(run,*case) for case in cases]):
            result=future.result();print(result['duration'],result['rateMbps'],result['algorithm'],'OK',round(result['elapsedSeconds'],2),'s',round(result['peakMemoryBytes']/1024**2,1),'MiB',flush=True)
    records=[json.loads((output/f'{d}s-{r}Mbps-{a}'/'validated.json').read_text()) for r,a,d in cases]
    for duration in ((600 if long_control else 20,) if control else (10,600)):
        for rate in ((10,) if control else (range(10,61,5) if duration==10 else (10,35,60))):
            hashes={r['summary']['trafficHash'] for r in records if r['duration']==duration and r['rateMbps']==rate}
            assert len(hashes)==1,(duration,rate,hashes)
            common={r['commonSrdHash'] for r in records if r['duration']==duration and r['rateMbps']==rate};assert len(common)==1
            physical={r['physicalEventsHash'] for r in records if r['duration']==duration and r['rateMbps']==rate};assert len(physical)==1
    assert digest(source_hashes(ROOT))==validators,'Sources changed during workload verification'
    print(('CONTROL' if control else 'DATA')+' ACCEPTANCE PASSED:',len(cases),'runs;',output,flush=True)
    return records

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--control',action='store_true');parser.add_argument('--long-control',action='store_true')
    parser.add_argument('--output',type=Path)
    args=parser.parse_args()
    output=args.output or ROOT/'data/validation'/('workloads-'+datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ'))
    control=args.control or args.long_control
    records=run_matrix(output,control=control,long_control=args.long_control)
    verified=[]
    for r in records:
        folder=output/f"{r['duration']}s-{r['rateMbps']}Mbps-{r['algorithm']}"
        verified.append({'path':(folder/'validated.json').relative_to(output).as_posix(),'sha256':file_hash(folder/'validated.json')})
    scope='long-packet-control' if args.long_control else 'short-packet-control' if control else 'scale-data-load'
    (output/'workloads.json').write_text(json.dumps({
        'status':'passed','scope':scope,'engineSHA256':records[0]['binarySHA256'],'sources':source_hashes(ROOT),
        'satellites':132 if args.long_control else 1584,'simSeconds':20 if scope=='short-packet-control' else 600,
        'runs':len(records),'verified':verified
    },indent=2),encoding='utf8')

if __name__=='__main__':main()
