# SPDX-License-Identifier: GPL-3.0-only
# Modified/adapted 2026-11-16; see THIRD_PARTY_NOTICES.txt.
"""Local INI adapter for the native ground-node engine."""
import re
import math

def resolve(entries, path, default=None):
    for entry in reversed(entries):
        key=entry['k'];out='';i=0
        while i<len(key):
            if key.startswith('**.',i):out+='(?:.*\\.)?';i+=3
            elif key.startswith('**',i):out+='.*';i+=2
            elif key[i]=='*':out+='[^.]*';i+=1
            elif key[i]=='[':
                end=key.index(']',i);part=key[i+1:end]
                if part=='*':out+=r'\[\d+\]'
                elif '..' in part:
                    a,b=map(int,part.split('..'));out+=r'\[(?:'+'|'.join(map(str,range(a,b+1)))+r')\]'
                else:out+=re.escape(key[i:end+1])
                i=end+1
            else:out+=re.escape(key[i]);i+=1
        if re.fullmatch(out,path):return entry['v'].strip('"')
    return default

def configuration(cfg):
    e=cfg['effectiveEntries'];get=lambda p,d=None:resolve(e,p,d)
    def num(p,d=0):
        raw=get(p,str(d));m=re.fullmatch(r'([-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:e[-+]?\d+)?)\s*([A-Za-z]*)',raw,re.I)
        if not m:raise ValueError('Unsupported INI expression: '+p+' = '+raw)
        units={'':1,'s':1,'ms':.001,'min':60,'h':3600,'B':1,'bps':1,'mps':1}
        allowed = ('','s','ms','min','h') if p.endswith(('sendInterval','startTime','updateInterval')) or p=='sim-time-limit' else ('','B') if p.endswith('packetSize') else ('','bps') if p.endswith('Datarate') else ('',)
        if m[2] not in allowed:raise ValueError('Unsupported INI unit: '+raw)
        v=float(m[1])*units[m[2]]
        if not math.isfinite(v):raise ValueError('Nonfinite INI value')
        return v
    def boolean(p,d=False):
        v=get(p,str(d).lower())
        if v not in ('true','false'):raise ValueError('Unsupported boolean '+p)
        return v=='true'
    if cfg['numTerminals'] or cfg['routingType']!='Sat_Net_DijkstraRouting':raise ValueError('Only Dijkstra ground-node models are supported')
    c={**cfg};base='LeoDijkstra.constellationCreator.';tc='LeoDijkstra.topologyControl.'
    for key in ['altitude','inclination','baseYear','baseDay','raanSpread']:
        c[key]=num(base+key,cfg[key])
    for key,setting in [('planes','planeCount'),('phasing','interPlaneSpacing')]:c[key]=int(num(base+setting,cfg[key]))
    c['numSatellites']=int(num('LeoDijkstra.numSatellites',cfg['numSatellites']))
    c['metric']=get('LeoDijkstra.satellite[0].net.routing.metric',cfg['metric'])
    if c['metric']!=cfg['metric'] or c['planes']!=cfg['planes'] or c['numSatellites']!=cfg['numSatellites']:raise ValueError('Ambiguous aliases: use explicit constellation/routing keys')
    for k,d in [('epochYear',20),('epochDay',1.5),('eccentricity',.000001)]:c[k]=num(base+k,d)
    for k,d in [('updateInterval',1),('islDelay',.0000033356),('groundlinkDelay',.0000033356),('islDatarate',100000000),('groundlinkDatarate',50000000),('maxGroundLinks',40)]:c[k]=num(tc+('numGroundLinks' if k=='maxGroundLinks' else k),d)
    c['bstar']=num('LeoDijkstra.satellite[0].NoradModule.bstar',.00042243)
    c['intraDisabled']=boolean(tc+'intraPlaneIslDisabled');c['interDisabled']=boolean(tc+'interPlaneIslDisabled')
    if num('simtime-resolution',-11)!=-11:raise ValueError('Only simtime-resolution=-11 is supported')
    c['seed']=int(num('seed-0-mt',23542));c['maxHops']=int(num('LeoDijkstra.station[0].traffic.maxHops',64))
    c['algorithm']=get('LeoDijkstra.satellite[0].net.routing.algorithm','dijkstra-'+cfg['metric'])
    if c['planes']<=1 or not (220<=c['altitude']<=2000 and 0<=c['inclination']<180 and c['eccentricity']==.000001 and 0<=c['phasing']<c['planes'] and c['numSatellites']//c['planes']>=3) or boolean('LeoDijkstra.allowUnevenPlanes'):
        raise ValueError('Unsupported replay geometry')
    for key in ['LeoDijkstra.numSatellites',base+'planeCount',base+'interPlaneSpacing','seed-0-mt']:
        value=num(key,0)
        if value!=int(value):raise ValueError('Expected integer: '+key)
    if not (isinstance(cfg['planes'],int) and 1<cfg['planes']<=cfg['numSatellites']<=5000) or cfg['simTimeLimit']<=0 or cfg['simTimeLimit']>86400 or cfg['minimumElevation'] < -90 or cfg['minimumElevation']>90 or c['updateInterval']<=0 or c['islDatarate']<=0 or c['groundlinkDatarate']<=0:
        raise ValueError('Invalid geometry, time or link rate')
    if cfg['metric'] not in ('delay','hop') or cfg['raanSpread']!=360 or cfg['numSatellites']%cfg['planes']:raise ValueError('Unsupported routing/Walker geometry')
    count=int(num('LeoDijkstra.nrOfGS',len(c['stations'])))
    c['stations']=[dict(st) for st in c['stations'] if st['id']<count]
    if count<1 or [st['id'] for st in c['stations']]!=list(range(count)):raise ValueError('Ground node settings are incomplete')
    for st in c['stations']:
        prefix=f"LeoDijkstra.station[{st['id']}].traffic."
        st['enabled']=boolean(prefix+'enabled');st['dstType']=get(prefix+'dstType','station');st['dstStation']=int(num(prefix+'dstStation',-1));st['sendInterval']=num(prefix+'sendInterval',5);st['packetSize']=num(prefix+'packetSize',1000)
        start=get(prefix+'startTime')
        if start is not None:st['startTime']=num(prefix+'startTime')
        if st['dstType'] not in ('station','mobile','any'):raise ValueError('Unknown destination node type')
        if st['sendInterval']<=0 or st['packetSize']<=0 or st['dstStation']>=len(c['stations']):raise ValueError('Invalid traffic parameters')
    return c
