# SPDX-License-Identifier: GPL-3.0-only
"""Independent exhaustive oracle: does not import product graph/selection helpers."""
from pathlib import Path
import json,math,random,subprocess
ROOT=Path(__file__).resolve().parent
EPS=1e-9

def srd_mapping(planes,per,k,l):
    def groups(n,size):
        count=math.ceil(n/size);lengths=[n//count+(i<n%count) for i in range(count)]
        return [i for i,length in enumerate(lengths) for _ in range(length)]
    q,x=groups(planes,k),groups(per,l);width=max(x)+1
    return [q[a]*width+x[b] for a in range(planes) for b in range(per)]

def enumerate_paths(case):
    mapping=srd_mapping(case['planes'],case['perPlane'],case['K'],case['L'])
    s,d=case['source'],case['destination'];same=mapping[s]==mapping[d];adj=[[] for _ in range(case['n'])]
    for u,v,delay,available,jitter in case['edges']:
        if not same or mapping[v]==mapping[s]:adj[u].append((v,delay,available,jitter))
    paths=[]
    def visit(u,path,delay,available,jitter):
        if u==d:paths.append({'path':path,'tuple':(delay,available,jitter)});return
        for v,dt,a,j in adj[u]:
            if v not in path:visit(v,path+[v],delay+dt,min(available,a),max(jitter,j))
    visit(s,[s],0,float('inf'),0)
    return paths

def same(a,b):return abs(a[0]-b[0])<=EPS and a[1:]==b[1:]
def dominates(a,b):return a[0]<=b[0]+EPS and a[1]>=b[1] and a[2]<=b[2] and (a[0]<b[0]-EPS or a[1]>b[1] or a[2]<b[2])
def select(paths,service,case):
    if not paths:return None
    if service=='LS':return min(paths,key=lambda p:(p['tuple'][0],-p['tuple'][1],p['tuple'][2]))
    if service=='TS':
        eligible=[p for p in paths if p['tuple'][0]<=case.get('tsDelayBoundMs',float('inf'))] or paths
        return min(eligible,key=lambda p:(-p['tuple'][1],p['tuple'][0]))
    eligible=[p for p in paths if p['tuple'][2]<case.get('hrJitterMs',15)]
    return min(eligible,key=lambda p:(-p['tuple'][1],p['tuple'][0])) if eligible else min(paths,key=lambda p:(p['tuple'][2],-p['tuple'][1],p['tuple'][0]))

def cases():
    result=[]
    for planes,per in [(4,3),(3,4)]:
        n=planes*per;neighbors=[set() for _ in range(n)]
        for u in range(n):
            q,x=divmod(u,per)
            for v in [q*per+(x+1)%per,((q+1)%planes)*per+((x+1)%per if q==planes-1 else x)]:neighbors[u].add(v);neighbors[v].add(u)
        for seed in range(8):
            rng=random.Random(seed);edges=[]
            for u in range(n):
                for v in sorted(neighbors[u]):
                    if v<u:continue
                    if seed==7 and (u==0 or v==0):continue
                    delay=1 if seed==0 else rng.randint(1,15);available=1 if seed==0 else rng.choice([.1,.3,.5,1.]);jitter=15 if seed==1 else rng.choice([0,14.999999999,15,15.000000001,30])
                    edges.extend([[u,v,delay,available,jitter],[v,u,delay,available,jitter]])
            for k,l in [(1,1),(2,2),(planes,per)]:
                for source,destination in [(0,1),(0,n-1)]:
                    result.append({'n':n,'planes':planes,'perPlane':per,'K':k,'L':l,'neighbors':[sorted(a) for a in neighbors],'edges':edges,'source':source,'destination':destination,'tsDelayBoundMs':0 if seed==2 else 12,'labelCap':100000})
    return result

def verify(executable=None):
    inputs=cases();exe=Path(executable) if executable else ROOT/'engine/oracle_driver.exe'
    run=subprocess.run([str(exe)],input=json.dumps(inputs),text=True,encoding='utf8',capture_output=True,check=True,timeout=120)
    outputs=json.loads(run.stdout);assert len(inputs)==len(outputs)
    checks=0;path_count=0
    for index,(case,out) in enumerate(zip(inputs,outputs)):
        assert out['srdOf']==srd_mapping(case['planes'],case['perPlane'],case['K'],case['L'])
        assert out['capHits']==0,('Reference limit reached',index)
        paths=enumerate_paths(case);path_count+=len(paths);front=[p for p in paths if not any(dominates(q['tuple'],p['tuple']) for q in paths)]
        for p in front:assert any(same(p['tuple'],tuple(q['tuple'])) for q in out['front']),('Missing frontier',index,p)
        for p in out['front']:
            assert len(p['path'])==len(set(p['path']))
            assert any(p['path']==q['path'] and same(tuple(p['tuple']),q['tuple']) for q in paths),('Invalid path',index,p)
            assert any(same(tuple(p['tuple']),q['tuple']) for q in front),('Dominated path',index,p)
        for service in ['LS','TS','HR']:
            wanted=select(paths,service,case);got=out[service]
            assert (wanted is None)==(got is None),(index,service)
            if wanted:assert same(wanted['tuple'],tuple(got['tuple'])),('Selection mismatch',index,service,wanted,got)
            checks+=1
    return {'cases':len(inputs),'selectionChecks':checks,'enumeratedPaths':path_count,'status':'passed','scope':'fixed-graph-core','independent':True}

if __name__=='__main__':print(json.dumps(verify(),indent=2))
