# SPDX-License-Identifier: GPL-3.0-only
"""Paired run-level statistics and version-bound local verification evidence."""
from pathlib import Path
from datetime import datetime, timezone
import hashlib, json, math, platform, random, statistics

METRICS=('deliveryRate','throughputMbps','meanDelayMs','jitterMs','unrecoveredRate','stableUnrecoveredRate','controlWireBytes','edgeChecks','labelExpand')

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
    base=Path(base)
    engine=file_hash(base/'engine/engine.exe')
    verification=base/'data/validation'
    result={'status':'not-recorded','engineSHA256':engine,'scope':[],'workloads':[]}
    sources=None

    def matching(path):
        nonlocal sources
        report=json.loads(path.read_text(encoding='utf8'))
        if report.get('status')!='passed' or report.get('engineSHA256')!=engine:return None
        if sources is None:sources=source_hashes(base)
        return report if report.get('sources')==sources else None

    for path in sorted(verification.glob('*/report.json'),reverse=True):
        try:
            report=matching(path)
            if report and report.get('checks') and all(c.get('status')=='passed' and file_hash(path.parent/c['log'])==c['sha256'] for c in report['checks']):
                result.update(status='passed',report=path.relative_to(base).as_posix(),createdAt=report['createdAt'],scope=report.get('scope',[]))
                break
        except (OSError,ValueError,KeyError):continue
    for path in sorted(verification.glob('*/workloads.json'),reverse=True):
        try:
            report=matching(path)
            if not report or not report.get('verified') or len(report['verified'])!=report['runs']:continue
            scope=report['scope']
            if scope in {item['scope'] for item in result['workloads']}:continue
            for item in report['verified']:
                check=path.parent/item['path']
                if file_hash(check)!=item['sha256']:raise ValueError('Workload record changed')
                case=json.loads(check.read_text(encoding='utf8'))
                outputs=case.get('outputs',{})
                if not outputs or any(file_hash(check.parent/name)!=sha for name,sha in outputs.items()):
                    raise ValueError('Workload output changed')
            result['workloads'].append({**{key:report[key] for key in ('scope','satellites','simSeconds','runs')},
                                        'report':path.relative_to(base).as_posix()})
        except (OSError,ValueError,KeyError):continue
    return result

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
            'unrecoveredRate':100*sum(f.get('firstPacketRecoverySeconds',f.get('recoverySeconds')) is None for f in recovery)/len(recovery) if recovery else None,
            'stableUnrecoveredRate':100*sum(f['stableRecoverySeconds'] is None for f in recovery)/len(recovery) if recovery and all('stableRecoverySeconds' in f for f in recovery) else None,
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
    inputs=[]
    for seed in seeds:
        rows=[by[seed,a] for a in algorithms]
        cfg=rows[0]['config']
        inputs.append({'seed':seed,'trafficHash':rows[0]['summary']['trafficHash'],
                       'physicalEventsHash':digest(cfg.get('physicalEvents',[])),
                       'linkRandomDraws':{row['algorithm']:row['summary'].get('linkRandomDraws') for row in rows}})
    base=algorithms[0];comparisons=[]
    for algorithm in algorithms[1:]:
        link_draws=[(row['linkRandomDraws'][base],row['linkRandomDraws'][algorithm]) for row in inputs]
        link_used=sum(any(n for n in pair) for pair in link_draws)
        unique=len({(row['trafficHash'],row['physicalEventsHash'],row['seed'] if any(draws) else None)
                    for row,draws in zip(inputs,link_draws)})
        known=all(None not in pair for pair in link_draws)
        interval_allowed=known and unique==len(seeds) and unique>1
        metrics=[]
        for metric in METRICS:
            differences=[];missing=[];left=[];right=[]
            for seed in seeds:
                a=measurements(by[seed,base]['summary'])[metric];b=measurements(by[seed,algorithm]['summary'])[metric]
                if a is None or b is None:missing.append(seed)
                else:left.append(a);right.append(b);differences.append(b-a)
            # Never silently drop failed/no-reception replications from an inferential result.
            interval=None
            if not missing and differences:
                interval=paired_interval(differences) if interval_allowed else {
                    'meanDifference':statistics.fmean(differences),'pairs':len(differences),'ci95':None}
            metrics.append({'name':metric,'baselineMean':statistics.fmean(left) if left else None,'comparisonMean':statistics.fmean(right) if right else None,'statistics':interval,'missingSeeds':missing,'observedPairs':len(differences),'status':'missing-observations' if missing else 'available'})
        comparisons.append({'baseline':base,'algorithm':algorithm,'metrics':metrics,'uniqueInputs':unique,
                            'intervalAvailable':interval_allowed,'inputClassification':
                            'unknown' if not known else 'fixed-input' if unique==1 else 'distinct-inputs' if interval_allowed else 'repeated-inputs',
                            'seedsUsingLinkRandomness':link_used})
    return {'schema':2,'method':'paired-percentile-bootstrap','bootstrapSamples':10000,'statisticsSeed':23542,'confidence':.95,'unit':'paired-seed','inputVariability':{'trafficProfiles':len({v['trafficHash'] for v in inputs}),'physicalEventProfiles':len({v['physicalEventsHash'] for v in inputs}),'perSeed':inputs},'stage':'final' if len(seeds)>=30 else 'pilot' if len(seeds)>=10 else 'exploratory','seeds':seeds,'algorithms':algorithms,'completedRuns':len(records),'comparisons':comparisons,'winner':None,'comparisonScope':'packet-system' if records[0]['config'].get('controlMode')=='packet' else 'routing-time-model','measurement':{key:records[0]['config'].get(key,0) for key in ('warmupSeconds','drainSeconds')}}
