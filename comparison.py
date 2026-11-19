# SPDX-License-Identifier: GPL-3.0-only
from pathlib import Path
import hashlib,json,re

ENV_KEYS=('warmupSeconds','drainSeconds','numSatellites','planes','phasing','altitude','inclination','raanSpread','baseYear','baseDay','epochYear','epochDay','bstar','eccentricity','simTimeLimit','seed','stations','islDatarate','groundlinkDatarate','islDelay','groundlinkDelay','minimumElevation','updateInterval','queueCapacity','intraDisabled','interDisabled','failureSchedule','randomFailures','randomStart','randomEnd','randomDuration','failureSeed','failureReplayEvents','congestionMode','congestionZones','congestionRefresh','congestionBuffer','congestionPacketBytes','scenario','maxHops','maxGroundLinks','failureKnowledge','slsInterval','slsMissCount','floodHopDelay','failureScopeHops','processingMode','controlMode','controlRefresh','controlTimeout','controlHeaderBytes','controlRecordBytes','jitterModel','islMode','scheduledEvents','physicalEvents','physicalEventsHash','modelVersion','metricVersion','commonKernelHash','commonSrd','commonSrdHash','controlMtu','controlReassemblyTimeout','controlResyncInterval','controlProtocolVersion')

def environment(config):
    env={key:config.get(key) for key in ENV_KEYS}
    if isinstance(env.get('scenario'),dict):
        env['scenario']=dict(env['scenario']);baseline=env['scenario'].get('baseline')
        if baseline:env['scenario']['baseline']={'route':baseline.get('route')}
    return env

def fingerprint(config):return hashlib.sha256(json.dumps(environment(config),sort_keys=True,ensure_ascii=False,separators=(',',':')).encode()).hexdigest()

def folder(base,run_id):
    if not re.fullmatch(r'[A-Za-z0-9-]{8,80}',run_id):raise ValueError('Invalid run ID')
    return Path(base)/'data/runs'/run_id

def runs(base):
    out=[]
    for p in sorted((Path(base)/'data/runs').glob('*/manifest.json'),key=lambda p:p.stat().st_mtime,reverse=True):
        try:
            m=json.loads(p.read_text(encoding='utf-8'));s=p.parent/'summary.json'
            if m.get('format') in ('leo-native-replay-v2','leo-native-replay-v3','leo-native-replay-v4') and s.is_file():out.append({'runId':m['runId'],'configName':m['configName'],'createdAt':m['createdAt'],'summary':json.loads(s.read_text())})
        except (OSError,ValueError,KeyError):continue
    return out

def run_metadata(base):
    formats = {'florasat-csv-ini-v1', 'leo-native-replay-v2', 'leo-native-replay-v3', 'leo-native-replay-v4'}
    results = []
    for path in (Path(base)/'data/runs').glob('*/manifest.json'):
        try:
            manifest = json.loads(path.read_text(encoding='utf-8'))
            run_id = manifest['runId']
            if manifest['format'] not in formats or folder(base, run_id) != path.parent:
                continue
            algorithm = (manifest.get('algorithmSettings') or {}).get('algorithm')
            if not algorithm and manifest['format'].startswith('leo-native-'):
                summary = path.parent/'summary.json'
                if summary.is_file():
                    algorithm = json.loads(summary.read_text(encoding='utf-8')).get('algorithm')
            results.append({'runId':run_id, 'configName':manifest['configName'],
                            'createdAt':str(manifest.get('createdAt') or ''), 'algorithm':algorithm})
        except (OSError, ValueError, TypeError, AttributeError, KeyError):
            continue
    return sorted(results, key=lambda row: (row['createdAt'], row['runId']), reverse=True)


def compare_bundles(a,b):
    def load(bundle):
        m=bundle['manifest'];f=bundle['files']
        if m.get('format') not in ('leo-native-replay-v2','leo-native-replay-v3','leo-native-replay-v4'):raise ValueError('계측이 포함된 버전 2 결과가 필요합니다.')
        return json.loads(f[m['normalizedPath']]),json.loads(f['summary.json'])
    ca,sa=load(a);cb,sb=load(b);ea,eb=environment(ca),environment(cb)
    differences=[k for k in ENV_KEYS if ea[k]!=eb[k]]
    physical=[k for k in differences if k not in ('scenario','congestionZones','congestionMode')]
    same_traffic=sa.get('trafficHash')==sb.get('trafficHash')
    metrics=('attempted','sent','received','dropped','deliveryRate','meanDelayMs','throughputMbps','replanned','reservedSwitches','peakMemoryBytes','elapsedSeconds')
    return {'comparable':a['manifest'].get('format')==b['manifest'].get('format')=='leo-native-replay-v4' and ca.get('processingMode')==cb.get('processingMode')=='neutral' and not differences and same_traffic,'trafficMatches':same_traffic,'differences':differences,
            'physicalDifferences':physical,'baseline':sa,'scenario':sb,
            'metrics':[{'name':k,'baseline':sa.get(k),'scenario':sb.get(k),'difference':sb[k]-sa[k] if isinstance(sa.get(k),(int,float)) and isinstance(sb.get(k),(int,float)) else None} for k in metrics]}

def compare(base,a,b):
    first=json.loads((folder(base,a)/'result.json').read_text(encoding='utf-8'))
    second=json.loads((folder(base,b)/'result.json').read_text(encoding='utf-8'))
    return compare_bundles(first['bundle'],second['bundle'])
