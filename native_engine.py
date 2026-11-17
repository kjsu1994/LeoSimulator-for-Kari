# SPDX-License-Identifier: GPL-3.0-only
"""One INI interpretation for the engine, editor, and saved replay."""
import ast
import functools
import math
import operator
import re

TICK = 1e-11
ALGORITHMS = {'dijkstra-delay':'Dijkstra · 지연 최소', 'dijkstra-hop':'Dijkstra · 홉 최소',
              'mcsr-ls':'MCSR · LS', 'mcsr-ts':'MCSR · TS', 'mcsr-hr':'MCSR · HR'}

@functools.lru_cache(maxsize=4096)
def pattern(key):
    out=''; i=0
    while i<len(key):
        if key.startswith('**.',i): out+='(?:.*\\.)?'; i+=3
        elif key.startswith('**',i): out+='.*'; i+=2
        elif key[i]=='*': out+='[^.]*'; i+=1
        elif key[i]=='[':
            end=key.index(']',i); part=key[i+1:end]
            if part=='*': out+=r'\[\d+\]'
            elif '..' in part:
                a,b=map(int,part.split('..'))
                if b<a or b-a>20000: raise ValueError('Invalid INI index range')
                out+=r'\[(?:'+'|'.join(map(str,range(a,b+1)))+r')\]'
            else: out+=re.escape(key[i:end+1])
            i=end+1
        else: out+=re.escape(key[i]); i+=1
    return re.compile(out)

def resolve(entries,path,default=None):
    for entry in reversed(entries):
        if pattern(entry['k']).fullmatch(path): return entry['v'].strip().strip('"')
    return default

UNITS={'s':1,'ms':1e-3,'us':1e-6,'ns':1e-9,'ps':1e-12,'min':60,'h':3600,'d':86400,
       'B':1,'kB':1000,'MB':1e6,'bps':1,'kbps':1e3,'Mbps':1e6,'Gbps':1e9,'km':1,'mps':1,'deg':1}
OPS={ast.Add:operator.add,ast.Sub:operator.sub,ast.Mult:operator.mul,ast.Div:operator.truediv,
     ast.USub:operator.neg,ast.UAdd:operator.pos}

def number(raw,variables=None):
    variables=variables if variables is not None else {}
    def variable(match):
        parts=match[1].split('=',1); name=parts[0].strip()
        if name not in variables:
            if len(parts)==1: raise ValueError('Undefined INI variable: '+name)
            variables[name]=parts[1].split(',')[0].strip()
        return '('+str(variables[name])+')'
    raw=re.sub(r'\$\{([^}]+)\}',variable,str(raw))
    raw=re.sub(r'(?<=[0-9)])\s*(Gbps|Mbps|kbps|bps|mps|deg|min|km|ms|us|ns|ps|MB|kB|B|s|h|d)\b',lambda m:'*'+str(UNITS[m[1]]),raw)
    def calc(node):
        if isinstance(node,ast.Constant) and type(node.value) in (int,float): return node.value
        if isinstance(node,ast.BinOp) and type(node.op) in OPS: return OPS[type(node.op)](calc(node.left),calc(node.right))
        if isinstance(node,ast.UnaryOp) and type(node.op) in OPS: return OPS[type(node.op)](calc(node.operand))
        raise ValueError('Unsupported INI expression: '+str(raw))
    try: value=float(calc(ast.parse(raw.strip(),mode='eval').body))
    except (SyntaxError,ZeroDivisionError,OverflowError) as e: raise ValueError('Invalid INI expression: '+str(raw)) from e
    if not math.isfinite(value): raise ValueError('Nonfinite INI value')
    return value

# key, caption, INI key, kind, default, min, max; the editor uses this same contract.
FIELDS=[
 ('numSatellites','위성 수','*.numSatellites','int',132,6,20000),
 ('planes','궤도면 수','*.**.planeCount','int',12,2,20000),
 ('altitude','고도 (km)','*.constellationCreator.altitude','number',550,220,2000),
 ('inclination','경사각 (°)','*.constellationCreator.inclination','number',53,0,179.999),
 ('simTimeLimit','실행 시간 (s)','sim-time-limit','number',600,TICK,1000000),
 ('seed','시드','seed-0-mt','int',23542,0,4294967295),
 ('islDatarate','ISL 전송률 (bps)','*.topologyControl.islDatarate','number',100000000,1,1e12),
 ('groundlinkDatarate','GSL 전송률 (bps)','*.topologyControl.groundlinkDatarate','number',50000000,1,1e12),
 ('minimumElevation','최소 고도각 (°)','*.topologyControl.minimumElevation','number',10,-90,90),
 ('queueCapacity','실제 큐 용량 (패킷, 0=무제한)','*.satellite[*].net.packetHandler.queueCapacity','int',500,0,1000000),
 ('updateInterval','위치·연결 갱신 (s)','*.topologyControl.updateInterval','number',1,TICK,3600),
 ('labelCap','라벨 상한','*.satellite[*].net.routing.labelCap','int',32,4,1024),
 ('routingQueueCapacity','가용 버퍼 계산 기준','*.satellite[*].net.routing.queueCapacity','int',100,1,1000000),
 ('hrJitterMs','HR 지터 상한 (ms)','*.satellite[*].net.routing.hrJitterMs','number',15,0,100000),
 ('tsDelayBoundMs','TS 지연 상한 (ms, -1=없음)','*.satellite[*].net.routing.tsDelayBoundMs','number',-1,-1,1000000),
 ('jitterWindow','지터 유효 시간 (s)','*.satellite[*].net.routing.jitterWindow','number',30,0,1000000),
 ('refreshInterval','MCSR 상태 갱신 (s)','*.satellite[*].net.routing.refreshInterval','number',0,0,3600),
 ('reserveDelay','예비 경로 전환 (s)','*.satellite[*].net.routing.reserveDelay','number',.001,0,3600),
 ('repairDelay','재계산 지연 (s)','*.satellite[*].net.routing.repairDelay','number',.005,0,3600),
 ('failureScopeHops','장애 정보 전파 범위 (홉)','*.satellite[*].net.routing.failureScopeHops','int',6,0,20000),
 ('flowTimeout','패킷 경로 보관 (s)','*.satellite[*].net.routing.flowTimeout','number',120,TICK,1000000),
 ('csibPeriod','CSIB 주기 (s)','*.satellite[*].net.routing.csibPeriod','number',180,TICK,1000000),
 ('csibMonitorInterval','CSIB 속성 감시 (s)','*.satellite[*].net.routing.csibMonitorInterval','number',0,0,1000000),
 ('csibComputeDelay','CSIB 계산 지연 (s)','*.satellite[*].net.routing.csibComputeDelay','number',0,0,3600),
 ('csibReportProcDelay','보고 처리 지연 (s)','*.satellite[*].net.routing.csibReportProcDelay','number',0,0,3600),
 ('csibHopProcDelay','CSIB 홉 처리 지연 (s)','*.satellite[*].net.routing.csibHopProcDelay','number',0,0,3600),
 ('csibMessageBytes','CSIB 크기 (B)','*.satellite[*].net.routing.csibMessageBytes','int',0,0,10000000),
 ('csibDetectDelay','CSIB 장애 감지 (s, -1=자동)','*.satellite[*].net.routing.csibDetectDelay','number',-1,-1,3600),
 ('slsInterval','SLS 간격 (s)','*.topologyControl.slsInterval','number',.2,TICK,3600),
 ('slsMissCount','SLS 누락 횟수','*.topologyControl.slsMissCount','int',3,1,10000),
 ('floodHopDelay','장애 정보 홉 지연 (s)','*.topologyControl.floodHopDelay','number',.002,0,3600),
 ('spfDelay','Dijkstra 재계산 지연 (s)','*.satellite[*].net.routing.spfDelay','number',.05,0,3600),
 ('srdK','실험 SRD K (0=자동)','*.satellite[*].net.routing.srdK','int',0,0,20000),
 ('srdL','실험 SRD L (0=자동)','*.satellite[*].net.routing.srdL','int',0,0,20000),
]
BOOL_FIELDS={'reservedPaths':False,'snapshotOnDelayUpdate':True,'csibEnabled':False,'csibVote':True,'congestionAware':False}
ENUM_FIELDS={'jitterSource':('arrival','delayChange'),'floodModel':('paper','consistent')}

def capabilities():
    return {'algorithms':ALGORITHMS,'fields':[dict(zip(('key','label','ini','type','default','min','max'),f)) for f in FIELDS],
            'booleans':BOOL_FIELDS,'choices':ENUM_FIELDS,'tickSeconds':TICK}

def configuration(cfg):
    c=dict(cfg);entries=cfg['effectiveEntries'];variables=dict(cfg.get('variables') or {})
    # Definitions may precede their use in an earlier INI key.
    for entry in entries:
        for name,values in re.findall(r'\$\{(\w+)=([^}]+)\}',entry['v']): variables.setdefault(name,values.split(',')[0].strip())
    get=lambda path,default=None:resolve(entries,path,default)
    def num(path,default):
        try: return number(get(path,default),variables)
        except ValueError as e: raise ValueError(path+': '+str(e)) from e
    def boolean(path,default=False):
        raw=get(path,str(default).lower())
        if raw not in ('true','false'): raise ValueError('Expected boolean: '+path)
        return raw=='true'
    def string(path,default): return get(path,default)
    for key,label,ini,kind,default,low,high in FIELDS:
        value=num(ini.replace('*','LeoDijkstra',1).replace('LeoDijkstra.**.','LeoDijkstra.constellationCreator.').replace('satellite[*]','satellite[0]'),default)
        if value<low or value>high or (kind=='int' and value!=int(value)): raise ValueError(f'{label}: {low} ~ {high}')
        c[key]=int(value) if kind=='int' else value
    base='LeoDijkstra.constellationCreator.';tc='LeoDijkstra.topologyControl.';routing='LeoDijkstra.satellite[0].net.routing.'
    for key,default in [('phasing',1),('raanSpread',360),('baseYear',20),('baseDay',1.5),('epochYear',20),('epochDay',1.5),('eccentricity',.000001)]:
        c[key]=num(base+('interPlaneSpacing' if key=='phasing' else key),default)
    c['bstar']=num('LeoDijkstra.satellite[0].NoradModule.bstar',.00042243)
    c['islDelay']=num(tc+'islDelay',.0000033356);c['groundlinkDelay']=num(tc+'groundlinkDelay',.0000033356)
    c['maxGroundLinks']=int(num(tc+'numGroundLinks',max(40,math.ceil(c['numSatellites']/8))))
    c['maxRoutes']=int(num('LeoDijkstra.mapDataSource.maxRoutes',8));c['routeLifetime']=num('LeoDijkstra.mapDataSource.routeLifetime',30)
    if c['maxRoutes']<0 or c['routeLifetime']<=0:raise ValueError('Invalid route display settings')
    c['maxHops']=int(num('LeoDijkstra.station[0].traffic.maxHops',64))
    c['intraDisabled']=boolean(tc+'intraPlaneIslDisabled');c['interDisabled']=boolean(tc+'interPlaneIslDisabled')
    c['numTerminals']=int(num('LeoDijkstra.numTerminals',0))
    c['routingType']=string('LeoDijkstra.satellite[0].net.routingType','Sat_Net_DijkstraRouting')
    c['metric']=string(routing+'metric','delay');service=string(routing+'service','LS').lower()
    default_algorithm=('mcsr-'+service) if c['routingType']=='Sat_Net_McsrRouting' else 'dijkstra-'+c['metric']
    c['algorithm']=string(routing+'algorithm',default_algorithm)
    if c['algorithm'] not in ALGORITHMS or c['routingType'] not in ('Sat_Net_McsrRouting','Sat_Net_DijkstraRouting'):raise ValueError('Unsupported routing algorithm')
    if string('network','Florasat.Dijkstra132.LeoDijkstra')!='Florasat.Dijkstra132.LeoDijkstra' or not boolean(base+'enabled',True) or string(tc+'walkerType','DELTA')!='DELTA':raise ValueError('Unsupported network/constellation')
    if any(c[k]!=int(c[k]) or not 0<=c[k]<=99 for k in ('baseYear','epochYear')) or any(not 1<=c[k]<367 for k in ('baseDay','epochDay')):raise ValueError('Invalid epoch')
    if c['numTerminals'] or boolean('LeoDijkstra.allowUnevenPlanes') or c['raanSpread']!=360 or c['eccentricity']!=.000001:raise ValueError('Unsupported Walker/terminal model')
    if c['numSatellites']%c['planes'] or c['numSatellites']//c['planes']<3 or not 0<=c['phasing']<c['planes']:raise ValueError('Invalid Walker grid')
    if num('simtime-resolution',-11)!=-11:raise ValueError('Only simtime-resolution=-11 is supported')
    if c['islDelay']<=0 or c['groundlinkDelay']<=0 or c['maxGroundLinks']<1 or c['maxHops']<0:raise ValueError('Invalid link/packet limits')
    for key,default in BOOL_FIELDS.items():c[key]=boolean(routing+key,default)
    for key,choices in ENUM_FIELDS.items():
        c[key]=string(routing+key,choices[0])
        if c[key] not in choices:raise ValueError('Unknown '+key)
    if bool(c['srdK'])!=bool(c['srdL']) or c['srdK']>c['planes'] or c['srdL']>c['numSatellites']//c['planes']:raise ValueError('Invalid SRD size')
    c['experimental']=bool(c['srdK'] or c['floodModel']!='paper')
    allowed_routing={f[2].rsplit('.',1)[-1] for f in FIELDS}|set(BOOL_FIELDS)|set(ENUM_FIELDS)|{'algorithm','metric','service','dstStation','visualizerModule','pathTraceFile'}
    for entry in entries:
        if '.net.routing.' in entry['k'] and entry['k'].rsplit('.',1)[1] not in allowed_routing and 'recording' not in entry['k']:raise ValueError('Unsupported routing key: '+entry['k'])
    count=int(num('LeoDijkstra.nrOfGS',13))
    if not 1<=count<=5000:raise ValueError('Invalid ground node count')
    c['stations']=[]
    for i in range(count):
        p=f'LeoDijkstra.station[{i}].';m=p+'mobility.';t=p+'traffic.'
        st={'id':i,'name':string(p+'label',f'GS{i}'),'mobile':string(p+'nodeType','station')=='mobile',
            'lat':num(m+'latitude',0),'lon':num(m+'longitude',0),'alt':num(m+'altitudeKm',0),
            'speed':num(m+'speed',0),'start':num(m+'routeStart',0),'enabled':boolean(t+'enabled'),
            'dstType':string(t+'dstType','station'),'dstStation':int(num(t+'dstStation',-1)),
            'sendInterval':num(t+'sendInterval',5),'packetSize':num(t+'packetSize',1000),'endTime':num(t+'endTime',c['simTimeLimit']),'gapThreshold':num(t+'gapThreshold',.1)}
        if st['mobile']:
            route=string(m+'route','')
            if route:st['route']=[[float(x) for x in pair.split(',')] for pair in route.split()]
        start=get(t+'startTime')
        if start is not None:
            uniform=re.fullmatch(r'uniform\((.+),(.+)\)',start)
            if uniform:st['startRange']=[number(x,variables) for x in uniform.groups()]
            else:st['startTime']=number(start,variables)
        if st['sendInterval']<TICK or st['packetSize']<1 or st['packetSize']!=int(st['packetSize']) or st['dstStation']>=count or (st['enabled'] and st['dstStation']==i) or st['dstType'] not in ('station','mobile','any'):raise ValueError('Invalid traffic at station '+str(i))
        if abs(st['lat'])>90 or abs(st['lon'])>180 or st['speed']<0 or not 0<=st['start']<=1 or st.get('startTime',0)<0 or st['endTime']<0:raise ValueError('Invalid station coordinates/time')
        if 'route' in st and (len(st['route'])<2 or any(len(x)!=2 or abs(x[0])>90 or abs(x[1])>180 for x in st['route'])):raise ValueError('Invalid mobile route')
        if st['dstStation'] < -1 or st['gapThreshold']<0 or ('startRange' in st and not 0<=st['startRange'][0]<=st['startRange'][1]):raise ValueError('Invalid traffic window')
        c['stations'].append(st)
    c['failureKnowledge']=string(tc+'failureKnowledge','instant')
    if c['failureKnowledge'] not in ('instant','propagate'):raise ValueError('Unknown failure knowledge model')
    c['failureSchedule']=string('LeoDijkstra.islFailures.schedule','')
    for key,default in [('randomFailures',0),('randomStart',0),('randomEnd',0),('randomDuration',0)]:c[key]=num('LeoDijkstra.islFailures.'+key,default)
    if c['randomFailures']<0 or int(c['randomFailures'])!=c['randomFailures'] or c['randomStart']<0 or c['randomEnd']<c['randomStart'] or c['randomDuration']<0:raise ValueError('Invalid random failure settings')
    c['failureSeed']=int(num('seed-1-mt',4242))
    if not 0<=c['failureSeed']<=4294967295:raise ValueError('Invalid failure seed')
    c['failureReplay']=string('LeoDijkstra.islFailures.replayFile','')
    c['congestionZones']=string('LeoDijkstra.congestion.zones','')
    c['congestionRefresh']=num('LeoDijkstra.congestion.refreshInterval',5)
    c['congestionBuffer']=int(num('LeoDijkstra.congestion.queueCapacity',500))
    c['congestionPacketBytes']=int(num('LeoDijkstra.congestion.packetBytes',1380))
    if c['congestionRefresh']<0 or c['congestionBuffer']<1 or c['congestionPacketBytes']<1:raise ValueError('Invalid congestion model')
    c['congestionMode']=string('LeoDijkstra.native.congestionMode','environment' if c['congestionZones'] else 'actual')
    if c['congestionMode'] not in ('actual','environment'):raise ValueError('Unknown congestion mode')
    c['scenarioFile']=string('LeoDijkstra.congestion.scenarioFile','')
    c['routeRecording']=string('LeoDijkstra.native.routeRecording','changes' if c['numSatellites']>=1000 else 'full')
    if c['routeRecording'] not in ('changes','full'):raise ValueError('Unknown recording mode')
    c['packetTrace']=boolean('LeoDijkstra.native.packetTrace')
    c['variables']=variables;c['capabilities']=capabilities();c['normalizedVersion']=2
    c['sweeps']={name:[number(x) for x in values.split(',')] for entry in entries for name,values in re.findall(r'\$\{(\w+)=([^}]+)\}',entry['v'])}
    return c
