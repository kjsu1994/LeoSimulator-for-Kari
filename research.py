# SPDX-License-Identifier: GPL-3.0-only
"""Paired run-level statistics and version-bound local verification evidence."""
from pathlib import Path
from datetime import datetime, timezone
import hashlib, json, math, platform, random, statistics

METRICS=('deliveryRate','throughputMbps','meanDelayMs','jitterMs','unrecoveredRate','controlWireBytes','edgeChecks','labelExpand')

def digest(value):
    return hashlib.sha256(json.dumps(value,sort_keys=True,ensure_ascii=False,allow_nan=False,separators=(',',':')).encode()).hexdigest()

def file_hash(path):
    h=hashlib.sha256()
    with Path(path).open('rb') as f:
        for block in iter(lambda:f.read(1024*1024),b''):h.update(block)
    return h.hexdigest()

def source_hashes(base):
    base=Path(base)
    paths=set(base.glob('*.py'))|set(base.glob('*.js'))|set(base.glob('*.html'))|set(base.glob('*.css'))
    paths.update(p for p in (base/'engine').rglob('*') if p.suffix in ('.h','.cpp','.cc') and p.is_file())
    return {p.relative_to(base).as_posix():file_hash(p) for p in sorted(paths)}

def evidence(base):
    base=Path(base);engine=file_hash(base/'engine/engine.exe')
    for path in sorted((base/'data/validation').glob('*/report.json'),reverse=True):
        try:
            report=json.loads(path.read_text(encoding='utf8'))
            if report.get('status')=='passed' and report.get('engineSHA256')==engine and report.get('sources')==source_hashes(base) and report.get('checks') and all(c.get('status')=='passed' and file_hash(path.parent/c['log'])==c['sha256'] for c in report['checks']):
                return {'status':'passed','report':path.relative_to(base).as_posix(),'engineSHA256':engine,'createdAt':report['createdAt'],'scope':report.get('scope',[])}
        except (OSError,ValueError,KeyError):continue
    return {'status':'not-recorded','engineSHA256':engine}

def paired_interval(values,samples=10000,seed=23542):
    if not values:return None
    mean=statistics.fmean(values)
    if len(values)<2:return {'meanDifference':mean,'ci95':None,'pairs':len(values)}
    rng=random.Random(seed);n=len(values)
    draws=sorted(statistics.fmean(values[rng.randrange(n)] for _ in range(n)) for _ in range(samples))
    def quantile(q):
        pos=q*(samples-1);i=int(pos);return draws[i]+(draws[min(i+1,samples-1)]-draws[i])*(pos-i)
    return {'meanDifference':mean,'ci95':[quantile(.025),quantile(.975)],'pairs':n}

def measurements(summary):
    s=summary.get('measurement',summary);received=s.get('received',0)
    flows=summary.get('flows',[])
    jitter=s.get('jitterMs')
    if jitter is None and received and 'measurement' not in summary:
        observed=[f for f in flows if f.get('received',0)>1]
        if observed:jitter=sum(f['jitterMs']*f['received'] for f in observed)/sum(f['received'] for f in observed)
    recovery=[f for e in summary.get('recovery',[]) if not e.get('restore') for f in e.get('flows',[]) if f.get('affected')]
    return {'deliveryRate':s.get('deliveryRate'),'throughputMbps':s.get('throughputMbps'),
            'meanDelayMs':s.get('meanDelayMs') if received else None,'jitterMs':jitter if received else None,
            'unrecoveredRate':100*sum(f.get('recoverySeconds') is None for f in recovery)/len(recovery) if recovery else None,
            'controlWireBytes':summary.get('control',{}).get('wireBytes'),
            'edgeChecks':summary.get('operations',{}).get('edgeChecks'),'labelExpand':summary.get('operations',{}).get('labelExpand')}

def aggregate(records,algorithms,seeds):
    from comparison import environment
    if len(set(seeds))!=len(seeds):raise ValueError('Duplicate research seeds')
    by={}
    for row in records:
        key=(row['seed'],row['algorithm'])
        if key in by:raise ValueError('Duplicate research run')
        by[key]=row
    expected={(s,a) for s in seeds for a in algorithms}
    if set(by)!=expected:raise ValueError('Incomplete paired experiment')
    for seed in seeds:
        rows=[by[seed,a] for a in algorithms];ref=rows[0]
        for row in rows:
            s=row['summary'];c=row['config']
            if s.get('metricVersion')!=4 or s.get('processingMode')!='neutral':raise ValueError('Research comparison requires neutral v4 metrics')
            if not s.get('trafficHash') or s['trafficHash']!=ref['summary'].get('trafficHash') or environment(c)!=environment(ref['config']):raise ValueError('Paired inputs do not match')
    changing={'seed','failureSeed','physicalEvents','physicalEventsHash'}
    profiles=[{k:v for k,v in environment(r['config']).items() if k not in changing} for r in records]
    if any(p!=profiles[0] for p in profiles):raise ValueError('Experiment profile changed between seeds')
    base=algorithms[0];comparisons=[]
    for algorithm in algorithms[1:]:
        metrics=[]
        for metric in METRICS:
            differences=[];missing=[];left=[];right=[]
            for seed in seeds:
                a=measurements(by[seed,base]['summary'])[metric];b=measurements(by[seed,algorithm]['summary'])[metric]
                if a is None or b is None:missing.append(seed)
                else:left.append(a);right.append(b);differences.append(b-a)
            # Never silently drop failed/no-reception replications from an inferential result.
            interval=paired_interval(differences) if not missing else None
            metrics.append({'name':metric,'baselineMean':statistics.fmean(left) if left else None,'comparisonMean':statistics.fmean(right) if right else None,'statistics':interval,'missingSeeds':missing,'observedPairs':len(differences),'status':'missing-observations' if missing else 'available'})
        comparisons.append({'baseline':base,'algorithm':algorithm,'metrics':metrics})
    return {'schema':1,'method':'paired-percentile-bootstrap','bootstrapSamples':10000,'statisticsSeed':23542,'confidence':.95,'unit':'independent-seed','stage':'final' if len(seeds)>=30 else 'pilot' if len(seeds)>=10 else 'exploratory','seeds':seeds,'algorithms':algorithms,'completedRuns':len(records),'comparisons':comparisons,'winner':None,'comparisonScope':'packet-system' if records[0]['config'].get('controlMode')=='packet' else 'routing-time-model','measurement':{key:records[0]['config'].get(key,0) for key in ('warmupSeconds','drainSeconds')}}
