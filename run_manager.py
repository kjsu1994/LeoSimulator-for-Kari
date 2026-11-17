# SPDX-License-Identifier: GPL-3.0-only
"""Local, owned native jobs; no external runner or account service."""
from pathlib import Path
from datetime import datetime,timezone
import csv
import ctypes
import hashlib
import json
import math
import os
import re
import subprocess
import threading
import time
import uuid

JOBS={}
LOCK=threading.Lock()
CONFIG_LOCK=threading.RLock()
NO_WIN=getattr(subprocess,'CREATE_NO_WINDOW',0)

def memory_bytes(process):
    if os.name!='nt':return 0
    class Counters(ctypes.Structure):
        _fields_=[('cb',ctypes.c_ulong),('faults',ctypes.c_ulong)]+[(name,ctypes.c_size_t) for name in ('peak','working','peakPaged','paged','peakNonpaged','nonpaged','pagefile','peakPagefile','private')]
    data=Counters();data.cb=ctypes.sizeof(data)
    if ctypes.windll.psapi.GetProcessMemoryInfo(ctypes.c_void_p(int(process._handle)),ctypes.byref(data),data.cb):return data.working
    return 0

def load_external_inputs(cfg,ini_path):
    def path(value):
        file=Path(value)
        return file if file.is_absolute() else Path(ini_path).parent/file
    if cfg.get('scenarioFile'):
        cfg['scenario']=json.loads(path(cfg['scenarioFile']).read_text(encoding='utf-8-sig'))
    scenario=cfg.get('scenario') or {}
    def valid_json(value):
        if isinstance(value,float) and not math.isfinite(value):raise ValueError('Non-finite scenario value')
        if isinstance(value,dict):
            for v in value.values():valid_json(v)
        elif isinstance(value,list):
            for v in value:valid_json(v)
    valid_json(scenario)
    if not isinstance(scenario,dict):raise ValueError('Invalid scenario object')
    entries=scenario.get('entries',[])
    if not isinstance(entries,list):raise ValueError('Invalid congestion scenario')
    for entry in entries:
        if not isinstance(entry,dict) or not isinstance(entry.get('target'),dict) or not isinstance(entry['target'].get('params',{}),dict):raise ValueError('Invalid scenario target')
        start=float(entry.get('start_s',0));end=entry.get('end_s');ramp=float(entry.get('ramp_s',0))
        if start<0 or start>=cfg['simTimeLimit'] or (end is not None and float(end)<=start) or ramp<0 or (end is not None and ramp>float(end)-start):raise ValueError('Invalid congestion time window')
        target=entry.get('target') or {};kind=target.get('kind');p=target.get('params') or {}
        if kind not in ('link','satellite','zone','region','path_hops'):raise ValueError('Unknown congestion target')
        n=cfg['numSatellites']
        for key in ('sat_a','sat_b','sat'):
            if key in p and (not isinstance(p[key],int) or not 0<=p[key]<n):raise ValueError('Invalid satellite ID')
        if kind=='satellite' and 'sat' not in p:raise ValueError('Missing satellite ID')
        if p.get('profile','uniform') not in ('uniform','hotspot') or not 0<=float(p.get('edge_pct',20))<=100:raise ValueError('Invalid region profile')
        if kind=='link' and (p.get('sat_a')==p.get('sat_b') or 'sat_a' not in p or 'sat_b' not in p):raise ValueError('Invalid target link')
        if p.get('direction','both') not in ('both','out','in') or p.get('link_rule','any') not in ('any','both'):raise ValueError('Invalid direction/rule')
        if kind=='link' and 'direction' in p:
            p['bidirectional']=p['direction']=='both'
            if p['direction']=='in':p['sat_a'],p['sat_b']=p['sat_b'],p['sat_a'];p['direction']='out'
        if kind=='path_hops':
            route=(scenario.get('baseline') or {}).get('route',[])
            if not isinstance(route,list) or any(type(v)!=int or not 0<=v<n for v in route) or not route or not 1<=p.get('hop_from',1)<=p.get('hop_to',1)<len(route):raise ValueError('Select a saved baseline route before selecting hops')
        shape=p.get('shape','rect')
        if kind=='region' or (kind=='zone' and p.get('zone_kind')=='latlon_box'):
            if shape=='circle':
                if not (-90<=float(p.get('center_lat',999))<=90 and -180<=float(p.get('center_lon',999))<=180 and 1<=float(p.get('radius_km',0))<=20000):raise ValueError('Invalid circular region')
            elif shape=='rect':
                if not (-90<=float(p.get('lat_min',999))<float(p.get('lat_max',-999))<=90 and -180<=float(p.get('lon_min',999))<=180 and -180<=float(p.get('lon_max',999))<=180 and p.get('lon_min')!=p.get('lon_max')):raise ValueError('Invalid rectangular region')
            else:raise ValueError('Unknown region shape')
        if kind=='zone':
            z=p.get('zone_kind')
            if z=='plane_range':
                if not 0<=p.get('plane_from',-1)<=p.get('plane_to',-1)<cfg['planes'] or not 0<=p.get('sat_from',-1)<=p.get('sat_to',-1)<n//cfg['planes']:raise ValueError('Invalid constellation zone')
            elif z=='srd':
                if not 0<=p.get('srd_from',-1)<=p.get('srd_to',-1)<n:raise ValueError('Invalid SRD range')
            elif z!='latlon_box':raise ValueError('Unknown zone type')
        if entry.get('fail'):
            if ramp or any(entry.get(k) for k in ('use_capacity','use_buffer','use_queue','penalty')):raise ValueError('Failure cannot be combined with congestion effects')
        else:
            if cfg['congestionMode']!='environment':raise ValueError('Environment effects require environment mode')
            pen=entry.get('penalty') or {}
            if not isinstance(pen,dict):raise ValueError('Invalid penalty')
            if not any(entry.get(k) for k in ('use_capacity','use_buffer','use_queue','penalty')):raise ValueError('Select a congestion effect')
            if not 0<float(entry.get('rho',.6))<1 or not 0<=float(pen.get('loss_pct',0))<=100 or float(pen.get('delay_ms',0))<0 or float(pen.get('jitter_ms',0))<0:raise ValueError('Invalid congestion effect')
    imported=scenario.get('failure_events',[])
    if not isinstance(imported,list):raise ValueError('Invalid failure events')
    cfg['failureReplayEvents']=list(imported)
    if cfg.get('congestionZones') and cfg['congestionMode']!='environment':raise ValueError('zones require environment mode')
    if cfg.get('failureReplay'):
        with path(cfg['failureReplay']).open(encoding='utf-8-sig',newline='') as file:
            rows=list(csv.DictReader(file))
        cfg['failureReplayEvents'] += [{'time':float(r['time']),'event':r['event'],'a':int(r['satA']),'b':int(r['satB'])} for r in rows]
    for e in cfg['failureReplayEvents']:
        if not isinstance(e,dict) or e.get('event') not in ('failure','restore') or not 0<=float(e.get('time',-1))<=cfg['simTimeLimit'] or any(not isinstance(e.get(k),int) or not 0<=e[k]<cfg['numSatellites'] for k in ('a','b')) or e['a']==e['b']:raise ValueError('Invalid failure replay')
    cfg['failureReplayEvents'].sort(key=lambda e:e['time'])
    cfg['externalInputs']={}
    for key in ('scenarioFile','failureReplay'):
        if cfg.get(key):
            source=path(cfg[key]);cfg['externalInputs'][key]={'path':str(source),'text':source.read_text(encoding='utf-8-sig')}
    return cfg

def new_id():return datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ-')+uuid.uuid4().hex

def execute(base,settings,ini,config_name,read_config,run_id=None,cancel=None,variables=None,frozen=None):
    from result_bundle import make_native_bundle,write_bundle
    run_id=run_id or new_id();base=Path(base);cancel=cancel or threading.Event()
    if frozen is None:
        with CONFIG_LOCK:
            cfg=load_external_inputs(read_config(config_name,variables=variables),ini)
            inputs={p.relative_to(settings).as_posix():p.read_bytes() for p in Path(settings).rglob('*.ini') if not p.name.startswith('.')}
    else:cfg,inputs=frozen
    folder=base/'data/runs'/run_id;folder.mkdir(parents=True,exist_ok=False)
    (folder/'engine-input.json').write_text(json.dumps(cfg,ensure_ascii=False,allow_nan=False),encoding='utf-8')
    start=time.monotonic();peak=0
    executable=base/'engine/engine.exe'
    if not executable.is_file():raise ValueError('Local engine is missing')
    with (folder/'run.log').open('wb') as log:
        process=subprocess.Popen([str(executable),str(folder/'engine-input.json'),str(folder)],stdout=log,stderr=subprocess.STDOUT,creationflags=NO_WIN)
        try:
            while process.poll() is None:
                if cancel.wait(.1):raise InterruptedError('실행을 중지했습니다.')
                peak=max(peak,memory_bytes(process))
                if peak>4*1024**3:raise RuntimeError('엔진 메모리가 4GiB 제한을 초과했습니다.')
        finally:
            if process.poll() is None:
                process.terminate()
                try:process.wait(timeout=1)
                except subprocess.TimeoutExpired:process.kill();process.wait(timeout=1)
    if cancel.is_set():raise InterruptedError('실행을 중지했습니다.')
    if process.returncode:raise RuntimeError((folder/'run.log').read_text(encoding='utf-8',errors='replace')[-4000:])
    summary=json.loads((folder/'summary.json').read_text());summary['elapsedSeconds']=time.monotonic()-start;summary['peakMemoryBytes']=peak
    (folder/'summary.json').write_text(json.dumps(summary,ensure_ascii=False,allow_nan=False,indent=2),encoding='utf-8')
    profile=json.loads((base/'replay-model.json').read_text(encoding='utf-8'))
    bundle,payload=make_native_bundle(inputs,folder,config_name,profile['modelHash'],cfg,run_id)
    build=base/'engine/build-info.json'
    if build.exists():bundle['manifest']['build']=json.loads(build.read_text())
    bundle['manifest']['engine']['sha256']=hashlib.sha256(executable.read_bytes()).hexdigest()
    write_bundle(folder,bundle,payload)
    result={'ok':True,'runId':run_id,'bundle':bundle}
    (folder/'result.json').write_text(json.dumps(result,ensure_ascii=False,allow_nan=False),encoding='utf-8')
    target=base/'data/latest_result.js';tmp=target.with_suffix('.tmp');tmp.write_text('window.SIM_RESULT='+json.dumps(result,ensure_ascii=True,allow_nan=False)+';\n',encoding='utf-8');os.replace(tmp,target)
    pointer=base/'data/latest.json';tmp=pointer.with_suffix('.tmp');tmp.write_text(json.dumps({'runId':run_id}),encoding='utf-8');os.replace(tmp,pointer)
    return result

def start(base,settings,ini,name,read_config,variables=None):
    # Validate before accepting the job; a rejected request creates no run folder.
    with CONFIG_LOCK:
        cfg=load_external_inputs(read_config(name,variables=variables),ini)
        inputs={p.relative_to(settings).as_posix():p.read_bytes() for p in Path(settings).rglob('*.ini') if not p.name.startswith('.')}
    with LOCK:
        if any(j['status'] in ('queued','running','stopping') for j in JOBS.values()):raise ValueError('다른 계산이 실행 중입니다.')
        run_id=new_id();event=threading.Event();job={'runId':run_id,'status':'queued','cancel':event,'base':str(base)};JOBS[run_id]=job
    def work():
        job['status']='running';job['started']=time.monotonic()
        try:
            result=execute(base,settings,ini,name,read_config,run_id,event,variables,(cfg,inputs))
            job['status']='completed';job['result']=result
        except InterruptedError as e:job.update(status='cancelled',error=str(e))
        except Exception as e:job.update(status='failed',error=str(e))
    thread=threading.Thread(target=work,daemon=True);job['thread']=thread;thread.start()
    return {'ok':True,'runId':run_id,'status':'queued'}

def state(run_id):
    with LOCK:job=JOBS.get(run_id)
    if not job:raise ValueError('알 수 없는 실행입니다.')
    out={k:v for k,v in job.items() if k not in ('cancel','thread','started','base')}
    if 'started' in job:out['elapsedSeconds']=time.monotonic()-job['started']
    try:out['progress']=json.loads((Path(job['base'])/'data/runs'/run_id/'progress.json').read_text())
    except (OSError,ValueError):pass
    return out

def cancel(run_id):
    with LOCK:job=JOBS.get(run_id)
    if not job:raise ValueError('알 수 없는 실행입니다.')
    if job['status'] in ('queued','running'):job['status']='stopping';job['cancel'].set()
    return {'ok':True,'runId':run_id,'status':job['status']}

def shutdown():
    with LOCK:jobs=list(JOBS.values())
    for job in jobs:job['cancel'].set()
    for job in jobs:job['thread'].join(timeout=2)
