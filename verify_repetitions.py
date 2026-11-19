# SPDX-License-Identifier: GPL-3.0-only
"""Exercise paired repetitions in an isolated small fixture, not a performance claim."""
from pathlib import Path
import json,shutil,sys,time
import server,run_manager
ROOT=Path(__file__).resolve().parent

def verify(output,final=False):
    base=Path(output);base.mkdir(parents=True,exist_ok=False)
    (base/'engine').mkdir();shutil.copy2(ROOT/'engine/engine.exe',base/'engine/engine.exe');shutil.copy2(ROOT/'replay-model.json',base/'replay-model.json')
    settings=base/'settings';settings.mkdir();ini=settings/'omnetpp.ini'
    ini.write_text('[General]\n*.numSatellites=12\n*.**.planeCount=4\n**.nrOfGS=2\n*.topologyControl.minimumElevation=-90\n*.native.controlRefresh=.25\n*.native.controlResyncInterval=.5\n[Config Verification]\nsim-time-limit=2s\n*.station[0].traffic.enabled=true\n*.station[0].traffic.dstStation=1\n*.station[0].traffic.startTime=uniform(0s,.2s)\n*.station[0].traffic.sendInterval=.02s\n*.islFailures.schedule="0-1@.8+.3"\n*.satellite[*].net.routing.csibEnabled=true\n*.satellite[*].net.routing.csibPeriod=.5s\n',encoding='utf8')
    reader=lambda name,variables=None:server.read_ini_config(name,entry_path=str(ini),variables=variables)
    results=[]
    for count in ([10,30] if final else [10]):
        job=run_manager.start(base,settings,ini,'Verification',reader,algorithms=['dijkstra-delay','dijkstra-hop','mcsr-ls','mcsr-ts','mcsr-hr'],research={'repetitions':count,'seedStart':100,'warmupSeconds':.25,'drainSeconds':.25})
        deadline=time.monotonic()+300
        while run_manager.state(job['runId'])['status'] in ('queued','running'):
            if time.monotonic()>deadline:run_manager.cancel(job['runId']);raise TimeoutError('Repetition fixture exceeded 300s')
            time.sleep(.05)
        state=run_manager.state(job['runId']);assert state['status']=='completed',state.get('error')
        report=state['statistics'];assert report['completedRuns']==count*5
        assert report['inputVariability']['trafficProfiles']==count
        assert report['stage']==('final' if count>=30 else 'pilot')
        for comparison in report['comparisons']:
            metric=next(m for m in comparison['metrics'] if m['name']=='deliveryRate')
            assert metric['statistics']['pairs']==count and metric['statistics']['ci95'] is not None
        results.append({'experimentId':job['runId'],'repetitions':count,'runs':count*5})
    record={'scope':'repetition-workflow-12-satellites','status':'passed','results':results,'performanceClaim':False}
    (base/'verified.json').write_text(json.dumps(record,indent=2),encoding='utf8');print(json.dumps(record,indent=2),flush=True)
    return record

if __name__=='__main__':verify(sys.argv[1],final='--final' in sys.argv)
